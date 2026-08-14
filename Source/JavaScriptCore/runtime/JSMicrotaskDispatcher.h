/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <JavaScriptCore/JSCell.h>
#include <JavaScriptCore/MicrotaskQueue.h>
#include <JavaScriptCore/VM.h>
#include <JavaScriptCore/WriteBarrier.h>

namespace JSC {

class JSMicrotaskDispatcher final : public JSCell {
public:
    using Base = JSCell;

    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;

    DECLARE_EXPORT_INFO;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.jsMicrotaskDispatcherSpace<mode>();
    }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    static JSMicrotaskDispatcher* create(VM&, Structure*, Ref<MicrotaskDispatcher>&&, JSGlobalObject* = nullptr, std::optional<uint64_t> jobOwner = std::nullopt, JSValue jobContext = JSValue());
    JS_EXPORT_PRIVATE static JSMicrotaskDispatcher* create(VM&, Ref<MicrotaskDispatcher>&&, JSGlobalObject* = nullptr, std::optional<uint64_t> jobOwner = std::nullopt, JSValue jobContext = JSValue());
    static JSMicrotaskDispatcher* createJobOwnerCarrier(VM&, JSGlobalObject&, std::optional<uint64_t>, JSValue jobContext);

    MicrotaskDispatcher* dispatcher() const { return m_dispatcher.get(); }

    JSGlobalObject* globalObject() const LIFETIME_BOUND { return m_globalObject.get(); }
    std::optional<uint64_t> jobOwner() const { return m_jobOwner; }
    void setJobOwner(uint64_t owner) { m_jobOwner = owner; }
    JSValue jobContext() const { return m_jobContext.get(); }
    void setJobContext(VM& vm, JSValue context) { m_jobContext.set(vm, this, context); }

    MicrotaskDispatcher::Type cachedType() const { return m_type; }

    DECLARE_VISIT_CHILDREN;
    static void destroy(JSCell*);

private:
    JSMicrotaskDispatcher(VM&, Structure*, RefPtr<MicrotaskDispatcher>&&, JSGlobalObject*, std::optional<uint64_t>, JSValue);

    const RefPtr<MicrotaskDispatcher> m_dispatcher;
    WriteBarrier<JSGlobalObject> m_globalObject;
    std::optional<uint64_t> m_jobOwner;
    WriteBarrier<Unknown> m_jobContext;
    MicrotaskDispatcher::Type m_type;
};

} // namespace JSC
