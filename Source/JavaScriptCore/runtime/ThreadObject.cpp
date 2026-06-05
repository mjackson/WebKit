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
#include "ThreadObject.h"

#include "CustomGetterSetter.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "LockObject.h"
#include "JSGlobalProxy.h"
#include "JSPromise.h"
#include "ObjectConstructor.h"
#include "ProxyObject.h"
#include "TopExceptionScope.h"
#include "TypedArrayController.h"
#include <wtf/StackAllocation.h>

namespace JSC {

const ClassInfo JSThread::s_info = { "Thread"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSThread) };

static JSC_DECLARE_HOST_FUNCTION(callThread);
static JSC_DECLARE_HOST_FUNCTION(constructThread);
static JSC_DECLARE_HOST_FUNCTION(threadProtoFuncJoin);
static JSC_DECLARE_HOST_FUNCTION(threadProtoFuncAsyncJoin);
static JSC_DECLARE_HOST_FUNCTION(threadFuncRestrict);
static JSC_DECLARE_CUSTOM_GETTER(threadCurrentGetter);
static JSC_DECLARE_CUSTOM_GETTER(threadIdGetter);
static JSC_DECLARE_HOST_FUNCTION(callConcurrentAccessError);
static JSC_DECLARE_HOST_FUNCTION(constructConcurrentAccessError);

JSThread::JSThread(VM& vm, Structure* structure, Ref<ThreadState>&& state)
    : Base(vm, structure)
    , m_state(WTF::move(state))
{
}

JSThread* JSThread::create(VM& vm, Structure* structure, Ref<ThreadState>&& state)
{
    JSThread* thread = new (NotNull, allocateCell<JSThread>(vm)) JSThread(vm, structure, WTF::move(state));
    thread->finishCreation(vm);
    return thread;
}

void JSThread::destroy(JSCell* cell)
{
    static_cast<JSThread*>(cell)->JSThread::~JSThread();
}

template<typename Visitor>
void JSThread::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSThread* thisObject = uncheckedDowncast<JSThread>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_result);
}

DEFINE_VISIT_CHILDREN(JSThread);

bool jsThreadsCanBlockOnCurrentThread(VM& vm)
{
    return vm.m_typedArrayController->isAtomicsWaitAllowedOnCurrentThread();
}

// ---------------- spawn / run / completion ----------------

static void settleJoinTicket(AsyncTicket& ticket, JSThread* thread, bool failed)
{
    // `thread` is rooted by the ticket's dependency vector.
    ticket.settle([thread, failed](DeferredWorkTimer::Ticket dwtTicket) {
        JSPromise* promise = uncheckedDowncast<JSPromise>(dwtTicket->target());
        JSGlobalObject* globalObject = promise->realm();
        VM& vm = globalObject->vm();
        if (failed)
            promise->reject(vm, thread->result());
        else
            promise->resolve(globalObject, vm, thread->result());
    });
}

static void threadMain(VM& vm, Ref<ThreadState> state)
{
    state->nativeThread = &Thread::currentSingleton();
    setCurrentThreadState(state.copyRef());
    {
        // The GIL: all JS execution is serialized by the shared VM's JSLock
        // (SPEC-api 5.2). Atom table and stack limits migrate on acquisition.
        JSLockHolder locker(vm);
        // Start from clean per-thread execution state: the previous GIL
        // holder may have left pointers into another (possibly dead)
        // thread's stack in the VM (e.g. the EXCEPTION_SCOPE_VERIFICATION
        // scope chain); see GILParkSavedExecutionState (LockObject.h).
        GILParkSavedExecutionState::resetForFreshThread(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

        JSThread* thread = uncheckedDowncast<JSThread>(state->jsThread.get());
        JSObject* function = asObject(state->fnSlot.get());
        JSGlobalObject* globalObject = function->globalObject();

        MarkedArgumentBuffer args;
        for (auto& slot : state->argSlots)
            args.append(slot.get());
        ASSERT(!args.hasOverflowed());

        auto callData = JSC::getCallData(function);
        NakedPtr<Exception> exception;
        JSValue callResult = JSC::call(globalObject, function, callData, jsUndefined(), args, exception);

        state->fnSlot.clear();
        state->argSlots.clear();

        JSValue resultValue;
        ThreadState::Phase phase;
        if (exception) {
            resultValue = exception->value();
            phase = ThreadState::Phase::Failed;
            scope.clearException();
        } else {
            resultValue = callResult;
            phase = ThreadState::Phase::Finished;
        }

        // Drain the shared VM microtask queue once (GIL-phase rule, 4.6.1).
        vm.drainMicrotasks();
        if (scope.exception()) [[unlikely]]
            scope.clearException();

        thread->setResult(vm, resultValue);

        Vector<Ref<AsyncTicket>> joiners;
        {
            Locker joinLocker { state->joinLock };
            state->phase.store(phase, std::memory_order_release);
            state->joinCondition.notifyAll();
            joiners = std::exchange(state->asyncJoiners, { });
        }
        bool failed = phase == ThreadState::Phase::Failed;
        for (auto& ticket : joiners)
            settleJoinTicket(ticket, thread, failed);

        // Clear owned Strongs (still under the JSLock; SPEC-api 5.10).
        state->threadLocals.clear();
        state->jsThread.clear();
        ThreadManager::singleton().unregisterThread(state);
    }
    setCurrentThreadState(nullptr);
}

JSC_DEFINE_HOST_FUNCTION(callThread, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    return throwVMTypeError(globalObject, scope, "calling Thread constructor without new is invalid"_s);
}

JSC_DEFINE_HOST_FUNCTION(constructThread, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue functionValue = callFrame->argument(0);
    auto callData = JSC::getCallData(functionValue);
    if (callData.type == CallData::Type::None)
        return throwVMTypeError(globalObject, scope, "Thread constructor requires a callable argument"_s);

    RefPtr<ThreadState> state = ThreadManager::singleton().allocateSpawnedThreadState();
    if (!state)
        return throwVMRangeError(globalObject, scope, "too many live Threads (or thread-ID space exhausted)"_s);

    JSValue prototype = callFrame->jsCallee()->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    Structure* structure = JSThread::createStructure(vm, globalObject, prototype.isObject() ? prototype : jsNull());
    JSThread* thread = JSThread::create(vm, structure, Ref { *state });

    // Root the cell, the function, and the arguments across the spawn
    // (SPEC-api 5.10); all created while holding the GIL.
    state->jsThread.set(vm, thread);
    state->fnSlot.set(vm, functionValue);
    for (size_t i = 1; i < callFrame->argumentCount(); ++i)
        state->argSlots.append(Strong<Unknown>(vm, callFrame->uncheckedArgument(i)));

    StackAllocationSpecification stackSpecification;
    if (unsigned stackKB = Options::jsThreadStackSizeKB())
        stackSpecification = StackAllocationSpecification::RequestSize(static_cast<size_t>(stackKB) * 1024);

    // Detach the native handle: join() synchronizes through
    // ThreadState::joinCondition, never through the pthread handle, so
    // keeping it joinable only leaks it (reported by TSAN as a thread leak
    // on every spawn).
    // Capture the VM by Ref: the native handle is detached, so nothing else
    // guarantees the VM outlives this thread. If the embedder drops its last
    // ref while we are running (e.g. main script exits without join()), the
    // VM must stay alive until threadMain returns or this is a use-after-free.
    Thread::create("JS Thread"_s, [state = Ref { *state }, protectedVM = Ref { vm }]() mutable {
        threadMain(protectedVM.get(), WTF::move(state));
    }, ThreadType::JavaScript, Thread::QOS::UserInitiated, Thread::defaultSchedulingPolicy, stackSpecification)->detach();

    return JSValue::encode(thread);
}

// ---------------- join / asyncJoin ----------------

JSC_DEFINE_HOST_FUNCTION(threadProtoFuncJoin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSThread* thread = dynamicDowncast<JSThread>(callFrame->thisValue());
    if (!thread)
        return throwVMTypeError(globalObject, scope, "Thread.prototype.join called on incompatible receiver"_s);

    ThreadState& state = thread->threadState();
    if (currentThreadStateIfExists() == &state)
        return throwVMError(globalObject, scope, createError(globalObject, "Thread cannot join itself"_s));

    if (state.phase.load(std::memory_order_acquire) == ThreadState::Phase::Running) {
        if (!jsThreadsCanBlockOnCurrentThread(vm))
            return throwVMTypeError(globalObject, scope, "Thread.prototype.join cannot block the current thread"_s);
        // Release the GIL while blocked — depth-free (GILDroppedSection,
        // LockObject.h): concurrent joiners wake in arbitrary order, which
        // DropAllLocks' strict-LIFO unwind protocol livelocks on (observed
        // in the join chains of lifecycle/join-semantics.js).
        GILDroppedSection droppedSection(vm);
        Locker joinLocker { state.joinLock };
        while (state.phase.load(std::memory_order_acquire) == ThreadState::Phase::Running)
            state.joinCondition.wait(state.joinLock);
    }

    JSValue result = thread->result();
    if (state.phase.load(std::memory_order_acquire) == ThreadState::Phase::Failed) {
        throwException(globalObject, scope, result);
        return { };
    }
    return JSValue::encode(result);
}

JSC_DEFINE_HOST_FUNCTION(threadProtoFuncAsyncJoin, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSThread* thread = dynamicDowncast<JSThread>(callFrame->thisValue());
    if (!thread)
        return throwVMTypeError(globalObject, scope, "Thread.prototype.asyncJoin called on incompatible receiver"_s);

    ThreadState& state = thread->threadState();
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    Ref<AsyncTicket> ticket = AsyncTicket::create(globalObject, promise, { thread });

    bool settleNow = false;
    {
        Locker joinLocker { state.joinLock };
        if (state.phase.load(std::memory_order_acquire) != ThreadState::Phase::Running)
            settleNow = true;
        else
            state.asyncJoiners.append(ticket.copyRef());
    }
    if (settleNow)
        settleJoinTicket(ticket, thread, state.phase.load(std::memory_order_acquire) == ThreadState::Phase::Failed);
    return JSValue::encode(promise);
}

// ---------------- Thread.current / id ----------------

static JSThread* jsThreadForState(JSGlobalObject* globalObject, VM& vm, Ref<ThreadState> state, JSValue prototype)
{
    if (!state->jsThread)
        state->jsThread.set(vm, JSThread::create(vm, JSThread::createStructure(vm, globalObject, prototype.isObject() ? prototype : jsNull()), state.copyRef()));
    return uncheckedDowncast<JSThread>(state->jsThread.get());
}

JSC_DEFINE_CUSTOM_GETTER(threadCurrentGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Ref<ThreadState> state = ensureCurrentThreadState();
    JSValue prototype = jsNull();
    if (JSObject* ctor = dynamicDowncast<JSObject>(JSValue::decode(thisValue))) {
        prototype = ctor->get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(jsThreadForState(globalObject, vm, WTF::move(state), prototype)));
}

JSC_DEFINE_CUSTOM_GETTER(threadIdGetter, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSThread* thread = dynamicDowncast<JSThread>(JSValue::decode(thisValue));
    if (!thread)
        return throwVMTypeError(globalObject, scope, "Thread.prototype.id called on incompatible receiver"_s);
    return JSValue::encode(jsNumber(thread->threadState().tid));
}

// ---------------- Thread.restrict ----------------

static bool isExcludedRestrictReceiver(JSGlobalObject* globalObject, JSObject* object)
{
    if (object == globalObject)
        return true;
    if (object == globalObject->globalThis())
        return true;
    if (object->inherits<ProxyObject>() || object->inherits<JSGlobalProxy>())
        return true;
    if (object->isEnvironment() || object->isGlobalLexicalEnvironment())
        return true;
    // Species-protected builtin prototype/constructor pairs (Dev 8). Pointer
    // comparisons only; never forces lazy slots.
    if (object == globalObject->arrayPrototype() || object == globalObject->objectPrototype())
        return true;
    if (static_cast<const void*>(object) == static_cast<const void*>(globalObject->regExpPrototype()))
        return true;
    if (static_cast<const void*>(object) == static_cast<const void*>(globalObject->promisePrototype()))
        return true;
    // Dev 11: structures that hijack the indexing header cannot take
    // ArrayStorage.
    if (object->structure()->hijacksIndexingHeader())
        return true;
    return false;
}

JSC_DEFINE_HOST_FUNCTION(threadFuncRestrict, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue argument = callFrame->argument(0);
    if (!argument.isObject())
        return throwVMTypeError(globalObject, scope, "cannot restrict this object"_s);
    JSObject* object = asObject(argument);
    if (isExcludedRestrictReceiver(globalObject, object))
        return throwVMTypeError(globalObject, scope, "cannot restrict this object"_s);

    auto& manager = ThreadManager::singleton();
    switch (manager.objectAffinity(object)) {
    case ThreadManager::Affinity::Owner:
        return JSValue::encode(object); // idempotent from owner
    case ThreadManager::Affinity::Foreign: {
        throwConcurrentAccessError(globalObject, scope, "Thread.restrict called from a non-owning thread"_s);
        return { };
    }
    case ThreadManager::Affinity::None:
        break;
    }

    // 5.7.1 conversions: defeat and pin off the cacheable fast paths.
    object->ensureArrayStorage(vm);
    if (!hasSlowPutArrayStorage(object->indexingType()))
        object->switchToSlowPutArrayStorage(vm);
    if (!object->structure()->isUncacheableDictionary())
        object->convertToUncacheableDictionary(vm);
    object->structure()->setHasBeenFlattenedBefore(true);

    manager.restrictObject(object, ensureCurrentThreadState().get());
    return JSValue::encode(object);
}

// ---------------- ConcurrentAccessError ----------------

static Structure* concurrentAccessErrorStructure(JSGlobalObject* globalObject, VM& vm)
{
    JSValue constructor = globalObject->getDirect(vm, Identifier::fromString(vm, "ConcurrentAccessError"_s));
    if (constructor && constructor.isObject()) {
        JSValue prototype = asObject(constructor)->getDirect(vm, vm.propertyNames->prototype);
        if (prototype && prototype.isObject())
            return ErrorInstance::createStructure(vm, globalObject, prototype);
    }
    return globalObject->errorStructure();
}

Exception* throwConcurrentAccessError(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral message)
{
    VM& vm = globalObject->vm();
    Structure* structure = concurrentAccessErrorStructure(globalObject, vm);
    ErrorInstance* error = ErrorInstance::create(vm, structure, String { message }, jsUndefined());
    return throwException(globalObject, scope, error);
}

JSC_DEFINE_HOST_FUNCTION(callConcurrentAccessError, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    Structure* structure = concurrentAccessErrorStructure(globalObject, vm);
    return JSValue::encode(ErrorInstance::create(globalObject, structure, callFrame->argument(0), callFrame->argument(1)));
}

JSC_DEFINE_HOST_FUNCTION(constructConcurrentAccessError, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue prototype = callFrame->jsCallee()->get(globalObject, vm.propertyNames->prototype);
    RETURN_IF_EXCEPTION(scope, { });
    Structure* structure = prototype.isObject() ? ErrorInstance::createStructure(vm, globalObject, prototype) : concurrentAccessErrorStructure(globalObject, vm);
    RELEASE_AND_RETURN(scope, JSValue::encode(ErrorInstance::create(globalObject, structure, callFrame->argument(0), callFrame->argument(1))));
}

// ---------------- property factories ----------------

static void linkConstructorAndPrototype(VM& vm, JSObject* constructor, JSObject* prototype, ASCIILiteral tag)
{
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->toStringTagSymbol, jsNontrivialString(vm, String { tag }), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly));
}

JSValue createThreadProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject);
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "join"_s), 0, threadProtoFuncJoin, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "asyncJoin"_s), 0, threadProtoFuncAsyncJoin, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirectCustomAccessor(vm, Identifier::fromString(vm, "id"_s), CustomGetterSetter::create(vm, threadIdGetter, nullptr), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 1, "Thread"_s, callThread, ImplementationVisibility::Public, NoIntrinsic, constructThread);
    linkConstructorAndPrototype(vm, constructor, prototype, "Thread"_s);
    constructor->putDirectCustomAccessor(vm, Identifier::fromString(vm, "current"_s), CustomGetterSetter::create(vm, threadCurrentGetter, nullptr), static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::CustomAccessor | PropertyAttribute::ReadOnly));
    constructor->putDirectNativeFunction(vm, globalObject, Identifier::fromString(vm, "restrict"_s), 1, threadFuncRestrict, ImplementationVisibility::Public, NoIntrinsic, static_cast<unsigned>(PropertyAttribute::DontEnum));
    return constructor;
}

JSValue createConcurrentAccessErrorProperty(VM& vm, JSObject* globalObjectArg)
{
    JSGlobalObject* globalObject = uncheckedDowncast<JSGlobalObject>(globalObjectArg);
    JSObject* prototype = constructEmptyObject(globalObject, globalObject->errorPrototype());
    prototype->putDirect(vm, vm.propertyNames->name, jsNontrivialString(vm, "ConcurrentAccessError"_s), static_cast<unsigned>(PropertyAttribute::DontEnum));
    prototype->putDirect(vm, vm.propertyNames->message, jsEmptyString(vm), static_cast<unsigned>(PropertyAttribute::DontEnum));

    JSFunction* constructor = JSFunction::create(vm, globalObject, 1, "ConcurrentAccessError"_s, callConcurrentAccessError, ImplementationVisibility::Public, NoIntrinsic, constructConcurrentAccessError);
    constructor->putDirect(vm, vm.propertyNames->prototype, prototype, static_cast<unsigned>(PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly));
    prototype->putDirect(vm, vm.propertyNames->constructor, constructor, static_cast<unsigned>(PropertyAttribute::DontEnum));
    return constructor;
}

} // namespace JSC
