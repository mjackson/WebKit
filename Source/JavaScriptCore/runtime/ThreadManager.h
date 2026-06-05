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
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class JSPromise;
class VM;

// Master gate for the phase-1 GIL'd shared-memory Thread API
// (docs/threads/SPEC-api.md). --useThreads is accepted as an alias for
// --useJSThreads. The GIL is the shared VM's JSLock; --useThreadGIL is
// reserved and inert in phase 1 (the GIL is always on).
ALWAYS_INLINE bool useJSThreadsEnabled()
{
    return Options::useJSThreads() || Options::useThreads();
}

// One async ticket type backing asyncJoin / asyncHold / asyncWait /
// property Atomics.waitAsync (SPEC-api 5.5). Wraps a DeferredWorkTimer
// ticket whose target is the promise; the ticket's pending-work
// registration keeps the shell alive until the promise settles.
class AsyncTicket final : public ThreadSafeRefCounted<AsyncTicket> {
public:
    // Must be called while holding the shared VM's JSLock.
    JS_EXPORT_PRIVATE static Ref<AsyncTicket> create(JSGlobalObject*, JSPromise*, Vector<JSCell*>&& dependencies = { });

    VM& vm() { return m_vm; }

    // For lock-release / asyncWait consumption arbitration (SPEC-api 5.5a).
    bool tryConsume()
    {
        bool expected = false;
        return m_consumed.compare_exchange_strong(expected, true);
    }

    // Schedules a settle task on the VM's run loop. Thread-safe; only the
    // first call has any effect. The task runs holding the JSLock with the
    // promise as the ticket target.
    JS_EXPORT_PRIVATE void settle(DeferredWorkTimer::Task&&);

    // asyncHold with-fn arity support: the function cell, rooted by the
    // underlying ticket's dependency vector.
    JSObject* extraDependency() const { return m_extraDependency; }
    bool grantWithFunction { false };

    std::atomic<uint8_t> state { 0 }; // Waiting = 0, Notified = 1, TimedOut = 2.

private:
    AsyncTicket(VM&, Ref<DeferredWorkTimer::TicketData>&&, JSObject* extraDependency);

    VM& m_vm;
    Ref<DeferredWorkTimer::TicketData> m_ticket;
    JSObject* m_extraDependency { nullptr };
    std::atomic<bool> m_settled { false };
    std::atomic<bool> m_consumed { false };
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

    uint16_t tid { 0 };
    bool isSpawned { false };

    // Compared, never dereferenced for identity purposes.
    RefPtr<WTF::Thread> nativeThread;

    std::atomic<Phase> phase { Phase::Running };

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

private:
    ThreadState(uint16_t tid, bool isSpawned)
        : tid(tid)
        , isSpawned(isSpawned)
    {
    }
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
    bool anyRestrictedObjects() const { return m_restrictedCount.load(std::memory_order_relaxed); }
    RefPtr<ThreadState> restrictionOwner(JSObject*);

    template<typename Functor>
    void forEachThreadState(const Functor& functor)
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

    Lock m_affinityLock; // rank 2
    UncheckedKeyHashMap<JSCell*, Ref<ThreadState>> m_affinityTable WTF_GUARDED_BY_LOCK(m_affinityLock);
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
