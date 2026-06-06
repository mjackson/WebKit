# INTEGRATE-ungil — GIL-removal landing ledger (IU; SPEC-ungil / UNGIL-HANDOUT §IM, TERM1.6)

Created by U-T1 per TERM1.6. IU is the landing ledger for the ungil
milestone, schema per the INTEGRATE-* house pattern. The seven mandated
tables (i)–(vii) are below as skeletons; each names its owning task. Until a
task fills its table, every "IU row" citation in SPEC-ungil / the handout is
an OBLIGATION on that landing task. IU adds call-site enumeration only and
NEVER re-rules a K4/N7/TERM1 disposition (the EXECUTED audit tables in the
handout are consumed verbatim).

Authority: docs/threads/UNGIL-HANDOUT.md (normative handout);
docs/threads/SPEC-ungil.md is the doc of record on conflict.

## Task log

- U-T1 (§A.1.2-7 / §A.3.6 base rerouting, DARK) — LANDED in-tree. All new
  behavior is keyed on `vm.m_gilOff` (per-VM, U0c) or
  `VM::isGILOffProcess()`; no shipping configuration sets either, so
  flag-off AND flag-on+GIL-on behavior is unchanged. Files (all
  U-T1-owned):
  - `runtime/VM.h` / `runtime/VM.cpp`:
    - `m_gilOff` + `gilOff()` (U0c; immutable, computed at the TOP of the VM
      ctor before any entry/codegen/m_mainVMLite registration; winner of
      `Heap::tryDesignateStickySharedServer()` under gilOffProcess, with the
      eager `noteSharedServerSticky()` at clientSet()==1).
    - `VM::isGILOffProcess()` — C++-side equivalent of the §A.1.3 level-(i)
      JSCConfig `gilOffProcess` byte. **OBLIGATION (U-T3): the Config byte
      (JSCConfig.h:106, beside the M4a slot) must stay derivation-identical
      (useJSThreads && !useThreadGIL && useVMLite && useSharedAtomStringTable
      && useSharedGCHeap, latched at Config finalization).**
    - `group3Primitives()` mode-split selector; rerouted same-name accessors:
      `exception/clearException/setException/lastException/clearLastException/
      exceptionForInspection/hasPendingTerminationException`,
      `stackPointerAtVMEntry/setStackPointerAtVMEntry`, `stackLimit`,
      `isSafeToRecurse`, `lastStackTop/setLastStackTop`, `updateStackLimits`.
      GIL-off + no installed same-VM lite falls back to the (inert) VM block
      — ctor tail / ~VM tail / §F.5 nested windows; recorded here as a
      deliberate dark-phase semantics (the activation tasks must not rely on
      VM-block contents GIL-off).
    - `addressOf*` / `*Offset()` emission helpers DELIBERATELY not
      mode-split (GIL-on emission only; gilOff compilations emit
      loadVMLite + VMLitePrimitives offsets — U-T3/U-T4). Comment block in
      VM.h is the in-tree marker.
    - §A.1.5: `isEntered()` mode split (`isAnyThreadEntered()` registry
      walk), `currentThreadEntryScope()`, per-lite service-bit routing +
      `backfillEntryScopeServiceBitsForLiteRegistration()`,
      VM-wide fan-out `requestVMWideEntryScopeService()` (registry-locked).
    - §A.1.6/A16: `allocateBakedScratchBufferIndex()` (registry index +
      install fan); `scratchBufferForSize/clearScratchBuffers/isScratchBuffer`
      gilOff dispatch to the CURRENT lite; `gatherScratchBufferRoots` now
      ALSO walks the VMLiteRegistry (per-VM filter) and scans each lite's
      ownership list (jit R2) — closes VMLite's Phase-A "not visited" GC
      caveat.
    - ANNEX A36: `m_vmEpoch`/`vmEpoch()` (process-monotonic, never 0).
  - `runtime/VMLite.h` / `runtime/VMLite.cpp` (L2 appends only):
    `gilOff` byte (+`offsetOfGilOff()` for U-T3), `State`
    (Live/Teardown/Collected/Detached, under-registry-lock-only),
    `ownerHasNoTlsDtor` (A36 r32), `clientHeap`, `entryScope`,
    `entryScopeServicesRawBits` (atomic; VM owns the packing),
    A16 segmented baked-buffer table (`scratchSegments`,
    `scratchBufferAtIndex` lock-free read, `ensureScratchBufferAtIndex`,
    `backfillBakedScratchBuffers`, `offsetOfScratchSegments()` for U-T4);
    process-wide `ScratchBufferRegistry` (rank OUTSIDE VMLiteRegistry::lock);
    carrier-TID hooks (`setCarrierTIDHooks`/`allocateCarrierTID`/
    `releaseCarrierTIDIfHooked`).
  - `runtime/JSLock.h` / `runtime/JSLock.cpp`: A36/A36C lazy per-(thread,VM)
    carriers — two TLS map slots (main thread: destructor-free plain
    thread_local, leaks accepted; non-main: ThreadSpecific whose dtor is the
    carrier-TLS-death path, U-T1 SKELETON: TEARDOWN-mark ->
    unregisterLite-LAST), epoch-before-carrier staleness check, the
    {lite, TID-tag, §10A.1 client slot} tuple swap at install and LIFO
    restore, `m_didInstallCarrierVMLite`/`m_entryThreadClient`,
    `uninstallVMLiteForVMDestruction` carrier arm. §A.1.4: the L7
    RELEASE_ASSERT now reads through the mode-split accessor (GIL-on: VM
    slot; GIL-off: the carrier lite's slot).
  - `runtime/VMEntryScope.cpp`: §A.1.5 per-lite entry record in
    setUpSlow/tearDownSlow (plus the transitional VM-member shadow, below).
  - `heap/Heap.h` / `heap/Heap.cpp`: `Heap::tryDesignateStickySharedServer()`
    (U0c designation primitive; CAS only, no assert); `friend class VM`
    (eager noteSharedServerSticky from the winner ctor);
    `friend class JSC::JSLock` on GCClient::Heap (A36C client-slot
    re-stamp); the r6-F5 per-VM registry root walk in the Msr/VMExceptions
    constraint (m_terminationException stays VM-global, both modes); the
    §10D `m_isSharedServer=false` arm conditioned on `!gilOffProcess`
    (pollIssRevertIfNeeded early-out + hint disarm).

- U-T1 AMENDMENT (post-review) — (a) JSLock stale-epoch carrier eviction
  rewritten onto the skeleton protocol (evictStaleCarrier; see obligation 4's
  eviction-path ownership rule) — closes a release-build registry UAF + I17
  TID leak; (b) willReleaseLock's depth-0 topCallFrame guard rerouted through
  group3Primitives() (table (iv) row); (c) GIL-off RELEASE_ASSERT tripwires
  on the VMEntryScope transitional shadow (obligation 1 HARD GATE); (d)
  obligation 9a added (exception-scope verification state, K4 table I);
  (e) ledger row 3 + obligation 5 ownership corrected (U-T8 / U-T8b per the
  handout §T task list).

- U-T4b (§A.1.3/6 FTL + OSR emission, DARK) — LANDED in-tree. All gilOff
  arms are codegen-time keyed on the COMPILED-FOR VM's `vm.gilOff()` (per
  §A.1.3; U0c fixes the mode pre-codegen); GIL-on/flag-off emission is
  byte-for-byte unchanged at every touched site (explicit dual arms — no
  shared restructuring of the GIL-on B3/assembly shapes). Files:
  - `ftl/FTLSaveRestore.{h,cpp}`: `materializeBakedScratchBufferPointer` /
    `...DataPointer` (the frozen A16 `loadVMLite -> segment -> [index]`
    emitters; address-dependent loads, clobber only dest), ScopedLambda
    base-materializer forms of save/restoreAllRegisters, and
    `restoreCalleeSavesFromCurrentVMLiteEntryFrameCalleeSavesBuffer`.
  - `ftl/FTLLowerDFGToB3.cpp`: per-site dual emission for ALL nine
    scratchBufferForSize main-path sites (define-fields, 3x ArrayPush,
    splice, unshift, NewArray, NewArrayWithSpread) via a per-site
    patchpoint (`bakedScratchBufferPointer`); JITCode-RESIDENT
    catchOSREntryBuffer set-site + ExtractCatchLocal/ClearCatchLocals +
    ExtractOSREntryLocal read-backs via baked indices; A16-ext gilOff
    fallbacks (see OPEN below) for ArithRandom (operationRandom call) and
    HasOwnProperty (probe skipped).
  - `ftl/FTLForOSREntryJITCode.{h,cpp}` (m_entryBuffer -> baked index),
    `ftl/FTLOSREntry.cpp` + `dfg/DFGOSREntry.cpp` (per-lite fill/readback;
    branch on bakedIndex != UINT_MAX so the same reader serves the future
    U-T4a DFG-tier index), `dfg/DFGCommonData.h`
    (catchOSREntryBufferBakedIndex append).
  - `ftl/FTLOSRExitCompiler.cpp`: full ExitScratchAddressing dual-mode
    compileStub/compileRecovery (every baked absolute — register dump,
    materialization pointer/argument slots, unwind scratch, activeLength,
    checkpoint tmps — becomes lite-resolved base + static offset,
    rematerialized per use); per-lite Group-3 unwind-entry resolution
    (callFrameForCatch/topEntryFrame); gilOff once-only exit compilation
    under a new `ftlOSRExitGenerationLock`. LOCK RANK (corrected — this lock
    is NOT a leaf, it is held across the whole of compileStub): OUTERMOST of
    {codeBlock->m_lock (ConcurrentJSLocker, getArrayProfile),
    ScratchBufferRegistry::m_lock -> VMLiteRegistry lock -> per-lite
    scratchBufferLock (via VM::allocateBakedScratchBufferIndex, the §LK.6
    chain), LinkBuffer/executable-allocator locks}; acquired ONLY from the
    exit-generation thunk's operation call with no other JSC lock held; must
    never be taken while holding any of the above; GIL-on never takes it.
    Sibling with the same rank, never nested with it:
    `ftlLazySlowPathGenerationLock` (FTLOperations.cpp). The U20 lock-order
    lint owner must carry both as an explicit outer-rank row (§LK side is
    frozen; this IU row is the binding rank record until then). gilOff, the
    winner does NOT repatch the exit jump (other mutators may be executing
    it; x86_64 rel32 repatch is not single-copy atomic and ISB1 only covers
    in-stop patching) — the jump stays on the generation thunk and every
    subsequent exit takes the locked fast path, returning the compiled stub
    for the thunk's tail call (data-only protocol; GIL-on repatch unchanged).
  - `ftl/FTLOperations.cpp` + `ftl/FTLLazySlowPath.cpp`: same once-only +
    no-live-repatch treatment for FTL lazy slow paths.
    operationCompileFTLLazySlowPath gilOff takes
    `ftlLazySlowPathGenerationLock`, generates only if `!stub()` (generate()
    RELEASE_ASSERTs !m_stub — N threads racing one unpatched jump would
    otherwise crash or double-publish m_stub/double-repatch), losers return
    the winner's stub; LazySlowPath::generate skips the repatchJump under
    gilOff (thunk round-trip per traversal, data-only). GIL-on both paths
    byte-for-byte unchanged.
  - `ftl/FTLLocation.{h,cpp}`: base-GPR `restoreInto` overload.
  - `ftl/FTLThunks.cpp` + `dfg/DFGOSRExitCompilerCommon.h`
    (adjustFrameAndStackInOSRExitCompilerThunk; signature widened
    MacroAssembler& -> AssemblyHelpers&, both callers are AssemblyHelpers):
    per-lite register-dump buffers + lite-resolved callFrameForCatch in the
    shared generation thunks.
  - `dfg/DFGOSRExitCompilerCommon.cpp` (adjustAndJumpToTarget exception
    tail, shared by DFG+FTL exits): lite-resolved
    targetInterpreterPCForThrow (payload-only variant store, M6-identical
    layout), topEntryFrame callee-save copy, callFrameForCatch.
  - Amplifier: `JSTests/threads/jit/ftl-osr-entry-catch-loop-amplifier.js`
    (concurrent catch + loop OSR entry, one CodeBlock; passes GIL'd).
  U-T4b OPEN items (owners named):
  1. **A16-ext lite-resident copies NOT landed** (no U-T1 L2 lite slots, no
     K.3 publish): ArithRandom gilOff falls back to operationRandom (JIT-side
     tear removed; the HOST-side shared WeakRandom advance remains the
     K4.VIII.10 runtime row — owner of the per-lite stream: the K-rows
     runtime task, with U-T4a/U-T4b re-pointing emission once the slot
     exists); HasOwnProperty gilOff skips the inline probe (conservative);
     RecordRegExpCachedResult gilOff FAIL-STOPS at codegen (DFG_CRASH in the
     gilOff arm of compileRecordRegExpCachedResult — the shared
     m_regExpGlobalData cachedResult is SEMANTIC state with no safe fallback,
     so the activation gate is enforced in code, same precedent as the
     Darwin loadVMLite RELEASE_ASSERT; GIL-on emission byte-for-byte
     unchanged). **gilOff activation (U-T6/U-T9) MUST NOT ship before the
     lite slots + re-pointed emission land** — for RecordRegExpCachedResult
     this is now mechanical (FTL compiles of the node crash under gilOff
     until the K4/U-T8b lite-resident copy lands and the tripwire is
     replaced by the re-pointed emission). Megamorphic FTL
     paths bake no VM cache address in FTLLowerDFGToB3 (they run through the
     shared InlineCacheCompiler machinery — that surface's A16-ext leg
     belongs to its owning task, not U-T4b).
  2. **DFG/Baseline legs untouched** (U-T4a boundary): DFGJITCompiler.cpp:527
     catch-buffer set-site, DFGSpeculativeJIT.cpp:17805/17812 catch
     readbacks, DFG OSR exit compiler scratch buffers, and every
     Baseline/DFG scratch bake still GIL-on-only. The shared read-side
     (DFGOSREntry/DFGCommonData index) is already in place for them.
  3. **Darwin loadVMLite gap inherited** (U-T3 OPEN): gilOff-mode emission
     RELEASE_ASSERTs on Darwin until the JSCConfig vmLiteTLSKey lands.
  4. handleExitCounts' CodeBlock-resident exit counters remain non-atomic
     cross-thread increments under gilOff (profiling-only; CodeBlock/jit
     rows own the ruling).

## (i) Supersession ledger (one row per SPEC-ungil SUPERSESSION; spec side already written, IU side written at landing)

| # | Spec side | IU side (landing record) | Task |
|---|-----------|--------------------------|------|
| 1 | r6 F5 — Heap.cpp:3585-class VMExceptions roots via VM accessors vs per-lite registry walk | LANDED: Heap.cpp Msr/VMExceptions constraint branches on vm.gilOff(); per-VM filter; m_terminationException VM-global both modes | U-T1 |
| 2 | §A.1.4 — JSLock.cpp:166 L7 RELEASE_ASSERT GIL-on-only; GIL-off asserts the LITE's slot empty | LANDED: one line serves both via the mode-split accessor (comment at the assert) | U-T1 |
| 3 | A36/§J.7 — JSLock.cpp:151 backstop (`!useJSThreads \|\| useThreadGIL`) | PARTIAL: re-keyed per-VM (`RELEASE_ASSERT(!m_vm->gilOff())` on the main-carrier install path; gilOff VMs take the carrier branch). The full §J.7 U1 replacement (TLS-tag equality + the A36C client check at the backstop) is a **U-T8 deliverable** per the handout §T task list ("J.7 replacement"); U-T5/U-T6 land only its prerequisites (stub deletion; per-thread clients) | U-T1 → U-T8 (prereqs U-T5/U-T6) |
| 4 | A16 vs vmstate:534-539 — VMLite::scratchBufferForSize reserve re-frozen as the GIL-off non-baked path; Group 5 repurposed as ownership list | LANDED: VM::scratchBufferForSize gilOff dispatch; ensureScratchBufferAtIndex appends to the ownership list; gatherScratchBufferRoots registry scan (jit R2) | U-T1 |
| 5 | §LK.6 re-rank vs vmstate §6.5.1/§7 — SBR -> VMLiteRegistry::lock -> scratchBufferLock LEGAL | LANDED: install fan + GC scan take scratchBufferLock under the registry lock; rank note on ScratchBufferRegistry | U-T1 |
| 6 | U0c vs heap §5.1 sticky trigger / §10D — designation CAS primitive; :4755 arm !gilOffProcess; HeapClientSet::add stays idempotent | LANDED except the HeapClientSet.cpp:69 RELEASE_ASSERT(gilOffProcess => server VM m_gilOff==1) — **OBLIGATION (U-T3 per handout task split; file outside U-T1's set)** | U-T1/U-T3 |
| 7 | A36 r9 F4 TID supersessions (vmstate §6.7 tid-0 GIL-on-only; carrier TIDs from the 2^15 TM space) | PARTIAL: carrier creation consumes the hook pair; **OBLIGATION: ThreadManager must register a carrier-TID allocator (I17 accounting incl. carriers) at initialization — until then GIL-off first entry RELEASE_ASSERTs** | U-T1 → api/U-T6 |
| 8 | heap §10A.1 re-stamp clause (SPEC-heap.md:281-283) vs §B.3 + A36C | PARTIAL: GIL-off install/restore re-stamp landed (tuple swap); GIL-on forwarding + re-stamp UNCHANGED; the {client, epoch} staleness upgrade of the §10A.1 slot itself is U-T6's | U-T1 → U-T6 |
| 9-… | (rows added as later tasks land their supersessions: §A.3.7 atom asserts, §B.3, SB1 vs EXIT1, M6/§6.5.1 vs A36 ~VM, heap §13.5 vs §A.3.8, DAL2, EC1, …) | — | U-T2…U-T14 |

## (ii) §F.2 predicate-consumer table (~60 rows: assert / BRANCH / EXCLUSIVITY CONSUMER) — U-T8

SKELETON. Columns: consumer site | predicate form consumed | class
(assert/branch/exclusivity) | GIL-off ruling (annex F2 fixed rulings;
~AsyncTicket/finalizer rows) | landed-at.

| Site | Form | Class | Ruling | Landed |
|------|------|-------|--------|--------|
| (U-T8 fills ~60 rows) | | | | |

## (iii) §E.4 settle-site lock-context table — U-T8

SKELETON. Columns: settle site | lock context at settle | routing
(same-thread / cross-thread ticket) | retirement rule (r17 F2 / r18 F2).

| Settle site | Lock context | Routing | Retirement |
|-------------|--------------|---------|------------|
| (U-T8 fills) | | | |

## (iv) §A.1.7 off-thread-reader table (per rerouted Group-3 field) — U-T8d

SKELETON. Columns: rerouted field | off-thread reader | disposition
(i) registry-resolve target lite (suspended, SUSPEND RULE r24) /
(ii) refused GIL-off with defined error / (iii) proven on-thread.

Seed rows from U-T1 (dispositions to be ruled by U-T8d):
| Field | Reader | Disposition |
|-------|--------|-------------|
| topCallFrame | SamplingProfiler.cpp:391-431 (suspends target) | (i) carrier-only v1 (AUD1.K1/SD18) — U-T8d |
| topCallFrame | JSLock.cpp willReleaseLock depth-0 clearLastException guard | RESOLVED at U-T1 amendment: routed through group3Primitives() (on-thread; carrier still installed at the read) — (iii) |
| m_exception / m_lastException | Heap.cpp Msr/VMExceptions (GC visit thread) | RESOLVED at U-T1: registry walk, per-VM filter (r6 F5) — not via accessors |
| exceptionForInspection() | inspector/debugger | U-T8d (currently routes to CURRENT lite when gilOff; off-thread use must go (i)/(ii)) |
| VM-block fallback reads (group3Primitives with no lite) | compiler threads via C++ helpers | U-T8d enumerates; dark-safe today |
| (U-T8d fills the rest per field) | | |

## (v) §E.1b.4 host-hook disposition table — U-T8e

SKELETON. Columns: globalObjectMethodTable / host-callback slot |
JS-reachable on spawned TS? | disposition {inline, carrier-queued, refused,
unreachable} | SD15 tracker handoff notes.

| Hook slot | Spawned-reachable | Disposition | Notes |
|-----------|-------------------|-------------|-------|
| (U-T8e fills) | | | |

## (vi) §F.6 embedder checklist (incl. (d) construction-order and (e) spawned-no-foreign-VM audits) — U-T8

SKELETON.
- (a) m_lock excludes only embedder threads — pending U-T8 (with §B.3
  supersession at U-T6).
- (b) SD10 continuation-affinity sign-off — recorded r21 (ALS slice by ALS1).
- (c) blocking-site enumeration (Bun) — pending U-T8 (§F.6 delta (c)).
- (d) construction-order audit — pending U-T8.
- (e) spawned-no-foreign-VM audit — RELEASE_ASSERT mechanism specified
  (TERM1.5/§F.5; message names §F.6(e)); landing at U-T6; death-test arm
  U-T6/U27.

## (vii) Per-row call-site enumerations deferred by annex K4/N7 rows — U-T8b (+owners named in rows)

SKELETON. One subsection per K4/N7 row that defers an enumeration to IU;
U-T8b (and §N owners U-T13 etc.) fill them. The implementation CONSUMES the
EXECUTED K4/N7 tables verbatim — these subsections add call sites only.

## U-T1 obligations / deferred refinements (tracked; each names its owner)

1. **Raw Group-3 / entry-record C++ consumers.** `vm.topCallFrame`,
   `vm.entryScope` (VMEntryScopeInlines.h ctor/dtor fast path, CallFrame.cpp,
   VMTraps.cpp:80/:497, SamplingProfiler.cpp:375/:786, Debugger.cpp:203,
   JSDollarVM.cpp:4339-4343, Heap.cpp:1343) read members directly and are
   correct GIL-off only because VMEntryScope::setUpSlow/tearDownSlow keeps a
   transitional VM-member SHADOW (see VMEntryScope.cpp comment). The
   activation tasks (U-T5/U-T9 for entry; U-T3/U-T4 emission + U-T8d for
   topCallFrame-class fields) must re-point or rule each site, then drop the
   shadow. GIL-off top-level-entry detection in VMEntryScopeInlines.h MUST
   key on the CURRENT LITE's record before N-mutator entry goes live —
   concurrent first entries would otherwise skip setUpSlow on all but one
   thread. **HARD GATE (U-T1 amendment): "shadow dropped + every raw
   consumer re-pointed" is a precondition of N-mutator entry; until it is
   discharged, RELEASE_ASSERT tripwires in VMEntryScope::setUpSlow/
   tearDownSlow fail-stop any second concurrent GIL-off top-level entry
   instead of letting the shadow race (last-writer-wins / early-null).**
   Rerouted at the U-T1 amendment (no longer raw): JSLock.cpp
   willReleaseLock's depth-0 topCallFrame guard now reads
   group3Primitives() — see table (iv).
2. **Entry-scope service VM-level word retirement.** GIL-off, transient
   VM-wide bits remain set on the VM-level word after every lite serviced
   its copy (the word is the backfill source); a late-registered lite
   re-observes them. Harmless for the current service set
   (FirePrimitiveGigacage re-fire is idempotent; PopListeners list is
   drained under the lock-holder), but U-T2's rule-3 trap fan-out subsumes
   this protocol and must ratify or replace the retirement rule.
3. **Carrier-TID provider.** ThreadManager registers
   `JSC::setCarrierTIDHooks` at initialization (api workstream / U-T6);
   accounting per I17 (carriers count vs 2^15; release on carrier teardown —
   the U-T1 TLS-death skeleton already calls the release hook).
4. **Carrier teardown protocol.** The U-T1 TLS-death path is a SKELETON
   (TEARDOWN mark -> unregisterLite LAST). U-T6 replaces it with the full
   r31/r32 EXIT1.3/EXIT1.9/A36 machinery (COLLECTED/DETACHED, ~VM walk +
   completion fence, deferred degenerate dtor, no-op M12 removal). ~VM's
   existing "no other registered lite points at this VM" assert will fire
   with live carriers until then — acceptable while dark.
   **Eviction-path ownership rule (U-T1 amendment):** the A36 stale-epoch
   eviction arm (JSLock.cpp evictStaleCarrier) runs the SAME skeleton —
   still-registered stale lite: TEARDOWN mark under the registry lock ->
   unregisterLite LAST -> releaseCarrierTIDIfHooked -> owner free; a map
   entry NEVER bare-deletes its lite. Once U-T6's ~VM collection walk
   lands, an UNregistered stale bit-SET (main-thread) lite was walk-freed
   (A36 r32) and the map FORGETS the pointer without touching it — the
   walk and the map can never both free; the unregistered bit-CLEAR arm
   RELEASE_ASSERTs until U-T6 lands the state-keyed deferred degenerate
   dtor there.
5. **Per-lite regexp members.** lite->executingRegExp / regExpAllocator
   exist (Phase A Group 4); the VM-side consumers (VM::m_executingRegExp,
   m_regExpAllocator/m_regExpAllocatorLock; RegExpInlines.h call sites) are
   NOT yet mode-split — owner: **U-T8b** (which consumes the K4 table I
   regexp row and lands AUD1.N2 / RegExp::m_ovector FIRST — both marked
   memory-unsafe-today in the handout), with U-T3/U-T4 covering only any
   emission-side leg. The corresponding r6 F5 root-walk extension for lazy
   regexp match buffers lands with the U-T8b reroute. Enumerated here when
   landed.
6. **m_microtaskQueue Group-3 slice** — §E owns the reroute (U-T9); not
   touched by U-T1.
7. **VMLite facility test gate.** INTEGRATE-vmstate.md's PENDING C++-test
   item (owner-only enqueue/drain, scratch growth, dtor-free-under-ASAN)
   still gates ACTIVATION: U-T1 added dark callers
   (VM::scratchBufferForSize gilOff dispatch, A16 installs) under the
   handout's authority; the test must land before any gate runs GIL-off.
8. **Pending corpus/amplifier arms (U-T1-named, runnable at activation):**
   (a) thrower parked pre-catch survives a forced full collection (per-lite
   exception roots); (b) two-VM root-walk arm (gilOff VM + GIL-on VM
   collected concurrently; per-VM filter); (c) U0b/U0c corpus —
   compile-heavy run THEN first spawn; two VMs constructed under
   gilOffProcess (loser ctor completes, loser spawn RangeErrors, loser
   embedder entry executes JS); (d) A36C two-VM alternating-entry and
   nested re-stamp arms (U-T6/U27 own the harness).
9a. **Exception-scope verification state per-lite (K4 table I).**
   `m_needExceptionCheck` / `m_throwingThread` (+ the rest of the
   ENABLE(EXCEPTION_SCOPE_VERIFICATION) block) are classed PER-LITE by the
   EXECUTED audit (K4 table I row 2: vmstate I15 — throw state is
   thread-local) but remain VM-global: their raw writers live outside
   U-T1's file set (ThrowScope.cpp:56/:95, LockObject.cpp:74-98,
   Interpreter.cpp:968). Owner: **U-T8b**, as a debug-only L2 VMLite tail
   append (NOT part of the frozen VMLitePrimitives ABI) serviced alongside
   its K4 consumption — or an explicit gilOff-debug-only ruling recorded
   here. Until then EXCEPTION_SCOPE_VERIFICATION builds are not
   N-mutator-safe GIL-off (dark today; comment at the VM.h block).
9b. **gilOffProcess Config byte + LLInt level-2 selection** — U-T3
   (JSCConfig.h:106; loadVMLite emitter; VMEntryRecord::m_vmLite). U-T1's
   `VM::isGILOffProcess()` is the C++ twin (see (i) row 6/ledger row 1
   note); U-T14 re-audits the flag-off branch budget against jit I1's
   permitted-delta list.
