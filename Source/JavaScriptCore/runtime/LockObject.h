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

class CallFrame;
class EntryFrame;
class ExceptionScope;

// Native state backing a JS Lock (SPEC-api 5.3). Non-recursive; sync holds
// are identified by WTF::Thread*, async holds by an AsyncTicket.
class NativeLockState final : public ThreadSafeRefCounted<NativeLockState> {
public:
    static Ref<NativeLockState> create() { return adoptRef(*new NativeLockState()); }

    WTF::Lock m_lock; // rank 4 leaf
    std::atomic<WTF::Thread*> m_holder { nullptr }; // sync holder; compared, never dereferenced
    std::atomic<bool> m_asyncHeld { false };

    Lock m_queueLock; // rank 3 (protects the next three)
    RefPtr<AsyncTicket> m_asyncHolder WTF_GUARDED_BY_LOCK(m_queueLock);
    Deque<Ref<AsyncTicket>> m_asyncWaiters WTF_GUARDED_BY_LOCK(m_queueLock);
    bool m_pumpPending WTF_GUARDED_BY_LOCK(m_queueLock) { false };

    bool heldByCurrentThread() const { return m_holder.load(std::memory_order_relaxed) == &Thread::currentSingleton(); }

    // Release the sync hold the current thread owns (no pump).
    void releaseSyncHold()
    {
        m_holder.store(nullptr, std::memory_order_relaxed);
        m_lock.unlock();
    }

    // R: after any m_lock release, schedule the async-acquirer pump if needed.
    void releasePump(VM&);

    // A-failure path: enqueue an async acquirer FIFO and schedule the pump.
    void enqueueAsyncAcquirer(Ref<AsyncTicket>&&, VM&);

    // Async release: clear the live async hold and unlock (callable from any
    // thread). Caller must have consumed the ticket already.
    void asyncReleaseInternal(AsyncTicket&, VM&);

    // P: run-loop pump task (SPEC-api 5.5a).
    void pump(VM&);

private:
    NativeLockState() = default;

    void schedPumpLocked(VM&) WTF_REQUIRES_LOCK(m_queueLock);
};

class JSLockObject final : public JSDestructibleObject {
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

    static JSLockObject* create(VM&, Structure*);
    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
    {
        return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    }

    DECLARE_EXPORT_INFO;

    NativeLockState& lockState() { return m_state.get(); }

private:
    JSLockObject(VM&, Structure*);

    Ref<NativeLockState> m_state;
};

// Settles a granted async-acquisition ticket (resolve with a release
// function, or run the held function then implicitly release). Shared with
// ConditionObject.cpp for asyncWait re-acquisition.
void settleLockGrant(NativeLockState&, AsyncTicket&);

// Phase-1 GIL stub fairness primitive: release the GIL, yield, and
// reacquire it, WITHOUT JSLock::DropAllLocks. DropAllLocks participates in
// JSLock's strict-LIFO m_lockDropDepth protocol (JSLock::grabAllLocks spins
// until all deeper droppers have unwound). A thread that repeatedly
// drops/regrabs via droppers therefore only ever leaves the GIL free while
// the drop depth is ABOVE the depths recorded by already-parked waiters, so
// those waiters can never finish unwinding: livelock, observed live in
// JSTests/threads/sync/condition-notify-all-shared-lock.js. This helper
// bypasses the depth bookkeeping (legal here: the release/reacquire pair is
// fully contained in one host-function frame, so per-thread LIFO is trivially
// preserved) while mirroring grabAllLocks' stack-pointer restoration so
// conservative scan still covers the original VM entry frames.
void jsThreadGILHandoffYield(VM&);

// Saves vm.topCallFrame/vm.topEntryFrame at construction and restores them at
// destruction. Required around every place a JS thread parks with the GIL
// dropped (cond.wait, lock.hold blocking, join, Atomics.wait) once GIL
// handoffs are not strictly LIFO: those two fields are VM-global and are
// normally kept consistent by the prev-pointer chain in VMEntryRecords, which
// assumes threads leave the VM in reverse order of entry. A resuming thread
// must put ITS frames back or the next SlowPathFrameTracer asserts
// (callFrame < vm.topEntryFrame) against a foreign thread's stack. Mirrors
// the SPEC-vmstate §0 JSLock hand-off treatment of lastStackTop /
// stackPointerAtVMEntry, scoped to the phase-1 stub's park sites.
class GILParkSavedExecutionState {
    WTF_MAKE_NONCOPYABLE(GILParkSavedExecutionState);
public:
    JS_EXPORT_PRIVATE explicit GILParkSavedExecutionState(VM&);
    JS_EXPORT_PRIVATE ~GILParkSavedExecutionState();

    // A freshly spawned JS thread inherits whatever execution state the
    // previous GIL holder left in the VM (possibly pointers into a now-dead
    // thread's stack, e.g. the EXCEPTION_SCOPE_VERIFICATION scope chain).
    // Call right after the thread's first GIL acquisition to start from a
    // clean slate; every park site then restores this thread's own state.
    JS_EXPORT_PRIVATE static void resetForFreshThread(VM&);

private:
    VM& m_vm;
    CallFrame* m_topCallFrame;
    EntryFrame* m_topEntryFrame;
#if ENABLE(EXCEPTION_SCOPE_VERIFICATION)
    ExceptionScope* m_topExceptionScope;
    bool m_needExceptionCheck;
#endif
};

// Depth-free replacement for JSLock::DropAllLocks at the phase-1 stub's
// park sites (join, cond.wait, Atomics.wait, lock.hold blocking).
// DropAllLocks enrolls the dropper in JSLock's strict-LIFO m_lockDropDepth
// protocol: grabAllLocks spins until every deeper dropper has unwound. With
// N threads parking and waking in arbitrary (non-LIFO) order, a woken
// shallow dropper spins forever while the deeper dropper it implicitly
// waits on can only be woken by JS the spinner was about to run: livelock
// (observed in join chains, JSTests/threads/lifecycle/join-semantics.js,
// and in condition-notify-all-shared-lock.js). This scope releases the GIL
// completely at construction and reacquires it at destruction with no depth
// bookkeeping; per-thread LIFO is trivially preserved by C++ scoping, and
// the stack-entry bookkeeping (stackPointerAtVMEntry, lastStackTop,
// topCallFrame, topEntryFrame) is saved in locals and restored exactly as
// grabAllLocks would.
class GILDroppedSection {
    WTF_MAKE_NONCOPYABLE(GILDroppedSection);
public:
    JS_EXPORT_PRIVATE explicit GILDroppedSection(VM&);
    JS_EXPORT_PRIVATE ~GILDroppedSection();

private:
    VM& m_vm;
    GILParkSavedExecutionState m_savedExecutionState;
    void* m_stackPointerAtVMEntry;
    unsigned m_lockCount { 0 };
};

} // namespace JSC
