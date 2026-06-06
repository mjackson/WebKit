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

// UNGIL §A.3.4/§J.8 (U-T5): the pre-M4 M7 structural entered-VM tripwire
// (the s_jsThreadsEntered* counters + sticky shared-server slot that used to
// live here) is DELETED, as licensed by SPEC-ungil §A.3.4. GIL-off, entry
// during a §A.3 stop PARKS instead of crashing: a thread completing entry
// acquires heap access through GCClient::Heap::acquireHeapAccess, whose
// §A.3.2b stop-word gate (SB1 seq_cst poll beside the F8 step-2 GSP load)
// mandatory-reverts and parks the entrant on its own NVS ticket until the
// conductor resumes — fresh acquisition is gated, so a §A.3 window never
// admits a new mutator (annex SB1/EXIT1; VMManager.cpp owns the window).
// The phase-1 single-entered-mutator premise the tripwire enforced is
// replaced wholesale by the thread-granular protocol.

void VMEntryScope::setUpSlow()
{
    // UNGIL §A.1.5 (re-frozen at U-T5): GIL-off, the per-entry record lives
    // ONLY on the CURRENT lite — the transitional VM-member shadow is
    // DROPPED (IU obligation 1). With N concurrently-entered threads a
    // VM-wide shadow would be a last-writer-wins data race, and a stale
    // shadow would make a sibling's exit invisible to VM-wide consumers.
    // VM-wide "entered" consumers go through VM::isEntered()/
    // isAnyThreadEntered() (registry walk) and per-thread consumers through
    // VM::currentThreadEntryScope() (both routed by U-T1).
    //
    // OPEN-BLOCKER (cross-file, recorded for the orchestrator):
    // VMEntryScopeInlines.h's ctor/dtor fast paths still gate setUpSlow/
    // tearDownSlow on the raw `vm.entryScope` shadow. That header is outside
    // U-T5's writable set; it must be re-keyed on the per-lite record when
    // m_vm.gilOff() (ctor: `!VMLite::current().entryScope`; dtor:
    // `VMLite::current().entryScope == this`) in the same wave. Until that
    // lands, a gilOff NESTED entry would reach setUpSlow again — the
    // RELEASE_ASSERT below fail-stops that shape loudly instead of letting
    // the per-lite record be silently overwritten (and then leaked, because
    // the un-rekeyed dtor gate would skip tearDownSlow).
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        RELEASE_ASSERT(!lite.entryScope);
        lite.entryScope = this;
    } else
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
    ASSERT_WITH_MESSAGE(!m_vm.hasCheckpointOSRSideState(), "Exitting the VM but pending checkpoint side state still available");

    // UNGIL §A.1.5 (U-T5): GIL-off, clear ONLY the per-lite record (the
    // CURRENT lite is the one this scope was recorded on — the dtor runs on
    // the ctor's thread). The VM-member shadow is dropped (see setUpSlow);
    // GIL-on keeps the landed single write.
    if (m_vm.gilOff()) [[unlikely]] {
        VMLite& lite = VMLite::current();
        RELEASE_ASSERT(lite.vm == &m_vm);
        RELEASE_ASSERT(lite.entryScope == this);
        lite.entryScope = nullptr;
    } else
        m_vm.entryScope = nullptr;

    // §A.1.5: executeEntryScopeServicesOnExit uses the CURRENT lite's bits
    // when gilOff (VM::has/clearEntryScopeService route there).
    if (m_vm.hasAnyEntryScopeServiceRequest()) [[unlikely]]
        m_vm.executeEntryScopeServicesOnExit();
}

} // namespace JSC
