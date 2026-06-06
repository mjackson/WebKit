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

- U-T10 (§C.1-2 atomic slot accessors) — LANDED in-tree. Files (U-T10-owned
  edits): `runtime/JSObject.h` (AtomicSlotOperation/AtomicSlotStatus/
  AtomicSlotRequest + the four §9.5 entry points on JSObject:
  `atomicSlotReadModifyWrite`/`atomicSlotCompareExchange` + the AtIndex pair),
  `runtime/ConcurrentButterfly.cpp` (ANNEX C1 implementation: lock-free
  inline/flat-OOL/segmented-fragment seq_cst 64-bit slot CAS/RMW loop; the
  flat-path SW discipline — ensureSharedWriteBit FIRST, I34 structureID +
  butterfly re-validation, THEN the slot CAS, Restart on validation failure,
  completed CAS never re-applied; the OM-locked third arm for dictionary/
  AS-shape with the AS PRE-LOCK SW protocol (r8 item 6) and under-lock
  dictionary-ness/offset/D7 re-checks; the indexed-by-shape arm — CoW
  materialize-first §4.8/I35, Int32/Double convert-to-Contiguous on FIRST
  atomic access (owner direct, foreign SW-set DCAS first), Contiguous = flat
  arm, AS/dict-indexed = third arm; write barrier after success),
  `runtime/ThreadAtomics.cpp` (§C.2 re-home: gilOff-only \*GilOff bodies for
  load/store/compareExchange/RMW dispatching through the §9.5 accessors with
  the probe/Restart loop; D3 receiver gates + messages and D7 writability
  carried verbatim; rope SVZ operands resolved OUTSIDE any lock via JSString
  resolution (§N.2) then re-probed; store's Missing arm stays on the generic
  OM add path). GIL-on/flag-off: byte-identical bodies (every new branch is
  `vm.gilOff()`-gated; ThreadAtomics.h signatures untouched per the frozen §7
  list). Corpus: `JSTests/threads/atomics/property-cas-storm-u5-as.js` (ANNEX
  C1 U5 amplifier: owner unlocked AS store storm vs foreign CAS, same index,
  SW initially 0 + exact locked-counter arm),
  `property-cas-storm-u28-flat.js` (U28-class lock-free-arm exactness storm:
  inline/OOL/Int32-converting/Double-converting/RMW + rope-SVZ contention),
  `property-cas-dictionary-delete-u5.js` (U5 dictionary arm: delete/re-add
  storm vs foreign CAS; no quarantined-slot resurrection).
  **ENTRY GATE record (§D.2): NOT SATISFIED — DEFERRED.** The OM Task-14
  PRE-INT bench verdict is NOT recorded anywhere in-tree
  (INTEGRATE-objectmodel §46 still instructs "record the verdict here"; no
  PROMOTE record exists), so the §D.2 HARD precondition of U-T10 ENTRY did
  not hold when this task landed. This task cannot run the jit Task-13
  GIL-stub construction bench (docs+code round, no builds), so U-T10
  proceeded on the no-PROMOTE arm — 8h ships as landed (cell-locked N2), which
  is the only arm consistent with §C's third arm as frozen. This is a GATE
  DEFERRAL, not gate satisfaction. OBLIGATION (orchestrator, HARD, before
  **U-T11 ENTRY** — U-T11 is the first consumer of these accessors (§C.3(a)
  routes the PWT pre-enqueue load through them), so "before U-T14 close" is
  too late: run the §L2.h bench, record the verdict in INTEGRATE-objectmodel
  §46, AND record an explicit gate-deferral ruling in SPEC-ungil-history.md
  (not a U-T10-owned file — the U-T14 close audit must not count this as an
  unrecorded supersession); a PROMOTE verdict retroactively requires landing
  Task 14 and re-reviewing §C's third arm AND the amend-round locked
  undefined-disambiguation arm (this file's locked-arm code is the surface
  to re-review).
  OPEN (owned by U-T11 per the task split): §C.3 PWT pre-enqueue routing —
  atomicsWaitOnProperty/atomicsWaitAsyncOnProperty now reach the §9.5 atomic
  load via atomicsLoadOnProperty's gilOff branch (forcing conversions outside
  listLock, as §C.3(a) requires), but the under-listLock SVZ re-validation +
  dequeue-and-restart arm and the 4.5-1a/G11 gate edits are U-T11's.

  **AMEND round (adversarial review, 2 reviewers) — LANDED:**
  1. **U5 D1-sentinel hardening (blocker, CONFIRMED).** The lock-free
     CAS/RMW loop validated {offset, structureID} provenance only at
     entry/I34 time; flag-on named deletes D1-store jsUndefined into the
     doomed slot BEFORE the structure publication (I30,
     storeUndefinedIntoDoomedSlotConcurrent) and never touch the butterfly
     word, so a loop iteration could read the quarantine sentinel through an
     in-flight delete (flat→dictionary convert + dictionary delete, AND the
     broader case the review missed: a plain non-dictionary delete, whose
     sentinel lands before its header CAS) — a CompareExchangeSVZ with
     expected===undefined would Apply on an ABSENT property (U5) and
     Load/failed-CAS would surface impossible undefined reads. Fix
     (ConcurrentButterfly.cpp): (a) atomicSlotLockFreeLoop re-validates
     structureID every iteration, between the seq_cst slot load and the CAS;
     (b) because the ID check alone cannot close the non-dictionary delete's
     pre-publication window, named-slot jsUndefined reads bounce out as the
     internal LockedRevalidate status and are disambiguated under the cell
     lock (both delete flavors hold it across their whole sentinel-store →
     publication window, §6 L4; the write stays a seq_cst slot CAS — the
     lock excludes deleters, not other CASers); (c) the indexed Contiguous
     call site pins a (nuke-checked) dispatch-time structureID
     (revalidateUndefined=false there — indexed deletes/holes are empty
     JSValue()s, caught by the !current restart, as the review itself
     notes). Corpus: `property-cas-delete-undefined-sentinel-u5.js`.
  2. **Probe accessor-attribute gap (blocker, CONFIRMED).**
     probeOwnPropertyForAtomicsConcurrent decided accessor-ness from the
     methodTable walk (structure S0) but recorded {offset, structureID}
     provenance from a RE-READ structure S1 without re-checking S1's
     attributes — a racing data→accessor reconfiguration between the reads
     handed the lock-free arm a kind=Data probe whose I34 check passes while
     the slot holds a GetterSetter (CAS-a-primitive-over-a-cell type
     confusion). Fix (ThreadAtomics.cpp): the probe now rejects
     Accessor|CustomAccessor|CustomValue against the SAME structure the
     provenance is taken from (kind=Accessor ⇒ existing D3 TypeErrors),
     mirroring the third arm's under-lock re-check; also closes a probe⇄
     third-arm livelock for CustomValue slots that answer slot.isValue().
  3. **Missing-arm TOCTOU (major, CONFIRMED for named adds; both reviewers).**
     probe(Missing) → isExtensible → putDirectMayBeIndex was three steps;
     a racing defineProperty(accessor / non-writable) or preventExtensions
     between probe and put was silently clobbered/overtaken (putDirect's
     define-own attribute-change transition to attributes 0). Fix: named
     adds route through JSObject::putDirectForAtomicsMissingAdd
     (ConcurrentButterfly.cpp; PutModePut through putDirectInternal's
     flag-on §2 loop — existence/extensibility re-derived in the SAME
     iteration whose E4 structureID-CAS publishes the add, so racing
     defines/preventExtensions fail the publication and return a non-null
     error; the GilOff store body then RESTARTS and the fresh probe throws
     the precise D3/D7/non-extensible TypeError; a racing plain writable
     data add is absorbed as a value-only, attribute-preserving replace =
     define-then-store linearization). Corpus:
     `property-store-missing-define-race.js`. **KNOWN RESIDUAL (OPEN):** the
     INDEXED Missing add stays on putDirectIndex verbatim — a racing indexed
     defineProperty forcing a sparse-map/SlowPutAS conversion cannot be made
     conditional without new OM machinery; recorded here so U-T14's close
     audit sees it (the GIL-on body has the identical shape, so flag-off/
     GIL-on behavior is unchanged).
  4. **ENTRY GATE finding (major, PARTIALLY CONFIRMED):** the claim that this
     record "presents the gate as satisfied" was FALSE (the original entry
     already recorded the missing verdict and the no-PROMOTE arm); CONFIRMED
     that the obligation was mis-timed — re-scoped above from "before U-T14
     close" to HARD before U-T11 ENTRY, with the SPEC-ungil-history
     deferral-ruling requirement added.
  GIL-on/flag-off after the amend: still byte-identical — every touched
  body is reached only via the vm.gilOff() dispatch or
  Options::useJSThreads() accessors; putDirectForAtomicsMissingAdd is
  called only from the GilOff store body; AtomicSlotStatus gains the
  accessor-internal LockedRevalidate value (never escapes to callers).

- U-T14 (CLOSE) — LANDED in-tree. Code writes: `runtime/OptionsList.h`,
  `runtime/Options.cpp`; ledger writes: this file. Runs LAST per the DAG.

  1. **DEFAULT FLIP (handout §T U-T14).** `Options::useThreadGIL` default
     true -> false (OptionsList.h), description updated. No shipping default
     configuration changes AT ALL: the option is only consulted under
     `useJSThreads` (every in-tree consumer is the paired
     `useJSThreads() && !useThreadGIL()` / `!useJSThreads() || useThreadGIL()`
     form — ArrayBuffer.cpp:180, JSLock.cpp:1237, SamplingProfiler.h:357,
     VMInspector.cpp:95, Watchdog.h, Debugger.cpp, VM.cpp:2648), and
     `useJSThreads` defaults false. Flag-off codegen and behavior:
     byte-identical. The U19 GIL-on oracle remains reachable via explicit
     `--useThreadGIL=1`.
  2. **U0 option-validation gate (LANDED HERE — it had NOT landed).** The
     VM.cpp:2646 and JSLock.cpp:1235 comments asserted "U0 option validation
     refuses GIL-off without the trio upstream", but no such check existed
     anywhere (grep useThreadGIL: zero hits in Options.cpp pre-close). With
     the default flip that gap would have made plain
     `--useJSThreads=1 --useSharedGCHeap=1`-less configs... still GIL'd only
     by accident of the trio terms in isGILOffProcess(), while the
     SHORT-FORM derivations (ArrayBuffer.cpp:180, VMInspector.cpp:95,
     SamplingProfiler.h:357 — `useJSThreads && !useThreadGIL` WITHOUT trio
     terms) would have DIVERGED from isGILOffProcess() (mixed-mode
     inconsistency). Landed in Options::notifyOptionsChanged immediately
     after the M_opts2 trio normalization: GIL-off without
     {useVMLite, useSharedAtomStringTable, useSharedGCHeap} forces
     useThreadGIL=1 (ANNEX U0C wording: "refused at option validation
     (forced useThreadGIL=1)"). This makes every short-form derivation
     equivalent to VM::isGILOffProcess() again. Flag-off: branch
     unreachable. Explicit useThreadGIL=1 never enters it (U19 unaffected).
  3. **Flag-off delta (a)/(b)/(b2) re-audit — EXECUTED (static).**
     - (a) LLInt Group-3 gilOffProcess branches: **ZERO in tree** (grep
       llint/ + offlineasm/ for gilOff|vmLite|loadVMLite: no matches; the
       only LLInt thread branches are the PRE-ungil R1.e
       useJSThreads/ifJSThreadsBranch set, LowLevelInterpreter64.asm:1609).
       The delta-(a) budget is UNCONSUMED and no LLInt re-baseline was ever
       consumed — flag-off LLInt codegen is trivially byte-identical.
       Consequence recorded as ACTIVATION BLOCKER AB-1 below.
     - (b) atomicsWaitImpl's useJSThreads branch present
       (AtomicsObject.cpp:514ff consumers at :536/:603/:657/:748) —
       sanctioned, unchanged.
     - (b2) §N.5 twin intrinsics
       (@atomicInternalFieldClaim/@atomicInternalFieldPublish): **NOT
       landed** (zero matches repo-wide incl. builtins/ and bytecompiler/).
       Delta (b2) unconsumed; its golden re-baseline never consumed; the
       §N.5 flag-off microbench gate is vacuous. **r17 F5 rule ("no host-op
       call reachable gilOffProcess=false") — VACUOUSLY SATISFIED** (the
       mode-keyed lowering does not exist; nothing emits the gilOff host-op
       arm). Recorded as part of AB-8 (U-T13 §N.5 leg absent).
     - No OTHER flag-off codegen/bytecode-shape delta found: builtins/,
       bytecompiler/, bytecode/ carry zero gilOff tokens; all ungil C++
       branches are vm.gilOff()/VM::isGILOffProcess()-keyed host code (not
       codegen shape); JIT-side emission (AssemblyHelpers loadVMLite, the
       U-T4b FTL dual arms) is reached only from gilOff-mode compilation
       per the U-T4a/U-T4b records. **VERDICT: permitted-delta-list
       compliance PASS statically; byte-identity still requires the
       golden-disasm gate at Build (no builds this round).**
  4. **U0/U0b/U0c gate mechanisms — verified PRESENT in-tree.** U0 = item 2
     + the ctor designation CAS (VM.cpp:392-401 ->
     Heap::tryDesignateStickySharedServer, Heap.cpp:4166) +
     verifyStickySharedServerDesignation (VM.cpp). U0b = loser spawn
     refusal (ThreadManager.cpp:336-358, RangeError; fail-safe
     over-refusal arm recorded there) + the loser main-carrier install
     escape (JSLock.cpp:1237). U0c = ctor-top immutable m_gilOff + eager
     winner noteSharedServerSticky at clientSet()==1. The U0b/U0c CORPUS
     arms (obligations item 8(c): two-VM construction under gilOffProcess,
     loser spawn RangeError, loser embedder entry EXECUTES JS,
     compile-heavy-then-first-spawn) — EXECUTION DEFERRED TO BUILD.
  5. **U19 full oracle / TSAN / amplifier battery — DEFERRED TO BUILD**
     (this round is no-build by orchestration). Exact configs owed: U19 =
     full corpus at {useJSThreads=1, useThreadGIL=1} green and UNCHANGED
     except SD6/SD7; all GIL-off-only SDs via //@ runThreadsGILOff/GILOn
     with OLD expectations GIL-on; TERM1 VM-wide terminate arms. Battery =
     the per-task standing arms (handout PER-TASK GATE LIST + EXIT1.8
     r28-r31 arms, SB1.6/ISB1.6/§N.5 arm64-hardware arms) plus the
     U-T12-recorded deferred amplifiers. These are RECORDED AS DEFERRED,
     NOT AS PASSES.
  6. **U20 lint close.** No automated lock-order lint tool exists in tree
     (Tools/Scripts has none; recorded residual, owner: Build/CI phase).
     Manual close pass over the named rule set: outer-rank sibling rows
     ftlOSRExitGenerationLock / ftlLazySlowPathGenerationLock recorded
     (this file, U-T4b entry — the binding rank record); the §E.4
     settle/wake-under-rank-3 table (JSLock.cpp:925-940) — all frozen rows
     COMPLIANT as landed; job-slot / LZ2.5 / WS1.4 / SB1.6 rule markers
     present at their owning sites (VMLite.cpp, VMManager.cpp normative
     contract blocks). This is a marker-presence pass, not a proof; the
     automated lint remains the discharge vehicle.
  7. **IU disposition completeness — scan executed.** Tables (ii)-(v) were
     EXECUTED IN CODE as doc-of-record comment blocks rather than as rows
     here: (ii) §F.2 predicate split + consumer rulings — VM.cpp:271ff/
     :796/:998 + JSLock.cpp token machinery; (iii) §E.4 settle-site
     lock-context table — JSLock.cpp:925-940; (iv) §A.1.7 — seed rows here
     + SamplingProfiler.h:323-360 carrier-only capture record; (v)
     §E.1b.4 host-hook dispositions — JSGlobalObject.cpp:680ff (+ SD15
     tracker rows :4267/:4289). Those blocks are the binding records; the
     skeleton tables above now POINT at them instead of being unfilled
     obligations.
     **Residual OPEN list at close (activation blockers; each named):**
     - **AB-1 (BLOCKER): LLInt Group-3 emission absent.** No JSCConfig
       gilOffProcess byte (JSCConfig.h unmodified), no LLInt loadVMLite
       emitter, no delta-(a) storage-selection branches (obligation 9b,
       U-T3). Under gilOffProcess LLInt would read/write VM-block Group-3
       state while C++/JIT use per-lite storage — UNSOUND, and unlike the
       JIT tiers there is NO in-LLInt tripwire.
     - AB-2: Darwin loadVMLite vmLiteTLSKey absent — gilOff JIT compilation
       RELEASE_ASSERTs on Darwin (fail-stop, acceptable).
     - AB-3: A16-ext lite-resident copies (U-T4b OPEN 1) —
       RecordRegExpCachedResult gilOff FTL compile DFG_CRASHes (fail-stop);
       ArithRandom/HasOwnProperty conservative fallbacks live.
     - AB-4: DFG/Baseline scratch-bake legs untouched (U-T4a boundary).
     - AB-5: HeapClientSet.cpp:69 RELEASE_ASSERT not wired (ledger row 6;
       I13 inner-CAS interim backstop stands).
     - AB-6: indexed Missing-add residual (U-T10 amend item 3) — GIL-on
       shape identical; acknowledged per its own record.
     - AB-7: OM Task-14 bench verdict + §D.2 gate-deferral ruling still
       unrecorded in INTEGRATE-objectmodel §46 / SPEC-ungil-history.md
       (U-T10 obligation, re-scoped to before U-T11 ENTRY — U-T11 has
       landed, so this is now a RETROACTIVE bookkeeping breach to be
       discharged at Build; the no-PROMOTE arm remains the landed shape).
     - AB-8: §N.5 twin intrinsics + mode-keyed lowering absent (U-T13's
       §N.5 leg) — GIL-off concurrent generator/async resume keeps the
       UNSYNCHRONIZED landed plain sequences: a REAL race GIL-off, hidden
       only while AB-1 blocks activation anyway.
     *(GIL-removal review round — the following rows were verified missing
     from this list even though each is recorded somewhere in a code
     comment; the close ruling's safety argument is re-stated against the
     COMPLETE inventory below.)*
     - ~~AB-9: JSThreadsSafepoint gilOff reroute~~ **CLOSED by the review
       round**: stopTheWorldAndRun now routes gilOff Class-A fires to
       jsThreadsThreadGranularStopTheWorldAndRun (the §A.3.3 licensed
       edit), and JSThreadsSafepoint::worldIsStopped() gained the §J.8
       thread-granular disjunct. This also re-validates the §A.3.4 license
       for the U-T5 M7-tripwire deletion (VMEntryScope.cpp): gilOff stop
       requests now reach the protocol that replaced the premise; the
       gilOn stub keeps its sampled entered-VM tripwire unchanged.
     - **AB-10 (BLOCKER): haveABadTime K.5 Class-4 stop absent.** The
       JSGlobalObject::haveABadTime body must run under ONE §A.3
       thread-granular stop GIL-off (ANNEX HBT/HBT2-HBT4; the body
       allocates, so the default-conductor closure rules need the Class-4
       variant first). Interim: RELEASE_ASSERT(!vm.gilOff()) tripwire at
       entry (landed by the review round — fail-stop, no longer silent).
     - **AB-11: ThreadObject spawn-overload migration.** gilOffProcess
       refuses EVERY spawn including the winner VM's
       (ThreadManager.cpp allocateSpawnedThreadState VM-blind form returns
       null -> RangeError): fail-safe over-refusal, but "N mutators in
       parallel" is structurally unreachable until ThreadObject.cpp
       migrates to the VM-aware overload.
     - **AB-12: E2A drain-loop wiring + U-T9-INT1 keepalive edits.**
       openThreadInbox / runSpawnedThreadDrainLoopAndClose /
       AsyncTicket::armKeepalive have ZERO callers; until the threadMain
       wiring + the four countsKeepalive edits land, GIL-off threads exit
       at fn-return (api 4.6.2 fallback semantics, NOT the chartered
       SPEC-ungil E.2/SD1 close semantics) and the E/SD16/SD17 corpus arms
       cannot be claimed.
     - **AB-13: GILDroppedSection spawned-arm J.3 split + §G consumer
       re-points.** Every spawned GIL-off park site (contended lock.hold,
       cond.wait, property Atomics.wait, join) reaches
       unlockAllForThreadParking's RELEASE_ASSERT(currentThreadIsHolding-
       Lock()) and fail-stops; the mayBlockSynchronously §G predicate has
       zero consumers re-pointed (ThreadAtomics.cpp/LockObject.cpp/
       ConditionObject.cpp/AtomicsObject.cpp owed). The in-code ordering
       constraint stands: the §C.4 lift MUST NOT land before the split.
     - ~~AB-14: VMEntryScopeInlines.h entry-gate re-key; HandleSet Strong
       seam wiring~~ **CLOSED by the review round**: the ctor/dtor fast
       paths are re-keyed on the per-lite record when gilOff (nested and
       sequential re-entry work; tearDownSlow runs), and Strong.h/
       StrongInlines.h now route allocate/free/set-slot through the locked
       strongHandle* seams (HandleSet.h declarations added).
     - **AB-15: SD7 generated-code arm.** The review round landed the §I
       item (1) C++ ctor/compile-surface gate (JSWebAssemblyHelpers.h
       throwIfWebAssemblyRefusedOnSpawnedThread, wired at the Module/
       Memory/Table/Global/Tag/Instance constructors and compile/
       instantiate/validate/streaming/promising entry points, BOTH GIL
       modes per SD7). Item (2) remains open: VMLite::isSpawned L2 append,
       JSToWasm spawned-TS prologue emission, jsCallICEntrypoint()
       nullptr under useJSThreads — without it a spawned thread can still
       WARM-call an exported wasm function created on the carrier.
     - **AB-16: RegExp.h ovector routing** (RegExp.cpp banner OPEN (1)):
       every gilOff global match RELEASE_ASSERTs until ovectorSpan()
       routes to regExpGilOffPerThreadMatchOvector — a HARD U-T9 entry
       gate (fail-stop, not silent).
     - **AB-17: per-lite traps + stack limits (VMTraps.h activation
       checklist items 1-4).** perThreadTrapsIfExists still aliases the VM
       trap word (per-thread termination/defer scopes are phase-1
       semantics; the §A.3 conductor re-fires every sample to compensate),
       and VM::updateStackLimits still writes the single VM-level
       softStackLimit (memory-safety grade under N-parallel entry).
       *GIL-removal review round 3: item 3 is no longer a SILENT hole —
       VM::updateStackLimits now RELEASE_ASSERTs (gilOff arm) that no
       OTHER lite of this VM is entered before publishing the shared soft
       limit. The assert is deleted by the same change that lands the
       §A.2.2 per-lite soft-limit reroute.*
       *CORRECTION (review round 4): the round-3 claim that the
       updateStackLimits walk was "the process-wide interim fail-stop for
       ANY second concurrent entry" was WRONG on two counts: (a) TOCTOU —
       the walk samples sibling entryScopes at lock/token-acquisition
       time, but the sibling-visible entered record is only published
       later in VMEntryScope::setUpSlow, so two concurrent entrants could
       both pass it pre-publication and then both run; (b) re-entry — a
       token-holding thread that tore down its VMEntryScope and re-enters
       JS through a fresh one never re-runs updateStackLimits at all. The
       DETERMINISTIC fail-stop now lives in VMEntryScope::setUpSlow's
       gilOff arm: the no-other-entered walk and the entered-record store
       run under ONE VMLiteRegistry-lock hold, so the second concurrent
       top-level entry aborts at publication regardless of interleaving.
       The updateStackLimits walk is RETAINED as an earlier (advisory)
       trip point only. Both asserts are deleted together by the §A.2.2
       reroute.*
     - **AB-18 (review round 3; was recorded ONLY in the
       WaiterListManager.cpp banner and missing from this list): SD6
       second half — the D8 single-flight gate deletion in
       AtomicsObject.cpp.** SPEC-ungil §C.6 (SD6, a BOTH-GIL-MODE delta)
       requires the per-wait-node allocation (LANDED, WaiterListManager.cpp)
       AND deletion of the D8 gate; AtomicsObject.cpp:501ff still carries
       syncTAWaitGateLock/vmsWithSyncTAWaitInFlight and still throws on a
       second concurrent main-thread TA wait. Until the gate is deleted
       (ordered AFTER the AB-13 split per the §C.4 constraint), the SD6
       corpus/U19 expectation ("second waiter parks on its own node") is
       NOT claimable in EITHER GIL mode, and the A26 termination-wake
       bypass coexists with the gate it was paired against.
     - **AB-19 (review round 3; was recorded ONLY in the Heap.cpp
       conductTIDRebiasUnderSharedStop banner and missing from this list):
       multi-VM gilOffProcess rebias tripwire re-key.** The §D.1 rebias
       fire loop routes through fireAllUnderClassAStop, whose run-inline
       branch runs assertAlreadyStoppedEvidenceCoversEveryMutator — a
       phase-1 tripwire whose premise (single entered VM) U0b retires
       (loser VMs may stay entered). In any multi-VM gilOffProcess
       process the FIRST rebias is a deterministic process abort. Owner:
       JSThreadsSafepoint.cpp (exempt entered loser-VM mutators when the
       stopped server is the U0c winner's heap). Until it lands: rebias
       (not just the two-VM amplifier arm) is only sound in SINGLE-VM
       gilOffProcess processes.
     - **AB-20 (review round 3): drainMicrotasksForGlobalObject sibling-
       lite clears.** The Bun-additions VM entry point now clears the VM
       default queue AND the CURRENT lite's queue when gilOff, but
       SIBLING lites' per-thread queues cannot be cleared cross-thread
       without breaking the I11 owner-only queue discipline. Needed: a
       per-lite "clear for global G" request word serviced at each
       owner's next drain (or a global-object liveness check at dequeue).
       Until then, a gilOff embedder clearing a global on one thread can
       still see stale-context microtasks drain on OTHER threads.
     - ~~AB-21 (review round 3; found by the FIRST actual gilOff boot —
       the U-T14 RUN items were DEFERRED-TO-BUILD and had never
       executed): gilOff single-carrier boot dies at the first Class-A
       watchpoint fire~~ **CLOSED by review round 4**: the §A.3
       thread-granular conductor (VMManager.cpp
       jsThreadsThreadGranularStopTheWorldAndRun) now RE-ACQUIRES its own
       client's heap access for the window (after the quiescence
       predicate is satisfied, before work(); released again before
       resume publication) — the conductor is exempt from the AHA §A.3 /
       Mode-stop gates and from allEnteredThreadsAreQuiescent (HBT2.1),
       and GSP cannot be pending under the JSThreadsStopScope GCL
       bracket, so the re-acquire neither parks nor invalidates the
       predicate, and the Heap::deferralDepthSlot
       `hasHeapAccess() || worldIsStoppedForAllClients()` asserts inside
       Class-A fire bodies are satisfied the honest way (real access).
       Debugger.cpp's runDebuggerWalkWithSpawnedThreadsStopped inherits
       the fix. Original record: `--useJSThreads=1 --useSharedGCHeap=1
       --useThreadGILOffUnsafe=1 -e 'print("hi")'` (debug) asserts
       `client->hasHeapAccess() || worldIsStoppedForAllClients()`
       (Heap::deferralDepthSlot, via DeferGCForAWhile) with stack:
       DeferredStructureTransitionWatchpointFire dtor ->
       WatchpointSet::fireAllSlow -> fireAllUnderClassAStop ->
       JSThreadsSafepoint::stopTheWorldAndRun (the AB-9-closure reroute)
       -> drainClassAFireQueue -> fireAllNow -> fireAllWatchpoints ->
       DeferGCForAWhile. The thread-granular stop runs the queued fire
       bodies at a point where the conductor's client has no heap access
       and the world is not stopped-for-all-clients, so any fire body
       that touches the heap trips the access assert. Owner:
       JSThreadsSafepoint.cpp (hold/reacquire the conductor's heap
       access across the fire-queue drain, or extend the stop witness to
       satisfy the deferral-slot predicate). Even SINGLE-carrier gilOff
       smoke runs are blocked until this lands.
     - **AB-22 (review round 4; obligation-1 residue, was MISSING from
       this list): raw vm.entryScope consumers.** The U-T5 shadow drop
       (VMEntryScope.cpp) discharged "shadow dropped" but NOT "every raw
       consumer re-pointed" (IU obligation 1). Round 4 re-pointed the
       crash-grade sites through VM::currentThreadEntryScope() (GIL-on
       byte-identical): CallFrame.cpp convertToZombieFrame (was a
       deterministic null-deref on the gilOff exception-unwind
       no-JS-throw-origin arm — reachable single-carrier, before
       AB-11/12) and CallFrame.cpp globalObjectOfClosestCodeBlock (was a
       silent nullptr GIL-off); SamplingProfiler.cpp noticeVMEntry's
       ASSERT (fired on the first gilOff entry with the profiler on,
       debug); and WIRED the SamplingProfiler.h
       shouldBindCurrentThreadAsJSCExecutionThread consult into
       noticeCurrentThreadAsJSCExecutionThreadWithLock (release-build
       hole: a spawned thread could bind and later be suspend-and-walked
       while free-running). REMAINING OPEN under this row: takeSample's
       `m_vm.entryScope` gate is constantly false gilOff, so the profiler
       is deliberately DORMANT on gilOff VMs (documented at the gate) —
       AUD1.K1's "carrier-only v1" does NOT yet deliver carrier samples
       gilOff; needs the per-lite Group-3 registry-resolve + the
       WhileTargetSuspendedScope takeSample wiring (U-T8d .cpp half).
       The U-T14 close item 7 claim that table (iv) was "executed IN
       CODE" at SamplingProfiler.h:323-360 is corrected accordingly: that
       block records the RULING; the .cpp halves were pending until this
       round (bind consult) / remain pending (suspend scope).
     - **AB-23 (review round 4): gilOff main-carrier identity re-key +
       default-queue drainer residual.** GIL-off, m_mainVMLite is never
       installed (A36), so every `lite != m_mainVMLite.get()` /
       `lite == vm.mainVMLite()` "main carrier" predicate was dead and
       VM::m_defaultMicrotaskQueue was an undrained sink. Round 4
       re-keyed the three predicates (VM::queueMicrotask,
       VM::drainMicrotasks, JSGlobalObject.cpp perLiteRealmRoutingLite)
       on the MAIN THREAD's carrier (lite->ownerHasNoTlsDtor, the A36 r32
       registration-fixed bit; that carrier also borrows &vm.clientHeap,
       F1B), so the main thread's carrier owns the in-object realm stream
       and the VM default queue — restoring the JSGlobalObject.cpp banner
       claim and giving the default queue a drainer. RESIDUAL OPEN: a
       gilOff VM entered ONLY from non-main threads still has no
       default-queue drainer for no-lite-window/off-thread enqueues
       (same service-request shape as AB-20's sibling-clear word; one
       mechanism can serve both).
     - **AB-24 (review round 4; found by the post-AB-21 smoke — the boot
       now gets PAST the Class-A fire and trivial scripts run): gilOff
       debug builds die at the first JIT-path allocation validation.**
       `--useJSThreads=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 -e
       'for (let i=0;i<1e5;i++){({}).x=i}'` (debug) asserts
       `heap.worldIsStoppedForAllClients() ||
       heap.mutatorSlowPathLock().isHeld()`
       (BlockDirectory::assertIsMutatorOrMutatorIsStopped, the
       isSharedServer arm) with stack operationNewObject ->
       JSFinalObject::create -> JSObject::finishCreation ->
       JSCell::classInfo validation -> the I5b lock-free directory-bits
       funnel. Cause: gilOff the server is shared from BOOT (U0c eager
       noteSharedServerSticky at clientSet()==1), so a FREE-RUNNING
       mutator's lock-free bits reads reach the shared-server assert arm,
       which only admits stopped-world or MSPL holders — phase-1 never
       exercised this shape (the GIL kept mutators out of each other's
       addBlock resize window by construction; single-client GIL-on runs
       take the pre-sticky arm). For a SINGLE entered mutator (the only
       shape AB-17's tripwire admits today) the read is benign and the
       assert is over-strict; for N mutators the underlying race
       (lock-free m_bits read vs a sibling addBlock resize, I5b) is REAL
       and needs the heap workstream's ruling (admit an access-holding
       client when the §A.3.1 entered count is 1? take MSPL on the
       validation paths? bits-epoch?). Owner: heap workstream
       (BlockDirectory.cpp is its surface). Until it lands: gilOff DEBUG
       smoke is limited to scripts that stay off the assert-bearing
       allocation validation paths (trivial boots pass post-AB-21);
       release builds do not assert but inherit the open I5b question for
       N>1. Fail-stop, not silent.
     **CLOSE RULING (re-stated against the complete list):** the default
     flip is safe-by-construction ONLY because the U0 validation now
     REFUSES the gilOff shape outright unless the explicit development
     escape hatch useThreadGILOffUnsafe=1 is ALSO set (landed by the
     review round; previously `--useJSThreads=1 --useSharedGCHeap=1` —
     two flags, since M_opts2 auto-forces the other two — produced a live
     gilOff process against AB-1's silent LLInt split-brain with NO
     in-code fail-stop). Build/Verify MUST treat AB-1, AB-8, AB-10..AB-13,
     AB-15..AB-20 (AB-21 closed at round 4), AB-24, and the OPEN residuals
     of AB-22/AB-23 as LAUNCH BLOCKERS for running the full-trio
     configuration, and the U0 refusal clause (Options.cpp) is only
     deleted when this list is discharged and the §B verification ladder
     (U19 oracle, TSAN, amplifier battery, golden disasm, B.5 bench) has
     actually run.
     **MILESTONE GATE STATUS (review round 3, explicit):** the GIL-removal
     milestone deliverable — N mutators actually executing JS in parallel
     in one VM — is **NOT MET** by this tree. With AB-11/AB-12 open, every
     spawn under gilOffProcess is refused (RangeError), so the GIL-off
     semantic deltas (SD1-SD5, SD8-SD19), the §E drain loop, §E.3
     keepalive, and the GIL-off corpus are structurally unrunnable; and
     the round-3 updateStackLimits fail-stop deliberately aborts any
     second concurrent ENTRY while AB-17 is open. U-T9/U-T11 rows record
     mechanism LANDED (code present, compiles, flag-off inert), not
     behavior DELIVERED — the milestone must not be reported as met until
     the AB list above is discharged and the §B ladder has run GIL-off.
  8. **§F.6 close items** — see table (vi) below (rewritten this round);
     the in-code row table (JSLock.cpp:941-955) keeps its OPEN markers for
     the Bun-side audits, which CANNOT be executed from this repository.
  **Spec-conflict record (per task instructions):**
  - (i) The orchestrator task card gave U-T14 an EMPTY owned-file list
    while the handout mandates in-tree deliverables (the flip; the U0
    gates). Resolved per the authority clause (handout > prompt) with the
    minimal write set {OptionsList.h, Options.cpp, INTEGRATE-ungil.md}.
  - (ii) VM.cpp/JSLock.cpp comments described U0 validation as already
    landed; it was not. Landing it here makes those comments true — no
    supersession, but the discrepancy is recorded (the comments were
    written against the spec, not the tree).
  - (iii) The handout's U-T14 RUN items (U19, TSAN, amplifiers, golden
    disasm, §B.5 composite, arm64-hardware arms) cannot execute in a
    no-build documentation+code round; recorded as DEFERRED-TO-BUILD
    above, never as passes.

  **Adversarial-review AMEND (round 2; write set per spec-conflict (i)):**
  - Finding R2-1 (LLInt delta-(a) branches + JSCConfig gilOffProcess byte
    absent; "confirms AB-1") — **TRUE FACTS, REFUTED AS A U-T14 DEFECT /
    NO NEW ACTION.** The absence is exactly AB-1, found and recorded BY
    this task's own delta re-audit (item 3) with the same consequence
    analysis (Group-3 VM-block vs per-lite divergence, no in-LLInt
    tripwire), and the close ruling already binds Build/Verify to treat
    AB-1 as a LAUNCH BLOCKER for any full-trio run. Owner is U-T3
    (obligation 9b), per the handout's own re-baseline schedule ("at
    U-T3"); llint/, offlineasm/ and JSCConfig.h are outside U-T14's write
    set, so neither the emission nor the suggested LLInt-entry tripwire
    can land here. No shipping config reaches the unsound state (trio
    defaults false; U0 forces GIL back on), so this stays an activation
    blocker, not a regression introduced by the flip.
  - Finding R2-2 (§N.5 twin intrinsics absent; "confirms AB-8") — **TRUE
    FACTS, REFUTED AS A U-T14 DEFECT / NO NEW ACTION.** Identical
    disposition: recorded as AB-8 by item 3 ((b2) unconsumed) and item 7,
    and the close ruling ALREADY names AB-8 (alongside AB-1) a hard
    LAUNCH BLOCKER for N>1 mutator runs — the reviewer's requested
    treatment is the landed treatment. Owner: U-T13's §N.5 leg
    (builtins/, bytecompiler/ outside this write set). The race is real
    GIL-off but unreachable while AB-1 blocks activation; the U19 GIL-on
    oracle remains the only legal config for the generator corpus until
    (b2) lands.
  - Finding R2-3 (gilOffProcess re-derived live at every call site; no
    process latch; setOptions can flip the derivation mid-process) —
    **CONFIRMED, FIXED HERE (Options.cpp, in-write-set).** Verified: the
    short forms (ArrayBuffer.cpp:180, VMInspector.cpp:95,
    SamplingProfiler.h, JSLock.cpp) and VM::isGILOffProcess() re-read
    Options live while VM::m_gilOff / Watchdog::m_gilOff latch at
    construction; Options::setOptions is callable after
    Options::finalize() (only Config::permanentlyFreeze — embedder
    optional — blocks it) and re-runs notifyOptionsChanged, whose U0 arm
    can itself force useThreadGIL 0 -> 1, silently splitting JSLock arm
    selection / detach-table consultation across consumers. FIX: a
    write-once shadow latch in notifyOptionsChanged — the derivation may
    change freely BEFORE g_jscConfig.options.isFinalized (Options::
    finalize runs at the tail of JSC::initialize, strictly before any VM
    can exist; the jsc shell parses all options pre-initialize), and any
    post-finalization notifyOptionsChanged that would CHANGE the
    derivation RELEASE_ASSERTs (fail-stop, per the reviewer's suggested
    shape). Flag-off + U19: derivation constantly false, assert
    unreachable, host-C++ only — no codegen-shape delta (delta list
    untouched). The JSCConfig gilOffProcess byte (U-T3) SUBSUMES this
    latch when it lands; the in-code comment records the replacement
    obligation. AB-1's JSCConfig leg remains open — this latch is the
    interim immutability backstop ANNEX U0C asks for, not the byte.

  **Adversarial-review AMEND (round 4 — GIL-removal blocker/major sweep;
  write set: CallFrame.cpp, SamplingProfiler.{h,cpp}, VMEntryScope.cpp,
  VMManager.cpp, VM.cpp, JSGlobalObject.cpp, VMLiteInlines.h,
  HandleSet.{h,cpp}, this file, SPEC-ungil-history.md,
  INTEGRATE-objectmodel.md):**
  - R4-1 (raw entryScope consumers; BLOCKER, CONFIRMED) — fixed + new row
    AB-22 (re-points landed; profiler-dormant residual recorded there).
  - R4-2 (updateStackLimits fail-stop TOCTOU + re-entry hole; MAJOR,
    CONFIRMED) — fixed: the deterministic tripwire moved into
    VMEntryScope::setUpSlow's registry-lock hold; AB-17 round-3 text
    corrected in place.
  - R4-3 (milestone structurally unreachable; BLOCKER as a REPORTING rule)
    — TRUE FACTS, NO NEW CODE ACTION: this is exactly the round-3
    MILESTONE GATE STATUS: NOT MET ruling above (AB-11/AB-12/AB-13/AB-17
    open; mechanism-LANDED rows are code-present, not
    function-delivered). The reporting rule stands.
  - R4-4 (conductor runs Class-A fires with no heap access; BLOCKER,
    CONFIRMED = AB-21) — fixed, AB-21 CLOSED (see the row).
  - R4-5 (useThreadGIL default flipped before the milestone gate; MAJOR,
    CONFIRMED as a recorded plan-ordering deviation) — discharged the
    DOCS way: explicit orchestrator ruling appended to
    SPEC-ungil-history.md superseding UNGIL-PLAN §J's flip-at-gate
    ordering and naming the Options.cpp U0 refusal clause +
    useThreadGILOffUnsafe hatch as the binding interim gate (deletable
    only with the AB list, per the close ruling above).
  - R4-6 (mainVMLite-keyed routing predicates dead GIL-off; MAJOR,
    CONFIRMED) — fixed + new row AB-23 (ownerHasNoTlsDtor re-key;
    non-main-only-VM default-queue residual recorded there). The smoke
    run exposed a FOURTH site of the same class: VMLite::setCurrent's K4
    §VIII cross-thread-entry noter keyed on `lite != mainVMLite()`, which
    noted the MAIN THREAD's own first gilOff install and made the
    setGlobalThis/setName immutable-after-init asserts fire on
    single-threaded boot — same re-key applied (VMLite.cpp:259).
  - R4-SMOKE (executed this round; the U-T14 RUN items were never
    executed before round 3's AB-21 boot): debug gilOff single-carrier
    `print("hi")` boot now PASSES end-to-end (AB-21 + the VMLite.cpp
    re-key); uncaught-throw exits cleanly through the re-pointed unwind
    path (R4-1); the GIL-on oracle (`--useJSThreads=1 --useThreadGIL=1`)
    and flag-off smoke are unchanged-green. The first JIT-path-allocating
    script exposes the NEXT pre-existing blocker — recorded as AB-24.
  - R4-7 (AB-7 records absent; MAJOR, CONFIRMED) — discharged the DOCS
    way: §D.2 gate-deferral ruling appended to SPEC-ungil-history.md and
    the verdict-deferral record written into INTEGRATE-objectmodel §46
    (naming U-T10's locked arm + U-T11 §C.3 PWT routing as the re-review
    surfaces on a PROMOTE outcome; bench run itself stays owed at the
    first Build round — AB-7 narrows to "run the bench", no longer
    "records missing").
  - R4-8 (Strong allocate/free/set-slot outlined + per-call gilOff branch
    flag-off; MAJOR, CONFIRMED for the Strong seam) — fixed structurally:
    strongHandle* are now ALWAYS_INLINE HandleSet.h wrappers testing a
    ctor-stamped HandleSet gilOff byte and falling through to the
    pre-ungil inline list ops; only the gilOff arm calls the out-of-line
    locked *Slow entry points (HandleSet.cpp). Flag-off codegen returns
    to the inline shape + one predicted-false byte test. The reviewer's
    aggregate items (currentThreadIsHoldingAPILock de-inline, JSLock
    isGILOffProcess Options loads, retireEntryTokenForLock thread_local,
    group3Primitives branch, JSPromise/RegExp gilOff tests) remain
    individually predicted-false host-C++ branches — ADJUDICATE TOGETHER
    at the §B.5 flag-off bench + golden-disasm gate (already owed at
    Build; the JSCConfig gilOffProcess byte (AB-1/U-T3) is the planned
    replacement for the isGILOffProcess re-derivation).
  - R4-9 (per-lite drains skip the per-tick hook; MAJOR, CONFIRMED) —
    fixed: VMLite::drainDefaultMicrotaskQueue now drains with
    performMicrotaskCheckpoint<true>, matching the §E.1b.4 disposition
    row for VM::m_onEachMicrotaskTick ("INLINE on the draining thread,
    spawned drains included"). Flag-off unreachable, unchanged.

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
| 9 | Phase-1 "useThreadGIL always on" (OptionsList.h text; SPEC-api phase-1 framing) vs handout §T U-T14 default flip + §0 U0 | LANDED: default false (OptionsList.h); U0 validation forces useThreadGIL=1 when {useVMLite, useSharedAtomStringTable, useSharedGCHeap} incomplete (Options.cpp, after the M_opts2 normalization) — restores equivalence of all short-form `useJSThreads && !useThreadGIL` derivations with VM::isGILOffProcess() | U-T14 |
| 10-… | (mid-program supersessions by U-T2…U-T13 were recorded in their task-log entries and in-code doc-of-record blocks rather than as rows here; see the U-T14 close entry item 7 for the pointer map) | — | U-T2…U-T13 |

## (ii) §F.2 predicate-consumer table (~60 rows: assert / BRANCH / EXCLUSIVITY CONSUMER) — U-T8

SKELETON. Columns: consumer site | predicate form consumed | class
(assert/branch/exclusivity) | GIL-off ruling (annex F2 fixed rulings;
~AsyncTicket/finalizer rows) | landed-at.

| Site | Form | Class | Ruling | Landed |
|------|------|-------|--------|--------|
| U-T14 CLOSE: executed IN CODE — the §F.2 predicate split and per-consumer rulings live as the doc-of-record comment blocks in JSLock.cpp (token machinery home) and VM.cpp (:271ff isEntered split, :796 row-21 citation, :998 token-meaning assert). This table intentionally stays a pointer, not a copy. | | | | U-T8 |

## (iii) §E.4 settle-site lock-context table — U-T8

SKELETON. Columns: settle site | lock context at settle | routing
(same-thread / cross-thread ticket) | retirement rule (r17 F2 / r18 F2).

| Settle site | Lock context | Routing | Retirement |
|-------------|--------------|---------|------------|
| U-T14 CLOSE: executed IN CODE — the full settle-site lock-context table (LockObject.cpp:275/:521, ThreadObject.cpp:246/:435, ThreadManager.cpp:78; r17 F2 decide-under-lock/act-after-drop status per row) is the doc-of-record block at JSLock.cpp:925-940. All frozen rows COMPLIANT as landed; the GIL-off inbox routing row landed with U-T9. | | | |

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
| U-T14 CLOSE: the remaining per-field enumeration was executed IN CODE by U-T8d — the carrier-only capture ruling + SUSPEND-RULE record is the doc-of-record block at SamplingProfiler.h:323-360 (AUD1.K1/SD18, (i) carrier-only v1), with the inspector/debugger exceptionForInspection ruling at Debugger.cpp:54ff. | | |

## (v) §E.1b.4 host-hook disposition table — U-T8e

SKELETON. Columns: globalObjectMethodTable / host-callback slot |
JS-reachable on spawned TS? | disposition {inline, carrier-queued, refused,
unreachable} | SD15 tracker handoff notes.

| Hook slot | Spawned-reachable | Disposition | Notes |
|-----------|-------------------|-------------|-------|
| U-T14 CLOSE: executed IN CODE — the full per-slot enumeration ({inline-safe, carrier-queued, refused-with-error, unreachable-on-spawned(proof)}) is the doc-of-record block at JSGlobalObject.cpp:680ff (baseGlobalObjectMethodTable), with the SD15 promiseRejectionTracker carrier-handoff rows at JSGlobalObject.cpp:4267/:4289. | | | U-T8e |

## (vi) §F.6 embedder checklist (incl. (d) construction-order and (e) spawned-no-foreign-VM audits) — U-T8

CLOSED at U-T14. The binding obligation text is the in-code row table at
JSLock.cpp:941-955 (U-T8 deliverable); this checklist records the CLOSE
dispositions. (a)/(c)/(d) bind OUT-OF-TREE Bun code and cannot be executed
from this repository — at close they are recorded as **BUN-INTEGRATION SHIP
BLOCKERS** (conditional sign-off: the in-tree contract side is complete;
the Bun-side enumerations must be discharged in the Bun repo before any
Bun build enables the full trio), NOT as satisfied audits.

- (a) JSLockHolder exclusivity (m_lock excludes only embedder threads,
  §F.1) — in-tree side COMPLETE (F1B arm + spawned token entry,
  JSLock.cpp; §B.3 supersession landed at U-T6). Bun-side critical-section
  enumeration: SHIP BLOCKER, Bun repo.
- (b) SD10 continuation-affinity sign-off — recorded r21 pre-close (ALS
  slice discharged by ANNEX ALS1); was the U-T9 entry gate; nothing owed
  at U-T14.
- (c) blocking-site enumeration (§F.6 delta (c)/DAL2.5) — in-tree DAL/§J.3
  /RHA-AHA mechanisms landed (U-T8/U-T11); the site CLASSES to audit are
  enumerated in the JSLock.cpp row. Bun-side enumeration: SHIP BLOCKER,
  Bun repo.
- (d) FIRST-VM-WINS construction-order audit row — RECORDED (this row is
  the U-T14 close item): normative requirements per ANNEX EC1 are (1)
  main-VM-first construction, (2) boot-assert vm.gilOff()==true
  immediately after constructing the intended spawner, (3) enumeration of
  EVERY Bun VM-construction site incl. lazy helper VMs, (4) no
  re-designation in v1. In-tree enforcement: U0c CAS + U0b spawn
  RangeError + the ThreadManager.cpp:336 backstop make a violated order
  fail loudly, not silently. Bun-side enumeration: SHIP BLOCKER, Bun repo.
- (e) spawned-no-foreign-VM — ENFORCED in-tree, both arms landed
  (JSLock.cpp:478 spawnedThreadEntryTokenLock scope check +
  JSLock.cpp:1027 lock()-front gate; both process-abort naming §F.6(e)).
  Death-test arm: deferred-to-Build with the U27 harness. Bun-side
  native-code audit (no spawned-thread JSContext creation): SHIP BLOCKER,
  Bun repo.

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
