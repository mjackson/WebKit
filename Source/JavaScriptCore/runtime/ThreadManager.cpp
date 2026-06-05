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
#include <wtf/NeverDestroyed.h>
#include <wtf/ThreadSpecific.h>

namespace JSC {

// ---------------- AsyncTicket ----------------

AsyncTicket::AsyncTicket(VM& vm, Ref<DeferredWorkTimer::TicketData>&& ticket, JSObject* extraDependency)
    : m_vm(vm)
    , m_ticket(WTF::move(ticket))
    , m_extraDependency(extraDependency)
{
}

Ref<AsyncTicket> AsyncTicket::create(JSGlobalObject* globalObject, JSPromise* promise, Vector<JSCell*>&& dependencies)
{
    VM& vm = globalObject->vm();
    JSObject* extraDependency = nullptr;
    if (!dependencies.isEmpty())
        extraDependency = dynamicDowncast<JSObject>(dependencies[0]);
    DeferredWorkTimer::Ticket ticket = vm.deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::AtSomePoint, vm, promise, WTF::move(dependencies));
    return adoptRef(*new AsyncTicket(vm, Ref { *ticket }, extraDependency));
}

void AsyncTicket::settle(DeferredWorkTimer::Task&& task)
{
    bool expected = false;
    if (!m_settled.compare_exchange_strong(expected, true))
        return;
    if (m_ticket->isCancelled())
        return;
    m_vm.deferredWorkTimer->scheduleWorkSoon(m_ticket.ptr(), WTF::move(task));
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
        // Lazy main/embedder ThreadState: tid 0, not spawned.
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

ThreadManager::Affinity ThreadManager::objectAffinity(JSObject* object)
{
    if (!anyRestrictedObjects())
        return Affinity::None;
    RefPtr<ThreadState> owner = restrictionOwner(object);
    if (!owner)
        return Affinity::None;
    return owner->nativeThread.get() == &Thread::currentSingleton() ? Affinity::Owner : Affinity::Foreign;
}

RefPtr<ThreadState> ThreadManager::restrictionOwner(JSObject* object)
{
    Locker locker { m_affinityLock };
    auto it = m_affinityTable.find(object);
    if (it == m_affinityTable.end())
        return nullptr;
    return RefPtr<ThreadState> { it->value.copyRef() };
}

void ThreadManager::restrictObject(JSObject* object, ThreadState& owner)
{
    Locker locker { m_affinityLock };
    auto result = m_affinityTable.add(object, Ref { owner });
    if (result.isNewEntry)
        m_restrictedCount.fetch_add(1, std::memory_order_relaxed);
}

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
