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

#pragma once

// VMLite — per-thread execution state for N threads sharing one logical VM
// (SPEC-vmstate §6, frozen layout §6.3).
//
// Phase A (this milestone): VMLites are CARRIERS ONLY (§6.1). Under the GIL,
// every JS-visible piece of execution state still lives in VM members under
// today's names; no interpreter/JIT/runtime path consults a VMLite. The
// carriers exist so threads can be tagged (tid), registered (VMLiteRegistry,
// VMLiteShared.h §6.5.1), TLS-installed (setCurrent, L4), and so the frozen
// VMLitePrimitives layout can be asserted layout-identical to the matching VM
// member block (§6.4 M6 equivalence asserts).
//
// Phase B (UNOWNED — SPEC-vmstate Dev 10; frozen contract only): a pinned
// register/TLS base makes `VM::field` accesses VMLitePrimitives-relative, and
// per-thread VMThreadContext/VMTraps land per §6.8. The layout below is the
// ABI Phase B consumes; freeze rules L1-L5 (§6.3) apply.

#include "Interpreter.h" // JSOrWasmInstruction (interpreter/Interpreter.h:61); brings JSCJSValue.h (EncodedJSValue).
#include "JSExportMacros.h"
#include <memory>
#include <type_traits>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class CallFrame;
class Exception;
class MicrotaskQueue;
class QueuedTask;
class RegExp;
class VM;
struct EntryFrame;
struct ScratchBuffer; // Defined in VM.h; VMLite.h must not include VM.h (VM.h includes us via M6).

// 15-bit thread-ID payload of the tagged butterfly word (SPEC-objectmodel
// §9.1; THREAD.md "2^15 thread-ID space"). ConcurrentButterfly.h declares the
// identical alias — a type-alias redeclaration naming the same type is legal.
using ButterflyTID = uint16_t;

// =============================================================================
// VMLitePrimitives — frozen ABI artifact (SPEC-vmstate §6.3, Groups 1-3).
//
// Names/types EXACTLY mirror the current VM declarations (including m_
// prefixes) — the ABI is consumed via offsetof and .asm. This X-macro is
// authoritative: VM (§6.4 M6) expands the SAME macro to declare its Group 1-3
// member block, and per-field static_asserts (M6, in VM.h) pin
//     OBJECT_OFFSETOF(VM, field) - OBJECT_OFFSETOF(VM, topCallFrame)
//         == OBJECT_OFFSETOF(VMLitePrimitives, field)
// so the two layouts cannot drift.
//
// Freeze rules (§6.3): L1 — field order frozen; add/remove/reorder is a spec
// revision. L5 — no numeric offsets are frozen beyond "Group 1 pair at
// 0x00/0x08"; everything else goes through OBJECT_OFFSETOF; padding is the
// ABI's choice.
//
// Deliberately NOT in VMLitePrimitives (§6.3; M6 relocates these just outside
// the VM block, names/types/sites unchanged): m_terminationException
// (cross-thread by design), maybeReturnPC, topJSPIContext,
// m_currentSoftReservedZoneSize (interleaved in today's Group-3 range; the
// §6.4(2) span assert forces it out), m_executingRegExp.
// =============================================================================

#define FOR_EACH_VMLITE_PRIMITIVE_FIELD(v) \
    /* Group 1: pair; order+adjacency ABI (loadpairq/storepairq of */ \
    /* "VM::topCallFrame" in LowLevelInterpreter.asm; VM.h pair assert) */ \
    v(CallFrame*, topCallFrame) \
    v(EntryFrame*, topEntryFrame) \
    /* Group 2: exception/unwind state (VM.h:395,397,878-890) */ \
    v(Exception*, m_exception) \
    v(Exception*, m_lastException) \
    v(CallFrame*, callFrameForCatch) \
    v(void*, targetMachinePCForThrow) \
    v(JSOrWasmInstruction, targetInterpreterPCForThrow) \
    v(uintptr_t, targetInterpreterMetadataPCForThrow) \
    v(void*, targetMachinePCAfterCatch) \
    v(CallFrame*, newCallFrameReturnValue) \
    v(EncodedJSValue, encodedHostCallReturnValue) \
    v(uint32_t, targetTryDepthForThrow) \
    v(uint32_t, osrExitIndex) \
    v(unsigned, varargsLength) \
    v(void*, osrExitJumpDestination) \
    /* Group 3: VM stack bookkeeping (VM.h:1237-1240). NOT the limits */ \
    /* generated code checks — those are VMTraps::m_stack (§6.8); one */ \
    /* authority, no duplicate here. */ \
    v(void*, m_stackPointerAtVMEntry) \
    v(void*, m_stackLimit) \
    v(void*, m_lastStackTop)

struct VMLitePrimitives {
#define VMLITE_DECLARE_FIELD(type, name) type name { };
    FOR_EACH_VMLITE_PRIMITIVE_FIELD(VMLITE_DECLARE_FIELD)
#undef VMLITE_DECLARE_FIELD

    // Per-field offsets, VMLitePrimitives-relative (Phase B consumes; L5:
    // never frozen numerically, always via OBJECT_OFFSETOF).
#define VMLITE_DECLARE_FIELD_OFFSET(type, name) \
    static constexpr ptrdiff_t offsetOf_##name() { return OBJECT_OFFSETOF(VMLitePrimitives, name); }
    FOR_EACH_VMLITE_PRIMITIVE_FIELD(VMLITE_DECLARE_FIELD_OFFSET)
#undef VMLITE_DECLARE_FIELD_OFFSET
};

// L3 (§6.3): deliberately NO standard-layout assert — targetInterpreterPCForThrow
// is a Variant (rev 7). OBJECT_OFFSETOF stays valid via __builtin_offsetof
// (§0); the §6.4(2) per-field equivalence asserts in VM.h are the real
// layout contract.
static_assert(std::is_trivially_copyable_v<VMLitePrimitives>); // variant of trivials
static_assert(OBJECT_OFFSETOF(VMLitePrimitives, topCallFrame) == 0);
static_assert(OBJECT_OFFSETOF(VMLitePrimitives, topEntryFrame) == sizeof(void*),
    "pair-load contract");

// =============================================================================
// VMLite — the per-thread carrier (SPEC-vmstate §6.3).
//
// Lifetime/registration (§6.5.1, VMLiteShared.h): registerLite(lite, vm) is
// the sole writer of `vm` (immutable after); a lite is unregistered before it
// is destroyed and before its thread's teardown setCurrent(nullptr); a VM
// must not die while a registered lite's `vm` points at it (§6.4.4 ~VM
// assert). The main thread's carrier is VM::m_mainVMLite (tid 0), installed/
// restored by JSLock mirroring the atom-table swap (§6.4.4 — M4 hunks in
// INTEGRATE-vmstate.md).
// =============================================================================

class VMLite {
    WTF_MAKE_NONCOPYABLE(VMLite);
    WTF_MAKE_TZONE_ALLOCATED(VMLite);
public:
    VMLitePrimitives primitives;  // FIRST member, offset 0 (asserted below). FROZEN (L1).
    uint16_t tid { 0 };           // ButterflyTID; 0 = main thread. §6.7: ThreadManager (api WS)
                                  // is the sole allocator; spawn writes it BEFORE setCurrent;
                                  // immutable while installed, so reads need no sync.
    VM* vm { nullptr };           // Set by VMLiteRegistry::registerLite(lite, vm) (§6.5.1, sole
                                  // writer); immutable after.
    // Group 4: regexp, lazy:
    RegExp* executingRegExp { nullptr };
    std::unique_ptr<BumpPointerAllocator> regExpAllocator;
    // Group 5: scratch buffers (§6.6; Phase A inert — reserved for the frozen
    // Phase-B contract: baked DFG/FTL scratch-buffer pointers become
    // VMLite-relative):
    Lock scratchBufferLock; // leaf rank (§7): fastMalloc only under it
    Vector<ScratchBuffer*> scratchBuffers;
    // Group 6: microtasks (§6.5), lazy (Phase A: exercised only by unit tests;
    // VM::queueMicrotask/drainMicrotasks are NOT rerouted):
    RefPtr<MicrotaskQueue> defaultMicrotaskQueue;

    // L2 (§6.3): new fields append after Group 6 only. Appended (task 7,
    // §6.6 plumbing): logically Group-5 state — L2 forbids inserting it next
    // to scratchBuffers above. Guarded by scratchBufferLock.
    size_t sizeOfLastScratchBuffer { 0 };

    static constexpr ptrdiff_t offsetOfPrimitives() { return OBJECT_OFFSETOF(VMLite, primitives); }
    static constexpr ptrdiff_t offsetOfTID() { return OBJECT_OFFSETOF(VMLite, tid); }

    // TLS accessors (L4): backed by `thread_local VMLite* t_currentVMLite` in
    // VMLite.cpp (NOT pthread_getspecific). Signatures frozen; the
    // implementation is replaceable in Phase B (pinned register/TLS base).
    JS_EXPORT_PRIVATE static VMLite* currentIfExists();   // TLS read
    JS_EXPORT_PRIVATE static VMLite& current();           // asserts non-null
    // Installs `lite` (may be null = uninstall) and returns the previous
    // value. Debug (I18/I20): asserts lite->tid != notTTLTID and that the
    // lite is registered. After the TLS write it invokes the TID-tag hook
    // (below) with `lite ? lite->tid : 0` — including for null — so the JIT's
    // per-thread butterfly TID tag stays coherent across §6.4.4
    // install/restore and multi-VM switches (jit CS3/I19).
    JS_EXPORT_PRIVATE static VMLite* setCurrent(VMLite*); // returns previous

    JS_EXPORT_PRIVATE VMLite();
    JS_EXPORT_PRIVATE ~VMLite(); // I20: asserts not installed here, not registered; poisons in debug.

    // Helpers defined in VMLiteInlines.h (I11 owner asserts build on these).
    inline bool isInstalledOnCurrentThread() const;
    inline BumpPointerAllocator& ensureRegExpAllocator(); // Group 4 lazy

    // ---- §6.5 Group 6: per-thread default microtask queue (Phase A inert —
    // VM::queueMicrotask/drainMicrotasks are NOT rerouted; Phase B routes
    // enqueue/drain to the current thread's queue; cross-thread enqueue stays
    // out of scope).
    //
    // COVERAGE STATUS (honest gap, tracked): these facilities (and the §6.6
    // scratch-buffer ones below) are JS-UNREACHABLE in Phase A, so the
    // JSTests/threads/vmstate suite cannot execute them; the task-7 C++ test
    // obligations (owner-only enqueue/drain, lazy-creation idempotence,
    // drain-exactly-once, scratchBufferForSize(0)==nullptr, geometric-growth
    // reuse, dtor-frees-under-ASAN) are an UNMET PENDING item in
    // INTEGRATE-vmstate.md and MUST land before any Phase-B routing consults
    // a VMLite. Do not treat this block as gated by the existing suites.
    // HARD GATE (review round 3): until that test lands, ANY new caller of
    // these facilities in any workstream's diff is a blocker by construction
    // — see the PENDING entry in INTEGRATE-vmstate.md. ----
    //
    // Lazy creation. GC visibility (§6.5): the queue is built with
    // MicrotaskQueue::create(*vm), whose constructor appends it to
    // VM::m_microtaskQueues — the single registration list GC markers
    // traverse. Both the append (M12) and the markers' iteration (M11) take
    // VMLiteRegistry::singleton().lock once those hunks are applied, so lazy
    // creation on a spawned thread cannot corrupt LIST MEMBERSHIP under
    // concurrent marking. The registry lock deliberately does NOT cover queue
    // CONTENTS: inside the locked forEach the marker calls visitAggregate,
    // which walks the queue's Deque lock-free. That is sound for the same
    // reason it is sound today (MicrotaskQueue.cpp:158-160): deque contents
    // are only visited at collector phases that suspend the mutator
    // (FixPoint/Begin — the "Sh" constraint runs in the fixpoint, world
    // stopped). Under useJSThreads this invariant must mean ALL mutators
    // stopped — verified, not assumed: M11 rationale + cross-WS checklist
    // item 12 in INTEGRATE-vmstate.md make it an explicit integration gate.
    // I11/I14: caller must be the installed owner thread AND hold the VM's
    // JSLock (asserted).
    JS_EXPORT_PRIVATE MicrotaskQueue& ensureDefaultMicrotaskQueue();

    // I11 helpers (VMLiteInlines.h): the per-thread queue is enqueued/drained
    // only by its owner — both debug-assert isInstalledOnCurrentThread().
    inline void enqueueMicrotaskToDefaultQueue(QueuedTask&&);
    // Runs a full microtask checkpoint on the default queue (no-op if the
    // queue was never created). Requires the JSLock (it enters the VM).
    inline void drainDefaultMicrotaskQueue();

    // ---- §6.6 Group 5: per-thread scratch buffers. Phase A inert: no baked
    // DFG/FTL pointer routes here yet (that is the frozen Phase-B contract;
    // this signature is the §6.6-reserved one and must not change). Mirrors
    // VM::scratchBufferForSize including the geometric-growth policy.
    //
    // GC CAVEAT (Phase A): unlike VM::m_scratchBuffers, these buffers are NOT
    // visited by VM::gatherScratchBufferRoots. Until Phase B adds
    // registry-wide root gathering, callers (unit tests) must leave
    // activeLength at 0 / must not park JSValues that need marking here.
    JS_EXPORT_PRIVATE ScratchBuffer* scratchBufferForSize(size_t);
    JS_EXPORT_PRIVATE void clearScratchBuffers(); // setActiveLength(0) on all; owner-only.
};
static_assert(OBJECT_OFFSETOF(VMLite, primitives) == 0);

// =============================================================================
// §6.7 — currentButterflyTID()
//
// SOLE defining TU is VMLite.cpp (INTEGRATE verifies ODR; the
// ConcurrentButterfly.h §9.1 shim and the jit ConcurrentButterflyOperations.cpp
// shim are both #if !__has_include("VMLite.h")-guarded and compile away now
// that this header exists). Returns the installed carrier's tid, or 0 when no
// VMLite is installed (I18: main thread / embedder threads / GIL phase — a
// never-entering thread touches no JS objects, so 0 is unobservable there);
// never notTTLTID (0x7fff).
// =============================================================================

JS_EXPORT_PRIVATE ButterflyTID currentButterflyTID();

// TID-tag hook (§6.7; jit CS3/I19 provider). Null by default; jit task 1b
// (ConcurrentButterflyOperations.cpp, initializeButterflyTIDTagForCurrentThread)
// registers its P5 tag-update body so every lite switch keeps
// g_jscButterflyTIDTag coherent without a runtime/ -> jit/ include. A null
// hook is a no-op (Phase-A standalone builds). The hook runs AFTER the TLS
// write in setCurrent, with `lite ? lite->tid : 0` (including uninstalls);
// api §5.2's explicit P5/clear calls remain and are idempotent with it.
JS_EXPORT_PRIVATE void setVMLiteTIDTagHook(void (*)(uint16_t));

} // namespace JSC
