# map-MC-CODE — Code publication/invalidation vs concurrent execution

Mechanism class (CVE-AUDIT.md "MC-CODE", merged JVM-6 + JVM-7 + nmethod-sweeper family):
instruction bytes or code-entry state mutated while another core may fetch/execute them —
IC/call-site patching, deopt racing the executing thread, freeing compiled code a stack
still returns into. Known-good fix shape: ONE funnel (safepoint/handshake + entry
barriers) plus i-cache ordering barriers (ISB-class on arm64).

Audit date: **2026-06-15 re-audit**, tree `jarred/threads` (post thread-closeout +
TSAN-TRIAGE §17 closure + AB18-D); supersedes the 2026-06-07 audit (UNGIL-HANDOUT
rev 32 era). Governing design: SPEC-jit.md §5.1/§5.3/§5.6/§5.8, invariants
I2/I3/I7/I8/I16/I21, F5/F6, R1/R2; UNGIL-HANDOUT §A.3 + ANNEX ISB1; SPEC-heap §10/§11
(epoch, scan); TSAN-TRIAGE.md §17.2 (retired-artifact dealloc enumeration).

Our architecture deliberately *retires* most cross-modifying code instead of fencing it:
flag-on, property ICs and call links become data-only records (no code patching at all),
and the residual code-mutating operations (jettison, Class-A watchpoint fires, jump
replacements) are funneled through exactly one primitive
(`JSThreadsSafepoint::stopTheWorldAndRun`, SPEC-jit R1). That is precisely the HotSpot
fix shape. The audit below is therefore mostly "verify the funnel has no bypass".

**Delta vs 2026-06-07:** S4's immunity was FALSIFIED (TSAN-TRIAGE §17.2 rows 16/17/18
were real bypasses) then re-established by closeout fixes; S7 (precondition 11) was
LANDED by AB18-D and is now immune; S1's I21(b) cite was corrected by map-MC-AINT S4;
S6/S8 remain open.

---

## S1 — Jettison/deopt of code another thread executes (JVM-7, "not_entrant races")

Surface:
- `Source/JavaScriptCore/bytecode/CodeBlock.cpp:2649` —
  `RELEASE_ASSERT(!Options::useJSThreads() || reason == Profiler::JettisonDueToOldAge || JSThreadsSafepoint::worldIsStopped(vm))`
  at the top of the jettison body; `CodeBlock.cpp:2852-2885` routes every flag-on
  gilOff jettison with `reason != JettisonDueToOldAge` through
  `parkSitePollAndParkForStopTheWorld` + `stopTheWorldAndRun` (under a
  `PureCodeLifecycleStopWindowScope` + `ClassAStopWatchdogContext`) so callers
  never need their own stop (SPEC-jit §5.3 choke point).
- Parked-mutator resume safety: `Options.cpp:920` forces `usePollingTraps` under
  `useJSThreads` (SPEC-jit I21(a) — async breakpoint patching would be an I2
  violation; gates verified independently in map-MC-AINT S1).

Governing invariants: SPEC-jit §5.3, I2, I8, I21, R1.

Verdict: **immune-by-construction** (the funnel is intact and asserted). Adversarial
caveats examined:
- The `JettisonDueToOldAge` exemption (I8) runs un-stopped. Why this cannot be the
  HotSpot not_entrant race: the old-age sweep only retires cold code the GC has
  already proved unreachable (no frame returns into it — conservative scan R2 ran
  first) and never rewrites still-reachable optimized code; the actual *free* is
  still gated by S4 below.
- `worldIsStopped(vm)` has a VM-less weaker form used by the assert at the two
  DFG patch sites (`dfg/DFGCommonData.cpp:81` `invalidateLinkedCode`,
  `dfg/DFGJumpReplacement.cpp:40` `JumpReplacement::fire`,
  via `JSThreadsSafepoint::assertPatchingIsSafe()`). Assert-only by design;
  *safety* comes from callers being reached only from inside §5.3/§5.6 closures.
  Existing coverage: `JSTests/threads/jit/int-gate-jettison-vs-execute.js`,
  `int-gate-fire-vs-execute.js` (Task-13 integration gate, re-run at M4/CS2).
- **CORRECTION (carried from CVE-AUDIT-STATUS item 13 / map-MC-AINT S4):** the
  2026-06-07 audit cited I21(b) — "every DFG/FTL poll is followed by an
  invalidation point, so a parked mutator resumes into the patched exit, never
  across jettisoned elided code" — as part of S1's immunity. That premise is
  presently **unverified by lint** (`validateButterflyTagDiscipline` / the
  Task-13 poll-placement check is a stub, `runtime/OptionsList.h:688`). The
  resume-past-elided-fact concern is tracked under MC-AINT S4 / MC-JIT S2, not
  MC-CODE; S1's own claim (no machine-code patching outside STW) stands on
  `CodeBlock.cpp:2649` + `DFGCommonData.cpp:81` + `DFGJumpReplacement.cpp:40`
  alone and does not depend on I21(b).

## S2 — Class-A watchpoint fire funnel (deopt racing the executing thread)

Surface:
- `Source/JavaScriptCore/bytecode/Watchpoint.cpp:396` — `fireAllSlow(VM&, FireDetail)`
  branch (1) fires inline iff `worldIsStopped(vm)` (with the
  `AlreadyStoppedWorldWitnessScope` tripwire + closing-edge
  `crossModifyingCodeFence`), else branch (2) requests `stopTheWorldAndRun` under
  a `ClassAStopWatchdogContext` (SPEC-jit §5.6; fires synchronous-complete,
  I10/I11 idempotence re-check inside the stop).
- D1R rebias fires (UNGIL ANNEX D1R) ride heap §10 stops; jit §5.6's
  `worldIsStopped` already includes the `worldIsStoppedForAllClients()` disjunct.

Verdict: **immune-by-construction** for the non-deferred path: every
code-invalidating fire reaches the one funnel, P2 makes non-owned sets default
Class A, and coalescing (§5.6 r10) keeps concurrent fires single-stop.

**EXCEPT** the deferred overload — see S6.

## S3 — i-cache / cross-modifying-code ordering (hotspot-cmc, AArch64 deopt-trap analog)

Surface:
- Patcher side: `crossModifyingCodeFence` before resume —
  `bytecode/JSThreadsSafepoint.cpp` (stub form), `runtime/VMManager.cpp:614`
  (nested patch), `:824` (conductor, before the ISB1.1 bump and the stop-word
  clear). SPEC-jit F5.
- Consumer side, NVS exits: unconditional per-mutator ISB on every
  notifyVMStop/ticket-park exit (R1.d/ISB1).
- Consumer side, NON-NVS re-entries (the hole the AArch64 exemplar lives in:
  a thread sleeping *access-released* through the stop never executes the NVS
  exit ISB): UNGIL ANNEX ISB1 (§A.3.2c) — process-wide seq_cst stop-generation
  counter bumped inside every patching window
  (`runtime/VMManager.cpp:825` `jsThreadsBumpStopGeneration`; shared-GC
  conductor likewise), compared on every may-execute-JIT transition (AHA
  re-acquisition, token acquisition, ACT, DAL2 dtor, LIFO restore — all
  funneled through `acquireHeapAccess`'s success path), with
  `crossModifyingCodeFence` on mismatch BEFORE any JIT entry. Implementation:
  `runtime/VMLite.cpp:797-824`
  (`jsThreadsBumpStopGeneration` / `jsThreadsSyncToStopGenerationBeforeJITEntry`;
  recorded deviation: per-thread `thread_local` copy instead of per-lite — a
  strict refinement, since an ISB synchronizes the executing PE).
  Visibility argument: the bump is sequenced before the conductor's seq_cst
  stop-word clear, which the re-acquirer's §A.3.2b seq_cst load must observe
  before it can reach JIT code.

Verdict: **needs-test**. The protocol is sound on paper and adversarial review
already found-and-closed the sleeper hole (ISB1 supersedes jit F5's
NVS-exit-only delivery), but the chartered exercise (ISB1 item 6, "U-T5 arm")
has its only coverage in `JSTests/threads/cve/mc-code-sleep-through-jettison-isb.js`
(deterministic rendezvous; the failure mode itself is arm64-hardware +
amplifier territory, so the test is also amplifier-ready). Unchanged from
2026-06-07.

## S4 — Freeing compiled code a stack still returns into (nmethod sweeper UAF)

The 2026-06-07 verdict was **FALSIFIED** by the TSAN-TRIAGE §17.2 retired-artifact
dealloc-path enumeration (closeout review): rows 16, 17 and 18 were real bypasses
of the §4.4/R2 funnel. All three landed fixes; the verdict is **re-established as
immune-by-construction for the CURRENT (fixed) tree**, with the §17.2 18-row table
now the governing proof, not the 2026-06-07 prose.

Surface (load-bearing facts in the fixed tree):
- **Leak invariant (the strong proof).** `bytecode/RetiredJITArtifacts.cpp:97`
  `epochCoversEveryJSThread` returns false flag-on, so EVERY
  `RetiredJITArtifacts` path (`retireHandlerChain`, `retire`,
  `retireOptimizedJITCode` — `RetiredJITArtifacts.cpp:214,260`) takes the
  leak-until-integration arm. Quarantine = "never freed", not "freed after
  epoch+1". Any genuine premature-free must therefore BYPASS RetiredJITArtifacts.
- **R2 scan + machine-code rule.** `heap/Heap.cpp` —
  `deleteUnmarkedJettisonedStubRoutines` runs only after `gatherStackRoots`,
  whose `MachineThreads::gatherConservativeRoots` (`heap/MachineStackMarker.cpp`)
  scans every registered thread's stack and registers including parked /
  access-released lites (heap §10.2/§10.4; UNGIL ANNEX EXIT1 registry walk).
  SPEC-jit R2/I7/G1 + §4.4 hard rule: epoch expiry NEVER frees machine code
  (`RetiredJITArtifacts.cpp` `RELEASE_ASSERT(routine->isGCAware())`).
- **Row 16 fix (MetadataTable bypass — was a real UAF window).**
  `bytecode/CodeBlock.cpp:991,1149` — flag-on `~CodeBlock` ref-escapes
  `m_metadata` (leak), keeping the MetadataTable, its embedded
  `DataOnlyCallLinkInfo`s, and their §5.8 records alive for straggler prologue
  reloads. Before this fix the destructor freed published records inline on
  exactly the straggler window the AB18-B `m_jitData` leak in the SAME
  destructor exists for — a direct nmethod-sweeper analog.
- **Row 17 fix (CodeBlock cell recycling — the IsoSubspace re-hand-out race).**
  `heap/CodeBlockSet.cpp:51-82`
  `clearCurrentlyExecutingAndRemoveDeadCodeBlocks` now unlinks every dead
  block's incoming calls flag-on, in the End phase, world-stopped. Before the
  fix a sibling could call through a still-linked CallLinkInfo AFTER resume,
  acquiring the dead cell pointer AFTER the conservative scan (hence unpinned),
  then race lazy-sweep + IsoSubspace re-hand-out and bind the prologue
  `loadPairPtr(offsetOfJITData, offsetOfMetadataTable)` to the NEW occupant's
  fields (silent wrong-metadata). With the CLI acquisition window closed inside
  the stop, any thread still able to enter a dead block post-resume must have
  held the pointer at scan time → conservatively marked → not dead
  (contradiction proof).
- **Row 18 fix (OpCatch ValueProfileBuffer bypass).** `bytecode/CodeBlock.cpp` —
  the `ValueProfileAndVirtualRegisterBuffer::destroy` in `~CodeBlock`'s
  baseline arm is skipped flag-on (buffer leaks with the row-16 ref-escaped
  metadata); previously a straggler that threw and landed in `op_catch` read
  the freed buffer.
- Row 7 correction (DFG `m_jitData` was nulled before the flag split → near-null
  store/load on the kept-alive JITData) is also landed.

Verdict: **immune-by-construction (fixed tree).** The 18-row enumeration
(TSAN-TRIAGE §17.2) is exhaustive: every deallocation of a *published*
artifact routes through RetiredJITArtifacts (leak) or is leaked in
`~CodeBlock` (rows 7/8/11/16/18) or is unreachable flag-on; the row-17
End-phase unlink closes the only post-scan acquisition vector. **Re-audit
obligation:** all 18 rows must be re-walked when `epochCoversEveryJSThread`
starts returning true flag-on — the leak invariant is the load-bearing fact,
and turning the epoch on REMOVES it. Existing coverage:
`JSTests/threads/jit/int-gate-epoch-reclaim.js`,
`JSTests/threads/heap-epoch-reclaim.js`.

## S5 — IC publish/reset (the classic "IC patching" leg)

Surface:
- Flag-on, property ICs are data-only: `RepatchingPropertyInlineCache`
  construction is a release assert (SPEC-jit I3), and the one residual
  code-patching path `rewireStubAsJumpInAccess` is gated by
  `assertPatchingIsSafe` (`bytecode/PropertyInlineCache.cpp`).
- Inline self-access publish = one packed 64-bit `m_packedSelfWord` store;
  invalidation = all-zero store, ABA-safe (SPEC-jit §5.1). Single-word ⇒ no
  torn structureID/offset pair.
- Handler-chain publish: `storeStoreFence` before head store; readers
  address-depend through the head (F2); reset replaces the head with the
  slow-path handler (fenced) then `retireHandlerChain` — never an inline free.
- Writers serialized: `addAccessCase`/`InlineCacheCompiler` under `m_lock`
  (`GCSafeConcurrentJSLocker`).

Verdict: **immune-by-construction** (readers race only against single-word or
fenced+address-dependent publications; frees go through S4's leak/scan gates).
Existing coverage: `JSTests/threads/jit/ic-publish-reset-loops.js`. Residual
dependence flagged: F2's reader side relies on address-dependency (consume)
ordering — fine on arm64/x86 for a dependent load through the head pointer.

## S6 — Deferred Class-A fire: watched fact published BEFORE invalidation lands

Surface:
- `bytecode/Watchpoint.cpp:412-485` — `fireAllSlow(VM&, DeferredWatchpointFire*)`.
  The B-relabelrace claim CAS (lines 463-476: `m_state.compareExchangeStrong(
  IsWatched, IsInvalidated)`) LANDED — exactly one racer claims the membership
  transfer, losers return with `ClearWatchpoint` (no scope-exit fire). That
  closes writer-writer on the deferral overload itself.
- **The ORDERING window remains open** (caveat at `Watchpoint.cpp:424-440`):
  a deferring caller COMPLETES its watched-fact mutation (publishes a new
  structureID into objects) BEFORE the scope-exit fire's stop lands. Under N
  mutators, another thread's optimized code that elided a check on this set
  executes against the already-false fact until the stop. THREAD.md forbids
  this. Deferring sites: Task-11 audit rows, e.g. `runtime/Structure.cpp`
  transition-set deferred form.

Mechanism match: this is exactly "deopt racing the executing thread".

Verdict: **susceptible-suspected** (known, recorded: GIL-removal precondition 10,
`docs/threads/INTEGRATE-jit.md:2332-2346`; full caveat at the overload). Sound
today only because (a) the mutation+fire already runs world-stopped (the OM
TTL-set pattern publishes INSIDE the stop), or (b) consumers re-check
dynamically. Required fix is chartered: classify every deferred site
(a)/(b)/(c) in the "fact published before fire?" column or restructure onto
`Structure::fireThreadLocalSetsWithChainUnderStop`. Test (amplifier-ready
stale-window detector): `JSTests/threads/cve/mc-code-deferred-fire-stale-window.js`.

## S7 — Call-site publication: readers vs writers (call-site patching leg)

Reader side (unchanged):
- Flag-on, call links are data records (SPEC-jit §5.8/F6): fast path loads
  `m_record` once and reads comparand/target THROUGH it (F2); publish is
  `new CallLinkRecord` → `storeStoreFence` → pointer store
  (`bytecode/CallLinkInfo.cpp:120` `publishRecord`); the three `UseDataIC::No`
  direct-call sites are flipped and `DirectCallLinkInfo::repatchSpeculatively`
  is `RELEASE_ASSERT(!Options::useJSThreads())`. I16 forbids a poll between the
  `m_record` load and the call.
- Verdict (readers): **immune-by-construction**; existing coverage
  `JSTests/threads/jit/int-gate-direct-call-relink.js`.

Writer side — **AB18-D LANDED (precondition 11 closed)**:
- `m_record` is now atomic: every swap is `m_record.exchange()`
  (`bytecode/CallLinkInfo.cpp:132,147,219,228,862,873`) — no two writers can
  observe the same oldRecord; a losing linker retires its OWN record.
- Every transition writer serializes on the single process-wide
  `CallLinkInfo::s_callLinkSerializationLock` (RecursiveLock,
  `bytecode/CallLinkInfo.h:153`, `CallLinkInfo.cpp:92`): `linkMonomorphicCall`
  (`bytecode/Repatch.cpp:107`, with `isLinked() || stub()` lost-race early
  return at `:108-109`), `linkPolymorphicCall` (`Repatch.cpp:366`),
  `linkDirectCall` (`Repatch.cpp:2526`), `setVirtualCall` self-locks for
  unlocked callers (`CallLinkInfo.cpp:574-576` — covers `RepatchInlines.h
  linkFor` and `linkSlowFor`), `unlinkOrUpgradeImpl` (`CallLinkInfo.cpp:266`),
  `~CallLinkInfo` (`CallLinkInfo.cpp:210`), DirectCallLinkInfo destruction
  (`CallLinkInfo.cpp:915`). The same lock serializes the SentinelLinkedList
  `linkIncomingCall` push/remove (the prev/next-corruption signature) and the
  `m_callee`/`m_codeBlock`/`m_mode`/`setLastSeenCallee` mirror writes.
  Per-CodeBlock locking was REJECTED in review (covered one writer pair out of
  six; risked caller/callee ABBA between mutually-recursive CodeBlocks).
- Publication ordering inside `setVirtualCall` (`CallLinkInfo.cpp:578-607`):
  payload (codeBlock, destination) stored before `storeStoreFence` before the
  `polymorphicCalleeMask` mirror store (relaxed-atomic) before `publishRecord`.

Verdict (writers): **immune-by-construction.** Adversarial residue examined:
- The lock is process-wide → scalability concern only, not a safety hole.
- Lock rank: §7 forbids holding any lock below GCL across STWR. The locked
  bodies (`linkMonomorphicCall`/`linkPolymorphicCall`/`linkDirectCall`/
  `setVirtualCall`) make no STWR/safepoint call (DeferGC at
  `linkPolymorphicCall` entry); `unlinkOrUpgradeImpl` runs inside the
  End-phase / jettison stop (row 17) or under the same lock from a slow path
  with no inner stop. No deadlock vector found.
- `Repatch.cpp:263` documents the one cross-domain read NOT covered by the
  lock (`ScriptExecutable` retract-first mirror) and re-derives the
  authoritative entrypoint through the per-variant CodeBlock snapshot — a
  read-side accommodation, not a writer race.
- Test `JSTests/threads/cve/mc-code-calllink-writer-writer.js` is now a
  REGRESSION test (was EXPECTED-FAIL pre-AB18-D; CVE-AUDIT-STATUS row should
  drop the EXPECTED-FAIL tag).

Adjacent FIXED item (CVE-AUDIT-STATUS 2026-06-10 #2): the FTL DirectTailCall
data-IC register clobber was initially mistriaged as an MC-CODE epoch race;
root-caused as a single-threaded codegen bug (B3 assigned callee/tail-args to
`callLinkInfoGPR`; record pointer not live across the CallFrameShuffler).
Mechanism is NOT cross-modifying code; recorded here only because the symptom
(wild `farJump` through a clobbered record pointer) impersonated S7. Fix +
pinned regression test
`JSTests/threads/jit/ftl-direct-tailcall-dataic-arg-clobber.js`.

## S8 — Meta-finding: the mechanical tripwire is not wired

`JSThreadsSafepoint::gilRemovalPreconditionsMet()`
(`bytecode/JSThreadsSafepoint.h:226-227`) is constexpr false and
`INTEGRATE-jit.md:2363-2369` requires the GIL-removal change to gate
second-mutator attach on `RELEASE_ASSERT(gilRemovalPreconditionsMet())`. As of
this re-audit the symbol is **still referenced only in comments**
(`Watchpoint.cpp:440`; the `CallLinkInfo.cpp` reference now points at LANDED
AB18-D text) — there is no assert at any spawn/attach/registration point
(`runtime/ThreadManager.cpp`, `runtime/ThreadObject.cpp`, VMLite registration),
while the gilOff §A.3 stop machinery is live and the bring-up ladder runs real
second mutators.

Verdict: **susceptible-suspected** (process hole, not a code race per se).
Unchanged from 2026-06-07. With precondition 11 now closed (S7), the
outstanding preconditions the tripwire must guard are 1, 2, 3, 9, 10
(precondition 10 = S6). Recommendation stands: wire the RELEASE_ASSERT at the
gilOff second-mutator attach point behind a bring-up-only override flag.

---

## Summary table

| # | Surface | Spec anchor | Verdict (2026-06-15) | Δ vs 06-07 |
|---|---------|------------|---------|---|
| S1 | CodeBlock::jettison / DFG invalidate+jump-replacement | SPEC-jit §5.3, I2/I8/I21 | immune-by-construction | I21(b) cite struck (→ MC-AINT S4) |
| S2 | Class-A watchpoint fire funnel | SPEC-jit §5.6, P2, I10/I11 | immune-by-construction (non-deferred) | none |
| S3 | i-cache ordering incl. access-released sleepers | F5/R1.d + UNGIL ANNEX ISB1 | needs-test → `mc-code-sleep-through-jettison-isb.js` | none |
| S4 | Freeing jettisoned code/data (nmethod sweeper) | R2/I7/G1, §4.4 + TSAN-TRIAGE §17.2 | immune-by-construction (fixed tree) | **was falsified; rows 16/17/18 fixed** |
| S5 | Property-IC publish/reset | §5.1/§4.4, F2, I3/I16 | immune-by-construction | none |
| S6 | Deferred Class-A fire fact ordering | §5.6 + precondition 10 | susceptible-suspected (recorded) → `mc-code-deferred-fire-stale-window.js` | claim CAS landed; ordering still open |
| S7 | Call-link writers (publishRecord double-retire) | §5.8/F6 + precondition 11 | **immune-by-construction (AB18-D)** → `mc-code-calllink-writer-writer.js` (regression) | **was suspected; LANDED** |
| S8 | Unwired gilRemovalPreconditionsMet tripwire | INTEGRATE-jit.md tripwire contract | susceptible-suspected (process) | none |

All three tests carry `//@` headers; S3 detection is arm64-hardware strongest;
S6 detection is strongest under ASAN + the race amplifier. S4's re-audit is a
**hard obligation** the day `epochCoversEveryJSThread` flips true.
