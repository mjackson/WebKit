/*
 * Copyright (C) 2026 WebKit contributors. All rights reserved.
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

#include <JavaScriptCore/CallFrame.h>
#include <JavaScriptCore/CachedBytecode.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/IdentifierInlines.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSCJSValuePropertyInlines.h>
#include <JavaScriptCore/JSFunction.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSGlobalObjectInlines.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/MicrotaskQueueInlines.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/SourceTaintedOrigin.h>
#include <JavaScriptCore/StackFrame.h>
#include <JavaScriptCore/SyntheticModuleRecord.h>
#include <JavaScriptCore/VM.h>
#include <limits>

namespace TestWebKitAPI {

using JSC::HeapType;
using JSC::Identifier;
using JSC::JSGlobalObject;
using JSC::JSLockHolder;
using JSC::VM;

static Vector<uint64_t> observedJobOwners;

struct ErrorMaterializerState {
    unsigned calls { 0 };
    size_t frameCount { 0 };
};

static String materializeErrorInfo(VM&, Vector<JSC::StackFrame>& frames, unsigned& line, unsigned& column, String& sourceURL, void* context)
{
    auto& state = *static_cast<ErrorMaterializerState*>(context);
    ++state.calls;
    state.frameCount = frames.size();
    line = 123;
    column = 45;
    sourceURL = "mapped.js"_s;
    return "mapped stack"_s;
}

static JSC::EncodedJSValue recordJobOwner(JSGlobalObject* globalObject, JSC::CallFrame*)
{
    observedJobOwners.append(globalObject->embedderJobOwner().value());
    return JSC::JSValue::encode(JSC::jsUndefined());
}

TEST(JavaScriptCore_JamEmbedderHooks, RemovesExactModuleRegistryEntry)
{
    WTF::initializeMainThread();
    JSC::initialize();

    VM& vm = VM::create(HeapType::Large).leakRef();
    JSLockHolder locker(vm);
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, JSC::jsNull()));
    auto* loader = globalObject->moduleLoader();

    auto firstSource = JSC::makeSource("export default 1"_s, { }, JSC::SourceTaintedOrigin::Untainted, { }, { }, JSC::SourceProviderSourceType::Module);
    auto secondSource = JSC::makeSource("export default 2"_s, { }, JSC::SourceTaintedOrigin::Untainted, { }, { }, JSC::SourceProviderSourceType::Module);
    JSC::loadAndEvaluateModule(globalObject, WTF::move(firstSource), nullptr);
    JSC::loadAndEvaluateModule(globalObject, WTF::move(secondSource), nullptr);

    ASSERT_EQ(loader->moduleRegistryEntries().size(), 2u);
    auto firstEntry = loader->moduleRegistryEntries().begin();
    Identifier firstKey = Identifier::fromUid(vm, firstEntry->key.first);
    auto* secondKey = (++firstEntry)->key.first;

    EXPECT_TRUE(loader->removeModuleRegistryEntry(firstKey));
    EXPECT_FALSE(loader->removeModuleRegistryEntry(firstKey));

    for (auto& entry : loader->moduleRegistryEntries())
        EXPECT_NE(entry.key.first, firstKey.impl());
    EXPECT_EQ(loader->moduleRegistryEntries().size(), 1u);
    EXPECT_EQ(loader->moduleRegistryEntries().begin()->key.first, secondKey);
}

TEST(JavaScriptCore_JamEmbedderHooks, DiscardsTasksForOneGlobalObject)
{
    WTF::initializeMainThread();
    JSC::initialize();

    VM& vm = VM::create(HeapType::Large).leakRef();
    JSLockHolder locker(vm);
    auto queue = JSC::MicrotaskQueue::create(vm);
    auto* firstGlobalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, JSC::jsNull()));
    auto* secondGlobalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, JSC::jsNull()));

    queue->enqueue(JSC::QueuedTask { nullptr, JSC::InternalMicrotask::InvokeFunctionJob, 0, firstGlobalObject, JSC::jsUndefined() });
    queue->enqueue(JSC::QueuedTask { nullptr, JSC::InternalMicrotask::InvokeFunctionJob, 0, secondGlobalObject, JSC::jsUndefined() });
    queue->enqueue(JSC::QueuedTask { nullptr, JSC::InternalMicrotask::InvokeFunctionJob, 0, firstGlobalObject, JSC::jsUndefined() });

    EXPECT_EQ(queue->discardTasksForGlobalObject(*firstGlobalObject), 2u);
    EXPECT_EQ(queue->size(), 1u);
    EXPECT_EQ(queue->discardTasksForGlobalObject(*firstGlobalObject), 0u);
    EXPECT_EQ(queue->discardTasksForGlobalObject(*secondGlobalObject), 1u);
    EXPECT_TRUE(queue->isEmpty());
}

static unsigned promiseCreationNotifications = 0;

static void countPromiseCreation(JSGlobalObject*, JSC::JSPromise*)
{
    promiseCreationNotifications++;
}

TEST(JavaScriptCore_JamEmbedderHooks, NotifiesPromiseCreationTracker)
{
    WTF::initializeMainThread();
    JSC::initialize();

    VM& vm = VM::create(HeapType::Large).leakRef();
    JSLockHolder locker(vm);
    static GlobalObjectMethodTable trackingTable = *JSGlobalObject::baseGlobalObjectMethodTable();
    trackingTable.promiseCreationTracker = &countPromiseCreation;
    auto* globalObject = JSGlobalObject::createWithCustomMethodTable(
        vm, JSGlobalObject::createStructure(vm, JSC::jsNull()), &trackingTable);

    promiseCreationNotifications = 0;
    auto* promise = JSC::JSPromise::create(vm, globalObject->promiseStructure());
    EXPECT_EQ(promiseCreationNotifications, 1u);
    EXPECT_TRUE(!!promise);

    // A realm without the tracker must notify nothing.
    auto* untracked = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, JSC::jsNull()));
    JSC::JSPromise::create(vm, untracked->promiseStructure());
    EXPECT_EQ(promiseCreationNotifications, 1u);
}

TEST(JavaScriptCore_JamEmbedderHooks, RestoresPromiseReactionJobOwners)
{
    WTF::initializeMainThread();
    JSC::initialize();

    VM& vm = VM::create(HeapType::Large).leakRef();
    JSLockHolder locker(vm);
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, JSC::jsNull()));
    auto* promise = JSC::JSPromise::create(vm, globalObject->promiseStructure());
    auto* handler = JSC::JSFunction::create(vm, globalObject, 0, "recordJobOwner"_s, recordJobOwner, JSC::ImplementationVisibility::Private);
    observedJobOwners.clear();

    globalObject->setEmbedderJobOwner(0);
    promise->performPromiseThenExported(vm, globalObject, handler, JSC::jsUndefined(), JSC::jsUndefined());
    globalObject->setEmbedderJobOwner(std::numeric_limits<uint64_t>::max());
    promise->performPromiseThenExported(vm, globalObject, handler, JSC::jsUndefined(), JSC::jsUndefined());

    globalObject->setEmbedderJobOwner(17);
    promise->fulfill(vm, JSC::jsUndefined());
    globalObject->microtaskQueue().performMicrotaskCheckpoint<false>(
        vm, [](JSGlobalObject*, JSGlobalObject*) { });

    ASSERT_EQ(observedJobOwners.size(), 2u);
    EXPECT_EQ(observedJobOwners[0], 0u);
    EXPECT_EQ(observedJobOwners[1], std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(globalObject->embedderJobOwner(), 17u);
}

TEST(JavaScriptCore_JamEmbedderHooks, MaterializesErrorInfoLazily)
{
    WTF::initializeMainThread();
    JSC::initialize();

    VM& vm = VM::create(HeapType::Large).leakRef();
    JSLockHolder locker(vm);
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, JSC::jsNull()));
    ErrorMaterializerState state;
    vm.setErrorInfoMaterializer(materializeErrorInfo, &state);

    auto source = JSC::makeSource("new Error('boom')"_s, { }, JSC::SourceTaintedOrigin::Untainted, "original.js"_s);
    JSC::JSValue error = JSC::evaluate(globalObject, source);
    ASSERT_TRUE(error.isObject());
    EXPECT_EQ(state.calls, 0u);

    auto* errorObject = error.getObject();
    EXPECT_EQ(errorObject->get(globalObject, vm.propertyNames->stack).toWTFString(globalObject), "mapped stack"_s);
    EXPECT_EQ(state.calls, 1u);
    EXPECT_GT(state.frameCount, 0u);
    EXPECT_EQ(errorObject->get(globalObject, vm.propertyNames->line).asNumber(), 123);
    EXPECT_EQ(errorObject->get(globalObject, vm.propertyNames->column).asNumber(), 45);
    EXPECT_EQ(errorObject->get(globalObject, vm.propertyNames->sourceURL).toWTFString(globalObject), "mapped.js"_s);

    errorObject->get(globalObject, vm.propertyNames->stack);
    EXPECT_EQ(state.calls, 1u);

    vm.setErrorInfoMaterializer(nullptr);
    JSC::JSValue defaultError = JSC::evaluate(globalObject, source);
    ASSERT_TRUE(defaultError.isObject());
    EXPECT_NE(defaultError.getObject()->get(globalObject, vm.propertyNames->stack).toWTFString(globalObject), "mapped stack"_s);
}

TEST(JavaScriptCore_JamEmbedderHooks, CreatesSyntheticModuleWithNamedExports)
{
    WTF::initializeMainThread();
    JSC::initialize();

    VM& vm = VM::create(HeapType::Large).leakRef();
    JSLockHolder locker(vm);
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, JSC::jsNull()));
    Vector<Identifier, 4> exportNames;
    exportNames.append(Identifier::fromString(vm, "answer"_s));
    JSC::MarkedArgumentBuffer exportValues;
    exportValues.append(JSC::jsNumber(42));

    auto* record = JSC::SyntheticModuleRecord::tryCreateWithExportNamesAndValues(
        globalObject, Identifier::fromString(vm, "test:synthetic"_s), exportNames, exportValues);
    ASSERT_TRUE(record);
}

TEST(JavaScriptCore_JamEmbedderHooks, BorrowsImmutableCachedBytecode)
{
    const uint8_t storage[] { 1, 2, 3, 4 };
    Ref<JSC::CachedBytecode> bytecode = JSC::CachedBytecode::createBorrowed(std::span(storage));

    EXPECT_EQ(bytecode->span().data(), storage);
    EXPECT_EQ(bytecode->span().size(), sizeof(storage));
}

} // namespace TestWebKitAPI
