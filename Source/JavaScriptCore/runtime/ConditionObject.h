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

#include "LockObject.h"

namespace JSC {

// One waiter on a JS Condition (SPEC-api 5.4). Sync waiters park on their
// own WTF::Condition under the queue lock; async waiters carry the ticket to
// re-enqueue on the lock when notified.
class CondWaiter final : public ThreadSafeRefCounted<CondWaiter> {
public:
    enum class Kind : uint8_t { Sync, Async };
    static Ref<CondWaiter> create(Kind kind) { return adoptRef(*new CondWaiter(kind)); }

    Kind kind;
    std::atomic<uint8_t> state { 0 }; // Waiting = 0 -> Notified = 1, flipped exactly once
    Condition condition; // sync
    RefPtr<AsyncTicket> ticket; // async
    RefPtr<NativeLockState> lock; // async: the lock to re-acquire on notify

private:
    explicit CondWaiter(Kind kind)
        : kind(kind)
    {
    }
};

class NativeConditionState final : public ThreadSafeRefCounted<NativeConditionState> {
public:
    static Ref<NativeConditionState> create() { return adoptRef(*new NativeConditionState()); }

    Lock queueLock; // rank 3
    Deque<Ref<CondWaiter>> waiters WTF_GUARDED_BY_LOCK(queueLock); // FIFO

private:
    NativeConditionState() = default;
};

class JSConditionObject final : public JSDestructibleObject {
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

    static JSConditionObject* create(VM&, Structure*);
    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    DECLARE_EXPORT_INFO;

    NativeConditionState& conditionState() { return m_state.get(); }

private:
    JSConditionObject(VM&, Structure*);

    Ref<NativeConditionState> m_state;
};

} // namespace JSC
