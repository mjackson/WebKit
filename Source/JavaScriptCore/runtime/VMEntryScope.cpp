/*
 * Copyright (C) 2013-2023 Apple Inc. All rights reserved.
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

#include "config.h"
#include "VMEntryScope.h"

#include "ConcurrentButterflyOperations.h"
#include "Heap.h"
#include "JSThreadsSafepoint.h"
#include "Options.h"
#include "SamplingProfiler.h"
#include "VM.h"
#include "VMLite.h"
#include "VMEntryScopeInlines.h"
#include "WasmCapabilities.h"
#include "WasmMachineThreads.h"
#include "Watchdog.h"
#include <atomic>

namespace JSC {

// Pre-M4 structural entered-VM tripwire (docs/threads/INTEGRATE-jit.md M7,
// review rounds 3+4, R3-4/R4-12). Counts VMs with a live top-level entry
// scope, permitting N clients of ONE shared GC server (the R3-11-legal
// config) and crashing every other concurrent-entry shape deterministically
// on the ENTERING thread. The shared-server slot is STICKY (mirrors the
// heap's sticky ISS bit): it is never cleared, so the exit path needs no
// counter/slot coordination and there is no clear-vs-install race; the
// pointer is compared for identity only, never dereferenced. One shared
// server per process is the supported pre-M4 envelope.
// DELETE at integration manifest M4: real parking makes entry during a stop
// park in notifyVMStop instead of crash, and the GIL-removal change replaces
// the premise wholesale.
static std::atomic<unsigned> s_jsThreadsEnteredLegacyVMs { 0 };
static std::atomic<unsigned> s_jsThreadsEnteredSharedClients { 0 };
static std::atomic<JSC::Heap*> s_jsThreadsEnteredSharedServer { nullptr };

void VMEntryScope::setUpSlow()
{
    if (Options::useJSThreads()) [[unlikely]] {
        // M7/R3-4: the phase-1 stub runs stop-the-world closures inline on
        // the premise that the requesting caller is the only RUNNING mutator.
        // Enforce at entry: no entering while a stub stop window is open, and
        // no second concurrently-entered VM — except clients of ONE shared GC
        // server (R3-11/R4-12; their cross-client stops are conducted by the
        // server and the phase-1 GIL serializes their execution). Two
        // counters so concurrent same-server client entries (legal) cannot
        // false-positive a single-counter "!previous" check, while each side
        // still trips on the other (mixed legacy+shared is unsupported).
        RELEASE_ASSERT(!JSThreadsSafepoint::worldIsStopped());
        JSC::Heap& server = m_vm.clientHeap.server();
        if (server.isSharedServer()) {
            s_jsThreadsEnteredSharedClients.fetch_add(1, std::memory_order_acq_rel);
            JSC::Heap* expected = nullptr;
            if (!s_jsThreadsEnteredSharedServer.compare_exchange_strong(expected, &server, std::memory_order_acq_rel)) {
                // One shared server per process pre-M4 (sticky slot,
                // identity-compared only).
                RELEASE_ASSERT(expected == &server);
            }
            RELEASE_ASSERT(!s_jsThreadsEnteredLegacyVMs.load(std::memory_order_acquire));
        } else {
            unsigned previouslyEntered = s_jsThreadsEnteredLegacyVMs.fetch_add(1, std::memory_order_acq_rel);
            RELEASE_ASSERT(!previouslyEntered); // sole legacy entry, as in round 3
            RELEASE_ASSERT(!s_jsThreadsEnteredSharedClients.load(std::memory_order_acquire));
        }
    }

    // UNGIL §A.1.5 (U-T1): the per-entry record lives on the CURRENT lite
    // when gilOff. The VM member is ALSO written as a transitional shadow:
    // raw `vm.entryScope` consumers (VMEntryScopeInlines.h's top-level-entry
    // fast path, CallFrame.cpp, VMTraps.cpp, SamplingProfiler.cpp, ...) are
    // enumerated in INTEGRATE-ungil.md and re-pointed by the activation
    // tasks — until then the shadow keeps every GIL-on-shaped reader exact,
    // and gilOff is unreachable (dark).
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        ASSERT(!lite.entryScope);
        lite.entryScope = this;
        // N-MUTATOR TRIPWIRE (U-T1): the transitional VM-member shadow below
        // is single-writer ONLY while at most one thread is entered GIL-off.
        // Dropping the shadow and re-keying every raw vm.entryScope consumer
        // (VMEntryScopeInlines.h top-level-entry detection FIRST) on the
        // per-lite record is a HARD precondition of N-mutator entry (IU
        // obligation 1). Until then, a second concurrent top-level entry
        // must fail-stop here rather than silently last-writer-win the
        // shadow — or, worse, skip setUpSlow's per-thread services on the
        // loser of a racy fast-path check.
        RELEASE_ASSERT(!m_vm.entryScope);
    }
    m_vm.entryScope = this;

#if ASSERT_ENABLED
    // SPEC-vmstate I14: an installed VMLite always belongs to the VM whose
    // JSLock this thread holds.
    if (Options::useVMLite()) {
        if (VMLite* lite = VMLite::currentIfExists())
            ASSERT(lite->vm == &m_vm);
    }
    // SPEC-jit I19: the per-thread butterfly TID tag must be coherent before
    // any JS runs on this thread (CS3; zero-init is correct only for the
    // main thread).
    if (Options::useJSThreads()) [[unlikely]]
        assertButterflyTIDTagCoherent();
#endif

    auto& thread = Thread::currentSingleton();
    if (!thread.isJSThread()) [[unlikely]] {
        Thread::registerJSThread(thread);

        if (Wasm::isSupported())
            Wasm::startTrackingCurrentThread();
#if HAVE(MACH_EXCEPTIONS)
        registerThreadForMachExceptionHandling(thread);
#endif
    }

    if (m_vm.hasAnyEntryScopeServiceRequest() || m_vm.hasTimeZoneChange()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnEntry();
}

void VMEntryScope::tearDownSlow()
{
    if (Options::useJSThreads()) [[unlikely]] {
        JSC::Heap& server = m_vm.clientHeap.server();
        if (server.isSharedServer())
            s_jsThreadsEnteredSharedClients.fetch_sub(1, std::memory_order_acq_rel);
        else
            s_jsThreadsEnteredLegacyVMs.fetch_sub(1, std::memory_order_acq_rel);
    }

    ASSERT_WITH_MESSAGE(!m_vm.hasCheckpointOSRSideState(), "Exitting the VM but pending checkpoint side state still available");

    // UNGIL §A.1.5: clear the per-lite record FIRST (the CURRENT lite is the
    // one this scope was recorded on — dtor runs on the ctor's thread), then
    // the transitional VM shadow (see setUpSlow).
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        ASSERT(lite.entryScope == this);
        lite.entryScope = nullptr;
        // N-mutator tripwire — see setUpSlow: GIL-off the shadow must still
        // be ours when we null it (single entered thread until the shadow is
        // dropped by the activation tasks, IU obligation 1).
        RELEASE_ASSERT(m_vm.entryScope == this);
    }
    m_vm.entryScope = nullptr;

    // §A.1.5: executeEntryScopeServicesOnExit uses the CURRENT lite's bits
    // when gilOff (VM::has/clearEntryScopeService route there).
    if (m_vm.hasAnyEntryScopeServiceRequest()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnExit();
}

} // namespace JSC
