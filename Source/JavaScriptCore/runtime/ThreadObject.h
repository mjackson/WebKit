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

#pragma once

#include "JSDestructibleObject.h"
#include "ThreadManager.h"

namespace JSC {

class JSThread final : public JSDestructibleObject {
public:
    using Base = JSDestructibleObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.destructibleObjectSpace();
    }

    static JSThread* create(VM&, Structure*, Ref<ThreadState>&&);
    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    ThreadState& threadState() { return m_state.get(); }
    JSValue result() const
    {
        JSValue value = m_result.get();
        return value ? value : jsUndefined();
    }
    void setResult(VM& vm, JSValue value)
    {
        m_result.set(vm, this, value ? value : jsUndefined());
    }

private:
    JSThread(VM&, Structure*, Ref<ThreadState>&&);

    Ref<ThreadState> m_state;
    WriteBarrier<Unknown> m_result;
};

// The five global properties installed by JSGlobalObject::init() under
// Options::useJSThreads() (SPEC-api 9.2-2).
JS_EXPORT_PRIVATE JSValue createThreadProperty(VM&, JSObject* globalObject);
JS_EXPORT_PRIVATE JSValue createLockProperty(VM&, JSObject* globalObject);
JS_EXPORT_PRIVATE JSValue createConditionProperty(VM&, JSObject* globalObject);
JS_EXPORT_PRIVATE JSValue createThreadLocalProperty(VM&, JSObject* globalObject);
JS_EXPORT_PRIVATE JSValue createConcurrentAccessErrorProperty(VM&, JSObject* globalObject);

// Throws a ConcurrentAccessError instance (Error subclass, name
// "ConcurrentAccessError").
JS_EXPORT_PRIVATE Exception* throwConcurrentAccessError(JSGlobalObject*, ThrowScope&, ASCIILiteral message);

// G11: may the current thread block (join / contended hold / cond.wait /
// property Atomics.wait)?
bool jsThreadsCanBlockOnCurrentThread(VM&);

} // namespace JSC
