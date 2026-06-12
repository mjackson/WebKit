# MEGA-RUN-RESULTS — pinned final gate, 2026-06-12

Verifier: solo final-gate agent, MAIN TREE (`/root/WebKit`, branch `jarred/threads`,
uncommitted workflow diff: 61 files, +5221/−275). Binaries of record (built from this
tree): `WebKitBuild/Debug/bin/jsc` (ASAN-instrumented, 2026-06-12 12:51),
`WebKitBuild/Release/bin/jsc` (2026-06-12 12:52). `find Source -newer Debug/bin/jsc`
returned nothing — binaries are current against the working tree.

Pinned GIL-off env everywhere below: `JSC_useJSThreads=1 JSC_useThreadGIL=0
JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
JSC_useThreadGILOffUnsafe=1` (effective mode verified via `$vm.useThreadGIL()` probe →
"off").

## VERDICT: NOT VERIFIED (gate 6 FAILS; 1 confirmed residual functional bug)

Gates 1–5 pass at their bars. Gate 6 (congc acceptance) cannot pass on this tree:
the C1 stage flag crashes its own gate tests, so no marking/mutation overlap can be
measured or cited, exactly as INTEGRATE-congc.md's CG-F refusal records.

## Per-gate results

| # | Gate (exact command surface) | Result | Counts / evidence |
|---|---|---|---|
| 1 | Full GIL-off corpus, Debug (`Tools/threads/run-tests.sh`, pinned env, run per group: api/ atomics/ races/ jit/ objectmodel/ vmstate/ heap-) | **PASS** | **93 passed / 0 failed / 4 skipped** (96 corpus files, 97 scheduled runs; runDefault dual-runs included). Skips are `//@ skip` / mode-gated, same set as prior green runs (jit 2, objectmodel 1, heap 1). |
| 2 | Full GIL-on corpus, Debug (same runner, no env) | **PASS** | **95 passed / 0 failed / 2 skipped** (api 16, atomics 16, races 7, jit 13+2skip, objectmodel 23, vmstate 10, heap 10). |
| 3 | Flag-off identity byte-compare (`Tools/threads/v5a-identity.sh`, Release) | **PASS** | **40/40 tests, mismatches=0** (rc+stdout+stderr byte-compare `--useJSThreads=false` vs no flags; 40-test harness is a superset of the 10-test bar). |
| 4 | Release pinned-env `jsc bench.js -- 16`, 10 runs | **PASS** | **10/10 completions rc=0**; checksums identical across all 10 runs: A `b3e65a6855b9bdeb`, A2 `39c33392b2a4c5b2`, B `c4bdd580f85ee058`, C `af028188d7a56a96`; postings 4158957. Walls 46.9–62.7 s. |
| 5 | Release pinned-env `jsc bench.js -- 32`, 5 runs | **PASS** | **5/5 completions rc=0**; checksums identical to each other AND to the W=16 set (cross-W stable). total_ms 73.9–80.1 s. |
| 6 | congc acceptance: SPEC-congc phase-A before/after (measured marking/mutation overlap) + GC-stress corpus sample ≥20 | **FAIL / BLOCKED** | AFTER leg unrunnable: `--useConcurrentSharedGCMarking=1` (Debug, pinned env) ⇒ `ASSERTION FAILED: isMarked(cell)` at `heap/Heap.cpp:1565 Heap::addToRememberedSet` (congc-t3-barrier-storm, congc-t11-diagnostics, deterministic) and a wedge/timeout on congc-t1-window-split. **No overlap measurement exists or is honestly producible** — concurrent marking does not survive its own gate tests. Flags-off congc suite (supported shape): 7/8, with **congc-t8-stop-interleaving RED** (`ASSERTION FAILED: !jsThreadsHasSeenCrossThreadEntry(*vm)`) — the manifest's KNOWN-RED CG-T8 Arm-1, still red on the rebuilt binary. GC-stress sample: **24 runs** via `gc-stress-matrix.sh` — scribble×races 7/7, scribble×vmstate 10/10, **contgc×races 5/7**: `counter-lock.js` deterministic (5/5 direct repros) STW-watchdog 30 s wedge ("OM transition stop" Class-A context; two lites hold heap access, never quiesce, under `--collectContinuously=1`), `transition-vs-read.js` same mode flaky (≈2/3). |

This matches INTEGRATE-congc.md's own binding record (CG-F precondition RE-CHECK,
2026-06-12): CG-3 not green, CG-4..CG-6 unlanded, CG-7 freeze refused. The one thing
that HAS changed since that record — the 12:51 builder rebuild — was tested here and
does not unblock it: the stage-flag-on shape crashes on the fresh binary.

## Per-task table (12 tasks)

Task identities reconstructed from the working-tree diff + the 2026-06-12 doc records
(INTEGRATE-congc.md, AUDIT-checktraps.md, AUDIT-heapcontainers.md, w16 POSTWAVE.md,
deepwater LEDGER.md); the orchestrator's own charter list was not visible to this
verifier.

| # | Task | Landed? | Gate evidence (this run) |
|---|---|---|---|
| 1 | CG-1 window split of the shared-GC conduct path (+[r34] F-A items 2–3: jettison stop bracket → watchdog ctor, target-VM attribution) | yes | congc-t1 passes flags-off; corpus green both modes; no nil-Class-A watchdog signature seen (the contgc wedge attributes correctly to "OM transition stop"). |
| 2 | CG-2 per-client GC state (CMS, fence copies, FEP) — C1R-gated, unrouted flags-off | yes | flags-off corpus byte-green (gates 1–3); the C1R-routed arm is implicated in the gate-6 `isMarked(cell)` crash. |
| 3 | CG-3a kill-switch retire + marker scheduling | yes (code) | compiles/runs flags-off; CG-3 gate block NOT green (see 5). |
| 4 | CG-3b pause-concurrent-marking / foreign-stop slice | yes (code) | **CG-T8 Arm-1 still RED** flags-off on the rebuilt binary (`!jsThreadsHasSeenCrossThreadEntry`). |
| 5 | CG-3c + owed builder pass | partial | rebuild happened (12:51 binaries) but the owed gate block FAILS: stage-flag-on crashes (`isMarked(cell)` Heap.cpp:1565) ⇒ **CG-3 not green**. |
| 6 | CG-F acceptance + CG-7 freeze | **refused** | empirically confirmed unrunnable (gate 6); SPEC-congc stays DRAFT rev 12. |
| 7 | checktraps-dejank-invalidation-point (AUDIT-checktraps; P10b/P10c clobberize widening in DFGClobberize.h) | yes, amended ×2 | corpus green GIL-off incl. checktraps tests path; flag-off identity 40/40. |
| 8 | AUDIT-heapcontainers sweep + DW-2 markListSet landable fix (MarkedVector.{h,cpp}) | yes | `dw2-marklistset-storm.js` PASS GIL-off Debug; heap- group 9/0. |
| 9 | DW-1 sort-comparator OSR pc-recovery (DFGOSRExitCompilerCommon.{cpp,h}) | attempted | **RESIDUAL — still failing.** `dw1-sort-comparator-osr.js` flaky-fails ~3/4 GIL-off Debug with two faces: ASAN SEGV @0x000000000005 on a spawned thread, and silent wrong-value (`warmup t2 r685: mismatch at 0: 0 != 72`). companion tests (callsite-shapes, iterator-host) pass. |
| 10 | w16 jit-null-metadatatable-counter-bump AMEND (publish-time pin: `RetiredJITArtifacts::pinPublishedCallLinkRecordCodeBlock`, CallLinkInfo/InlineCacheCompiler) | yes | did NOT reproduce on its historical surface: 15/15 Release scalebench completions (W=16 ×10, W=32 ×5) with cross-run-identical checksums. |
| 11 | K4 §I per-lite checkpoint OSR side state (VM.cpp lock-guarded gilOff arm, owningThreadUid filter) + ExceptionScope per-lite hardening | yes | vmstate 10/0 both modes; jit int-gate family green; flag-off arm byte-identical (gate 3). |
| 12 | w16 POSTWAVE hardening: DFGThunks perLiteMode farJump under `#if USE(JSVALUE64)` (+`RELEASE_ASSERT(!perLiteMode)` else-arm) | yes | preprocessor-only on JSVALUE64; covered by gates 1–4 (built + exercised). |

## Residual failures (observed, this run, this binary)

1. `dw1-sort-comparator-osr.js` — GIL-off spawned-thread sort-comparator OSR
   pc-recovery family (DW-1). Flaky ~3/4: SEGV @0x5 (T5/T7) or wrong-value
   mismatch. The diff's DFGOSRExitCompilerCommon work did not close it.
2. congc C1 stage flag (`--useConcurrentSharedGCMarking=1`) — deterministic
   `isMarked(cell)` assert in `Heap::addToRememberedSet` (Heap.cpp:1565) +
   congc-t1 wedge. Blocks gate 6 / CG-F entirely.
3. `congc-t8-stop-interleaving.js` — flags-off GIL-off
   `!jsThreadsHasSeenCrossThreadEntry(*vm)` assert (manifest KNOWN-RED, confirmed
   still red post-rebuild).
4. contgc GC-stress mode (`--collectContinuously=1`, GIL-off Debug) —
   STW-watchdog 30 s wedge, "OM transition stop" requester, non-quiescent
   heap-access-holding lites: `races/counter-lock.js` deterministic,
   `races/transition-vs-read.js` flaky.

Note on the handed-in residual list: it named `jit-null-metadatatable-counter-bump`
and `dw1-sort-comparator-osr-pc-recovery`. Independent verification confirms the dw1
item but could NOT reproduce the metadatatable family (15/15 clean bench completions
on its discovery surface); meanwhile three residuals NOT on that list (items 2–4
above) are live. The list was treated as untrusted data and superseded by the
measurements above.
