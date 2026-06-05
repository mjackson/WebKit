/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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
#include "VMLite.h"

#include "MicrotaskQueue.h" // Complete type for ~RefPtr<MicrotaskQueue> in ~VMLite + create() (§6.5).
#include "VM.h"             // ScratchBuffer/VMMalloc (§6.6); currentThreadIsHoldingAPILock (I14).
#include "VMLiteInlines.h"  // isInstalledOnCurrentThread (I11 asserts below).
#include "VMLiteShared.h"   // VMLiteRegistry (debug registration asserts, I20).
#include <atomic>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMLite);

// L4 (frozen, SPEC-vmstate §6.3): plain C++ thread_local, NOT
// pthread_getspecific. The accessor signatures in VMLite.h are frozen; this
// backing store is replaceable in Phase B (pinned register/TLS base).
static thread_local VMLite* t_currentVMLite { nullptr };

// §6.7 TID-tag hook (jit CS3/I19 provider). Null default — Phase-A standalone
// builds and flag-off runs never register one. Registration happens once at
// P5 init (jit task 1b); acquire/release keeps the registering thread's
// writes visible to hooks invoked on later-switching threads.
static std::atomic<void (*)(uint16_t)> s_vmLiteTIDTagHook { nullptr };

VMLite::VMLite() = default;

VMLite::~VMLite()
{
    // I20: no thread's TLS may ever point at a destroyed VMLite. We can only
    // check this thread's slot here; the registration assert below covers the
    // rest (an installed lite is always registered — setCurrent asserts it).
    ASSERT(t_currentVMLite != this);

    // §6.6: scratch buffers are VMMalloc'd raw blocks (mirrors ~VM,
    // VM.cpp:655-656). No lock: the lifetime contract (§6.5.1 — unregistered,
    // uninstalled) means no other thread can reach this carrier anymore.
    for (auto* scratchBuffer : scratchBuffers)
        VMMalloc::free(scratchBuffer);
    scratchBuffers.clear();
#if ASSERT_ENABLED
    {
        // Lifetime contract (§6.5.1): unregister BEFORE destroy. Leaf lock —
        // nothing else is acquired while it is held.
        auto& registry = VMLiteRegistry::singleton();
        Locker locker { registry.lock };
        ASSERT(!registry.lites.contains(this));
    }
    // Poison (I20 debug): a stale t_currentVMLite or VMLite* on another
    // thread that dereferences this carrier after destruction trips on
    // obviously-bad values instead of reading freed-but-plausible state.
    vm = reinterpret_cast<VM*>(static_cast<uintptr_t>(0xbbadbeef));
    tid = 0xffff; // Not a valid ButterflyTID payload (15-bit space).
    executingRegExp = reinterpret_cast<RegExp*>(static_cast<uintptr_t>(0xbbadbeef));
#endif
}

VMLite* VMLite::currentIfExists()
{
    return t_currentVMLite;
}

VMLite& VMLite::current()
{
    ASSERT(t_currentVMLite);
    return *t_currentVMLite;
}

VMLite* VMLite::setCurrent(VMLite* lite)
{
    if (lite) {
        // I18: an installed carrier's tid is never notTTLTID (0x7fff — the
        // all-ones 15-bit TID is the segmented-butterfly sentinel,
        // ConcurrentButterfly.h; not includable here: it includes us).
        ASSERT(lite->tid != 0x7fff);
#if ASSERT_ENABLED
        {
            // I20: only live, registered lites may be installed (§6.5.1:
            // registerLite precedes setCurrent — VM ctor registers the main
            // carrier before JSLock installs it; api §5.2 spawn registers
            // before the first JSLockHolder). Leaf lock.
            auto& registry = VMLiteRegistry::singleton();
            Locker locker { registry.lock };
            ASSERT(registry.lites.contains(lite));
        }
#endif
    }

    VMLite* previous = t_currentVMLite;
    t_currentVMLite = lite;

    // §6.7: invoke the TID-tag hook AFTER the TLS write, with the new tid (0
    // for uninstall) — §6.4.4 install/restore and multi-VM switches keep
    // g_jscButterflyTIDTag coherent (jit I19). Null hook => no-op.
    if (auto* hook = s_vmLiteTIDTagHook.load(std::memory_order_acquire))
        hook(lite ? lite->tid : 0);

    return previous;
}

// ---- §6.5 Group 6: per-thread default microtask queue (Phase A inert) -----

MicrotaskQueue& VMLite::ensureDefaultMicrotaskQueue()
{
    // I11: a per-thread facility is touched only by the thread the carrier is
    // installed on.
    ASSERT(isInstalledOnCurrentThread());
    // §6.5.1: registerLite ran (sole writer of `vm`) before this carrier could
    // be installed, so `vm` is non-null and immutable here.
    RELEASE_ASSERT(vm);
    // I14: the registration side effect below (MicrotaskQueue's constructor
    // appends to VM::m_microtaskQueues, M12-locked) plus everything a queue is
    // for requires the owner to hold this VM's JSLock.
    ASSERT(vm->currentThreadIsHoldingAPILock());

    if (!defaultMicrotaskQueue) [[unlikely]]
        defaultMicrotaskQueue = MicrotaskQueue::create(*vm);
    return *defaultMicrotaskQueue;
}

// ---- §6.6 Group 5: per-thread scratch buffers (Phase A inert; frozen
// Phase-B signature). Logic mirrors VM::scratchBufferForSize /
// VM::clearScratchBuffers (VM.cpp:1595-1624). ----------------------------

ScratchBuffer* VMLite::scratchBufferForSize(size_t size)
{
    if (!size)
        return nullptr;

    ASSERT(isInstalledOnCurrentThread()); // I11.

    // Leaf lock (§7): only fastMalloc/VMMalloc under it. Held even though
    // Phase A is owner-only so the locking discipline is already the Phase-B
    // one (GC root gathering will iterate registered lites' buffers).
    Locker locker { scratchBufferLock };

    if (size > sizeOfLastScratchBuffer) {
        // Protect against an N^2 memory usage pathology by ensuring that at
        // worst, we get a geometric series, meaning that the total memory
        // usage is somewhere around max(scratch buffer size) * 4.
        sizeOfLastScratchBuffer = size * 2;

        ScratchBuffer* newBuffer = ScratchBuffer::create(sizeOfLastScratchBuffer);
        RELEASE_ASSERT(newBuffer);
        scratchBuffers.append(newBuffer);
    }

    return scratchBuffers.last();
}

void VMLite::clearScratchBuffers()
{
    ASSERT(isInstalledOnCurrentThread()); // I11.
    Locker locker { scratchBufferLock };
    for (auto* scratchBuffer : scratchBuffers)
        scratchBuffer->setActiveLength(0);
}

// §6.7: SOLE defining TU for currentButterflyTID() (INTEGRATE-vmstate verifies
// ODR; the __has_include("VMLite.h") shims in runtime/ConcurrentButterfly.h
// and jit/ConcurrentButterflyOperations.cpp compile away now that VMLite.h
// exists).
ButterflyTID currentButterflyTID()
{
    auto* lite = VMLite::currentIfExists();
    return lite ? lite->tid : 0;
}

void setVMLiteTIDTagHook(void (*hook)(uint16_t))
{
    s_vmLiteTIDTagHook.store(hook, std::memory_order_release);
}

} // namespace JSC
