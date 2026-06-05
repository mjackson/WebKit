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

#include "DeferredWorkTimer.h"
#include "Options.h"
#include "Strong.h"
#include "Weak.h"
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSObject;
class JSPromise;
class VM;

// Master gate for the phase-1 GIL'd shared-memory Thread API
// (docs/threads/SPEC-api.md). The GIL is the shared VM's JSLock and is
// always on in phase 1.
//
// SPEC-api 9.2-1 paired edit (a), landed: this gate reads ONLY the canonical
// Options::useJSThreads(), the same predicate every other workstream
// (objectmodel JSObject.h hooks, jit TID-tag paths, vmstate flag
// implications and its planned GIL-off backstop assert) gates on. The
// prep-stub --useThreads alias must NOT be honored here: it would let a
// --useThreads=1 run spawn real OS threads into the shared heap with all
// flag-gated concurrent-mode machinery (keyed on useJSThreads) switched
// off. The alias OptionsList.h entry itself is now a dead option with zero
// code consumers; its one-line deletion is the only remaining 9.2-1 INT
// action (OptionsList.h is a shared hot file, not api-editable).
ALWAYS_INLINE bool useJSThreadsEnabled()
{
    return Options::useJSThreads();
}

class ThreadState;

// One async ticket type backing asyncJoin / asyncHold / asyncWait /
// property Atomics.waitAsync (SPEC-api 5.5). Wraps a DeferredWorkTimer
// ticket whose target is the promise; the ticket's pending-work
// registration — performed at creation, under the JSLock — is what keeps
// the shell alive until the promise settles (I20/4.6.3), NOT settle time.
class AsyncTicket final : public ThreadSafeRefCounted<AsyncTicket> {
public:
    // Must be called while holding the shared VM's JSLock. Captures the
    // calling thread's ThreadState as the registrant.
    JS_EXPORT_PRIVATE static Ref<AsyncTicket> create(JSGlobalObject*, JSPromise*, Vector<JSCell*>&& dependencies = { });
    JS_EXPORT_PRIVATE ~AsyncTicket();

    VM& vm() { return m_vm; }

    // The registering thread's ThreadState (SPEC-api 5.5 / 4.6.2). Tickets
    // are process-owned and outlive their registering thread: this Ref keeps
    // the registrant ThreadState alive past its completion sequence, so a
    // finished thread's pending continuation (e.g. its own asyncHold) still
    // settles (I20). In the GIL phase the settling thread is whichever one
    // drains the single shared VM queue (I12 relaxation); post-GIL this is
    // the routing key for the per-ThreadState ticket inbox (5.5).
    ThreadState& registrant() const { return m_registrant.get(); }

    // The promise this ticket settles (SPEC-api 5.5 / 5.10): the Strong is
    // created at registration (host call, under the JSLock) and cleared by
    // the settle task (which runs under the JSLock); a never-settled
    // ticket's pending work is cancelled at DWT VM shutdown (5.5), and its
    // Strong dies with the ticket. Read it only from a settle task or while
    // holding the JSLock.
    Strong<JSPromise>& promise() { return m_promise; }

    // True once DWT VM-shutdown cancelPendingWork ran for the underlying
    // ticket (settle() is then a no-op). Timer tasks check this to bail out
    // before touching VM state after shutdown.
    bool isCancelled() const { return m_ticket->isCancelled(); }

    // For lock-release / asyncWait consumption arbitration (SPEC-api 5.5a).
    bool tryConsume()
    {
        bool expected = false;
        return m_consumed.compare_exchange_strong(expected, true);
    }

    // Lock-grant delivery gate (SPEC-api 4.3(b), tightened — see the
    // "Landed deviations" D6 entry in docs/threads/INTEGRATE-api.md).
    // NativeLockState installs m_asyncHolder (pump / immediate-grant path)
    // BEFORE the grant's settle task runs; until that task delivers the
    // grant to user code (runs the held fn, or resolves the promise with a
    // release fn), the hold is not observable by JS and MUST NOT be
    // consumable by cond.asyncWait's (b) arm — consuming it would unlock
    // m_lock while the with-fn settle task later runs fn believing it holds
    // the lock (mutual-exclusion hole, I6). markGrantDelivered() is called
    // by settleLockGrant's settle tasks (under the JSLock); readers
    // (cond.asyncWait, also under the JSLock in phase 1) load-acquire.
    void markGrantDelivered() { m_grantDelivered.store(true, std::memory_order_release); }
    bool grantDelivered() const { return m_grantDelivered.load(std::memory_order_acquire); }

    // Schedules a settle task on the VM's run loop via
    // DeferredWorkTimer::scheduleWorkSoon (SPEC-api 5.5): never settles
    // synchronously inside the registering call (I12). Thread-safe; only the
    // first call has any effect. The task runs holding the JSLock with the
    // promise as the ticket target; after it returns, the ticket's promise
    // Strong is cleared (5.10).
    JS_EXPORT_PRIVATE void settle(DeferredWorkTimer::Task&&);

    // asyncHold with-fn arity support: the function cell, rooted by the
    // underlying ticket's dependency vector.
    JSObject* extraDependency() const { return m_extraDependency; }
    bool grantWithFunction { false };

    std::atomic<uint8_t> state { 0 }; // Waiting = 0, Notified = 1, TimedOut = 2.

private:
    AsyncTicket(VM&, Ref<DeferredWorkTimer::TicketData>&&, Ref<ThreadState>&& registrant, JSObject* extraDependency);

    VM& m_vm;
    Ref<DeferredWorkTimer::TicketData> m_ticket;
    Ref<ThreadState> m_registrant;
    Strong<JSPromise> m_promise;
    JSObject* m_extraDependency { nullptr };
    std::atomic<bool> m_settled { false };
    std::atomic<bool> m_consumed { false };
    std::atomic<bool> m_grantDelivered { false };
};

// Native state of one JS thread (SPEC-api 5.1). Main/embedder threads get a
// lazily created ThreadState (tid 0, isSpawned = false) on first use.
class ThreadState final : public ThreadSafeRefCounted<ThreadState> {
public:
    enum class Phase : uint8_t { Running, Finished, Failed };

    static Ref<ThreadState> create(uint16_t tid, bool isSpawned)
    {
        return adoptRef(*new ThreadState(tid, isSpawned));
    }

    ~ThreadState()
    {
        // SPEC-api 5.10: every Strong must have been cleared (under the
        // JSLock) before the last reference drops.
        RELEASE_ASSERT(!result);
        RELEASE_ASSERT(!fnSlot);
        RELEASE_ASSERT(argSlots.isEmpty());
        RELEASE_ASSERT(threadLocals.isEmpty());
        RELEASE_ASSERT(!jsThread);
        // asyncJoiners must have been drained too: by the completion
        // sequence (spawned threads) or by the 5.10 finalizer hook
        // (never-completing lazy/embedder ThreadStates, VM teardown) —
        // otherwise this destructor, which can run off the JSLock (TLS slot
        // teardown of an exiting embedder thread, possibly after VM death),
        // would drop the last refs to unsettled AsyncTickets whose
        // Strong<JSPromise> is still set (5.10 violation / VM UAF).
        {
            Locker joinLocker { joinLock };
            RELEASE_ASSERT(asyncJoiners.isEmpty());
        }
    }

    uint16_t tid { 0 };
    bool isSpawned { false };

    // Compared, never dereferenced for identity purposes.
    RefPtr<WTF::Thread> nativeThread;

    std::atomic<Phase> phase { Phase::Running };

    // The thread's result (fn's return value, or the thrown exception value
    // when phase == Failed). F1: written under the JSLock in the completion
    // sequence, release-fenced by the subsequent Phase store; read under the
    // JSLock by join() and the asyncJoin settle tasks. Cleared ONLY by the
    // 5.10 finalizer hook registered at TS::jsThread creation.
    Strong<Unknown> result;

    Lock joinLock;
    Condition joinCondition;
    Vector<Ref<AsyncTicket>> asyncJoiners WTF_GUARDED_BY_LOCK(joinLock);

    // Owner-thread-only ThreadLocal storage (SPEC-api 5.8).
    UncheckedKeyHashMap<uint64_t, Strong<Unknown>> threadLocals;

    // Roots the JSThread cell while the thread is alive (Thread.current
    // identity); cleared (under the JSLock) in the completion sequence.
    Strong<Unknown> jsThread;

    // Root fn/args between spawn and invocation (cleared right after the
    // call, under the JSLock, on the spawned thread).
    Strong<Unknown> fnSlot;
    Vector<Strong<Unknown>> argSlots;

    // Post-GIL async-ticket inbox (SPEC-api 5.1/5.5). Phase 1: inert — the
    // GIL relaxation (I12) settles tickets on whichever thread drains the
    // single shared VM queue, so nothing reads or writes these yet.
    Lock inboxLock; // rank 3
    Vector<Ref<AsyncTicket>> inbox WTF_GUARDED_BY_LOCK(inboxLock);
    bool inboxOpen WTF_GUARDED_BY_LOCK(inboxLock) { false };

private:
    ThreadState(uint16_t tid, bool isSpawned)
        : tid(tid)
        , isSpawned(isSpawned)
    {
    }
};

// One Thread.restrict affinity-table entry (SPEC-api 5.7.2). The owner is the
// restricting thread's Ref<ThreadState> (NEVER its TID: lazy main/embedder
// ThreadStates all share tid 0); ownership checks compare
// owner->nativeThread.get() against &WTF::Thread::currentSingleton(). The
// per-insert Weak<JSObject> carries a WeakHandleOwner finalizer that prunes
// this entry (and decrements the restricted-object count) when the restricted
// cell dies, so the table never roots restricted objects and never grows
// unboundedly.
struct ThreadAffinityEntry {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ThreadAffinityEntry);

    ThreadAffinityEntry(Ref<ThreadState>&& owner, Weak<JSObject>&& weak)
        : owner(WTF::move(owner))
        , weak(WTF::move(weak))
    {
    }

    Ref<ThreadState> owner;
    Weak<JSObject> weak;
};

class ThreadManager final {
    WTF_MAKE_NONCOPYABLE(ThreadManager);
public:
    JS_EXPORT_PRIVATE static ThreadManager& singleton();

    static constexpr uint16_t mainThreadTID = 0;
    static constexpr uint16_t notTTLTID = 0x7fff; // reserved

    static uint16_t currentTID();
    JS_EXPORT_PRIVATE static bool isJSThreadCurrent(); // true iff spawned Thread

    // Allocates a TID and registers a new spawned ThreadState. Returns null
    // on TID exhaustion or when the live-thread cap is exceeded (the caller
    // throws RangeError).
    RefPtr<ThreadState> allocateSpawnedThreadState();
    void unregisterThread(ThreadState&);

    uint64_t allocateThreadLocalKey() { return ++m_nextThreadLocalKey; }

    // Thread.restrict affinity table (SPEC-api 5.7.2).
    enum class Affinity : uint8_t { None, Owner, Foreign };
    Affinity objectAffinity(JSObject*);
    void restrictObject(JSObject*, ThreadState& owner);
    // Mandatory 5.7.2 fast path: one relaxed load, zero => no restricted
    // objects anywhere => no lock, no table probe.
    bool anyRestrictedObjects() const { return m_restrictedCount.load(std::memory_order_relaxed); }
    RefPtr<ThreadState> restrictionOwner(JSObject*);
    // Called by the per-insert Weak finalizer (5.7.2) when a restricted cell
    // dies; removes the entry and decrements the restricted-object count.
    // expectedEntry is the finalizing Weak's context (the ThreadAffinityEntry
    // it was created for): the entry is removed only if it is still THAT
    // entry, so a finalizer that runs late — after the dead cell's address
    // was recycled and the new object at that address was itself restricted
    // (which replaces the table entry) — cannot evict the new object's
    // restriction (deleted-slot-reuse hazard, cf. THREAD.md regime 3).
    void pruneRestrictedObject(JSCell*, void* expectedEntry);

    // Frozen signature (SPEC-api 7); diag/future N-mutator. NOT a GC root
    // source.
    void forEachThreadState(const Invocable<void(ThreadState&)> auto& functor)
    {
        Locker locker { m_lock };
        for (auto& entry : m_threads)
            functor(entry.value.get());
    }

public:
    ThreadManager() = default; // for LazyNeverDestroyed only; use singleton()

private:
    Lock m_lock; // rank 1 (SPEC-api 5.9)
    UncheckedKeyHashMap<uint16_t, Ref<ThreadState>> m_threads WTF_GUARDED_BY_LOCK(m_lock); // spawned only
    Deque<uint16_t> m_freeTIDs WTF_GUARDED_BY_LOCK(m_lock); // empty until Dev-10 rebias lands
    uint16_t m_nextTID WTF_GUARDED_BY_LOCK(m_lock) { 1 }; // 0 = main, 0x7fff = notTTLTID

    std::atomic<uint64_t> m_nextThreadLocalKey { 0 };

    Lock m_affinityLock; // rank 2 (never held together with the PWT lock, 5.9)
    UncheckedKeyHashMap<JSCell*, std::unique_ptr<ThreadAffinityEntry>> m_affinityTable WTF_GUARDED_BY_LOCK(m_affinityLock);
    std::atomic<size_t> m_restrictedCount { 0 };
};

// The sole current-thread ThreadState lookup (SPEC-api 5.1). Never goes
// through ThreadManager::m_threads (tid-0 collision with embedder threads).
ThreadState* currentThreadStateIfExists();
JS_EXPORT_PRIVATE Ref<ThreadState> ensureCurrentThreadState();
void setCurrentThreadState(RefPtr<ThreadState>&&);

// 5.7 choke point; returns true if the access is allowed, otherwise throws
// ConcurrentAccessError and returns false. Callers gate on
// isUncacheableDictionary() first.
JS_EXPORT_PRIVATE bool threadRestrictCheck(JSGlobalObject*, JSObject*);

} // namespace JSC
