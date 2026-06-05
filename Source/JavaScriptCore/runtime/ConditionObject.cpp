/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ConditionObject.h"

#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ThreadObject.h"

namespace JSC {

const ClassInfo JSConditionObject::s_info = { "Condition"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSConditionObject) };

static JSC_DECLARE_HOST_FUNCTION(callCondition);
static JSC_DECLARE_HOST_FUNCTION(constructCondition);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncWait);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncAsyncWait);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncNotify);
static JSC_DECLARE_HOST_FUNCTION(conditionProtoFuncNotifyAll);

JSConditionObject::JSConditionObject(VM& vm, Structure* structure)
    : Base(vm, structure)
    , m_state(NativeConditionState::create())
{
}

JSConditionObject* JSConditionObject::create(VM& vm, Structure* structure)
{
    JSConditionObject* object = new (NotNull, allocateCell<JSConditionObject>(vm)) JSConditionObject(vm, structure);
    object->finishCreation(vm);
    return object;
}

void JSConditionObject::destroy(JSCell* cell)
{
    static_cast<JSConditionObject*>(cell)->JSConditionObject::~JSConditionObject();
}

JSC_DEFINE_HOST_FUNCTION(callCondition, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling Condition constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructCondition, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue prototype = callFrame->jsCallee()->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    Structure* structure = JSConditionObject::createStructure(vm, globalObject, prototype.isObject() ? prototype : jsNull());
    return JSValue::encode(JSConditionObject::create(vm, structure));
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncWait, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSConditionObject* conditionObject = dynamicDowncast<JSConditionObject>(callFrame->thisValue());
    if (!conditionObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait called on incompatible receiver"_s);
    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->argument(0));
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait requires a Lock argument"_s);

    NativeLockState& lock = lockObject->lockState();
    if (!lock.heldByCurrentThread())
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait requires the lock to be held by the caller"_s);
    if (!jsThreadsCanBlockOnCurrentThread(vm))
        return throwVMTypeError(globalObject, scope, "Condition.prototype.wait cannot block the current thread"_s);

    NativeConditionState& condition = conditionObject->conditionState();
    Ref<CondWaiter> waiter = CondWaiter::create(CondWaiter::Kind::Sync);

    // Enqueue before releasing the JS lock: no lost wakeups (SPEC-api F3).
    {
        Locker queueLocker { condition.queueLock };
        condition.waiters.append(waiter.copyRef());
    }

    // Release the JS lock, then the GIL; park.
    lock.releaseSyncHold();
    lock.releasePump(vm);
    {
        // Depth-free GIL drop (see GILDroppedSection in LockObject.h):
        // waiters park and wake in arbitrary order, which DropAllLocks'
        // strict-LIFO unwind protocol cannot tolerate. Do NOT reacquire
        // lock.m_lock inside this scope either: holding it across the GIL
        // reacquire would deadlock against the other waiters of the same
        // Lock woken by one notifyAll.
        GILDroppedSection droppedSection(vm);
        Locker queueLocker { condition.queueLock };
        while (!waiter->state.load(std::memory_order_acquire))
            waiter->condition.wait(condition.queueLock);
    }
    // GIL held again. Reacquire the Lock without ever blocking on m_lock
    // while holding the GIL (the holder needs the GIL to run JS and
    // release): tryLock, and on failure hand the GIL off around a yield so
    // the holder can finish (depth-free; see jsThreadGILHandoffYield).
    // Spin-with-yield is acceptable for the phase-1 GIL stub; the real
    // implementation replaces this with a parking acquire.
    while (!lock.m_lock.tryLock())
        jsThreadGILHandoffYield(vm);
    lock.m_holder.store(&Thread::currentSingleton(), std::memory_order_relaxed);
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncAsyncWait, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSConditionObject* conditionObject = dynamicDowncast<JSConditionObject>(callFrame->thisValue());
    if (!conditionObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.asyncWait called on incompatible receiver"_s);
    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->argument(0));
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.asyncWait requires a Lock argument"_s);

    NativeLockState& lock = lockObject->lockState();

    // (a) sync-held by the caller, or (b) async-held; both consume the hold.
    if (lock.heldByCurrentThread()) {
        lock.releaseSyncHold();
        lock.releasePump(vm);
    } else {
        RefPtr<AsyncTicket> holder;
        {
            Locker queueLocker { lock.m_queueLock };
            holder = lock.m_asyncHolder;
        }
        if (!holder)
            return throwVMTypeError(globalObject, scope, "Condition.prototype.asyncWait requires the lock to be held"_s);
        holder->tryConsume(); // (b) is unvalidated; an outstanding release fn then throws
        lock.asyncReleaseInternal(*holder, vm);
    }

    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, { lockObject });
    ticket->grantWithFunction = false; // resolves with a fresh release fn on re-grant

    Ref<CondWaiter> waiter = CondWaiter::create(CondWaiter::Kind::Async);
    waiter->ticket = ticket.ptr();
    waiter->lock = &lock;
    {
        NativeConditionState& condition = conditionObject->conditionState();
        Locker queueLocker { condition.queueLock };
        condition.waiters.append(WTF::move(waiter));
    }
    return JSValue::encode(promise);
}

static EncodedJSValue notifyImpl(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned maxCount)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSConditionObject* conditionObject = dynamicDowncast<JSConditionObject>(callFrame->thisValue());
    if (!conditionObject)
        return throwVMTypeError(globalObject, scope, "Condition.prototype.notify called on incompatible receiver"_s);

    NativeConditionState& condition = conditionObject->conditionState();
    unsigned woken = 0;
    Vector<Ref<CondWaiter>> asyncWoken;
    {
        Locker queueLocker { condition.queueLock };
        while (woken < maxCount && !condition.waiters.isEmpty()) {
            Ref<CondWaiter> waiter = condition.waiters.takeFirst();
            waiter->state.store(1, std::memory_order_release);
            if (waiter->kind == CondWaiter::Kind::Sync)
                waiter->condition.notifyOne();
            else
                asyncWoken.append(WTF::move(waiter));
            woken++;
        }
    }
    // Async waiters re-enter the lock's async-acquirer queue (5.5a A-failure
    // path); never holding two rank-3 locks at once.
    for (auto& waiter : asyncWoken)
        waiter->lock->enqueueAsyncAcquirer(Ref { *waiter->ticket }, vm);

    // Fair-handoff point for the phase-1 GIL stub: woken sync waiters (and
    // waiters woken by EARLIER notify calls that are still unwinding their
    // GIL droppers) need the GIL to finish wait(). Without this, a notifier
    // that loops in JS (e.g. "notifyAll until everyone reports done") holds
    // the GIL forever and those waiters can never run. Unconditional and
    // depth-free on purpose: see jsThreadGILHandoffYield (LockObject.h) for
    // why a DropAllLocks-based handoff livelocks against parked waiters.
    jsThreadGILHandoffYield(vm);

    return JSValue::encode(jsNumber(woken));
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncNotify, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return notifyImpl(globalObject, callFrame, 1);
}

JSC_DEFINE_HOST_FUNCTION(conditionProtoFuncNotifyAll, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return notifyImpl(globalObject, callFrame, std::numeric_limits<unsigned>::max());
}

JSValue createConditionProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "wait"_s), 1, conditionProtoFuncWait, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "asyncWait"_s), 1, conditionProtoFuncAsyncWait, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "notify"_s), 0, conditionProtoFuncNotify, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "notifyAll"_s), 0, conditionProtoFuncNotifyAll, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 0, "Condition"_s, callCondition, ImplementationVisibility::Public, NoIntrinsic, constructCondition);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, "Condition"_s), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
    return constructor;
}

} // namespace JSC
