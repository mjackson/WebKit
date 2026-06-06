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

#include "JSCConfig.h"      // g_jscConfig.vmLiteTLSKey (Darwin mirror arm, UNGIL §A.1.1 / jit App. R5).
#include "MicrotaskQueue.h" // Complete type for ~RefPtr<MicrotaskQueue> in ~VMLite + create() (§6.5).
#include "VM.h"             // ScratchBuffer/VMMalloc (§6.6); currentThreadIsHoldingAPILock (I14).
#include "VMLiteInlines.h"  // isInstalledOnCurrentThread (I11 asserts below).
#include "VMLiteShared.h"   // VMLiteRegistry (debug registration asserts, I20).
#include <atomic>
#include <bit>
#include <mutex>
#include <wtf/Atomics.h> // crossModifyingCodeFence (ANNEX ISB1, U-T5).
#include <wtf/FastMalloc.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

#if OS(DARWIN)
#include <pthread.h>
#endif

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMLite);

// L4 (frozen, SPEC-vmstate §6.3): plain C++ thread_local, NOT
// pthread_getspecific. The accessor signatures in VMLite.h are frozen; this
// backing store is replaceable in Phase B (pinned register/TLS base).
static thread_local VMLite* t_currentVMLite { nullptr };

// ===========================================================================
// UNGIL §A.1.1 (U-T4a): the JIT/LLInt-visible mirror of t_currentVMLite.
//
// DEFINED in runtime/VM.cpp (full rationale block there); the gilOff-mode
// `loadVMLite` emitter (jit/AssemblyHelpers.cpp, U-T3) bakes its initial-exec
// TPOFF into Baseline/DFG/FTL code, and the LLInt offlineasm macro (U-T3
// OPEN obligation) will read the same symbol. This TU re-declares it with
// the IDENTICAL language linkage + TLS model so the stores below resolve to
// the same thread-invariant slot.
//
// COHERENCE CONTRACT (VM.cpp:230-246, discharged HERE — the dedicated IU
// obligation row VM.cpp demands, owner "the VMLite.cpp slice", is satisfied
// by this hunk; record the row in INTEGRATE-ungil.md, which is outside this
// task's writable set): VMLite::setCurrent — the SOLE writer of
// t_currentVMLite — mirrors EVERY TLS write here (and, on Darwin,
// pthread_setspecific's the same value into the g_jscConfig.vmLiteTLSKey
// slot), immediately after the t_currentVMLite store and BEFORE the TID-tag
// hook fires, INCLUDING null/uninstall writes (carrier teardown, thread
// exit): an unmirrored clear would leave a reused thread's generated code
// reading a stale/freed lite.
//
// Flag-off / GIL-on identity: the mirror is a plain TLS store on a path
// (lite install/uninstall) that flag-off code reaches only at JSLock
// acquire/release; no generated code reads the symbol except gilOff-mode
// compilations (§A.1.3 COMPILED-FOR-VM-mode rule), and no shipping
// configuration constructs a gilOff VM until the activation tasks land.
// ===========================================================================
#if OS(LINUX)
extern "C" __attribute__((tls_model("initial-exec"))) thread_local VMLite* g_jscCurrentVMLite;
#else
extern "C" thread_local VMLite* g_jscCurrentVMLite;
#endif

static ALWAYS_INLINE void mirrorCurrentVMLiteForGeneratedCode(VMLite* lite)
{
    // ELF / generic arm: the thread_local mirror itself (generated code reads
    // it via IE-TLS on Linux; inert elsewhere but kept coherent — it costs
    // one TLS store and removes a per-OS divergence in the writer).
    g_jscCurrentVMLite = lite;

#if OS(DARWIN)
    // Darwin arm (jit App. R5 mechanics): Mach-O TLV has no constant offset,
    // so generated code reads a pthread TSD slot instead; the key is created
    // at the P5-init point (jit slice) and published through the M4a-style
    // JSCConfig slot. JSC_CONFIG_HAS_VMLITE_TLS_KEY is defined in
    // JSCConfig.h BESIDE the vmLiteTLSKey member (the
    // JSC_ASSEMBLYHELPERS_HAS_LOAD_VMLITE inversion pattern, see
    // jit/AssemblyHelpers.cpp) — until that slot lands (outside this task's
    // writable set; OPEN obligation escalated at the U-T3 amendment), this
    // arm compiles away and the Darwin loadVMLite emitter keeps its
    // RELEASE_ASSERT_NOT_REACHED fail-stop, so no torn contract is possible.
#if defined(JSC_CONFIG_HAS_VMLITE_TLS_KEY)
    // FAIL-STOP, not skip-if-absent: once this arm compiles in, the Darwin
    // loadVMLite emitter emits unconditional TSD loads on the premise that
    // EVERY install/uninstall reached this slot. A lite install racing ahead
    // of pthread_key_create (P5-init ordering bug) must crash here, loudly —
    // a silent skip would leave this thread's generated code reading a null
    // or (on thread reuse after a skipped uninstall) stale/freed lite. This
    // also ENFORCES the install-after-key-creation ordering the activation
    // checklist otherwise only asserts in comments.
    RELEASE_ASSERT(g_jscConfig.vmLiteTLSKey);
    int result = pthread_setspecific(static_cast<pthread_key_t>(g_jscConfig.vmLiteTLSKey), lite);
    RELEASE_ASSERT(!result);
#endif
#endif
}

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
    // Buffers installed at A16 baked indices are owned by this same list
    // (ensureScratchBufferAtIndex appends), so this loop frees them too.
    for (auto* scratchBuffer : scratchBuffers)
        VMMalloc::free(scratchBuffer);
    scratchBuffers.clear();

    // A16: free the segment arrays (the buffers they pointed at were freed
    // above via the ownership list).
    for (auto& segmentSlot : scratchSegments) {
        if (auto* segment = segmentSlot.load(std::memory_order_relaxed)) {
            segmentSlot.store(nullptr, std::memory_order_relaxed);
            fastFree(segment);
        }
    }
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

    // UNGIL §A.1.1 (U-T4a): mirror the write for generated code — after the
    // t_currentVMLite store, before the TID-tag hook, including null
    // (uninstall) writes. See the contract block above the mirror
    // declaration; gilOff-mode Baseline/DFG emission (loadVMLite) is sound
    // only because every writer path funnels through here.
    mirrorCurrentVMLiteForGeneratedCode(lite);

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
// Phase-B signature). ------------------------------------------------------
//
// ANNEX A16 NON-BAKED ARM (NORMATIVE, F9 re-freeze vs vmstate:534-539): the
// reserved VMLite::scratchBufferForSize(size_t) is implemented "over the
// segmented table by size-class index" — NOT by transplanting
// VM::scratchBufferForSize's `scratchBuffers.last()` geometric series.
// That transplant is memory-UNSAFE here: A16 repurposed `scratchBuffers` as
// the lite's buffer-OWNERSHIP list, and ensureScratchBufferAtIndex appends
// baked-index buffers to it too — including from OTHER threads via
// VM::allocateBakedScratchBufferIndex's install fan and the registration
// backfill — so `.last()` can be an arbitrarily small baked buffer that a
// stale `size <= sizeOfLastScratchBuffer` check never re-validates
// (undersized return => caller heap overflow). The ownership list must
// never be a lookup structure.
//
// Size classes are powers of two (class c serves sizes in (2^(c-1), 2^c],
// buffer size 2^c): at most one buffer per class per lite, total per-lite
// non-baked footprint <= 2x the largest request — the same geometric-series
// memory bound VM's policy targets. Each class lazily claims ONE
// process-wide ScratchBufferRegistry index (monotonic, never freed), so the
// per-lite storage is the ordinary A16 segmented table: registration
// backfill pre-installs the classes other lites already use, and the
// two-load read below serves repeats lock-free.
//
// s_scratchSizeClassLock rank: taken with NO other lock held (it is the
// outermost acquisition on this path) and only ScratchBufferRegistry::m_lock
// (via allocateIndex) is acquired under it — consistent with SBR sitting
// outside VMLiteRegistry::lock (§LK.6); scratchBufferLock (inside
// ensureScratchBufferAtIndex) is taken only after it is released.

static constexpr unsigned numScratchSizeClasses = sizeof(size_t) * 8;
static Lock s_scratchSizeClassLock;
static uint64_t s_scratchSizeClassAllocated WTF_GUARDED_BY_LOCK(s_scratchSizeClassLock) { 0 };
static unsigned s_scratchSizeClassIndices[numScratchSizeClasses] WTF_GUARDED_BY_LOCK(s_scratchSizeClassLock);

ScratchBuffer* VMLite::scratchBufferForSize(size_t size)
{
    if (!size)
        return nullptr;

    ASSERT(isInstalledOnCurrentThread()); // I11.

    // bit_ceil of a size above 2^63 is unrepresentable (UB); no plausible
    // scratch request approaches it, so fail-stop first.
    RELEASE_ASSERT(size <= (static_cast<size_t>(1) << (numScratchSizeClasses - 1)));
    size_t classSize = std::bit_ceil(size); // Smallest power of two >= size.
    unsigned sizeClass = std::countr_zero(classSize);

    unsigned index;
    {
        Locker locker { s_scratchSizeClassLock };
        if (!(s_scratchSizeClassAllocated & (1ull << sizeClass))) {
            s_scratchSizeClassIndices[sizeClass] = ScratchBufferRegistry::singleton().allocateIndex(classSize);
            s_scratchSizeClassAllocated |= 1ull << sizeClass;
        }
        index = s_scratchSizeClassIndices[sizeClass];
    }

    // Fast path: the two-load lock-free read (repeat requests in this class,
    // or a class another lite already claimed that our registration backfill
    // / a racing install fan populated).
    if (ScratchBuffer* buffer = scratchBufferAtIndex(index))
        return buffer;

    // Slow path: idempotent install under scratchBufferLock; appends to the
    // `scratchBuffers` ownership list (GC scan + teardown free), exactly
    // like the baked arm.
    ensureScratchBufferAtIndex(index, classSize);
    ScratchBuffer* buffer = scratchBufferAtIndex(index);
    RELEASE_ASSERT(buffer);
    return buffer;
}

// NOTE: VMLite::sizeOfLastScratchBuffer (VMLite.h L2 append, task 7) is
// RETIRED by the size-class dispatch above: it stays 0 forever and no code
// may consult it (a stale high-water check against the shared ownership
// list is exactly the undersized-buffer hazard documented above). The field
// itself is outside this task's writable set (VMLite.h); removing it is an
// orchestrator-tracked cleanup, harmless meanwhile.

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

// ---- ANNEX A16 (UNGIL §A.1.6, U-T1): process-wide baked-index registry +
// per-lite segmented table. Dark until U-T4 emission allocates indices. ----
//
// U-T4a STATUS (codegen-side contract, recorded here because this TU is the
// runtime half the emission consumes). This TU is the RUNTIME HALF ONLY —
// nothing below certifies U-T4a complete:
//
//   LANDED (runtime side, this TU + VM.cpp/JSLock.cpp): the registry
//   (allocateIndex/sizeForIndex/indexCount), the per-lite segmented table
//   (scratchBufferAtIndex two-load read / ensureScratchBufferAtIndex /
//   backfillBakedScratchBuffers), VM::allocateBakedScratchBufferIndex's
//   install fan (VM.cpp), the JSLock.cpp registration backfill, the §A.1.1
//   g_jscCurrentVMLite mirror in setCurrent above, and the annex-A16
//   non-baked arm (VMLite::scratchBufferForSize over the segmented table by
//   size-class index — see the block above it).
//
//   NOT LANDED BY THIS SLICE (the U-T4a codegen half; jit/ + dfg/ are
//   OUTSIDE this slice's writable file set — VMLite.cpp only): the
//   Baseline/DFG mode-keyed (codeBlock->vm->m_gilOff, §A.1.3
//   COMPILED-FOR-VM rule) Group-3 + scratch emission, the per-row A16-ext
//   emission, and the golden-disasm re-baseline (shared with U-T4b). The
//   only loadVMLite machinery in the tree is the U-T3 emitter in
//   jit/AssemblyHelpers.cpp; U-T4a MUST NOT be marked complete until the
//   emission slice lands against this runtime half.
//
//   NOT YET LANDABLE — A16 EXTENSION (AUD1.K4): the lite-resident copies of
//   VM::m_megamorphicCache, VM::m_hasOwnPropertyCache,
//   JSGlobalObject::m_regExpGlobalData (SD19) and JSGlobalObject::
//   m_weakRandom (K4.VIII.10) require L2 member appends + offsetOf*
//   accessors in VMLite.h, which is OUTSIDE this task slice's writable file
//   set (VMLite.cpp only). Until those slots exist, gilOff-mode compilation
//   MUST NOT emit lite-relative inline fast paths for those four rows — the
//   emission slice keeps them dark (no gilOff VM is constructible before
//   the activation tasks, so this is a sequencing constraint, not a live
//   hole). Activation checklist (each item outside this file): (1) VMLite.h
//   slot appends + offsets; (2) registration-time slot fill (lazy §K.3
//   publish for ensure* contents) at the JSLock.cpp/spawn registration
//   sites; (3) registry-walk root scan + ~VM walk for the cell-holding
//   RegExpGlobalData copies (AUD1.K2); (4) the K4.VI.2 epoch-bump fan-out
//   inside the firing stop; (5) the per-row Baseline/DFG emission keyed on
//   the COMPILED-FOR VM's mode (codeBlock->vm->m_gilOff, §A.1.3).
//   Flag-off/GIL-on keeps today's baked VM/global addresses (golden gates
//   intact) regardless.

ScratchBufferRegistry& ScratchBufferRegistry::singleton()
{
    static LazyNeverDestroyed<ScratchBufferRegistry> registry;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        registry.construct();
    });
    return registry;
}

unsigned ScratchBufferRegistry::allocateIndex(size_t size)
{
    Locker locker { m_lock };
    unsigned index = m_sizes.size();
    // The per-lite segmented table is fixed-capacity (L2 append; VMLite.h).
    // Exceeding it would need a spec revision of the segment geometry, not a
    // silent overflow.
    RELEASE_ASSERT(index < VMLite::maxScratchSegments * VMLite::scratchSegmentSize);
    m_sizes.append(size);
    return index;
}

size_t ScratchBufferRegistry::sizeForIndex(unsigned index) const
{
    Locker locker { m_lock };
    return m_sizes[index];
}

unsigned ScratchBufferRegistry::indexCount() const
{
    Locker locker { m_lock };
    return m_sizes.size();
}

void VMLite::ensureScratchBufferAtIndex(unsigned index, size_t size)
{
    ASSERT(index < maxScratchSegments * scratchSegmentSize);
    // scratchBufferLock is acquired under VMLiteRegistry::lock by the
    // VM-side install fan — legal per the §LK.6 re-rank (see
    // ScratchBufferRegistry's class comment). fastMalloc/VMMalloc only under
    // it, as before.
    Locker locker { scratchBufferLock };

    auto& segmentSlot = scratchSegments[index >> scratchSegmentShift];
    auto* segment = segmentSlot.load(std::memory_order_relaxed);
    if (!segment) {
        segment = static_cast<std::atomic<ScratchBuffer*>*>(
            fastZeroedMalloc(scratchSegmentSize * sizeof(std::atomic<ScratchBuffer*>)));
        // Release-publish the zeroed segment for the lock-free readers.
        segmentSlot.store(segment, std::memory_order_release);
    }

    auto& entry = segment[index & (scratchSegmentSize - 1)];
    if (entry.load(std::memory_order_relaxed))
        return; // Idempotent: a racing fan/backfill already installed it.

    ScratchBuffer* buffer = ScratchBuffer::create(size);
    RELEASE_ASSERT(buffer);
    // Ownership list FIRST (the repurposed Group 5: backs the jit-R2 GC scan
    // and the dtor free above), then the release-publish readers load from.
    scratchBuffers.append(buffer);
    entry.store(buffer, std::memory_order_release);
}

void VMLite::backfillBakedScratchBuffers()
{
    auto& registry = ScratchBufferRegistry::singleton();
    unsigned count = registry.indexCount();
    for (unsigned index = 0; index < count; ++index)
        ensureScratchBufferAtIndex(index, registry.sizeForIndex(index));
}

// ---- UNGIL §A.2.1 per-lite traps seam (U-T2). -----------------------------
//
// The §A.2.1 contract appends (L2, after Group 6) `VMThreadContext
// threadContext` to VMLite, giving every thread its own VMTraps (trap word +
// StackManager stack limits) that generated code reaches via the chained
// offset lite->threadContext.traps().m_trapBits. VMLite.h is OUTSIDE U-T2's
// owned file set, so the member append cannot land here; until it does, the
// per-lite traps ALIAS the VM-level word:
//   - rule-3 fan-outs (VMTraps::fireTrapVMWide) set the VM word exactly once
//     (callers skip pointer-identical duplicates);
//   - the token-acquisition OR (orVMWideTrapBitsIntoLite) is a no-op;
//   - per-lite readers (D9 park-lite polls, the W1 captured-lite poll)
//     observe VM-word — i.e. phase-1 — semantics;
//   - the JIT chained-offset consumers are dark (U-T3/U-T4).
//
// NOT ACTIVATABLE BY A LOCAL FLIP: flipping the return below to
// `&lite.threadContext.traps()` is necessary but NOT sufficient — the
// VMLite.h member append, the §F.1 JSLock.cpp orVMWideTrapBitsIntoLite
// wiring (currently ZERO callers), the §A.2.2 VM.cpp updateStackLimits
// re-target (memory-safety grade under N-parallel entry), and the park-site
// W1/D9 predicate split must all land first. The full activation checklist —
// each item outside U-T2's owned file set, recorded with the orchestrator —
// lives with the seam declaration in VMTraps.h. Until then, §A.2 clauses 1-2
// are NOT in effect and N-mutator GIL-off entry must not be enabled;
// VMTraps.cpp carries interim single-shared-word TERM1.2 semantics (the
// sibling-visibility take rule + carrier re-entry shield) that keep VM-wide
// termination delivered to every entered thread under the alias.

VMTraps* perThreadTrapsIfExists(VMLite& lite)
{
    // Unregistered/poisoned lites carry no usable VM; callers only walk
    // registered lites (under the registry lock), so this is belt-and-
    // suspenders for pre-registration probes.
    if (!lite.vm)
        return nullptr;
    return &lite.vm->traps();
}

// ---- UNGIL §A.3.2c (ANNEX ISB1, U-T5): stop-generation counter +
// per-thread context-sync on non-NVS JIT re-entry. ----
//
// ISB1.1 state: one process-wide seq_cst uint64 stop-generation counter.
// EVERY §A.3 conductor (and every heap §10 conductor that patched/jettisoned
// code — the cheap conservative form bumps for every conductor) increments it
// INSIDE the stop window, before resume (the §A.3 conductor in VMManager.cpp
// calls jsThreadsBumpStopGeneration() between the patcher-side
// crossModifyingCodeFence and the stop-word clear).
//
// ISB1.2 consumption: every transition into "may execute JIT code" that did
// NOT pass through an NVS exit — F8 AHA re-acquisition (including the
// §A.3.2b bit-already-clear path), §F token acquisition and ACT (both funnel
// through GCClient::Heap::acquireHeapAccess, which calls the sync below on
// its success path), the DAL2 dtor and the §F.5 LIFO restore (both re-enter
// through the same AHA) — loads the counter, compares the per-THREAD copy,
// and on mismatch executes a context-synchronizing instruction
// (WTF::crossModifyingCodeFence: ISB on arm64, a serializing instruction on
// x86-64) BEFORE any JIT-code entry, then stores the new value. NVS exit
// keeps the unconditional R1.d ISB and ALSO refreshes the copy
// (jsThreadsNVSExitInstructionSync, called by the notifyVMStop/ticket-park
// exits in VMManager.cpp).
//
// DEVIATION RECORDED (spec letter vs storage): ISB1.1 says "per-lite uint64
// copy (L2 append)". The L2 append lives in VMLite.h, which is OUTSIDE
// U-T5's writable file set, so the copy is a thread_local here instead. This
// is a strict refinement, not a weakening: the guarantee ISB1 carries is
// per-CPU-THREAD instruction-stream synchronization (an ISB synchronizes the
// executing PE, not a carrier), and a thread that has synced for generation
// G has synced for ALL its carriers at G — a per-lite copy would only force
// redundant extra ISBs on multi-carrier threads. Lift into VMLite.h (L2
// append + offsetOf accessor) if a JIT-inlined fast path ever wants it.
//
// ISB1.5 cost: GIL-on/flag-off zero (the counter never bumps — only gilOff
// §A.3 conductors call the bump — and the compare sits on gilOff-only
// paths). GIL-off steady state: one relaxed load + compare per access/token
// transition; the seq_cst bump is conductor-only. Visibility of the bump to
// re-acquirers needs no seq_cst load here: the bump is sequenced before the
// conductor's seq_cst stop-word CLEAR, and a re-acquirer only reaches JIT
// code after its seq_cst stop-word load observes that clear (the §A.3.2b
// gate), which carries the synchronizes-with edge.

static std::atomic<uint64_t> s_jsThreadsStopGeneration { 1 };
static thread_local uint64_t t_jsThreadsStopGenerationSeen { 0 };

void jsThreadsBumpStopGeneration()
{
    s_jsThreadsStopGeneration.fetch_add(1, std::memory_order_seq_cst);
}

void jsThreadsSyncToStopGenerationBeforeJITEntry()
{
    uint64_t generation = s_jsThreadsStopGeneration.load(std::memory_order_relaxed); // ISB1.5: relaxed + compare.
    if (generation != t_jsThreadsStopGenerationSeen) [[unlikely]] {
        WTF::crossModifyingCodeFence(); // arm64 ISB / x86-64 serializing instruction, BEFORE any JIT entry.
        t_jsThreadsStopGenerationSeen = generation;
    }
}

void jsThreadsNVSExitInstructionSync()
{
    // R1.d: every mutator leaving an NVS park executes an ISB
    // unconditionally; ISB1.2: the NVS exit also refreshes the per-thread
    // copy. ORDER IS LOAD-BEARING: sample the generation BEFORE the fence
    // and record that pre-fence value. Recording a post-fence sample could
    // mark a bump that landed between the fence and the load as "synced"
    // although no ISB ran after its window's patch; with the pre-fence
    // sample any such bump stays unrecorded and the next JIT-entry compare
    // (jsThreadsSyncToStopGenerationBeforeJITEntry) issues the ISB. The
    // conductor's patch is sequenced before its bump, so an observed value
    // is always covered by the fence below.
    uint64_t generation = s_jsThreadsStopGeneration.load(std::memory_order_relaxed);
    WTF::crossModifyingCodeFence();
    t_jsThreadsStopGenerationSeen = generation;
}

// ---- UNGIL §A.3.6/ANNEX A36 carrier-TID hooks (U-T1). ----

static std::atomic<uint16_t (*)()> s_allocateCarrierTIDHook { nullptr };
static std::atomic<void (*)(uint16_t)> s_releaseCarrierTIDHook { nullptr };

void setCarrierTIDHooks(uint16_t (*allocate)(), void (*release)(uint16_t))
{
    s_allocateCarrierTIDHook.store(allocate, std::memory_order_release);
    s_releaseCarrierTIDHook.store(release, std::memory_order_release);
}

uint16_t allocateCarrierTID()
{
    auto* hook = s_allocateCarrierTIDHook.load(std::memory_order_acquire);
    RELEASE_ASSERT_WITH_MESSAGE(hook,
        "GIL-off carrier registration requires the ThreadManager carrier-TID provider (UNGIL annex A36; INTEGRATE-ungil.md)");
    uint16_t tid = hook();
    RELEASE_ASSERT(tid && tid != 0x7fff); // never tag 0 or notTTLTID (A36/TTL).
    return tid;
}

void releaseCarrierTIDIfHooked(uint16_t tid)
{
    if (auto* hook = s_releaseCarrierTIDHook.load(std::memory_order_acquire))
        hook(tid);
}

} // namespace JSC
