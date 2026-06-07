# SPEC-congc-history.md

Companion to `SPEC-congc.md` (draft rev 4). Per the frozen-spec
convention: this file is NON-NORMATIVE EXCEPT the sections marked
ANNEX ... (BINDING), which are part of the binding spec text and
exist here only for the body size cap (50000 bytes).

## Rev log

- rev 1 (2026-06-07): initial draft per the thread-specs2 charter.
  Grounding pass: SPEC-heap rev 13 (Deviation 4, §10/§10A/§10B,
  I1-I17), SPEC-ungil rev 32 (§A.3, §B, EXIT1/HBT4/ISB1/SB1
  annexes), and the live tree (Heap.cpp 5913 lines incl. the landed
  §10 implementation and the fix-shared-heap-corruption
  diagnostics). Not yet adversarially reviewed; freeze pending the
  review loop + the whole-design cross-check.

- rev 2 (2026-06-07): adversarial round 1 — 10 reviewer findings
  received, 9 distinct (the two mid-cycle-attach findings are one
  defect), ALL ACCEPTED as real after verification against the
  tree; none refuted. Dispositions (F-numbers used by the rev 2
  body cites):
  - F1 (blocker, two findings merged): §4.2 cited a nonexistent
    §9.3; mid-cycle ATTACH had no fence-copy/FEP init — a client
    attaching between windows of a live cycle ran with the
    attach-default unfenced copy (every store skips the remembered
    set; live objects collected) and broke the CG-I3 assert.
    VERIFIED: rev 1 §9 had only 9.1/9.2; `HeapClientSet::add`
    blocks only in-window (`HeapClientSet.h:54-68`), so
    between-window attach is reachable. FIX: new §9.3 (GBL-section
    fence/threshold+FEP snapshot before HCS publish; assist-visitor
    registration deferral; CG-I14 amendment; CG-I20; CG-T9 attach
    storm arm). The §10B.4-quiescence alternative was rejected for
    spawn-latency/starvation (recorded in §9.3).
  - F2 (major): §9.2(1) flushed the CMS BEFORE the landed teardown;
    `lastChanceToFinalize` runs with access re-acquired and can
    execute write barriers (`Heap::addToRememberedSet`,
    `Heap.cpp:1427`), whose appends post-flush died with the
    client. Lost-object interleaving (rev 1 full text + reviewer
    interleaving): cycle Concurrent between windows; exit; CMS
    flushed empty; finalizer stores white W into black surviving B;
    barrier appends B to the flushed CMS; HCS remove before next
    WND-open; CMS destroyed; B never re-scanned; W freed live.
    CG-I17's "empty when leaving HCS" assert contradicted the
    rev 1 normative order — proof the order was wrong. FIX:
    teardown -> permanent access drop -> final flush -> epoch=MAX
    -> HCS remove; early flush optional; CG-T9 arm.
  - F3 (major): ANNEX CGN1 N3's progress sentence ("in-window
    convergence guarantees progress since holders are then
    parked") was inverted — a holder parked WHILE holding 10a is
    the livelock, not the proof. FIX: CG-I18 (cell-lock no-park)
    in §8.2 with grounding (SINFAC I6 precondition
    `Heap.cpp:5125-5127`; F8 parking only in access acquisition
    `Heap.cpp:5707-5758`); N3 row amended below; CG-A2 row schema
    now classifies mutator sides against CG-I18; debug asserts +
    CG-T5 arm.
  - F4+F5 (major+blocker, one fix): conductor lifecycle between
    windows was unspecified — no access/poll contract, no GCA
    owner identity, no resume predicate, no wake-up path once the
    legacy conn/parking machinery (`stopIfNecessarySlow`
    `Heap.cpp:2421`, `waitForCollector` `:2497`) is kept dead
    (CG-I7); the §3.4 `:4590` guard as written would also have
    blocked the conductor's own poll, and a conductor blocked in
    native code (or exiting) stalled the cycle with GCA set
    forever. VERIFIED: GCA is a bare bool (`Heap.h:1290`); rev 1
    §7.1 "served by SINFAC polls" named machinery that does not
    exist. FIX: §3.5 `m_gcConductorThread` identity; §3.7 closed
    loop (conductor access-released all tenure; between-window
    waits = the landed MainDrain shape `SlotVisitor.cpp:623-636`;
    wake-ups = helper termination notifyAll `:629, :645` +
    scheduler timeout; no JS/RHA/EXIT1 mid-cycle, release-assert);
    §7.1 rewritten (conductor runs the Collector arm
    `Heap.cpp:1997-2002` under ISS; Mutator arm `!ISS`-only);
    CG-I19; CG-T8/T9 arms. The rev 1 SINFAC-conn sentence is
    superseded INTERNALLY (pre-freeze rev supersession, this
    entry; both-sides discipline N/A — nothing frozen cited it).
  - F6 (major): §9.1(2) cited `m_parallelMarkersShouldExit`-class
    signaling AND the `:2315-2342` rightToRun loop — non-equivalent
    mechanisms (Exit is one-shot cycle-terminate set in runEndPhase
    `Heap.cpp:2027`, consumed `SlotVisitor.cpp:664, :673`).
    FIX: normative new pause pair (`m_parallelMarkersShouldPause` +
    `m_pausedParallelMarkers` under `m_markingMutex`), checkpoints
    at the helper-wait isReady lambda (`SlotVisitor.cpp:661-667`)
    and the per-batch drain safepoints (`:522, :578`), ack count,
    lost-wakeup argument, per-batch pause granularity feeding the
    CG-I12 bound. Resolves old open item 3.
  - F7 (major): §7.2's client-less conductor reused a conduct body
    written against `GCClient::Heap&` with no refactor table.
    FIX: ANNEX CGA2 below (BINDING); §7.2 points at it; old open
    item 1 (VMTraps poll) RESOLVED as CGA2 row R6.
  - F8 (major): §5.2(ii) gated donation on
    `Options::minimumNumberOfCellsToKeep` — VERIFIED nonexistent
    (grep of Source/JavaScriptCore; nearest is
    `minimumNumberOfScansBetweenRebalance`, OptionsList.h:402,
    different semantics) — and named no trigger site. FIX: new
    option `sharedGCMutatorMarkStackDonationThreshold` (§13.2;
    default `s_segmentCapacity`, `GCSegmentedArray.h:62, :116`);
    trigger site pinned to the SINFAC hot-poll tail only
    (rank-7-9b-free by SINFAC's I6 precondition), never inside
    `addToRememberedSet`.
  - F9 (blocker): rev 1 WND-open order (GCL -> GSP -> release
    access) deadlocked vs an interleaved `JSThreadsStopScope`:
    the conductor re-acquired access at each WND-close, then
    block-acquired GCL holding access; the §A.3 fan-out (ungil
    §A.3 rule 2) waits for that access forever; mutual stall to
    the 30s watchdog (`JSThreadsSafepoint.cpp:379, :403`).
    VERIFIED new-path: the landed election/poll only ever tryLock
    GCL (`Heap.cpp:4523, :4585, :5036`), and HBT4 pins
    release-access-before-GCL for all §A.3 conductors. FIX: §3.1
    order = access-released -> GCL -> GSP; non-final WND-closes do
    not re-acquire conductor access (§3.2); HBT4 order extended to
    GC-conductor re-entry (§9.1(3)); CG-T8 arm at the GCL acquire.
  Size cap: body compressed to 49995 bytes (<= 50000); full
  rejection rationales live in this entry per the overflow rule.

- rev 3 (2026-06-07): adversarial round 2 — 10 reviewer findings
  received (the 10th truncated in transmission; its surviving text
  identified the same defect as F10's flag-off half and is folded
  there), 7 distinct defects after merging duplicates
  (findings 1+5+10 = F10; findings 2+6+8 = F13; the rest 1:1).
  ALL ACCEPTED as real after verification against the tree; none
  refuted. Dispositions (F-numbers cited by the rev 3 body):
  - F10 (blocker; 3 findings merged): the §3.4 second-cycle claim
    guarded only `tryConductSharedCollectionForPoll` and cited
    `:4550-4554` — VERIFIED to be the tryLock-FAILURE follower
    arm; the election WINNER arm (`Heap.cpp:4523-4532`) checks
    only `m_lastServedTicket >= ticket` under `*m_threadLock`,
    sets GCA, and conducts unconditionally. Under the window model
    "GCL free && GCA set && unserved tickets" is a steady state
    (today only the wind-down instants `:4534-4537`, where phase
    is already NotRunning), so a between-windows sync requester
    wins tryLock and nests a second `conductSharedCollection`
    (interleaving: ANNEX CGD1.1). Also accepted (the truncated
    finding's half): an UNCONDITIONAL "GCA set => back off" guard
    would change flag-off behavior, because GCA-true-with-GCL-free
    IS reachable flag-off in wind-down — rev 2's "impossible ...
    backs off" sentence was doubly wrong. FIX: §3.4 rewritten as a
    GCL-tryLock SITE ENUMERATION (`:4523`, `:4585`, `:5036`) with
    the PHASE-GATED predicate `GCA && m_currentPhase != NotRunning`
    (unreachable flag-off => CG-I0 holds byte-for-byte); CG-I21;
    CG-T8 requester-storm arm.
  - F11 (blocker): `pollIssRevertIfNeeded` (`Heap.cpp:5010-5105`)
    tryLocks GCL (`:5036`) then loops
    `while (m_gcConductorActive) waitFor(1_ms)` (`:5040-5043`)
    HOLDING GCL and heap access — sound today (GCA-true/GCL-free
    lasts instants), a process-wide deadlock against the
    between-windows steady state (ANNEX CGD1.2). The rev 2 §9.2(3)
    text addressed the revert OUTCOME (phase must be NotRunning)
    but not this landed pre-check's structure. FIX: §3.4 `:5036`
    row + §9.2(3) cross-ref — never wait for GCA under GCL when
    phase != NotRunning; return with the hint armed; bounded
    wind-down wait only when NotRunning. CG-T9 arm (revert-pending
    poll storm mid-cycle).
  - F13 (blocker; 3 findings merged): rev 2 §3.7/§7.1/CG-I19
    pinned the conductor's between-window wait to
    `drainInParallelPassively`'s "MainDrain wait"
    (`SlotVisitor.cpp:623-636`) and §9.1(2) exempted it from pause
    checks. VERIFIED wrong on every arm: (a) the
    `drainInParallelPassively` guard (`:718-731`) falls back to
    ACTIVE `drainInParallel()` (donateAndDrain + counted
    drainFromShared(MainDrain)) on `numberOfGCMarkers()==1`,
    `mutatorWaitingBit` (dead under ISS), `!hasHeapAccess()` —
    which under ISS forwards to mainClientHasHeapAccess()
    (`Heap.h:405-412`), i.e. an UNRELATED thread's state when the
    conductor is access-released or not the main client — or
    `worldIsStopped()`; (b) even the MainDrain wait actively
    steals and drains on hasWork (`:680-688`, `:705`) and is
    counted in active/waiting; (c) the combination with the
    §9.1(2) exemption yields, per branch, either a pause-predicate
    that can never close (watchdog stall at
    `JSThreadsSafepoint.cpp:401`) or a conductor draining cells
    concurrently with a §A.3 window's jettison (ANNEX CGD1.3);
    (d) numberOfGCMarkers()==1 was unruled. FIX: §3.7/§7.1
    rewritten — ISS conductor wait = `donateAll()` +
    `waitForTermination` (`SlotVisitor.cpp:753-758`, `:737-751`),
    no counters, never visitChildren, never
    drainInParallelPassively/drainFromShared; ==1 markers =>
    Concurrent never scheduled (one-window degenerate); CG-I19
    amended; CG-T8 arm kept.
  - F14 (major): §9.1(2) never defined WHICH thread classes the
    pause denominator counts; `performIncrementOfDraining`
    (`SlotVisitor.cpp:527-585`) maintains NO counters, so giving
    the C4 assist path the `:578` checkpoint (as rev 2 did) makes
    a pausing assist mutator overshoot the ack predicate AND park
    holding access where the §A.3 fan-out cannot reach it (it
    passes no stop poll while waiting on
    m_markingConditionVariable) — watchdog stall; omitting it
    races the §A.3 window for up to one increment, which is the
    bounded, fan-out-compatible choice. FIX: participant set
    pinned to drainFromShared(HelperDrain) helpers only (§9.1(2));
    new §9.1(6): assist takes NO checkpoint, bound = one
    increment, parked by the mutator fan-out at its next poll;
    §7.4 cross-ref; CG-T10 arm.
  - F15 (major): §3.1's (a)access-released -> (b)GCL -> (c)GSP
    order, read literally at the FIRST window, double-acquires the
    non-recursive GCL (election tryLocked it while access-held,
    `:4523`) and inverts the landed GSP-before-release order
    (`:4768-4771`) — contradicting CG-I0 (flag-off = one window =
    the first one, byte-for-byte). FIX: first-window carve-out in
    §3.1 (the first WND-open IS the landed entry; tryLock while
    access-held is §A.3-safe because non-blocking); (a)-(c) and
    the blocking acquire pinned to RE-ENTRY; §9.1(3) and CG-I19
    re-phrased to match; the F9 rejection re-grounded as
    re-entry-only.
  - F16 (major): §9.1(2)'s rev 2 ack predicate
    ("paused == active + waiting") composed with
    `didReachTermination` (`SlotVisitor.cpp:594-598`) was
    underdetermined: decrementing active on pause allows
    termination to fire with undonated work parked in paused
    helpers' local stacks (lost marks); not decrementing makes the
    equality unsatisfiable (pauser deadlock). FIX: normative
    counter protocol — pause checkpoint does donateAll (paused
    helpers hold NO local work) + leaves its counter
    (waiting--/active-- as appropriate) + paused++; ShouldPause
    gates counter re-entry; pauser predicate = active==0 &&
    waiting==0 under m_markingMutex (exited helpers count in
    neither — SUPERSEDED rev 4, F17: this was asserted as a landed
    property but is FALSE against the tree; it is now a normative
    EDIT, the §9.1(2)(c) exit delta); didReachTermination
    additionally gated on paused==0 (CG-I22); CG-T8 mid-batch
    sub-arm.
  - F12 (labeling): rev 2's grounding sentence for §3.4 cited the
    follower arm as if it guarded winners — corrected as part of
    the F10 rewrite (kept as a separate number because two
    findings called out the citation error independently of the
    missing guard).
  Body size: compressed to 49989 bytes (<= 50000); the full
  interleavings/derivations moved to ANNEX CGD1 per the overflow
  rule; §2.1's consumer list deduplicated against ANNEX CGA1
  (the annex table is the audit of record).

- rev 4 (2026-06-07): adversarial round 3 — 6 reviewer findings
  received, 3 distinct defects after merging duplicates
  (findings 1+3+5 = F17, two blockers + one blocker restatement;
  findings 2+4+6 = F18, three majors stating the same
  charter/miscite defect; the §5.3 finding = F19, 1:1). ALL
  ACCEPTED as real after verification against the tree; none
  refuted. Dispositions (F-numbers cited by the rev 4 body):
  - F17 (blocker; 3 findings merged): the rev 3 §9.1(2)/F16 pauser
    predicate `active==0 && waiting==0` was grounded on the
    sentence "helpers that exit (TimedOut/Done) leave both
    counters" — VERIFIED FALSE: `drainFromShared` increments
    `m_numberOfWaitingParallelMarkers` unconditionally at the top
    of every loop iteration (SlotVisitor.cpp:621) and decrements
    only on the resume-to-active path (:688); ALL FOUR return
    paths exit with the increment leaked — MainDrain TimedOut
    (:626), MainDrain Done (:629-630), HelperDrain TimedOut
    (:641-642), HelperDrain Done on m_parallelMarkersShouldExit
    (:673-674). Grep-complete: the only writers in the tree are
    :621/:688 plus the zero-init (Heap.h:1261); the only readers
    are the steal denominator (:682) and a diagnostic log
    (Heap.cpp:1867); termination uses ACTIVE only
    (didReachTermination, SlotVisitor.cpp:594-598) — which is why
    the leak is benign tip-of-tree and why the spec's predicate
    turned it into a guaranteed liveness failure: every cycle's
    runEndPhase sets m_parallelMarkersShouldExit
    (Heap.cpp:2026-2031), each helper Done-returns leaking +1, so
    from the second cycle onward every §A.3 pause wait wedges
    forever. Worse: in the GIL-off conductor the stop scope is
    constructed (VMManager.cpp:561) BEFORE `requestStart` is
    sampled (:579), so the wedge is not even watchdog-covered —
    silent process hang with GCL held. FIX (body §9.1(2)(c) EXIT
    DELTA): normative waiting-- on all four return paths (they
    hold m_markingMutex); the flag-off delta (steal-denominator
    heuristic only) is benign-ruled under CG-I0 with the full
    derivation in ANNEX CGD2.1; debug assert
    active==waiting==paused==0 after `m_helperClient.finish()`;
    the gate clause extended to a fresh helper's FIRST :621
    increment (transient — checkpoint (a) moves it to paused under
    the same mutex, so the pauser predicate re-closes); CG-I22
    re-worded ("a property CREATED by the F17 exit delta, not
    landed"); CG-T8 gains the second-cycle sub-arm (one completed
    cycle BEFORE the injected stop — the only way the leak is
    test-reachable).
  - F18 (major; 3 findings merged): §13.3(b) chartered the
    marker-pause call site OUT to "`JSThreadsSafepoint.cpp` ...
    the scope ctor lives there (`:334-337`)" — VERIFIED WRONG both
    halves: the ctor/dtor live at Heap.cpp:5456-5482 (heap-owned
    per §13.1); bytecode/JSThreadsSafepoint.cpp:334-338 is merely
    one USE site (the `std::optional` declaration + `.emplace`).
    The other construction sites — runtime/VMManager.cpp:561 (the
    GIL-off §A.3 thread-granular conductor, the path every GIL-off
    jettison/haveABadTime takes once JSThreadsSafepoint.cpp:239-241
    reroutes gilOff requests) and SharedHeapTestHarness.cpp:1039/
    :1073/:1107 — were never mentioned. Read literally, the
    §13.3(b) row places the pause call at the stub use site and
    leaves the VMManager conductor pausing NOTHING: a §A.3 window
    jettisoning while HelperDrain markers are mid-visitChildren,
    the exact CGD1.3 UAF class rev 3 closed. The §14 freeze gate
    ("INTEGRATE-congc.md matches §13 exactly") would have forced a
    fabricated integration row for a call site that must not
    exist. FIX: §9.1(2) now pins CALL SITES = the ctor/dtor ONLY
    (covering every construction by construction; dtor order made
    normative: resume markers BEFORE releasing GCL so a WND-open
    cannot interleave with paused markers); §13.3(b) DELETED with
    the miscite recorded; §13.3(c) states VMManager.cpp:561 is
    covered with ZERO VMManager edits; the §9.1 intro cite gains
    the `bytecode/` path; §14 CG-3 re-worded (no foreign
    integration row); CG-T8 gains the GIL-off VMManager-conductor
    jettison sub-arm.
  - F19 (blocker): §5.3(3)'s GIL-off fail-safe pinned the CLIENT
    copies always-fenced — but NO emitted code reads the copies:
    Baseline bakes the SERVER `addressOf*` as AbsoluteAddress
    (AssemblyHelpers.h:2045, :2052, :2116; Heap.h:723/:726 return
    &m_mutatorShouldBeFenced/&m_barrierThreshold, the server
    members; branchIfBarriered reads
    VM::offsetOfHeapBarrierThreshold — also server), and DFG/FTL
    load VM_heap_barrierThreshold / VM_heap_mutatorShouldBeFenced
    off the VM (FTLLowerDFGToB3.cpp:27281, :27323, :27355). With
    §5.3(1) dropping the setMutatorShouldBeFenced ISS forcing
    (Heap.cpp:3928-3940 — its own banner says the fence "must hold
    at all times" with N mutators) and the §13.3(a) reroute
    unlanded, GIL-off C1 mutators would run JIT code with
    mutatorShouldBeFenced=false and blackThreshold between cycles
    after the first endMarking lower — eliding mandatory
    store-store fences and skipping barrier slow paths (lost
    remembered-set appends / unfenced butterfly publication). The
    fail-safe failed exactly in the case it existed for. NOT the
    recorded open item 2 (which weighs the COST of a pin vs
    blocking C1 GIL-off): the defect is that the pin attached to
    storage the baked addresses do not read. FIX: §5.3(1)'s
    forcing-drop is additionally gated on NOT GIL-off — GIL-off
    keeps the landed forcing, so the SERVER MASTER (what emitted
    code reads) stays tautological and the copies snapshot
    tautological from it; FEP stays at the raise (CG-I3
    unaffected); §13.3(a) row re-worded; CG-T3 gains the GIL-off
    two-cycle fence-storm sub-arm asserting the server pair;
    derivation: ANNEX CGD2.2. Open item 2 (pin cost) remains open
    and now reads against the server pin.
  Body size: compressed to 49959 bytes (<= 50000); compressions
  moved rationale prose to existing annex pointers (CGD1.1-1.3,
  this entry, ANNEX CGD2); no normative clause was weakened —
  every trimmed sentence survives here or in an annex.

Open items for the review loop (tracked, not yet ruled):
1. RESOLVED rev 2 (F7): collector-thread conductor needs no
   VMTraps poll — ANNEX CGA2 row R6.
2. §5.3(3) fail-safe pin (GIL-off always-fenced until the JIT
   reroute lands) costs every GIL-off barrier a fence; reviewers
   should weigh pinning per-stage vs blocking C1 GIL-off on the
   jit row. STILL OPEN — rev 4 F19 re-pointed the pin to the
   SERVER master (the storage emitted code actually reads); the
   cost question is unchanged by that fix.
3. RESOLVED rev 2 (F6): §9.1(2) now pins a fresh pause pair; the
   lost-wakeup argument is in the body.
4. CG-I12's "bounded by one window + one marker-pause": needs a
   measured bound on window length at C1 (fixpoint windows include
   constraint solving; heap §10.6 stack scans are O(threads)).
   STILL OPEN (CG-T8 measures); rev 2 adds the per-batch pause
   granularity that makes the marker-pause half derivable.

---

## ANNEX CGA1 (BINDING) — "the mutator"-singular audit table

Audit patterns (CG-T1 grep set, `heap/**` only):
`m_mutatorDidRun`, `m_mutatorSlotVisitor`, `m_mutatorMarkStack`,
`mutatorShouldBeFenced`, `m_barrierThreshold`, `stopTheMutator`,
`resumeTheMutator`, `mutatorWaitingBit`, `mutatorHasConnBit`,
`hasAccessBit`, `stoppedBit`, `m_worldState`, `mutatorState()`,
`m_mutatorState`, `sanitizeStackForVM`, `shadowChicken`,
`m_mutatorExecutionVersion`, `m_barriersExecuted`,
`m_incrementBalance`, `mutatorIsStopped`, `rightToRun`,
`m_currentThreadState`, `m_machineThreads`. Every match must map to
a row below (or a row added by the implementing change with the
same discipline). Dispositions per SPEC-congc §4.3.

| # | Site | Disposition | Rule |
|---|---|---|---|
| A1 | `m_worldState` bit machine: `Heap.cpp:2348` (stopTheMutator), `:2390` (resumeTheMutator), `:2421-2459` (stopIfNecessarySlow), `:2497-2533` (waitForCollector), `:2534-2600` (acquireAccessSlow), `:2601-2670` (releaseAccessSlow), `:2652-2686` (conn relinquish + unpark), `:2688-2714` (handleNeedFinalize), `:2747` (notifyThreadStopping), asserts `:2354-2384` | LANDED-N-ARY (superseded when ISS) | Unreachable/no-op once ISS (RELEASE_ASSERTs `:2352`, `:2393`; reroute `:2421-2427`, `:2545-2616`); CG-I7 keeps them dead in ALL stages. `!ISS`: untouched (CG-I0). |
| A2 | `m_mutatorDidRun` writes `Heap.cpp:2433, 2519, 2594` (legacy paths); consumer `:2234-2237` (`m_mutatorExecutionVersion`) | FOLDED | §4.1: per-client `m_didRunSinceLastWindow` set in AHA success tail + SINFAC hot-poll exit; conductor ORs into the `:2234` consumer at WND-open, clears in-window. Legacy writes stay `!ISS`-only. CG-I9. |
| A3 | `m_mutatorSlotVisitor` (`Heap.h:1182`; ctor `Heap.cpp:378`; `forEachSlotVisitor` `HeapInlines.h:279`; assist use `Heap.cpp:3974`) | STAGE-GATED (C4) | §7.4: per-client assist visitors registered at ACT; the server's `m_mutatorSlotVisitor` remains for `!ISS` and as the conductor's own assist slot. CG-I14. |
| A4 | `m_mutatorMarkStack` (`Heap.h:1183`; lock-free append `Heap.cpp:1479`; clear `:527`, `:1809`; size log `:1929`; empty assert `:2032`) | FOLDED | §5.2 CMS: per-client stack + leaf lock when ISS; window/threshold drains under `m_markingMutex`. `:2032` assert becomes "all CMS empty" at endMarking (in-window walk). CG-I2/I10. |
| A5 | `m_mutatorShouldBeFenced`/`m_barrierThreshold` (`Heap.h:722-726, 1209`; writes `Heap.cpp:473-474`, `:3928-3940`, raises/lowers `:1111`, `:1247`, init `:4456`; readers `:714`, `:746`, `:1433`, `:3324`; JIT bakes `addressOf*` `Heap.h:723,726`) | FOLDED | §5.3: server master mutated in-window only + FEP; per-client copies republished in the mutating window; consumers read current client's copy when ISS. JIT address: §13.3(a) charter; GIL-off pinned always-fenced until it lands. CG-I3. |
| A6 | `m_barriersExecuted++` (`Heap.cpp:1432`; reset `:2301`) | RACY-TOLERATED | Diagnostic counter; relaxed increments documented benign (TSAN suppression row). |
| A7 | `sanitizeStackForVM(vm())` (`Heap.cpp:1704`, `:2206`, `:2675`) | VM-SINGULAR-DEFERRED | Per-VM, not per-mutator; self-guards on entered state. Post-GIL per-thread stacks are vmstate/ungil territory (lite-owned stacks); conductor calls it only in-window. |
| A8 | shadow chicken + `vm().topCallFrame` (`Heap.cpp:2253-2254`) | VM-SINGULAR-DEFERRED | In-window read of the one main VM's state; ungil §A.1 reroutes topCallFrame per-lite — when that lands this becomes a registry walk (already noted in-tree `:2249-2252`). |
| A9 | `mutatorState()` / `m_mutatorState` (`Heap.cpp:583`, `:3337`, `:3384-3390`; slot dispatch `Heap.h:1080-1095`) | LANDED-N-ARY | Already per-client via `mutatorStateSlot()` routing when ISS (Heap.h:1080-1095). No change. |
| A10 | Per-client deferral (`Heap.h:1061`; heap I17) | LANDED-N-ARY | No change; CIND/assist consult calling client (also gates C4 assist entry). |
| A11 | `SlotVisitor::m_mutatorIsStopped`/`m_rightToRun`/`updateMutatorIsStopped` (`SlotVisitor.h:166-168, 236-239`; `SlotVisitor.cpp:469-486`; resume loop `Heap.cpp:2315-2342`) | WINDOW-CONFINED -> STAGE-GATED (C1) | Semantics is "world is stopped", not "the one mutator": keyed on `m_heap.worldIsStopped()`, flipped by stopThePeriphery/resumeThePeriphery which become per-window (§3). N visitors already handled by the `:2315-2342` loop. C4 adds per-client visitors to the same machinery. |
| A12 | `m_raceMarkStack` + lock (`Heap.h:1169, 1184`; `aboutToMarkSlow` `MarkedBlock.cpp:345+`) | LANDED-N-ARY | Locked; mutator-count-independent. CG-T3 exercises under N. |
| A13 | Re-whiten CAS protocol (`Heap.cpp:1444-1467`) and barrier race comments (`:734-738`, `:1473-1477`) | LANDED-N-ARY | Single-word CAS vs collector; argument independent of mutator count (monotone isMarked). CG-T3. |
| A14 | `m_incrementBalance` (`Heap.cpp:1969` reset; `:3959-3978` assist) | STAGE-GATED (C4) | Per-client balance; reset folds at WND-open. Until C4: ISS early-return stays (`:3950`). |
| A15 | `m_currentThreadState`/machine-threads scan (`Heap.cpp:1879` fixpoint assert; gatherStackRoots `:1024-1080`) | WINDOW-CONFINED | Conservative scan in-window only (assert `:1037` KEEPS); N-thread coverage landed (heap §10.6/T6, AHA registration `Heap.cpp:5678`). |
| A16 | Activity callbacks (`Heap.cpp:790-792`; reroute `:1595-1600`) | STAGE-GATED (C2) | §7.2: RCAC tickets + collector-thread wake; SINFAC fallback. |
| A17 | Collector-thread plumbing (`shouldCollectInCollectorThread` `:1631-1648`, `collectInCollectorThread` `:1650`, run loop `:333-357`, `m_collectorThreadIsRunning`, `:1686` assert) | STAGE-GATED (C2) | §7.2 conductor; legacy stop bits stay dead (CG-I7). |
| A18 | `handleNeedFinalize`/`needFinalizeBit` (`Heap.cpp:2688-2714`; finalize `:2753`) | WINDOW-CONFINED | Shared mode finalizes in-window (conduct loop); heap §10B(5)'s "no JS finalizers in the stop window" + ungil §F.3(b) carve-out unchanged. The legacy bit path is `!ISS`-only. |
| A19 | `requestCollection` legacy asserts (`Heap.cpp:2806-2823`) | LANDED-N-ARY | `!ISS`-only (`:2812` assert); shared path is `requestCollectionShared` `:4479`. |
| A20 | Eden/full activity callback objects themselves (`m_fullActivityCallback` etc.) | STAGE-GATED (C2) | Fire-side only; their timers run on the main run loop; C2 routes their "collect now" into RCAC. |

## ANNEX CGB1 (BINDING) — BlockDirectoryBits out-of-window audit (T8 extension)

Charter: re-run the T8 audit (heap §14 T8; stop-mode banner
`BlockDirectory.cpp:613`) with the question changed from "is this
in-window/MSPL/!ISS" to "may this run concurrent with marking
helpers (C1) or a sweeping client (C3)". Seed rows from the current
tree (the executed audit at C3 entry must cover every
`BlockDirectoryBits` accessor; rows here are the known
classification anchors):

| # | Site | C1/C3 classification |
|---|---|---|
| B1 | `m_bits.resize` in `addBlock` (`BlockDirectory.cpp:177-181`; BVL+MSPL per heap I5b) | WRITER — gains release-publication of the storage descriptor (CG-F3). |
| B2 | `parallelNotEmptyBlockSource` lock-free `markingNotEmpty().findBit` (`BlockDirectory.cpp:539-559`; in-window assert `:556`) | READER — C1 relaxes the assert for marker helpers; converts to acquire-read of the published descriptor, stale-bound-tolerant (CG-I8(d)). |
| B3 | `IncrementalSweeper` lock-free isDestructible/isEmpty reads (`IncrementalSweeper.cpp:60` banner) | READER — C3 sweeper-client runs them under MSPL per quantum (CG-I13); no lock-free out-of-window directory reads survive C3. |
| B4 | `assertNoUnswept` skip (`BlockDirectory.cpp:495-519`) | DEBUG — keeps its shared-mode skip; C3 narrows the skip to "no MSPL and not in-window". |
| B5 | `assertIsMutatorOrMutatorIsStopped` consumers (e.g. `:511`, `:556`) | DEBUG — predicate re-derived per stage: in-window OR MSPL OR owning-LA-thread OR (C1) registered marker helper on acquire-published storage. |
| B6 | Eden-bit store under BVL (heap §5.2(2), `LocalAllocator.cpp` `:250`-area edits) | WRITER — unchanged (BVL); marking helpers never write eden bits. |
| B7 | `MarkedBlock::Handle::sweep` directory-bit reads under MSPL (`Heap.cpp:1487-1498` banner) | READER — unchanged (MSPL). |
| B8 | TLC teardown bit flips (`GCThreadLocalCache.h:87-91`, MSPL) | WRITER — unchanged (MSPL; heap I5b). |

Executed-audit obligation: the C3 change adds every remaining
accessor as a row with one of {IN-WINDOW, BVL, MSPL,
ACQUIRE-PUBLISHED-READER, DEBUG, !ISS}; CG-T7 fails on any accessor
without a row.

## ANNEX CGN1 (BINDING) — cell-lock coverage audit (CG-A2)

EXECUTED AT C1 ENTRY (gate for CG-T5; rows appended here when the
audit runs — the annex is binding as an OBLIGATION now and as a
TABLE once executed). Row schema: {class / visitChildren site,
mutator-side writer protocol (om §/ungil §N row), visitor-side
read disposition per SPEC-congc §8.2}. Seed dispositions fixed
now (normative):

- N1. JSObject butterfly/shape storage: om §6/§9 frozen protocol;
  visitor follows the om-specified read side (segmented/flat tag
  decode); IN-PROTOCOL.
- N2. JSString ropes: ungil §N.2 release-CAS publish / acquire
  read; visitor acquire-reads fiber words; never resolves;
  IN-PROTOCOL.
- N3. JSMap/JSSet/WeakMap impls: ungil §N.1 cell-locked INCLUDING
  reads — visitor side is CELL-LOCKED with tryLock+revisit
  (CG-I15); a failed tryLock re-queues the cell on the visitor's
  own stack. Termination (rev 2, F3 — the rev 1 sentence here
  stated the inverse and is SUPERSEDED): by CG-I18 (SPEC-congc
  §8.2) no thread parks, passes a stop poll, or releases access
  while holding 10a, so IN-WINDOW every 10a lock is free and each
  retry succeeds; out-of-window a failed tryLock defers to the
  race stack / next window. Each CELL-LOCKED row's mutator side
  is audited against CG-I18 when CG-A2 executes.
- N4. Structure: `Structure::m_lock` (rank 10b) — visitor uses the
  existing concurrent-JIT-safe read paths (Riptide already races
  compiler threads here); IN-PROTOCOL.
- N5. ArrayBuffer/wasm memory words: ungil §N.6 torn-pair rules;
  visitor reads {base,length} per the N6 read order; IN-PROTOCOL.
- N6. Profiling-class fields (FunctionRareData jit item 7 etc.):
  RACY-TOLERATED rows must each name the field and the reason;
  TSAN suppressions key on this list ONLY.

## ANNEX CGA2 (BINDING) — C2 client-less conductor refactor surface

Charter (rev 2, F7): SPEC-congc §7.2's collector-thread conductor
is not a client and never holds access; the landed conduct path is
written against one. Every conductorClient/vm() use in the conduct
path gets a row; the C2 change adds rows for any site this table
misses (CG-T6 fails on an unclassified site).

| # | Site | Disposition |
|---|---|---|
| R1 | `conductSharedCollection(GCClient::Heap&)` signature (`Heap.cpp:4757`) | Parameter becomes nullable (`GCClient::Heap*`); null = standalone conductor (C2). |
| R2 | Step-3 own-access release (`Heap.cpp:4769-4770`) and tail `conductorClient.acquireHeapAccess()` (`:4955`) | SKIP when null — the collector thread has no access to release/re-acquire; the §10.4 barrier then waits on ALL clients (no "every client except the conductor's own" carve-out). |
| R3 | Main-VM in-window work: `sanitizeStackForVM(vm())` (`Heap.cpp:1704, :2206`), shadow chicken (`:2253-2254`), AtomStringTable scope + TID-rebias teardown (`:4880-4915` area) | KEEP, executed by the conductor thread IN-WINDOW — licensed by heap §10B rule 2 (phase-loop vm() asserts gain `|| WSAC`); the heap T9 audit classified sites for a client conductor, and every such site is in-window, where client-ness is irrelevant. |
| R4 | Step-8 resume pass / per-client TLC loops (`Heap.cpp:4923-4925`) | UNCHANGED — already loop over HCS; the conductor's own (nonexistent) client simply contributes no entry. |
| R5 | §10D revert poll context (`pollIssRevertIfNeeded`) | NOT run by the collector conductor (main client's thread only, heap §10D); no change. |
| R6 | VMTraps poll in `runSharedGCElection` (`Heap.cpp:4562-4572`) | SKIPPED for the collector thread: it never enters a VM, so no JSThreads/debugger conductor ever needs IT parked via traps — its §A.3 compatibility is being access-free + condvar-parked (rev 1 open item 1 RESOLVED). |
| R7 | Collector run loop (`Heap.cpp:333-357`; `shouldCollectInCollectorThread` `:1631-1648`) | REWIRED: wait on the `m_threadCondition`-class signal for granted-unserved tickets (RCAC/activity wakes, SPEC-congc §7.2), then run the election-equivalent (tryLock GCL, set GCA + `m_gcConductorThread`, conduct, drop both per the continuity bound CG-I12). |
| R8 | `m_currentThreadState`/conservative-scan registration | N/A — the collector thread is never a mutator root; it contributes no stack to heap §10.6 gathering (it is not in HCS). |

## ANNEX CGS1 (BINDING) — supersessions to record at freeze

Recorded BOTH SIDES at freeze time (SPEC-congc rev N + the sibling
history file), per the supersession convention. Flag-gated:
each clause below is superseded only under the named stage flag;
flag-off keeps the frozen text operative (CG-I0).

1. SPEC-heap.md:23 (Deviation 4 disabled-feature list) — vs
   SPEC-congc §7 (C1: concurrent marking; C2: collector
   continuity + activity-callback collection; C3:
   mutator-concurrent sweeping; C4: incremental assist).
2. SPEC-heap §5.4 (`performIncrement` early-return when ISS;
   activity callbacks never fire when shared) — vs §7.2/§7.4.
3. SPEC-heap §10B(3) (collector thread quiesced once shared;
   stopTheMutator/resumeTheMutator unreachable) — PARTIAL: the
   quiescence clause vs §7.2; the unreachable clause is KEPT
   (CG-I7).
4. SPEC-heap §10B(7) (deviation-4 features disabled) — vs §7.
5. SPEC-heap §10 step 7 "full synchronous collection ... world
   suspended for the entire cycle" (conduct banner) — vs §3
   (window model); the step ORDERING per window is kept.
6. SPEC-heap I5 ("shared mode runs marking-start, stop/prepare
   iteration, conservative scan, constraint solving, sweep
   scheduling, precise-vector iteration only on the conductor ...
   while WSAC") — NARROWED, not removed: marking DRAIN moves
   out-of-window at C1 (helpers), sweep execution to the sweeper
   client at C3; the listed phase-control items stay WSAC-only
   (SPEC-congc §8.1).
7. SPEC-heap I11 "legal contexts: §10 step 7" — REINTERPRETED:
   "the cycle's final window" (SPEC-congc CG-I11); no semantic
   change to the I11 conditions themselves.
8. setMutatorShouldBeFenced ISS forcing (`Heap.cpp:3936-3937`,
   specced via heap §10B(5) "always-fenced once ISS") — vs
   SPEC-congc §5.3 under `useConcurrentSharedGCMarking`.

No SPEC-ungil clause is superseded: §A.3/EXIT1/HBT4/ISB1 are
composed with unchanged (SPEC-congc §9); the ISB1.1 bump cadence
change (§3.2) is an extension in the direction ISB1 already
licenses (every window that may jettison bumps).

## ANNEX CGD1 (BINDING) — rev 3 interleavings and the ISS conductor-wait derivation

Referenced by SPEC-congc §3.4, §3.7, §9.2(3). The interleavings
are the normative reachability proofs for the rev 3 rules; the
test charters arm them.

### CGD1.1 — second conductor via the election winner arm (F10)

1. Conductor mid-cycle, BETWEEN windows: GCL free (§3.4), GCA
   true, `m_currentPhase == Concurrent`.
2. A mutator hits an allocation trigger and calls
   `requestCollectionShared` (legal between windows — it holds
   access, `Heap.cpp:4486-4491`): `m_lastGrantedTicket++ >
   m_lastServedTicket` (granted tickets are served only at the
   cycle-end ticket drain, `:4852-4863`).
3. It calls `runSharedGCElection(ticket)`: served < ticket; the
   `:4523` tryLock SUCCEEDS (GCL free); `:4526-4530` sees
   served < ticket, re-sets GCA, calls `conductSharedCollection`.
4. Two threads now run the phase machinery: the second conduct's
   entry asserts pass (GSP false, WSAC false between windows), it
   issues a nested `requestStopAll(GC)`, runs
   `collectInMutatorThread` against phase state owned by the
   parked first conductor; on exit it clears GCA (`:4536`) under
   the live conductor. Double endMarking/finalize on unwind.
   The rev 3 guard (GCA && phase != NotRunning => follower) makes
   step 3 fall to the `:4550-4554` wait instead.

Flag-off half (the truncated 10th finding): GCA-true-with-GCL-free
IS reachable today — election wind-down unlocks GCL (`:4534`)
before clearing GCA (`:4536`); same shape in the poll path
(`:4600-4604`). An unconditional GCA back-off would alter flag-off
behavior at that race (today a tryLock winner with unserved
tickets conducts). Phase-gating excludes it: in flag-off shared
mode, any point where GCL is free has `m_currentPhase ==
NotRunning` (the one window spans the whole cycle), so the guard
never fires flag-off.

### CGD1.2 — revert-poll deadlock (F11)

Config: GIL-on shared, C1 (§13.2 allows: stage flags gate only on
`useSharedGCHeap`).
1. Concurrent cycle live; conductor between windows (GCL free,
   GCA true).
2. A spawned thread EXIT1s mid-cycle (legal, §9.2); HCS shrinks
   to 1; `m_issRevertPending` armed.
3. Main client's next SINFAC poll (`Heap.cpp:5143-5151` region)
   enters `pollIssRevertIfNeeded`; the `:5036` tryLock SUCCEEDS;
   it enters `while (m_gcConductorActive)
   m_gcElectionCondition.waitFor(*m_threadLock, 1_ms)`
   (`:5040-5043`) — holding GCL AND heap access.
4. GCA cannot clear until the cycle ends; the cycle cannot end:
   the next WND-open blocks on GCL (held by the poller), and even
   the §10.4 barrier could never complete (the poller holds
   access). Every §A.3 stop also wedges on GCL. Permanent.
   Today this loop is sound only because GCA-true/GCL-free lasts
   the few wind-down instructions.

### CGD1.3 — why the ISS conductor must not enter the legacy drain arms (F13)

`drainInParallelPassively` (`SlotVisitor.cpp:718-735`) branches to
ACTIVE `drainInParallel()` when `numberOfGCMarkers()==1 ||
(m_worldState & mutatorWaitingBit) || !m_heap.hasHeapAccess() ||
worldIsStopped()`. Under ISS: `mutatorWaitingBit` is never set
(CG-I7, legacy bits dead); `Heap::hasHeapAccess()` forwards to
`mainClientHasHeapAccess()` (`Heap.h:405-412`) — when the
conductor IS the main client's thread (common C0/C1 case) it is
access-released all tenure, so the guard fires and the conductor
takes `drainInParallel()`: `donateAndDrain()` visits cells with NO
counter updates, then `drainFromShared(MainDrain)` enters the
counters and steals/drains on hasWork (`:683-690`, `:705`). When
the conductor is a spawned thread, the branch keys on the MAIN
client's unrelated access state — nondeterministic. Failure
modes vs §9.1(2):
- UAF: a foreign §A.3 stop's pause predicate closes while the
  conductor is mid-visitChildren inside donateAndDrain (in neither
  counter, exempted from checkpoints by rev 2) — the §A.3 window
  jettisons/patches under a live visitor.
- Deadlock: a MainDrain-parked conductor IS counted (waiting++ on
  entry, `:616-620`), but rev 2 gave it no checkpoint and
  exempted it — the ack equality is unsatisfiable; the §A.3
  conductor stalls to the 30s watchdog
  (`JSThreadsSafepoint.cpp:401`).
- hasWork-wakeup race: during a pause, shared stacks are non-empty
  exactly because paused helpers donated — a MainDrain conductor
  wakes on hasWork and drains concurrently with the §A.3 window.
`waitForTermination` (`:737-751`) has none of these: pure condvar
loop, no counters, no stealing; with `didReachTermination` gated
on `m_pausedParallelMarkers == 0` (CG-I22) it also cannot exit
mid-pause. Zero-helper case: a passive conductor with
`numberOfGCMarkers()==1` would wait forever (nobody drains), hence
the §3.7 rule that Concurrent is never scheduled there.

## ANNEX CGD2 (BINDING) — rev 4 counter-leak derivation and the F19 reader table

Referenced by SPEC-congc §9.1(2)(c), §5.3(3), CG-I22, CG-T3, CG-T8.

### CGD2.1 — F17: the waiting-counter leak and the CG-I0 benign ruling

Landed shape (`SlotVisitor::drainFromShared`,
`Source/JavaScriptCore/heap/SlotVisitor.cpp:607-710`): every loop
iteration takes `m_markingMutex` and does
`m_numberOfWaitingParallelMarkers++` (:621); the paired decrement
(:688) runs only when the thread re-enters ACTIVE (steal + drain).
The four return paths all exit COUNTED in waiting:
- MainDrain TimedOut (:626) — every in-window
  `drainInParallel(MainDrain)` slice that times out;
- MainDrain Done (:629-630) — every fixpoint-window drain that
  reaches termination;
- HelperDrain TimedOut (:641-642);
- HelperDrain Done on `m_parallelMarkersShouldExit` (:673-674) —
  taken by EVERY helper at EVERY cycle end (`runEndPhase` sets the
  flag and notifies, Heap.cpp:2026-2031; the helper task wrapper,
  Heap.cpp:1828-1847, just returns the visitor).
Writer/reader census (grep-complete over Source/JavaScriptCore):
writers = :621, :688, zero-init Heap.h:1261; readers = the
stealSomeCellsFrom denominator (:682), a diagnostic dataLog
(Heap.cpp:1867). `didReachTermination` (:594-598) reads ONLY
`m_numberOfActiveParallelMarkers` — hence the leak has no
observable effect tip-of-tree beyond steal granularity, and hence
no landed code ever needed the counter balanced.

Wedge interleaving (why rev 3 was a guaranteed deadlock, not a
race): (1) cycle 1, any fixpoint window — the conductor's
in-window MainDrain returns Done, waiting leaks +1; (2) cycle 1
end — every helper Done-returns, leaking +numberOfGCMarkers()-1;
(3) cycle 2 goes Concurrent; a sibling fires haveABadTime/jettison
-> `JSThreadsStopScope` ctor sees phase != NotRunning, calls the
§9.1(2) pause, waits for active==0 && waiting==0 — waiting > 0
forever with no live thread behind it. In the GIL-off conductor
the scope is constructed at VMManager.cpp:561 BEFORE
`requestStart` is sampled at :579, so the wait precedes watchdog
coverage (`watchdogAssertStopProgress`) — silent wedge with GCL
held, blocking every future WND-open and every other §A.3 stop.

CG-I0 disposition of the exit delta: adding waiting-- on the four
return paths changes, flag-off, ONLY the :682 steal denominator
(a work-partitioning hint) and the :1867 diagnostic value; it is
not observable in collection behavior, the bench gates, or any
assert. Ruled BENIGN-DELTA under CG-I0's inspection gate; the
implementing change cites this annex in INTEGRATE-congc.md. (The
alternative — gating the decrement on ISS — was rejected:
divergent counter semantics per mode is exactly the class of
latent trap F17 itself instantiates.)

### CGD2.2 — F19: emitted-code readers of the fence/threshold pair

The complete reader set of the barrier fence/threshold consulted
by EMITTED code (verified rev 4):
- Baseline/common JIT: `AssemblyHelpers.h:2045`, `:2052` bake
  `AbsoluteAddress(vm.heap.addressOfBarrierThreshold())`;
  `:2116` bakes `addressOfMutatorShouldBeFenced()`. `Heap.h:723`
  and `:726` return the addresses of the SERVER members
  `m_mutatorShouldBeFenced` (Heap.h:1204) / `m_barrierThreshold`
  (Heap.h:1209).
- `branchIfBarriered` (AssemblyHelpers.h:2056-2059) loads
  `VM::offsetOfHeapBarrierThreshold()` off a VM register — the
  same server member through the VM.
- FTL: `FTLLowerDFGToB3.cpp:27281` (VM_heap_barrierThreshold),
  `:27323`/`:27355` (VM_heap_mutatorShouldBeFenced) — server
  members via AbstractHeap offsets.
NONE of these read the §5.3(2) GCH client copies; the copies are
consulted only by the re-pointed C++ runtime readers (§5.3(3)).
Therefore a GIL-off fail-safe must pin the SERVER pair: keeping
the landed `setMutatorShouldBeFenced` forcing (Heap.cpp:3928-3940)
when GIL-off makes master AND copies tautological, so every
reader — baked server address, VM-offset load, or client copy —
remains fenced until the §13.3(a) per-client reroute lands. The
rev 3 copies-only pin left every JIT store under-fenced from the
first GIL-off endMarking lower onward.
