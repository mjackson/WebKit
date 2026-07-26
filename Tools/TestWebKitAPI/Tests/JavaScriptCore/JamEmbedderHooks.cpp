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

#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/IdentifierInlines.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSGlobalObjectInlines.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/SourceCode.h>
#include <JavaScriptCore/SourceTaintedOrigin.h>
#include <JavaScriptCore/VM.h>

namespace TestWebKitAPI {

using JSC::HeapType;
using JSC::Identifier;
using JSC::JSGlobalObject;
using JSC::JSLockHolder;
using JSC::VM;

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

} // namespace TestWebKitAPI
