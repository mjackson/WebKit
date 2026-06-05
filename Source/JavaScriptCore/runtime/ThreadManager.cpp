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

#include "config.h"
#include "ThreadManager.h"

#include "JSCInlines.h"
#include "JSPromise.h"
#include "ThreadObject.h"
#include "WeakHandleOwner.h"
#include "WeakInlines.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSpecific.h>

namespace JSC {

// ---------------- AsyncTicket ----------------

AsyncTicket::AsyncTicket(VM& vm, Ref<DeferredWorkTimer::TicketData>&& ticket, Ref<ThreadState>&& registrant, JSObject* extraDependency)
    : m_vm(vm)
    , m_ticket(WTF::move(ticket))
    , m_registrant(WTF::move(registrant))
    , m_extraDependency(extraDependency)
{
}

// Out of line so the Strong<JSPromise> / Ref<ThreadState> members destruct
// where both types are complete. A never-settled ticket's Strong dies here
// (SPEC-api 5.5: not an error; DWT VM-shutdown cancelPendingWork is the
// liveness backstop). Destroying a still-set Strong mutates the VM's
// HandleSet, which requires the API lock: settle tasks clear the Strong
// under the JSLock, and every other last-ref drop site (DWT shutdown, the
// 5.6 timeout timer task) runs under a JSLockHolder. The assert documents
// that discipline for any future holder of the last Ref.
AsyncTicket::~AsyncTicket()
{
    ASSERT(!m_promise || m_vm.currentThreadIsHoldingAPILock());
}

Ref<AsyncTicket> AsyncTicket::create(JSGlobalObject* globalObject, JSPromise* promise, Vector<JSCell*>&& dependencies)
{
    VM& vm = globalObject->vm();
    JSObject* extraDependency = nullptr;
    if (!dependencies.isEmpty())
        extraDependency = dynamicDowncast<JSObject>(dependencies[0]);
    // Registration under the JSLock (SPEC-api 5.5): addPendingWork is the
    // shell-liveness point (I20/4.6.3) — a pending asyncJoin/asyncHold/
    // asyncWait/waitAsync keeps the shell alive from HERE, not from settle
    // time. The registrant is the calling thread's ThreadState (4.6.2).
    DeferredWorkTimer::Ticket ticket = vm.deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::AtSomePoint, vm, promise, WTF::move(dependencies));
    Ref<AsyncTicket> result = adoptRef(*new AsyncTicket(vm, Ref { *ticket }, ensureCurrentThreadState(), extraDependency));
    result->m_promise.set(vm, promise);
    return result;
}

void AsyncTicket::settle(DeferredWorkTimer::Task&& task)
{
    bool expected = false;
    if (!m_settled.compare_exchange_strong(expected, true))
        return;
    if (m_ticket->isCancelled())
        return;
    // The wrapper keeps the ticket alive through the settle task and clears
    // the promise Strong afterwards, under the JSLock (SPEC-api 5.10: the
    // AT::Strong<JSPromise> is created at registration, cleared at settle).
    m_vm.deferredWorkTimer->scheduleWorkSoon(m_ticket.ptr(), [protectedThis = Ref { *this }, task = WTF::move(task)](DeferredWorkTimer::Ticket dwtTicket) mutable {
        task(dwtTicket);
        protectedThis->m_promise.clear();
    });
}

// ---------------- current ThreadState ----------------

static ThreadSpecific<RefPtr<ThreadState>>& threadStateSlot()
{
    static LazyNeverDestroyed<ThreadSpecific<RefPtr<ThreadState>>> slot;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        slot.construct();
    });
    return slot;
}

ThreadState* currentThreadStateIfExists()
{
    return threadStateSlot()->get();
}

Ref<ThreadState> ensureCurrentThreadState()
{
    RefPtr<ThreadState>& slot = *threadStateSlot();
    if (!slot) {
        // Lazy main/embedder ThreadState (SPEC-api 5.1): tid 0, not spawned,
        // created on first access (Thread.current, ThreadLocal storage, lock
        // holder identity, ...) and stable thereafter for this thread.
        // Distinct embedder threads get distinct ThreadStates — identity is
        // this TLS slot / nativeThread, never the tid (all lazy TSs share
        // tid 0, which is why lookups NEVER go through m_threads). The lazy
        // TS is registered nowhere and carries no Strongs yet; whoever
        // creates its FIRST Strong must call ensureJSThreadForState()
        // (ThreadObject.h) first so the 5.10 finalizer hook exists — that
        // hook (not a completion sequence, which lazy TSs never run) is what
        // clears jsThread/threadLocals/result at VM teardown.
        Ref<ThreadState> state = ThreadState::create(ThreadManager::mainThreadTID, false);
        state->nativeThread = &Thread::currentSingleton();
        slot = state.ptr();
    }
    return *slot;
}

void setCurrentThreadState(RefPtr<ThreadState>&& state)
{
    *threadStateSlot() = WTF::move(state);
}

// ---------------- ThreadManager ----------------

ThreadManager& ThreadManager::singleton()
{
    static LazyNeverDestroyed<ThreadManager> manager;
    static std::once_flag onceKey;
    std::call_once(onceKey, [&] {
        manager.construct();
    });
    return manager;
}

uint16_t ThreadManager::currentTID()
{
    if (ThreadState* state = currentThreadStateIfExists())
        return state->tid;
    return mainThreadTID;
}

bool ThreadManager::isJSThreadCurrent()
{
    ThreadState* state = currentThreadStateIfExists();
    return state && state->isSpawned;
}

RefPtr<ThreadState> ThreadManager::allocateSpawnedThreadState()
{
    Locker locker { m_lock };
    if (m_threads.size() >= Options::maxJSThreads())
        return nullptr;
    uint16_t tid;
    if (!m_freeTIDs.isEmpty())
        tid = m_freeTIDs.takeFirst();
    else {
        if (m_nextTID >= notTTLTID)
            return nullptr; // lifetime TID exhaustion (Dev 10: no reuse pre-rebias)
        tid = m_nextTID++;
    }
    Ref<ThreadState> state = ThreadState::create(tid, true);
    m_threads.add(tid, state.copyRef());
    return RefPtr<ThreadState> { WTF::move(state) };
}

void ThreadManager::unregisterThread(ThreadState& state)
{
    Locker locker { m_lock };
    m_threads.remove(state.tid);
    // TIDs are retired forever in phase 1 (SPEC-api Deviation 10).
}

// Per-insert Weak<JSObject> finalizer (SPEC-api 5.7.2): prunes the affinity
// table when a restricted cell dies. finalize() runs on a thread holding the
// JSLock (GC finalization / lastChanceToFinalize), with no rank 1-3 lock
// held, so taking the rank-2 affinity lock here is 5.9-legal.
class ThreadAffinityWeakHandleOwner final : public WeakHandleOwner {
public:
    void finalize(Handle<Unknown> handle, void* context) final
    {
        // context is the ThreadAffinityEntry this Weak was created for (set
        // at Weak construction in restrictObject); pruneRestrictedObject uses
        // it as an identity check so a stale finalizer never removes a
        // successor entry installed at a recycled cell address.
        ThreadManager::singleton().pruneRestrictedObject(handle.get().asCell(), context);
    }
};

static WeakHandleOwner& threadAffinityWeakHandleOwner()
{
    static NeverDestroyed<ThreadAffinityWeakHandleOwner> owner;
    return owner.get();
}

ThreadManager::Affinity ThreadManager::objectAffinity(JSObject* object)
{
    if (!anyRestrictedObjects())
        return Affinity::None;
    RefPtr<ThreadState> owner = restrictionOwner(object);
    if (!owner)
        return Affinity::None;
    // Owner identity is the restricting thread's ThreadState/nativeThread,
    // never its TID (5.7.2; lazy main/embedder TSs all share tid 0). The
    // nativeThread pointer is compared, never dereferenced.
    return owner->nativeThread.get() == &Thread::currentSingleton() ? Affinity::Owner : Affinity::Foreign;
}

// Deleted-slot-reuse hazard, applied to the affinity table: the table is
// keyed by raw JSCell*, and a dead restricted cell's entry is pruned by its
// Weak's finalizer — which runs when the containing WeakBlock is swept and
// can therefore LAG the MarkedBlock sweep that recycles the dead cell's
// memory. Until the finalizer runs, a NEW object allocated at the recycled
// address aliases the stale key. Every reader/writer below therefore treats
// an entry whose Weak is not Live-and-equal-to-the-probed-object as ABSENT
// (Weak<T>::get() returns null for any non-Live impl, so a dead-but-not-yet-
// finalized Weak can never report a match).

static std::unique_ptr<ThreadAffinityEntry> makeAffinityEntry(JSObject* object, ThreadState& owner)
{
    // The Weak is created under the GIL (Thread.restrict is a host call)
    // and carries the pruning finalizer; the table holds no Strong on the
    // restricted object (5.7.2: entries pruned by per-insert Weak<JSObject>
    // finalizers). The entry's own address is the Weak's context, giving the
    // finalizer an identity check (see pruneRestrictedObject).
    auto entry = makeUnique<ThreadAffinityEntry>(Ref { owner }, Weak<JSObject> { });
    entry->weak = Weak<JSObject>(object, &threadAffinityWeakHandleOwner(), entry.get());
    return entry;
}

RefPtr<ThreadState> ThreadManager::restrictionOwner(JSObject* object)
{
    Locker locker { m_affinityLock };
    auto it = m_affinityTable.find(object);
    if (it == m_affinityTable.end())
        return nullptr;
    // Stale-key check (header comment above): a recycled-address alias must
    // never make a never-restricted object appear restricted.
    if (it->value->weak.get() != object)
        return nullptr;
    return RefPtr<ThreadState> { it->value->owner.copyRef() };
}

void ThreadManager::restrictObject(JSObject* object, ThreadState& owner)
{
    Locker locker { m_affinityLock };
    auto result = m_affinityTable.ensure(object, [&] {
        return makeAffinityEntry(object, owner);
    });
    if (result.isNewEntry) {
        m_restrictedCount.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (result.iterator->value->weak.get() == object)
        return; // Live entry for THIS object: idempotent re-restrict (foreign re-restrict was rejected by the caller's affinity step (0)).
    // Stale entry: the previous occupant of this address died and its
    // finalizer has not run yet. Replace the entry so the NEW object's
    // restriction is recorded with the correct owner. The count is already
    // accounted (one entry before, one after). Destroying the old entry
    // deallocates its dead WeakImpl, so its finalizer will not run; if it
    // already started racing us, its identity check (context != entry)
    // makes it a no-op.
    result.iterator->value = makeAffinityEntry(object, owner);
}

void ThreadManager::pruneRestrictedObject(JSCell* cell, void* expectedEntry)
{
    Locker locker { m_affinityLock };
    auto it = m_affinityTable.find(cell);
    if (it == m_affinityTable.end())
        return;
    // Identity check: only the finalizer belonging to THIS entry may remove
    // it. A late finalizer for a dead predecessor at a recycled address must
    // not evict the live successor entry restrictObject installed.
    if (it->value.get() != expectedEntry)
        return;
    m_affinityTable.remove(it);
    m_restrictedCount.fetch_sub(1, std::memory_order_relaxed);
}

// NOTE for reviewers: in THIS tree the only caller is threadFuncRestrict's
// affinity step — that is by design, not an omission. The 14 generic-path
// hook sites (JSObject.h / JSObjectInlines.h / JSObject.cpp) are
// INTEGRATOR-applied after the obj-model merge, per the frozen SPEC-api I14
// ("INT gate via 9.2-6; //@ skipped until then") and the exact ready-to-apply
// diff in docs/threads/INTEGRATE-api.md 9.2-6. Those files belong to the
// objectmodel workstream's merge surface, so the api workstream must not
// land the hooks itself; JSTests/threads/api/thread-restrict.js stays
// //@ skip'ped until the integrator applies the hook and deletes the skip
// line (the 9.2-6 apply checklist).
bool threadRestrictCheck(JSGlobalObject* globalObject, JSObject* object)
{
    auto& manager = ThreadManager::singleton();
    if (!manager.anyRestrictedObjects()) [[likely]]
        return true;
    switch (manager.objectAffinity(object)) {
    case ThreadManager::Affinity::None:
    case ThreadManager::Affinity::Owner:
        return true;
    case ThreadManager::Affinity::Foreign:
        break;
    }
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwConcurrentAccessError(globalObject, scope, "Concurrent access to a thread-restricted object"_s);
    return false;
}

} // namespace JSC
