/*
 * Copyright (C) 2013-2017 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSMicrotask.h"

#include "CatchScope.h"
#include "Debugger.h"
#include "DeferTermination.h"
#include "GlobalObjectMethodTable.h"
#include "JSGlobalObject.h"
#include "JSObjectInlines.h"
#include "JSPromise.h"
#include "JSPromisePrototype.h"
#include "JSPromiseReaction.h"
#include "Microtask.h"
#include "ObjectConstructor.h"
#if USE(BUN_JSC_ADDITIONS)
#include "InternalFieldTuple.h"
#include "JSArray.h"
#include "ObjectInitializationScope.h"
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

static ALWAYS_INLINE JSCell* dynamicCastToCell(JSValue value)
{
    if (value.isCell())
        return value.asCell();
    return nullptr;
}

static void promiseResolveThenableJobFastSlow(JSGlobalObject* globalObject, JSPromise* promise, JSPromise* promiseToResolve)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSObject* constructor = promiseSpeciesConstructor(globalObject, promise);
    if (scope.exception()) [[unlikely]]
        return;

    auto [resolve, reject] = promiseToResolve->createResolvingFunctions(vm, globalObject);

    auto capability = JSPromise::createNewPromiseCapability(globalObject, constructor);
    if (!scope.exception()) [[likely]] {
        promise->performPromiseThen(globalObject, resolve, reject, capability, jsUndefined());
        if (!scope.exception()) [[likely]]
            return;
    }

    JSValue error = scope.exception()->value();
    if (!scope.clearExceptionExceptTermination()) [[unlikely]]
        return;

    MarkedArgumentBuffer arguments;
    arguments.append(error);
    ASSERT(!arguments.hasOverflowed());
    auto callData = JSC::getCallData(reject);
    call(globalObject, reject, callData, jsUndefined(), arguments);
    EXCEPTION_ASSERT(scope.exception() || true);
}

static void promiseResolveThenableJobWithoutPromiseFastSlow(JSGlobalObject* globalObject, JSPromise* promise, JSValue onFulfilled, JSValue onRejected, JSValue context)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

    JSObject* constructor = promiseSpeciesConstructor(globalObject, promise);
    if (scope.exception()) [[unlikely]]
        return;

    auto [resolve, reject] = JSPromise::createResolvingFunctionsWithoutPromise(vm, globalObject, onFulfilled, onRejected, context);

    auto capability = JSPromise::createNewPromiseCapability(globalObject, constructor);
    if (!scope.exception()) [[likely]] {
        promise->performPromiseThen(globalObject, resolve, reject, capability, jsUndefined());
        if (!scope.exception()) [[likely]]
            return;
    }

    JSValue error = scope.exception()->value();
    if (!scope.clearExceptionExceptTermination()) [[unlikely]]
        return;

    MarkedArgumentBuffer arguments;
    arguments.append(error);
    ASSERT(!arguments.hasOverflowed());
    auto callData = JSC::getCallData(reject);
    call(globalObject, reject, callData, jsUndefined(), arguments);
    EXCEPTION_ASSERT(scope.exception() || true);
}

static void promiseResolveThenableJob(JSGlobalObject* globalObject, JSValue promise, JSValue then, JSValue resolve, JSValue reject, JSValue asyncContext)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_CATCH_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    // AsyncLocalStorage support: set async context before calling then method
    // Matches behavior from PromiseOperations.js promiseResolveThenableJobWithAsyncContext:
    //   var prev = @getInternalField(@asyncContext, 0);
    //   @putInternalField(@asyncContext, 0, asyncContext);
    //   try { then.@call(...) } finally { @putInternalField(@asyncContext, 0, prev); }
    JSValue previousAsyncContext;
    bool hasAsyncContext = false;
    if (!asyncContext.isUndefined()) {
        if (auto* asyncContextData = globalObject->m_asyncContextData.get()) {
            previousAsyncContext = asyncContextData->getInternalField(0);
            asyncContextData->putInternalField(vm, 0, asyncContext);
            hasAsyncContext = true;
        }
    }
#endif

    {
        MarkedArgumentBuffer arguments;
        arguments.append(resolve);
        arguments.append(reject);
        ASSERT(!arguments.hasOverflowed());

        callMicrotask(globalObject, then, promise, dynamicCastToCell(then), arguments, "|then| is not a function"_s);
        if (!scope.exception()) [[likely]] {
#if USE(BUN_JSC_ADDITIONS)
            // Restore async context before returning
            if (hasAsyncContext) {
                if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                    asyncContextData->putInternalField(vm, 0, previousAsyncContext);
            }
#endif
            return;
        }
    }

    JSValue error = scope.exception()->value();
    if (!scope.clearExceptionExceptTermination()) [[unlikely]] {
#if USE(BUN_JSC_ADDITIONS)
        // Restore async context before returning
        if (hasAsyncContext) {
            if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                asyncContextData->putInternalField(vm, 0, previousAsyncContext);
        }
#endif
        return;
    }

#if USE(BUN_JSC_ADDITIONS)
    // Restore async context after exception handling
    if (hasAsyncContext) {
        if (auto* asyncContextData = globalObject->m_asyncContextData.get())
            asyncContextData->putInternalField(vm, 0, previousAsyncContext);
    }
#endif

    MarkedArgumentBuffer arguments;
    arguments.append(error);
    ASSERT(!arguments.hasOverflowed());
    call(globalObject, reject, jsUndefined(), arguments, "|reject| is not a function"_s);
    EXCEPTION_ASSERT(scope.exception() || true);
}

void runInternalMicrotask(JSGlobalObject* globalObject, InternalMicrotask task, std::span<const JSValue, maxMicrotaskArguments> arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (task) {
    case InternalMicrotask::PromiseResolveThenableJobFast: {
        auto* promise = jsCast<JSPromise*>(arguments[0]);
        auto* promiseToResolve = jsCast<JSPromise*>(arguments[1]);

        if (!promiseSpeciesWatchpointIsValid(vm, promise)) [[unlikely]]
            RELEASE_AND_RETURN(scope, promiseResolveThenableJobFastSlow(globalObject, promise, promiseToResolve));

        switch (promise->status()) {
        case JSPromise::Status::Pending: {
#if USE(BUN_JSC_ADDITIONS)
            // AsyncLocalStorage support: wrap context with async context if present
            // Matches behavior from PromiseOperations.js promiseResolveThenableJobFast:
            //   var asyncContext = @getInternalField(@asyncContext, 0);
            //   if (asyncContext)
            //       context = [jsUndefined(), asyncContext];
            JSValue context = jsUndefined();
            if (auto* asyncContextData = globalObject->m_asyncContextData.get()) {
                JSValue asyncContext = asyncContextData->getInternalField(0);
                if (!asyncContext.isUndefined()) {
                    // Create array [context, asyncContext]
                    ObjectInitializationScope initializationScope(vm);
                    JSArray* contextArray = JSArray::tryCreateUninitializedRestricted(initializationScope,
                        globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 2);
                    if (contextArray) {
                        contextArray->initializeIndex(initializationScope, 0, context);
                        contextArray->initializeIndex(initializationScope, 1, asyncContext);
                        context = contextArray;
                    }
                }
            }
            auto* reaction = JSPromiseReaction::create(vm, globalObject->promiseReactionStructure(), promiseToResolve, jsUndefined(), jsUndefined(), context, promise->reactionsOrResult());
#else
            auto* reaction = JSPromiseReaction::create(vm, globalObject->promiseReactionStructure(), promiseToResolve, jsUndefined(), jsUndefined(), jsUndefined(), promise->reactionsOrResult());
#endif
            promise->setReactionsOrResult(vm, reaction);
            break;
        }
        case JSPromise::Status::Rejected: {
            if (!promise->isHandled()) {
                if (globalObject->globalObjectMethodTable()->promiseRejectionTracker) {
                    globalObject->globalObjectMethodTable()->promiseRejectionTracker(globalObject, promise, JSPromiseRejectionOperation::Handle);
                    RETURN_IF_EXCEPTION(scope, void());
                }
            }
            scope.release();
            globalObject->queueMicrotask(InternalMicrotask::PromiseResolveWithoutHandlerJob, promiseToResolve, promise->reactionsOrResult(), jsNumber(static_cast<int32_t>(JSPromise::Status::Rejected)), jsUndefined());
            break;
        }
        case JSPromise::Status::Fulfilled: {
            scope.release();
            globalObject->queueMicrotask(InternalMicrotask::PromiseResolveWithoutHandlerJob, promiseToResolve, promise->reactionsOrResult(), jsNumber(static_cast<int32_t>(JSPromise::Status::Fulfilled)), jsUndefined());
            break;
        }
        }
        promise->markAsHandled();
        return;
    }

    case InternalMicrotask::PromiseResolveThenableJobWithoutPromiseFast: {
        auto* promise = jsCast<JSPromise*>(arguments[0]);
        JSValue onFulfilled = arguments[1];
        JSValue onRejected = arguments[2];
        JSValue context = arguments[3];

        if (!promiseSpeciesWatchpointIsValid(vm, promise)) [[unlikely]]
            RELEASE_AND_RETURN(scope, promiseResolveThenableJobWithoutPromiseFastSlow(globalObject, promise, onFulfilled, onRejected, context));

        switch (promise->status()) {
        case JSPromise::Status::Pending: {
#if USE(BUN_JSC_ADDITIONS)
            // AsyncLocalStorage support: wrap context with async context if present
            // Matches behavior from PromiseOperations.js promiseResolveThenableJobWithoutPromiseFast:
            //   var asyncContext = @getInternalField(@asyncContext, 0);
            //   if (asyncContext)
            //       context = [context, asyncContext];
            if (auto* asyncContextData = globalObject->m_asyncContextData.get()) {
                JSValue asyncContext = asyncContextData->getInternalField(0);
                if (!asyncContext.isUndefined()) {
                    // Create array [context, asyncContext]
                    ObjectInitializationScope initializationScope(vm);
                    JSArray* contextArray = JSArray::tryCreateUninitializedRestricted(initializationScope,
                        globalObject->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 2);
                    if (contextArray) {
                        contextArray->initializeIndex(initializationScope, 0, context);
                        contextArray->initializeIndex(initializationScope, 1, asyncContext);
                        context = contextArray;
                    }
                }
            }
#endif
            auto* reaction = JSPromiseReaction::create(vm, globalObject->promiseReactionStructure(), jsUndefined(), onFulfilled, onRejected, context, promise->reactionsOrResult());
            promise->setReactionsOrResult(vm, reaction);
            break;
        }
        case JSPromise::Status::Rejected: {
            if (!promise->isHandled()) {
                if (globalObject->globalObjectMethodTable()->promiseRejectionTracker) {
                    globalObject->globalObjectMethodTable()->promiseRejectionTracker(globalObject, promise, JSPromiseRejectionOperation::Handle);
                    RETURN_IF_EXCEPTION(scope, void());
                }
            }

            scope.release();
            globalObject->queueMicrotask(InternalMicrotask::PromiseReactionJobWithoutPromise, onRejected, promise->reactionsOrResult(), context, jsUndefined());
            break;
        }
        case JSPromise::Status::Fulfilled: {
            scope.release();
            globalObject->queueMicrotask(InternalMicrotask::PromiseReactionJobWithoutPromise, onFulfilled, promise->reactionsOrResult(), context, jsUndefined());
            break;
        }
        }

        promise->markAsHandled();
        return;
    }

    case InternalMicrotask::PromiseResolveThenableJob: {
        JSValue promise = arguments[0];
        JSValue then = arguments[1];
        JSValue resolve = arguments[2];
        JSValue reject = arguments[3];
#if USE(BUN_JSC_ADDITIONS)
        JSValue asyncContext = arguments[4];
        RELEASE_AND_RETURN(scope, promiseResolveThenableJob(globalObject, promise, then, resolve, reject, asyncContext));
#else
        RELEASE_AND_RETURN(scope, promiseResolveThenableJob(globalObject, promise, then, resolve, reject, jsUndefined()));
#endif
    }

    case InternalMicrotask::PromiseResolveWithoutHandlerJob: {
        auto* promise = jsCast<JSPromise*>(arguments[0]);
        JSValue resolution = arguments[1];
        switch (static_cast<JSPromise::Status>(arguments[2].asInt32())) {
        case JSPromise::Status::Pending: {
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        case JSPromise::Status::Fulfilled: {
            scope.release();
            promise->resolvePromise(globalObject, resolution);
            break;
        }
        case JSPromise::Status::Rejected: {
            scope.release();
            promise->rejectPromise(globalObject, resolution);
            break;
        }
        }
        return;
    }

    case InternalMicrotask::PromiseReactionJob: {
        JSValue promiseOrCapability = arguments[0];
        JSValue handler = arguments[1];
        JSValue argument = arguments[2];
        JSValue context = arguments[3];

        ASSERT(!promiseOrCapability.isUndefinedOrNull());

#if USE(BUN_JSC_ADDITIONS)
        // AsyncLocalStorage support: extract and set async context if present in context array
        // Matches behavior from PromiseOperations.js promiseReactionJobWithoutPromiseUnwrapAsyncContext:
        //   if (@isJSArray(context)) {
        //       prev = @getInternalField(@asyncContext, 0);
        //       @putInternalField(@asyncContext, 0, context[1]);
        //       context = context[0];
        //   }
        JSValue previousAsyncContext;
        bool hasAsyncContext = false;
        dataLog("PromiseReactionJob: handler = ", handler, ", context = ", context, ", isArray = ", context.isCell() && context.asCell()->inherits<JSArray>(), "\n");
        if (auto* contextArray = jsDynamicCast<JSArray*>(context)) {
            dataLog("PromiseReactionJob: contextArray length = ", contextArray->length(), "\n");
            if (contextArray->length() == 2) {
                if (auto* asyncContextData = globalObject->m_asyncContextData.get()) {
                    previousAsyncContext = asyncContextData->getInternalField(0);
                    JSValue asyncContext = contextArray->getIndexQuickly(1);
                    dataLog("PromiseReactionJob: Setting async context from ", previousAsyncContext, " to ", asyncContext, ", about to call handler\n");
                    asyncContextData->putInternalField(vm, 0, asyncContext);
                    // Verify it was set
                    JSValue verifyContext = asyncContextData->getInternalField(0);
                    dataLog("PromiseReactionJob: Verified async context is now ", verifyContext, "\n");
                    // Check if it's an array and print its contents
                    if (auto* verifyArray = jsDynamicCast<JSArray*>(verifyContext)) {
                        dataLog("PromiseReactionJob: Async context is an array with length ", verifyArray->length(), "\n");
                        if (verifyArray->length() > 0) {
                            dataLog("PromiseReactionJob: Array[0] = ", verifyArray->getIndexQuickly(0), "\n");
                        }
                        if (verifyArray->length() > 1) {
                            dataLog("PromiseReactionJob: Array[1] = ", verifyArray->getIndexQuickly(1), "\n");
                        }
                    }
                    context = contextArray->getIndexQuickly(0);
                    hasAsyncContext = true;
                }
            }
        }
#endif

        JSValue result;
        JSValue error;
        {
            auto catchScope = DECLARE_CATCH_SCOPE(vm);
            // Use MarkedArgumentBuffer with updated context
#if USE(BUN_JSC_ADDITIONS)
            dataLog("PromiseReactionJob: About to call handler, async context should be set\n");
            if (auto* asyncContextData = globalObject->m_asyncContextData.get()) {
                JSValue currentContext = asyncContextData->getInternalField(0);
                dataLog("PromiseReactionJob: Current async context before calling handler: ", currentContext, "\n");
            }
#endif
            if (context.isUndefinedOrNull()) {
                MarkedArgumentBuffer args;
                args.append(argument);
                ASSERT(!args.hasOverflowed());
                result = callMicrotask(globalObject, handler, jsUndefined(), dynamicCastToCell(handler), args, "handler is not a function"_s);
            } else {
                MarkedArgumentBuffer args;
                args.append(argument);
                args.append(context);
                ASSERT(!args.hasOverflowed());
                result = callMicrotask(globalObject, handler, jsUndefined(), dynamicCastToCell(context), args, "handler is not a function"_s);
            }

            if (catchScope.exception()) {
                error = catchScope.exception()->value();
                if (!catchScope.clearExceptionExceptTermination()) [[unlikely]] {
#if USE(BUN_JSC_ADDITIONS)
                    // Restore async context before returning
                    if (hasAsyncContext) {
                        if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                            asyncContextData->putInternalField(vm, 0, previousAsyncContext);
                    }
#endif
                    scope.release();
                    return;
                }
            }
        }
#if USE(BUN_JSC_ADDITIONS)
        dataLog("PromiseReactionJob: handler completed, result = ", result, ", error = ", error, "\n");
#endif

        if (error) {
            if (auto* promise = jsDynamicCast<JSPromise*>(promiseOrCapability)) {
                promise->rejectPromise(globalObject, error);
#if USE(BUN_JSC_ADDITIONS)
                // Restore async context after promise operation
                if (hasAsyncContext) {
                    if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                        asyncContextData->putInternalField(vm, 0, previousAsyncContext);
                }
#endif
                return;
            }

            JSValue reject = promiseOrCapability.get(globalObject, vm.propertyNames->reject);
            RETURN_IF_EXCEPTION(scope, void());

            MarkedArgumentBuffer arguments;
            arguments.append(error);
            ASSERT(!arguments.hasOverflowed());
            scope.release();
            call(globalObject, reject, jsUndefined(), arguments, "reject is not a function"_s);
#if USE(BUN_JSC_ADDITIONS)
            // Restore async context after operation
            if (hasAsyncContext) {
                if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                    asyncContextData->putInternalField(vm, 0, previousAsyncContext);
            }
#endif
            return;
        }

        if (auto* promise = jsDynamicCast<JSPromise*>(promiseOrCapability)) {
            promise->resolvePromise(globalObject, result);
#if USE(BUN_JSC_ADDITIONS)
            // Restore async context after promise operation
            if (hasAsyncContext) {
                if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                    asyncContextData->putInternalField(vm, 0, previousAsyncContext);
            }
#endif
            return;
        }

        JSValue resolve = promiseOrCapability.get(globalObject, vm.propertyNames->resolve);
        RETURN_IF_EXCEPTION(scope, void());

        MarkedArgumentBuffer arguments;
        arguments.append(result);
        ASSERT(!arguments.hasOverflowed());
        scope.release();
        call(globalObject, resolve, jsUndefined(), arguments, "resolve is not a function"_s);
#if USE(BUN_JSC_ADDITIONS)
        // Restore async context after operation
        if (hasAsyncContext) {
            if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                asyncContextData->putInternalField(vm, 0, previousAsyncContext);
        }
#endif
        return;
    }

    case InternalMicrotask::PromiseReactionJobWithoutPromise: {
        JSValue handler = arguments[0];
        JSValue argument = arguments[1];
        JSValue context = arguments[2];

#if USE(BUN_JSC_ADDITIONS)
        // AsyncLocalStorage support: extract and set async context if present in context array
        // Matches behavior from PromiseOperations.js promiseReactionJobWithoutPromiseUnwrapAsyncContext:
        //   if (@isJSArray(context)) {
        //       prev = @getInternalField(@asyncContext, 0);
        //       @putInternalField(@asyncContext, 0, context[1]);
        //       context = context[0];
        //   }
        JSValue previousAsyncContext;
        bool hasAsyncContext = false;
        if (auto* contextArray = jsDynamicCast<JSArray*>(context)) {
            if (contextArray->length() == 2) {
                if (auto* asyncContextData = globalObject->m_asyncContextData.get()) {
                    previousAsyncContext = asyncContextData->getInternalField(0);
                    JSValue asyncContext = contextArray->getIndexQuickly(1);
                    asyncContextData->putInternalField(vm, 0, asyncContext);
                    context = contextArray->getIndexQuickly(0);
                    hasAsyncContext = true;
                }
            }
        }
#endif

        // Use MarkedArgumentBuffer with updated context
        if (context.isUndefinedOrNull()) {
            MarkedArgumentBuffer args;
            args.append(argument);
            ASSERT(!args.hasOverflowed());
            scope.release();
            callMicrotask(globalObject, handler, jsUndefined(), dynamicCastToCell(handler), args, "handler is not a function"_s);
        } else {
            MarkedArgumentBuffer args;
            args.append(argument);
            args.append(context);
            ASSERT(!args.hasOverflowed());
            scope.release();
            callMicrotask(globalObject, handler, jsUndefined(), dynamicCastToCell(context), args, "handler is not a function"_s);
        }

#if USE(BUN_JSC_ADDITIONS)
        // Restore async context after handler execution
        if (hasAsyncContext) {
            if (auto* asyncContextData = globalObject->m_asyncContextData.get())
                asyncContextData->putInternalField(vm, 0, previousAsyncContext);
        }
#endif

        return;
    }

    case InternalMicrotask::InvokeFunctionJob: {
        JSValue handler = arguments[0];
        scope.release();
        callMicrotask(globalObject, handler, jsUndefined(), nullptr, ArgList { }, "handler is not a function"_s);
        return;
    }

#if USE(BUN_JSC_ADDITIONS)
    case InternalMicrotask::BunPerformMicrotaskJob: {
        // Bun's performMicrotask function:
        // arguments[0]: performMicrotask function
        // arguments[1]: job function
        // arguments[2]: async context
        // arguments[3]: first argument to job (optional)
        // arguments[4]: second argument to job (optional)
        JSValue performMicrotaskFunction = arguments[0];
        MarkedArgumentBuffer args;
        // Add non-empty arguments only
        for (size_t i = 1; i < maxMicrotaskArguments; ++i) {
            if (!arguments[i].isEmpty())
                args.append(arguments[i]);
        }
        ASSERT(!args.hasOverflowed());
        scope.release();
        callMicrotask(globalObject, performMicrotaskFunction, jsUndefined(), nullptr, args, "performMicrotask is not a function"_s);
        return;
    }

    case InternalMicrotask::BunInvokeJobWithArguments: {
        // Simple job invocation with arguments:
        // arguments[0]: job function
        // arguments[1-4]: arguments (optional)
        JSValue job = arguments[0];
        MarkedArgumentBuffer args;
        // Add non-empty arguments only
        for (size_t i = 1; i < maxMicrotaskArguments; ++i) {
            if (!arguments[i].isEmpty())
                args.append(arguments[i]);
        }
        ASSERT(!args.hasOverflowed());
        scope.release();
        callMicrotask(globalObject, job, jsUndefined(), nullptr, args, "job is not a function"_s);
        return;
    }
#endif

    case InternalMicrotask::Opaque: {
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
