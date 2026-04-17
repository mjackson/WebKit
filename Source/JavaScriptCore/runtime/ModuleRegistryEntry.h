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

#include "AbstractModuleRecord.h"
#include "ErrorInstance.h"
#include "JSObject.h"
#include "JSSourceCode.h"
#include "ScriptFetchParameters.h"

namespace JSC {

class ModuleRegistryEntry final : public JSCell {
    friend class LLIntOffsetsExtractor;
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    enum class Status : uint8_t {
        New,
        Fetching,
        Fetched,
        FetchFailed,
        InstantiationFailed,
        EvaluationFailed,
    };

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleRegistryEntrySpace<mode>();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    static ModuleRegistryEntry* create(VM&, Structure*, Identifier key, ScriptFetchParameters::Type, JSValue scriptFetcher);
    static ModuleRegistryEntry* create(VM&, Identifier key, ScriptFetchParameters::Type, JSValue scriptFetcher);

    const Identifier& key() const;
    ScriptFetchParameters::Type moduleType() const;
    AbstractModuleRecord* record() const;
    JSPromise* ensureFetchPromise(JSGlobalObject*);
    JSPromise* ensureModulePromise(JSGlobalObject*);
    JSPromise* loadPromise() const;
    JSValue error(JSGlobalObject*) const;
    JSValue fetchError() const;
    JSValue instantiationError() const;
    JSValue evaluationError() const;
    Status status() const;

    void record(VM&, AbstractModuleRecord*);
    void loadPromise(VM&, JSPromise*);
    void fetchError(JSGlobalObject*, JSValue);
    void instantiationError(JSGlobalObject*, JSValue);
    void evaluationError(JSGlobalObject*, JSValue);
    void status(Status);

    void provideFetch(JSGlobalObject*, SourceCode&&);
    void provideFetch(JSGlobalObject*, JSSourceCode*);
    void fetchComplete(JSGlobalObject*, AbstractModuleRecord*);

private:
    ModuleRegistryEntry(VM&, Structure*, Identifier key, ScriptFetchParameters::Type, JSValue scriptFetcher);

    void finishCreation(VM&);

    const Identifier m_key;
    const ScriptFetchParameters::Type m_type;
    WriteBarrier<Unknown> m_scriptFetcher;
    WriteBarrier<AbstractModuleRecord> m_record;
    WriteBarrier<JSPromise> m_fetchPromise;
    WriteBarrier<JSPromise> m_modulePromise;
    WriteBarrier<JSPromise> m_loadPromise;
    WriteBarrier<Unknown> m_fetchError;
    WriteBarrier<Unknown> m_instantiationError;
    WriteBarrier<Unknown> m_evaluationError;
    Status m_status { Status::New };
};

} // namespace JSC
