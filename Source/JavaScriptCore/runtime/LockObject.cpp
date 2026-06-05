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
#include "LockObject.h"

#include "CustomGetterSetter.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "JSNativeStdFunction.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ThreadObject.h"
#include <wtf/RunLoop.h>

namespace JSC {

const ClassInfo JSLockObject::s_info = { "Lock"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSLockObject) };

static JSC_DECLARE_HOST_FUNCTION(callLock);
static JSC_DECLARE_HOST_FUNCTION(constructLock);
static JSC_DECLARE_HOST_FUNCTION(lockProtoFuncHold);
static JSC_DECLARE_HOST_FUNCTION(lockProtoFuncAsyncHold);
static JSC_DECLARE_CUSTOM_GETTER(lockLockedGetter);

JSLockObject::JSLockObject(VM& vm, Structure* structure)
    : Base(vm, structure)
    , m_state(NativeLockState::create())
{
}

JSLockObject* JSLockObject::create(VM& vm, Structure* structure)
{
    JSLockObject* object = new (NotNull, allocateCell<JSLockObject>(vm)) JSLockObject(vm, structure);
    object->finishCreation(vm);
    return object;
}

void JSLockObject::destroy(JSCell* cell)
{
    static_cast<JSLockObject*>(cell)->JSLockObject::~JSLockObject();
}

GILParkSavedExecutionState::GILParkSavedExecutionState(VM& vm)
    : m_vm(vm)
    , m_topCallFrame(vm.topCallFrame)
    , m_topEntryFrame(vm.topEntryFrame)
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    , m_topExceptionScope(vm.m_topExceptionScope)
    , m_needExceptionCheck(vm.m_needExceptionCheck)
#endif
{
    ASSERT(vm.apiLock().currentThreadIsHoldingLock());
}

GILParkSavedExecutionState::~GILParkSavedExecutionState()
{
    ASSERT(m_vm.apiLock().currentThreadIsHoldingLock());
    m_vm.topCallFrame = m_topCallFrame;
    m_vm.topEntryFrame = m_topEntryFrame;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    m_vm.m_topExceptionScope = m_topExceptionScope;
    m_vm.m_needExceptionCheck = m_needExceptionCheck;
#endif
}

void GILParkSavedExecutionState::resetForFreshThread(VM& vm)
{
    ASSERT(vm.apiLock().currentThreadIsHoldingLock());
    vm.topCallFrame = nullptr;
    vm.topEntryFrame = nullptr;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    vm.m_topExceptionScope = nullptr;
    vm.m_needExceptionCheck = false;
#endif
}

GILDroppedSection::GILDroppedSection(VM& vm)
    : m_vm(vm)
    , m_savedExecutionState(vm)
    , m_stackPointerAtVMEntry(vm.stackPointerAtVMEntry())
{
    JSLock& apiLock = vm.apiLock();
    ASSERT(apiLock.currentThreadIsHoldingLock());
    while (apiLock.currentThreadIsHoldingLock()) {
        apiLock.unlock();
        m_lockCount++;
    }
}

GILDroppedSection::~GILDroppedSection()
{
    JSLock& apiLock = m_vm.apiLock();
    for (unsigned i = 0; i < m_lockCount; ++i)
        apiLock.lock();

    // Same restoration grabAllLocks performs after its LIFO spin; the
    // intervening holders left THEIR stack-entry bookkeeping in the VM.
    // (m_savedExecutionState's destructor then restores
    // topCallFrame/topEntryFrame, running after this body.)
    m_vm.setStackPointerAtVMEntry(m_stackPointerAtVMEntry);
    m_vm.setLastStackTop(Thread::currentSingleton());
}

void jsThreadGILHandoffYield(VM& vm)
{
    if (!vm.apiLock().currentThreadIsHoldingLock())
        return;
    GILDroppedSection droppedSection(vm);
    Thread::yield();
}

// ---------------- NativeLockState pump machinery (SPEC-api 5.5a) ----------------

void NativeLockState::schedPumpLocked(VM& vm)
{
    if (m_pumpPending)
        return;
    m_pumpPending = true;
    vm.runLoop().dispatch([state = Ref { *this }, &vm] {
        state->pump(vm);
    });
}

void NativeLockState::releasePump(VM& vm)
{
    Locker queueLocker { m_queueLock };
    if (!m_asyncWaiters.isEmpty())
        schedPumpLocked(vm);
}

void NativeLockState::enqueueAsyncAcquirer(Ref<AsyncTicket>&& ticket, VM& vm)
{
    Locker queueLocker { m_queueLock };
    m_asyncWaiters.append(WTF::move(ticket));
    schedPumpLocked(vm);
}

void NativeLockState::asyncReleaseInternal(AsyncTicket& ticket, VM& vm)
{
    {
        Locker queueLocker { m_queueLock };
        ASSERT_UNUSED(ticket, m_asyncHolder == &ticket);
        m_asyncHolder = nullptr;
        m_asyncHeld.store(false, std::memory_order_release);
    }
    m_lock.unlock();
    releasePump(vm);
}

void NativeLockState::pump(VM& vm)
{
    {
        Locker queueLocker { m_queueLock };
        m_pumpPending = false; // clear-before-tryLock is normative
    }
    if (!m_lock.tryLock())
        return; // holder's release will re-run R with pump-pending false
    RefPtr<AsyncTicket> grant;
    {
        Locker queueLocker { m_queueLock };
        if (m_asyncWaiters.isEmpty()) {
            m_lock.unlock();
            return;
        }
        grant = m_asyncWaiters.takeFirst();
        m_asyncHeld.store(true, std::memory_order_release);
        m_asyncHolder = grant;
    }
    settleLockGrant(*this, *grant);
}

// Settle a granted async acquisition. The settle task runs on a run-loop
// turn holding the JSLock.
void settleLockGrant(NativeLockState& state, AsyncTicket& ticket)
{
    Ref<NativeLockState> protectedState { state };
    Ref<AsyncTicket> protectedTicket { ticket };
    if (ticket.grantWithFunction) {
        JSObject* function = ticket.extraDependency();
        ticket.settle([state = WTF::move(protectedState), ticket = WTF::move(protectedTicket), function](DeferredWorkTimer::Ticket dwtTicket) {
            JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
            JSGlobalObject* globalObject = promise->realm();
            VM& vm = globalObject->vm();
            auto callData = JSC::getCallData(function);
            MarkedArgumentBuffer args;
            NakedPtr<Exception> exception;
            JSValue result = JSC::call(globalObject, function, callData, jsUndefined(), args, exception);
            // E: implicit post-fn release (unless cond.asyncWait consumed the hold).
            if (ticket->tryConsume())
                state->asyncReleaseInternal(ticket.get(), vm);
            if (exception)
                promise->reject(vm, exception->value());
            else
                promise->resolve(globalObject, vm, result);
        });
        return;
    }
    ticket.settle([state = WTF::move(protectedState), ticket = WTF::move(protectedTicket)](DeferredWorkTimer::Ticket dwtTicket) {
        JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
        JSGlobalObject* globalObject = promise->realm();
        VM& vm = globalObject->vm();
        JSNativeStdFunction* releaseFunction = JSNativeStdFunction::create(vm, globalObject, 0, "release"_s, [state, ticket](JSGlobalObject* lexicalGlobalObject, CallFrame*) -> EncodedJSValue {
            VM& innerVM = lexicalGlobalObject->vm();
            auto scope = DECLARE_THROW_SCOPE(innerVM);
            if (!ticket->tryConsume())
                return throwVMError(lexicalGlobalObject, scope, createError(lexicalGlobalObject, "Lock release function called more than once"_s));
            state->asyncReleaseInternal(ticket.get(), innerVM);
            return JSValue::encode(jsUndefined());
        });
        promise->resolve(globalObject, vm, releaseFunction);
    });
}

// ---------------- host functions ----------------

JSC_DEFINE_HOST_FUNCTION(callLock, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling Lock constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructLock, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue prototype = callFrame->jsCallee()->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    Structure* structure = JSLockObject::createStructure(vm, globalObject, prototype.isObject() ? prototype : jsNull());
    return JSValue::encode(JSLockObject::create(vm, structure));
}

JSC_DEFINE_HOST_FUNCTION(lockProtoFuncHold, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->thisValue());
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.hold called on incompatible receiver"_s);
    JSValue functionValue = callFrame->argument(0);
    auto callData = JSC::getCallData(functionValue);
    if (callData.type == CallData::Type::None)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.hold requires a callable argument"_s);

    NativeLockState& state = lockObject->lockState();
    if (state.heldByCurrentThread())
        return throwVMError(globalObject, scope, createError(globalObject, "Lock is not recursive"_s));

    if (!state.m_lock.tryLock()) {
        if (!jsThreadsCanBlockOnCurrentThread(vm))
            return throwVMTypeError(globalObject, scope, "Lock.prototype.hold cannot block the current thread"_s);
        // Never hold m_lock across JSLock::grabAllLocks' LIFO unwind spin,
        // and never block on m_lock while holding the GIL (the holder needs
        // the GIL to run JS and release). Same reacquisition shape as
        // conditionProtoFuncWait; see the deadlock note there and the
        // depth-free-handoff rationale on jsThreadGILHandoffYield.
        while (!state.m_lock.tryLock())
            jsThreadGILHandoffYield(vm);
    }
    state.m_holder.store(&Thread::currentSingleton(), std::memory_order_relaxed);

    MarkedArgumentBuffer args;
    NakedPtr<Exception> exception;
    JSValue result = JSC::call(globalObject, functionValue, callData, jsUndefined(), args, exception);

    // Epilogue guard: cond.asyncWait may have consumed the hold (4.3(a)).
    if (state.heldByCurrentThread()) {
        state.releaseSyncHold();
        state.releasePump(vm);
    }

    if (exception) {
        throwException(globalObject, scope, exception.get());
        return { };
    }
    return JSValue::encode(result);
}

JSC_DEFINE_HOST_FUNCTION(lockProtoFuncAsyncHold, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(callFrame->thisValue());
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.asyncHold called on incompatible receiver"_s);

    JSValue functionValue = callFrame->argument(0);
    bool hasFunction = !functionValue.isUndefined();
    if (hasFunction) {
        auto callData = JSC::getCallData(functionValue);
        if (callData.type == CallData::Type::None)
            return throwVMTypeError(globalObject, scope, "Lock.prototype.asyncHold requires a callable argument when one is provided"_s);
    }

    NativeLockState& state = lockObject->lockState();
    if (state.heldByCurrentThread())
        return throwVMError(globalObject, scope, createError(globalObject, "Lock is not recursive"_s));

    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    Vector<JSCell*> dependencies;
    if (hasFunction)
        dependencies.append(asObject(functionValue));
    dependencies.append(lockObject);
    Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, WTF::move(dependencies));
    ticket->grantWithFunction = hasFunction;

    // A: try to acquire immediately; otherwise queue FIFO.
    if (state.m_lock.tryLock()) {
        {
            Locker queueLocker { state.m_queueLock };
            state.m_asyncHeld.store(true, std::memory_order_release);
            state.m_asyncHolder = ticket.ptr();
        }
        settleLockGrant(state, ticket);
    } else
        state.enqueueAsyncAcquirer(ticket.copyRef(), vm);

    return JSValue::encode(promise);
}

JSC_DEFINE_CUSTOM_GETTER(lockLockedGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSLockObject* lockObject = dynamicDowncast<JSLockObject>(JSValue::decode(thisValue));
    if (!lockObject)
        return throwVMTypeError(globalObject, scope, "Lock.prototype.locked called on incompatible receiver"_s);
    NativeLockState& state = lockObject->lockState();
    return JSValue::encode(jsBoolean(state.m_lock.isLocked() || state.m_asyncHeld.load(std::memory_order_acquire)));
}

JSValue createLockProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "hold"_s), 1, lockProtoFuncHold, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "asyncHold"_s), 0, lockProtoFuncAsyncHold, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectCustomAccessor(vm, Identifier::fromString(vm, "locked"_s), CustomGetterSetter::create(vm, lockLockedGetter, nullptr), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 0, "Lock"_s, callLock, ImplementationVisibility::Public, NoIntrinsic, constructLock);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, "Lock"_s), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
    return constructor;
}

} // namespace JSC
