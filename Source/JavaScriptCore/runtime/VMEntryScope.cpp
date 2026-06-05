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

    m_vm.entryScope = nullptr;

    if (m_vm.hasAnyEntryScopeServiceRequest()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnExit();
}

} // namespace JSC
