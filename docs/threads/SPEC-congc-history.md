# SPEC-congc-history.md

Companion to `SPEC-congc.md` (draft rev 6). Per the frozen-spec
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

- rev 5 (2026-06-07): adversarial round 4 — 8 reviewer findings
  received, 8 distinct, ALL ACCEPTED as real after verification
  against the tree; none refuted. Dispositions (F-numbers cited by
  the rev 5 body):
  - F20 (blocker): the F10 phase-gated guard left one state
    unhandled — the PREDECESSOR conductor's deferred GCA clear.
    VERIFIED: both landed conduct wrappers release GCL BEFORE
    clearing GCA under `*m_threadLock` (runSharedGCElection:
    unlock `Heap.cpp:4533`, clear `:4536`;
    tryConductSharedCollectionForPoll: unlock `:4600`, clear
    `:4603`). Flags-on interleaving (ANNEX CGD3.1): cycle-1
    conductor T1 finishes the final WND-close (phase NotRunning),
    unlocks GCL, is descheduled before its clear; requester T2
    tryLocks `:4523` — the guard predicate is FALSE (phase
    NotRunning, the legitimate pass-through), T2 re-sets GCA,
    restamps `m_gcConductorThread`, conducts cycle 2; cycle 2
    goes between-windows (GCL free, phase Concurrent); T1 resumes
    and clears GCA + the thread MID-CYCLE-2 — every §3.4 guard
    then evaluates false (GCA false, phase Concurrent), so a
    third requester nests a conductor (the exact CGD1.1
    catastrophe F10 closed); cycle-2 followers' `:4550-4554`
    wait predicate wakes spuriously; the §9.2(4)/CG-I21
    discriminators read a nulled owner. Neither §3.4/§3.5 nor
    CGD1.1 considered the predecessor's clear racing a successor
    (CGD1.1's flag-off half analyzed only the new winner
    conducting during wind-down). FIX (body §3.4 WIND-DOWN
    CLEAR): the clear becomes OWNERSHIP-CHECKED — under
    `*m_threadLock`, clear GCA + `m_gcConductorThread` only if
    `m_gcConductorThread == &Thread::current()`; notifyAll
    unconditional; restamp over a non-null owner legal only when
    phase == NotRunning (debug assert). The alternative
    (clearing inside the final WND-close before the GCL release)
    was NOT taken: it removes the flag-off-reachable
    GCA-true/GCL-free wind-down state entirely, a larger CG-I0
    delta than the ownership check (whose flag-off delta —
    followers keep the untimed GCA wait across a takeover — is
    benign-ruled in CGD3.1). §3.5 amended; CG-I21 extended
    (owner-only clear, both identities debug-asserted); CG-T8
    F20 sub-arm (amplifier-descheduled conduct return +
    second-cycle between-windows requester storm).
  - F21 (major): CG-I10's "neither taken with ranks 7-9b held"
    contradicted §5.2's own normative premise that
    `addToRememberedSet` callers may hold rank 7-9b allocation
    locks while taking the CMS lock — the invariant was
    unsatisfiable on the design's hottest path, and CG-T2's lint
    would fail every barrier append under an allocation lock (or
    be silently weakened). The design is sound only under the
    leaf reading: the CMS lock is TERMINAL (the append does
    setCellState + append only — nothing acquired under it), so
    7-9b -> CMS creates no cycle; the dangerous edge is
    exclusively m_markingMutex-with-7-9b, already prevented by
    the F8 donation-site restriction. Rev-2 regression: F8 pinned
    the donation site with the 7-9b rationale and CG-I10's
    wording was never reconciled with the append side. FIX:
    CG-I10 restated as three separable clauses (m_markingMutex
    never under 7-9b; CMS terminal leaf, MAY be under 7-9b;
    m_markingMutex > CMS only at the 7-9b-free drain/donation
    sites); §5.2 parenthetical and the §10 lock-table row
    re-worded; CG-T2 charter encodes the three clauses.
  - F22 (major): under the window split, `finishChangingPhase`'s
    phase store (`m_currentPhase = m_nextPhase`, Heap.cpp:2213)
    executes AFTER resumeThePeriphery (`:2187`) — i.e. after the
    WND-close released GCL per §3.2 as written — making the store
    race every load-bearing reader the spec itself created: the
    §3.4 guards (read under `*m_threadLock` post-tryLock; the
    writer holds neither lock at the store), the §9.1(2)
    stop-scope ctor check, and CG-I4's "phase transitions mutate
    in-window only". The value race is benign mid-cycle (both
    values != NotRunning) but is an unsuppressed TSAN report —
    a guaranteed §12 C1 ladder blocker — and the §3.4 flag-off
    soundness sentence silently assumed store-before-release.
    Flag-off the race does not exist (callers hold GCL across
    the store); the window split creates it, so ruling it is this
    spec's obligation. FIX (body §3.2 PHASE-STORE ORDER): all
    four phase-field updates complete BEFORE the close's GCL
    release, under any §13.2 flag; CG-I4 cross-cites; CG-T8 F22
    sub-arm (stop-scope ctor injected just after a non-final
    WND-close).
  - F23 (major): §3.2's unconditional "THEN release GCL" had no
    final-window carve-out symmetric to F15's entry carve-out:
    GCL release at cycle boundaries lives in the CALLERS
    (`Heap.cpp:4533`, `:4600` — verified), so a literal final
    WND-close release double-unlocks a WTF::Lock not held, and
    CG-I0 REQUIRES the caller unlocks to remain (flag-off CG-1
    builds the close helper). FIX (body §3.2 FINAL-CLOSE
    CARVE-OUT): only closes preceding another window of the same
    cycle release GCL; the final close (the ->NotRunning edge,
    which the `:4852-4863` m_requests-empty exit postdates)
    leaves GCL held for the landed caller (or the CGA2 R7
    collector loop at C2). CG-I12's wording ("never held BETWEEN
    windows") already matched the intent and stands.
  - F24 (major): ANNEX CGA2 R7 specified the C2 collector-loop
    election-equivalent (tryLock GCL, set GCA + thread, conduct)
    with NO §3.4 guard, while §3.4/CG-I21 enumerated a CLOSED
    three-site list — an implementer satisfying both texts ships
    the CGD1.1 nesting at C2 (mid-cycle RCAC tickets are granted
    and unserved until cycle end, so a between-windows collector
    wake wins tryLock against the steady state). FIX: R7 row
    amended below (guard + re-wait); §3.4 and CG-I21 re-worded
    from the closed list to "landed AND stage-added sites";
    CG-T6 R7-guard arm.
  - F25 (major): the C4 assist-visitor lifecycle was unsatisfiable
    for a mid-cycle EXIT1: §9.2 permits between-windows exit and
    §9.2(2) rules HCS removal legal, but §9.2(1)'s teardown order
    had no visitor step; unregistering at DCT violates CG-I14
    (forEachSlotVisitor mutation in-window/between-cycles only),
    destroying without unregistering violates CG-I17 and leaves
    the `:2315-2342` in-window walks dereferencing a dangling
    visitor (UAF); blocking DCT until the next window was
    specified nowhere. The attach side had the needed mechanism
    (§9.3(3) deferral); the exit side had no analog — and
    §9.3(3) itself had no cancellation rule, so
    attach-then-exit in one gap left the conductor registering a
    visitor for a destroyed GCH. FIX (body §9.2(1) C4 DELTA,
    derivation CGD3.3): heap-allocated visitor; deferred
    UNREGISTRATION with ownership transfer to the pending list;
    cancellation of a pending deferred registration; conductor
    applies pending (un)registrations at each WND-open before
    any walk; CG-I14/CG-I17/§7.4 amended; CG-T9
    attach-then-exit-in-one-gap arm.
  - F26 (major): the C3 sweeper was invisible to the §A.3 stop
    protocol: the fan-out counts ENTERED threads (ungil §A.3
    rule 1 registry walk — a standalone client has no lite), and
    the §9.1(2) pause participant set was pinned (F14) to
    HelperDrain helpers — so a §A.3 window (jettison,
    haveABadTime heap iteration) ran concurrently with active
    sweeping, invalidating the frozen SPEC-heap §10A exemption's
    "world stopped" premise (jit R1.i: conductor may write heap
    without access). CG-I13's "no sweeper-vs-window special
    case" covers GC windows only (the sweeper parks via F8/GSP;
    §A.3 stops set no client-visible state per §9.1(5)). FIX
    (body §9.1(7), derivation CGD3.2): per-quantum sweeper ack
    extending the §9.1(2) ctor pause (option (a) of the
    review's two; option (b) — GCL-subordinate quantum
    bracketing — rejected as a new lock-shape on the sweep hot
    path); flag-gated supersession of the heap §10A premise
    recorded as CGS1 row 11; CG-I13 extended; CG-T7 F26 arm.
  - F27 (major, recording defect): ANNEX CGS1 omitted the
    flag-gated supersessions of SPEC-heap's whole-conduct GCL
    tenure — the §10.2 election pseudocode holds GCL across
    conduct (SPEC-heap.md:254-257) and step 9 says "Conductor
    releases GCL" (SPEC-heap.md:274) — and of the §10D step (1)
    pre-check structure (SPEC-heap.md:296: timed loop until
    "...∧ GCL tryLock succeeds ∧ ..."), both contradicted under
    `useConcurrentSharedGCMarking` by §3.1/§3.2/§3.4 and the
    `:5036` row/CGD1.2. CGS1 row 5 covered only the step-7
    banner — a different clause. FIX: CGS1 rows 9-10 added
    below (recorded two-sided at freeze per §13.4); no body
    change beyond the rows' existence (the §13.4 pointer already
    says "list = ANNEX CGS1").
  - Size cap: body compressed to 49983 bytes (<= 50000). Per the
    overflow rule, the §9.1(2) MECHANISM/COUNTER PROTOCOL full
    text was relocated VERBATIM to ANNEX CGP1 (BINDING) below —
    no clause weakened or changed; the body keeps a normative
    summary. Other trimmed sentences survive in this rev log
    (rev 2 F1/F6/F8, rev 3 F13, rev 4 F17-F19 entries) or the
    CGD annexes; the §8.2 CG-I15 termination argument's full
    text lives in ANNEX CGN1 N3 (where it already was, verbatim).

- rev 6 (2026-06-07): adversarial round 5 — 7 reviewer findings
  received, 7 distinct, ALL ACCEPTED as real after verification
  against the tree; none refuted. NOTE: the rev 6 body trims every
  inline "(rev N, F#)" attribution to "(F#)" for the size cap —
  the F-number -> rev mapping is THIS log (F1-F9 rev 2, F10-F16
  rev 3, F17-F19 rev 4, F20-F27 rev 5, F28-F34 rev 6).
  Dispositions:
  - F28 (blocker): back-to-back ticket-drain cycles inside one
    conduct call had NO legal WND-open arm for the successor
    cycle. VERIFIED: the landed step-7 loop
    (`Heap.cpp:4852-4863`-region: `for(;;){ if (m_requests
    .isEmpty()) break; ... collectInMutatorThread(); }`) runs
    whole cycles back-to-back; under C1, F23 makes each cycle's
    final close (->NotRunning) leave GCL HELD and §3.2 places the
    m_requests exit AFTER it — so a non-empty re-check starts
    cycle 2 with GCL held, the conductor access-released, no
    election entry. The F15 FIRST-WINDOW arm is inapplicable
    (requires the landed access-held tryLock entry); the RE-ENTRY
    arm self-deadlocks (blocking acquire of a non-recursive
    WTF::Lock already held). The stop sequence (GSP/
    requestStopAll/GBL barrier, `:4768-4793`) also executes once
    per conduct call above the loop, so the successor had no
    specified re-stop. Mid-cycle tickets (allocation triggers,
    §9.1(4) RCAC) make the multi-cycle conduct COMMON under C1.
    The alternative reading (intermediate cycle ends release GCL)
    was checked and rejected: it contradicts F23's own
    ->NotRunning definition and creates an unruled steady state
    {GCL free, GCA set, NotRunning, unserved tickets} in which
    the §3.4 guard's NotRunning pass-through admits a foreign
    election winner CONCURRENT with the original conductor's
    loop re-entry — two threads in the conduct machinery (the
    CGD1.1 class, invisible to F10/F20 because phase is
    NotRunning). FIX (body §3.1 TICKET-DRAIN SUCCESSOR arm; full
    text ANNEX CGD4.1): GCL ownership TRANSFERS from the
    predecessor's final close to the successor's first WND-open,
    which runs steps (c)-(e) only; §3.2 F23 re-worded
    (caller-release OR transfer); §3.4 gains the inter-cycle
    state ruling (foreign tryLocks FAIL — no guard needed);
    §9.1(1)/(4) re-worded; CG-I12's §A.3 wait bound restated
    (+re-stop +successor first window); CG-T8 F28 sub-arm.
  - F29 (blocker): the F26 sweeper ack was attached to the
    §9.1(2) ctor pause, which is gated on `m_currentPhase !=
    NotRunning` — but C3 incremental sweeping is a BETWEEN-CYCLES
    activity (phase == NotRunning): `notifyIncrementalSweeper()`
    fires in the end phase (`Heap.cpp:2083`),
    `startSweeping`/`scheduleTimer` (`IncrementalSweeper.cpp:152`,
    `:41`) then run timer-sliced quanta while mutators run.
    VERIFIED against the tree (end-phase call site; timer
    re-arming in `doWork`). Consequence as specified: every §A.3
    stop between cycles — the dominant case — skipped the pause
    entirely; the sweeper flag was never set; the §A.3 window's
    heap writes (jit-R1.i exemption) raced live sweep quanta —
    exactly the CGD3.2 hazard F26 was accepted to close, still
    open everywhere except the narrow mid-cycle case. The phase
    gate is sound for the MARKER pause (helpers exist only while
    a cycle is live) but anti-correlated with sweeper activity.
    FIX (body §9.1(2)/(7); ANNEX CGP1 SWEEPER EXTENSION rewritten
    below): the sweeper flag/ack is keyed on
    `useSharedGCIncrementalSweep` && sweeper-client-registered,
    with NO phase gate; the marker pause keeps its gate; CGS1
    row 11 amended; CG-T7's F26 arm re-pinned to phase ==
    NotRunning (the only config in which the defect was
    test-reachable).
  - F30 (blocker): two further holes in the same CGP1 sweeper
    protocol. (1) IDLE-SWEEPER LIVENESS: the ctor's wait
    predicate was the ack bit unconditionally whenever a sweeper
    client is registered — but an idle sweeper (timer-parked, no
    current quantum; the majority state) executes no quantum
    boundary, never polls the flag, never acks: the ctor wedges
    to the 30s watchdog (`JSThreadsSafepoint.cpp:401`), and in
    the GIL-off conductor the scope is constructed at
    `VMManager.cpp:561` BEFORE `requestStart` is sampled (`:580`
    — re-verified; the CGD2.1 shape), so the wedge is a SILENT
    hang with GCL held. The stated bound ("one quantum") silently
    assumed an in-quantum sweeper. (2) QUANTUM-ENTRY RACE: the
    rev 5 text governed only a CURRENT quantum ("on set it
    finishes/aborts the quantum"); nothing forbade STARTING a new
    quantum after the flag was set (the sweeper is invisible to
    the §A.3 fan-out — F26's own premise — and §A.3 stops set no
    client-visible heap state), so a timer wake mid-§A.3-window
    could acquire access/MSPL and mutate
    freelists/newlyAllocated/directory bits concurrently with the
    window — CGD3.2 re-opened through the entry edge. There was
    no entry-side analog of the helper protocol's "ShouldPause
    gates counter (re-)entry" clause. FIX (ANNEX CGP1 rewritten
    below; body §9.1(7) summary): two-bit state machine under
    `m_markingMutex` — `m_sweeperInQuantum` set at quantum ENTRY
    (entry REFUSED while the pause flag is set; access/MSPL only
    after a gated entry), cleared at exit; ctor predicate
    `!m_sweeperInQuantum || acked` (idle sweeper passes with no
    ack); ack lifetime = cleared by the dtor's resume with the
    flag, before the GCL release; bound = one quantum in-quantum,
    zero otherwise; CG-T7 gains the idle-sweeper and
    delayed-timer-entry shapes.
  - F31 (major): in-window conductor-executed write barriers had
    no CMS/fence disposition. VERIFIED: runEndPhase runs
    `iterateExecutingAndCompilingCodeBlocks -> writeBarrier
    (codeBlock)` (`Heap.cpp:2036-2039`) and
    `m_codeBlocks->iterateCurrentlyExecuting -> writeBarrier`
    (`:2085-2088`), reaching `addToRememberedSet` (`:1427`),
    which §5.2 routes via `currentThreadClient()` and whose
    `m_mutatorShouldBeFenced` read (`:1434`) §5.3(3) re-points to
    the current client's copy — undefined (null deref as written)
    for the §7.2 C2 standalone conductor (explicitly NOT a
    client), and unstated for the C0/C1 client-conductor (its
    in-window CMS appends postdate the WND-open drain). Neither
    CGA2's "conductorClient/vm() use" pattern nor CGA1's grep set
    caught these TLS-routed inline-wrapper calls. FIX (body
    §5.2 CONDUCTOR-CONTEXT clause; full text ANNEX CGD4.5; CGA2
    row R9 below; CGA2 audit pattern extended to TLS-client-
    routed calls): null-client barrier executions fall back to
    the SERVER `m_mutatorMarkStack` (`:1479`) + SERVER fence
    master — sound because in-window only (WSAC, single writer);
    debug assert null-client => WSAC; client-conductor in-window
    CMS appends are NEXT-CYCLE grey, drained at the next cycle's
    WND-open (legacy parallel: the End-phase
    `m_mutatorMarkStack` appends consumed next cycle,
    MarkStackMergingConstraint.cpp:64-68 comment); CG-I2 gains
    the conductor-context exemption.
  - F32 (major): `MarkStackMergingConstraint` — the tree's sole
    consumer of `m_mutatorMarkStack` — had no disposition.
    VERIFIED: `quickWorkEstimate` reads the stack's size
    (`MarkStackMergingConstraint.cpp:47`; also `:54`
    prepareToExecuteImpl) and `executeImplImpl` transfers it
    (`:72-73`); the file is in `heap/**` and matches CGA1's
    `m_mutatorMarkStack` grep pattern, so CG-T1 as chartered
    FAILED on it as unclassified — the audit of record was
    incomplete against its own grep. Convergence semantics under
    CMS were unspecified (walk CMSes under what order, vs
    server-only). FIX (body §5.2; CGA1 row A21 below): when C1R
    the constraint covers the SERVER stack (F31
    conductor-context appends) + `m_raceMarkStack` ONLY; CMS work
    is accounted exclusively through the WND-open drain into the
    shared mutator stack, which hasWork/didReachTermination
    already counts (`SlotVisitor.cpp:600-605`); NORMATIVE: the
    §3.1(e) WND-open drain precedes the window's first
    constraint-solver pass (so the estimate is never stale-low
    for CMS work). `!C1R`: today's code.
  - F33 (major): the C0 routing predicate was ambiguous — §5.2
    ("when ISS"), §5.3(3) ("when ISS"), and the §4.1 didRun fold
    were ISS-keyed and landed at C0 (CG-2), while the master rule
    requires flag-off shared mode = today's protocol
    BYTE-FOR-BYTE (Heap.cpp:1479 lock-free append, server fence
    reads). A per-barrier CMS lock flag-off is a protocol AND
    plausible bench delta; the spec's own convention (F17 ->
    CGD2.1; F20 -> CGD3.1) demands an explicit ruling for any
    flag-off delta, and none existed. FIX (ruling ANNEX CGD4.4):
    option (b) — routing is FLAG-GATED via C1R := ISS &&
    `useConcurrentSharedGCMarking` (body notation + §5.2,
    §5.3(3), §4.1); flag-off keeps `:1479` + the server fence
    pair + landed didRun behavior; §9.2(1)/§9.3(1) gain explicit
    !C1R no-op arms; CG-T1 records the gate. Option (a)
    (benign-rule the reroutes and soften the master rule) was
    REJECTED: it weakens the strongest gate in the spec for zero
    benefit (C0 lands the fields either way).
  - F34 (major): the F25 deferral discrimination ("between
    cycles ... directly" in §9.2(1); "while m_currentPhase !=
    NotRunning DEFERS" in §9.3(3)) keyed on phase reads by
    ACT/DCT threads that hold neither GCL nor `*m_threadLock` —
    outside F22's enumerated GCL-ordered reader set. VERIFIED:
    nothing in rev 5 synchronized those reads; the bare load
    races `finishChangingPhase`'s store (`Heap.cpp:2213`) — an
    unsuppressed TSAN report (§12 ladder blocker) — and a
    stale-NotRunning read lets a DCT/ACT mutate
    `forEachSlotVisitor` directly against a cycle whose first
    WND-open is concurrently storing the phase — the CGD3.3(a)
    skip/UAF shape the pending list exists to prevent. FIX (body
    §9.2(1)/§9.3(3) rewritten; full text ANNEX CGD4.3): ACT/DCT
    NEVER read phase and ALWAYS enqueue; the pending list is
    applied only at WND-open (before any walk) or quiesced (GCL
    held + phase NotRunning under `*m_threadLock`: §10D revert /
    server teardown); §3.2 F22's reader note now states the
    rewrite leaves NO other phase reader; CG-I14 re-worded;
    CG-T9 gains the NotRunning -> first-WND-open edge arm.
  - Size cap: body compressed to 49998 bytes (<= 50000). Per the
    overflow rule the trimmed prose survives here or in annexes:
    the §9.1(2) and §9.1(7) summaries defer to ANNEX CGP1 (which
    GOVERNS); the §6.1 mechanism list deduplicated against
    §2.3(a); the inline rev attributions moved to this entry's
    mapping note; the §2.2 per-client cite list collapsed to its
    heap §10A pointer; no normative clause was weakened — F28-F34
    full texts are ANNEX CGD4 + the CGP1 rewrite.

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
4. CG-I12's wait bound: needs a measured bound on window length
   at C1 (fixpoint windows include constraint solving; heap
   §10.6 stack scans are O(threads)). STILL OPEN (CG-T8
   measures); rev 2 adds the per-batch pause granularity that
   makes the marker-pause half derivable; rev 6 (F28) extends
   the measured quantity by the inter-cycle re-stop + the
   successor's first window when a drain successor starts.

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
| A21 | `MarkStackMergingConstraint.cpp:47, :54, :72-73` (estimate/prepare/transfer of `m_mutatorMarkStack`) — added rev 6, F32: the file is in `heap/**`, matches the `m_mutatorMarkStack` pattern, and had no row | FOLDED (§5.2) | When C1R: the constraint covers the SERVER `m_mutatorMarkStack` (F31 conductor-context appends only) + `m_raceMarkStack`; CMS work is accounted exclusively via the §5.2(i) WND-open drain into the shared mutator stack (hasWork/didReachTermination count it, `SlotVisitor.cpp:600-605`); NORMATIVE: the §3.1(e) drain precedes the window's first constraint-solver pass. `!C1R`: today's code (CG-I0). |

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

Charter (rev 2, F7; pattern EXTENDED rev 6, F31): SPEC-congc
§7.2's collector-thread conductor is not a client and never holds
access; the landed conduct path is written against one. Every
conductorClient/vm() use AND every TLS-client-routed call
(`currentThreadClient()` consumers reached from the conduct path,
e.g. inline `writeBarrier` wrappers — the F31 class neither the
old pattern nor CGA1's grep caught) in the conduct path gets a
row; the C2 change adds rows for any site this table misses
(CG-T6 fails on an unclassified site).

| # | Site | Disposition |
|---|---|---|
| R1 | `conductSharedCollection(GCClient::Heap&)` signature (`Heap.cpp:4757`) | Parameter becomes nullable (`GCClient::Heap*`); null = standalone conductor (C2). |
| R2 | Step-3 own-access release (`Heap.cpp:4769-4770`) and tail `conductorClient.acquireHeapAccess()` (`:4955`) | SKIP when null — the collector thread has no access to release/re-acquire; the §10.4 barrier then waits on ALL clients (no "every client except the conductor's own" carve-out). |
| R3 | Main-VM in-window work: `sanitizeStackForVM(vm())` (`Heap.cpp:1704, :2206`), shadow chicken (`:2253-2254`), AtomStringTable scope + TID-rebias teardown (`:4880-4915` area) | KEEP, executed by the conductor thread IN-WINDOW — licensed by heap §10B rule 2 (phase-loop vm() asserts gain `|| WSAC`); the heap T9 audit classified sites for a client conductor, and every such site is in-window, where client-ness is irrelevant. |
| R4 | Step-8 resume pass / per-client TLC loops (`Heap.cpp:4923-4925`) | UNCHANGED — already loop over HCS; the conductor's own (nonexistent) client simply contributes no entry. |
| R5 | §10D revert poll context (`pollIssRevertIfNeeded`) | NOT run by the collector conductor (main client's thread only, heap §10D); no change. |
| R6 | VMTraps poll in `runSharedGCElection` (`Heap.cpp:4562-4572`) | SKIPPED for the collector thread: it never enters a VM, so no JSThreads/debugger conductor ever needs IT parked via traps — its §A.3 compatibility is being access-free + condvar-parked (rev 1 open item 1 RESOLVED). |
| R7 | Collector run loop (`Heap.cpp:333-357`; `shouldCollectInCollectorThread` `:1631-1648`) | REWIRED: wait on the `m_threadCondition`-class signal for granted-unserved tickets (RCAC/activity wakes, SPEC-congc §7.2), then run the election-equivalent (tryLock GCL; AMENDED rev 5, F24 — after tryLock success, under `*m_threadLock`, the §3.4 guard applies: `m_gcConductorActive && m_currentPhase != NotRunning` => unlock GCL and re-wait on the ticket signal (a mid-cycle RCAC ticket against a between-windows C1 cycle otherwise nests a conductor, CGD1.1-at-C2); else set GCA + `m_gcConductorThread`, conduct, drop both per the continuity bound CG-I12; the final-close GCL release is THIS loop's per F23). CG-T6 R7-guard arm. |
| R8 | `m_currentThreadState`/conservative-scan registration | N/A — the collector thread is never a mutator root; it contributes no stack to heap §10.6 gathering (it is not in HCS). |
| R9 | (rev 6, F31) TLS-routed in-window barrier executions: runEndPhase `iterateExecutingAndCompilingCodeBlocks -> writeBarrier(codeBlock)` (`Heap.cpp:2036-2039`) and `m_codeBlocks->iterateCurrentlyExecuting -> writeBarrier` (`:2085-2088`), reaching `addToRememberedSet` (`:1427`, fence read `:1434`) | `currentThreadClient()` is NULL for the C2 conductor: null-client executions append to the SERVER `m_mutatorMarkStack` (`:1479`) and read the SERVER fence master — sound in-window only (WSAC, single writer; consumed next cycle per CGA1 A21); debug assert in `addToRememberedSet`: null client => WSAC. Client-conductor (C0/C1): own-CMS appends, NEXT-CYCLE grey (SPEC-congc §5.2; ANNEX CGD4.5). |

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
9. (rev 5, F27) SPEC-heap §10.2 election pseudocode + §10 step 9
   whole-conduct GCL tenure (SPEC-heap.md:254-257: `GCA = true;
   conduct(); GCL.unlock()`; SPEC-heap.md:274: "Conductor releases
   GCL, re-checks its ticket") — vs SPEC-congc §3.1/§3.2/§3.4
   (GCL per-window, released between windows, re-acquired at each
   WND-open; final close leaves GCL to the caller per F23; the
   election outer shape, follower waits, and GCL-busy rule are
   KEPT). Flag-gated on `useConcurrentSharedGCMarking`.
10. (rev 5, F27) SPEC-heap §10D step (1) pre-check wait structure
   (SPEC-heap.md:296: timed loop until ticket-quiescent ∧
   `m_currentPhase==NotRunning` ∧ GCL tryLock succeeds ∧
   size()==1) — vs SPEC-congc §3.4 `:5036` row / ANNEX CGD1.2 (no
   GCA wait under GCL when phase != NotRunning; the revert
   OUTCOME conditions are unchanged). Flag-gated on
   `useConcurrentSharedGCMarking`.
11. (rev 5, F26; amended rev 6, F29/F30) SPEC-heap §10A jit-R1.i
   exemption premise ("a JSThreads conductor inside its
   JSThreadsStopScope stopped window may WRITE heap memory
   without access (world stopped, GCL held)") — the "world
   stopped" premise is superseded at C3 by SPEC-congc §9.1(7):
   the standalone sweeper is excluded by the FLAG-KEYED,
   phase-independent pause/ack + quantum-entry gate (ANNEX CGP1
   sweeper extension), not the §A.3 fan-out — in-quantum sweeps
   are acked out AND fresh quantum entry is refused for the
   scope's whole tenure, including BETWEEN GC cycles. The
   exemption itself stands once the gate is in place. Flag-gated
   on `useSharedGCIncrementalSweep`.

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

## ANNEX CGP1 (BINDING) — §9.1(2) marker-pause mechanism + counter protocol (full text)

Relocated VERBATIM from the rev 4 body §9.1(2) at rev 5 (size
cap); BINDING per the frozen-spec convention. The body's §9.1(2)
summary and this text are one normative unit; on any perceived
divergence THIS text governs.

MECHANISM (NORMATIVE, rev 3): a NEW pair
`bool m_parallelMarkersShouldPause` + `unsigned
m_pausedParallelMarkers`, both guarded by `m_markingMutex`.
NOT reused: `m_parallelMarkersShouldExit` (one-shot
cycle-terminate, `Heap.cpp:2027`; `SlotVisitor.cpp:664, :673`)
and the `:2315-2342` rightToRun loop (history rev 2, F6).
PARTICIPANT SET (rev 3, F14): the pause pair covers EXACTLY the
helpers inside `drainFromShared(HelperDrain)` — the counters'
only maintainers (`SlotVisitor.cpp:620-621, :687-688`). The
conductor is in no counter and needs no checkpoint (§3.7; window
re-open blocks at the GCL acquire, §3.1(b), held by the foreign
scope); C4 assist visitors take NO checkpoint (§9.1 rule 6).
COUNTER PROTOCOL (rev 3, F16): pause = set ShouldPause +
`notifyAll` `m_markingConditionVariable`, then wait (same
mutex/condvar) until `m_numberOfActiveParallelMarkers == 0 &&
m_numberOfWaitingParallelMarkers == 0`. A pausing helper LEAVES
its counter: (a) the helper-wait `isReady` lambda
(`SlotVisitor.cpp:661-667`) gains `|| shouldPause`; a woken
waiting helper does waiting--, paused++, notifyAll, waits for
`!shouldPause`, then waiting++ and re-evaluates; (b) the helper
drain-batch safepoint (`SlotVisitor.cpp:522`, HelperDrain
visitors only) on shouldPause does `donateAll()` (a PAUSED
HELPER HOLDS NO LOCAL WORK), active--, paused++, notifyAll,
waits, then active++ — granularity = one drained batch (the
CG-I12 bound); (c) EXIT DELTA (rev 4, F17 — a NEW normative
edit; rev 3's "exited helpers leave both counters" was FALSE):
every `drainFromShared` return does waiting-- (TimedOut `:626`,
`:642`; Done `:630`, `:674`) — the landed exits ALL leak the
`:621` increment (sole writers `:621`/`:688`), leaving the
predicate permanently unsatisfiable once any helper exited
(every cycle end, `Heap.cpp:2027`). Flag-off delta = the `:682`
steal denominator only; benign-ruled under CG-I0 (derivation:
ANNEX CGD2.1). Debug assert: active==waiting==paused==0 after
`m_helperClient.finish()`. ShouldPause gates counter (re-)entry
INCLUDING a fresh helper's FIRST `:621` waiting++ (transient:
checkpoint (a) moves it to paused under the same mutex), so the
predicate is stable once reached. `didReachTermination`
(`SlotVisitor.cpp:594-598`) additionally requires
`m_pausedParallelMarkers == 0` — false termination becomes
structurally impossible and waitForTermination stays parked
across the foreign stop (CG-I22). No lost wakeup: every
flag/count write and wait shares `m_markingMutex`. Resume:
clear flag + `notifyAll`. Helpers hold no lock a §A.3 window
needs, so the pause terminates (CG-I16).

SWEEPER EXTENSION (rev 5, F26; REWRITTEN rev 6, F29/F30). The
rev 5 text — "the ctor [the phase-gated §9.1(2) pause]
additionally sets a sweeper-pause flag ...; the C3 sweeper polls
it at each quantum boundary; on set it finishes/aborts the
quantum, drops MSPL, RHA-releases access, acks and parks ...;
the ctor's wait predicate gains the sweeper ack (when the C3
flag is on and a sweeper client is registered); bound: one
quantum" — is SUPERSEDED in full: it rode the phase-gated pause
although sweeping runs between cycles (F29), wedged on an idle
sweeper whose ack never comes (F30(1)), and did not gate quantum
ENTRY (F30(2)); derivations ANNEX CGD4.2 + the rev 6 log.
NORMATIVE, current:

- Gate: when `useSharedGCIncrementalSweep` is on AND a sweeper
  client is registered, the `JSThreadsStopScope` ctor sets
  `m_sweeperShouldPause` under `m_markingMutex` UNCONDITIONALLY —
  no `m_currentPhase` condition (the marker pause keeps its
  phase gate; the two are independent).
- Entry gating (F30(2)): the sweeper sets
  `m_sweeperInQuantum = true` under `m_markingMutex` at quantum
  ENTRY, and the entry is REFUSED while `m_sweeperShouldPause`
  is set (the flag gates quantum entry exactly as ShouldPause
  gates marker counter (re-)entry); a refused sweeper re-arms
  its timer or parks on `m_markingConditionVariable`.
  Access/MSPL acquisition happens only AFTER a successful gated
  entry; the bit clears under the same mutex at quantum exit.
- In-quantum pause: the sweeper polls the flag at each quantum
  boundary; on set it finishes/aborts the current quantum, drops
  MSPL, RHA-releases access, clears `m_sweeperInQuantum`, sets
  `m_sweeperAcked` (same mutex), notifyAll, and parks until the
  flag clears.
- Ctor wait predicate (F30(1)):
  `!m_sweeperInQuantum || m_sweeperAcked` — an idle sweeper
  (timer-parked, between quanta or between sweep lists)
  satisfies it IMMEDIATELY with no ack; no quantum boundary is
  required of an idle sweeper.
- Ack lifetime: the dtor's resume clears `m_sweeperShouldPause`
  AND `m_sweeperAcked` together, BEFORE releasing GCL (same
  clause as the marker resume); no ack survives a scope.
- Bound: one sweep quantum when in-quantum at ctor time; ZERO
  otherwise. No lost wakeup: all four states share
  `m_markingMutex`/`m_markingConditionVariable`.

## ANNEX CGD3 (BINDING) — rev 5 interleavings and derivations

Referenced by SPEC-congc §3.4 (F20), §9.1(7) (F26), §9.2(1)
(F25). Normative reachability proofs; CG-T6/T7/T8/T9 arm them.

### CGD3.1 — F20: the predecessor's stale GCA clear vs a successor cycle

Flags-on (`useConcurrentSharedGCMarking`):
1. Cycle-1 conductor T1 completes the final WND-close (phase ->
   NotRunning, GCL held per F23), returns from
   `conductSharedCollection`, executes the caller unlock
   (`Heap.cpp:4533` or `:4600`), and is descheduled BEFORE its
   deferred clear block (`:4535-4538` / `:4601-4604`).
2. Requester T2 (new granted ticket) tryLocks GCL at `:4523` —
   succeeds; the §3.4 guard is FALSE (GCA true but phase ==
   NotRunning — the designed wind-down pass-through, CGD1.1
   flag-off half); T2 re-sets GCA (already true), restamps
   `m_gcConductorThread = T2`, conducts cycle 2.
3. Cycle 2 reaches Concurrent and releases GCL between windows
   (§3.4 steady state: GCL free, GCA true, phase Concurrent).
4. T1 resumes and executes `m_gcConductorActive = false` + clears
   `m_gcConductorThread` + notifyAll — MID-CYCLE-2.
Consequences without the fix: every §3.4 tryLock site now sees
`GCA == false && phase == Concurrent` => guard false => a third
requester T3 between cycle-2 windows wins tryLock and NESTS a
conductor against the live cycle (the ANNEX CGD1.1 interleaving:
nested requestStopAll, double endMarking/finalize); cycle-2
followers parked on `served < ticket && GCA` (`:4550-4554`,
§9.1(3)) wake spuriously and re-contend; the nulled
`m_gcConductorThread` breaks the §3.4 FOREIGN discrimination, the
§9.2(4) EXIT1 release-assert, and CG-I21's premise (clearing is
not stamping — the "stamped once" assert does not catch this).
FIX: ownership-checked clear (body §3.4). T1's clear compares
`m_gcConductorThread == &Thread::current()` — T2 restamped, so
T1 skips the clear (notifyAll still runs); T2's own clear at its
wind-down matches and clears.
Rejected alternative: moving the clear inside the final WND-close
before the GCL release eliminates the wind-down state entirely —
but that state IS reachable flag-off today (`:4534-4537`), so
removing it is a larger CG-I0 surface than gating the clear; it
also still needs the ownership check for the C2 R7 loop unless
that loop is restructured too.
CG-I0 disposition (flag-off delta of the ownership check): the
stale-clear race EXISTS today flag-off (T1 descheduled between
`:4533` and `:4536`, T2 conducts under whole-cycle GCL, T1's
clear lands mid-T2-cycle). Today's effect: followers in the
untimed GCA wait wake, re-loop, and fall to the timed 1ms branch
(GCA false) — correct but degraded; GCA is not otherwise
load-bearing mid-cycle flag-off (WSAC covers the `:1683` assert;
GCL is held whole-cycle). With the fix: GCA stays true through
T2's cycle, followers stay in the untimed wait, woken by T2's
serve/clear notifyAll — no lost wakeup (same condvar), no
behavior change observable in collection results, bench gates, or
asserts. Ruled BENIGN-DELTA under CG-I0's inspection gate (the
CGD2.1 precedent class); the implementing change cites this annex
in INTEGRATE-congc.md.

### CGD3.2 — F26: sweeper invisibility to §A.3 (reachability)

Config: C3 flag on, GIL-off or GIL-on shared. The §7.3(2) sweeper
is a STANDALONE client (`markStandalone()`+ACT): no VM, no lite,
never "entered". The §A.3 quiescence predicate is a
VMLiteRegistry walk over ENTERED threads
(SPEC-ungil §A.3 rules 1-2: "parked/not-entered/access-released"
per EXIT1 sampling) — the sweeper never appears in it; §A.3 stops
set no client-visible heap state (ungil §A.3.8, §9.1(5)), so the
sweeper's F8/GSP polls see nothing either; the §9.1(2) pause
participant set is pinned to HelperDrain helpers (F14). Hence a
JSThreadsStopScope window (haveABadTime heap iteration, jettison
CodeBlock walks/patching, heap writes under the §10A jit-R1.i
exemption) runs concurrently with a live sweep quantum mutating
the same heap (destructor execution, freelist/newlyAllocated
writes, directory-bit writes). The §9.1(7) ack closes it: the
scope ctor (the ONLY pause site, F18) gains the sweeper flag/ack
(ANNEX CGP1 sweeper extension); CGS1 row 11 records the premise
supersession. Option (b) (sweeper brackets each quantum with
GCL-subordinate state so the ctor's GCL hold excludes quanta) was
REJECTED: it puts a GCL-shaped acquisition on the sweep hot path
and serializes sweeping against GC windows (the sweeper must run
BETWEEN windows; GCL is held in-window — wrong exclusion).

### CGD3.3 — F25: why mid-cycle EXIT1 had no legal visitor path

For a C4 client exiting BETWEEN windows (legal per §9.2): (a)
unregister at DCT => forEachSlotVisitor mutation outside
{in-window, between cycles} — CG-I14 violation racing the
`Heap.cpp:2315-2342` in-window walks; (b) destroy without
unregistering => CG-I17 violation + the next WND-open/close walk
dereferences freed storage (UAF); (c) block DCT until the next
window => unspecified, and the exiting thread has already
permanently dropped access and takes no checkpoint — unbounded
and deadlock-prone vs a cycle whose next window may wait on HCS
state. The deferral mirror of §9.3(3) is the unique remaining
shape; ownership transfer (heap-allocated visitor) is what lets
GCH destruction proceed before the next WND-open applies the
unregistration. The cancellation rule closes the
attach-then-exit-in-one-gap leak (pending registration for a
destroyed GCH). CG-T9 arms both shapes.

## ANNEX CGD4 (BINDING) — rev 6 rulings and full texts

Referenced by SPEC-congc §3.1/§3.2/§3.4 (F28), §9.1(7)
(F29/F30 — see also the CGP1 rewrite), §9.2(1)/§9.3(3) (F34),
§5.2 (F31/F33). CG-T7/T8/T9 + CG-T1 arm them.

### CGD4.1 — F28: the ticket-drain successor arm (full text)

Reachability: the landed conduct body's step-7 loop checks
`m_requests` under `*m_threadLock` and re-runs
`collectInMutatorThread()` until empty (the ticket drain,
`Heap.cpp:4852-4863` region). Under C1, any §A.3 stop (§9.1(4):
RCAC enqueue) or allocation trigger landing between windows
leaves a granted-unserved ticket, so a SECOND full cycle inside
the same conduct call is the common case. At that point, per F23,
the predecessor cycle's final close (->NotRunning) has left GCL
HELD; the world is resumed; GSP/WSAC are clear; the conductor is
access-released; GCA + `m_gcConductorThread` remain stamped.
Neither rev 5 §3.1 arm could open the successor's first window
(F15 arm: no landed election entry exists; RE-ENTRY arm: blocking
acquire of the already-held non-recursive GCL = self-deadlock),
and no clause specified the successor's re-stop (the landed
GSP/requestStopAll/GBL sequence runs once, above the loop).

NORMATIVE (the §3.1 third arm): when the conduct loop observes
`m_requests` non-empty and begins a successor cycle,
- GCL ownership TRANSFERS: the predecessor's F23 final close
  leaves GCL held; the successor's first WND-open performs NO
  GCL acquisition and consists of §3.1 steps (c)-(e) only —
  seq_cst GSP=true, `requestStopAll(GC)`, GBL barrier + WSAC,
  per-client flush. The conductor stays access-released; GCA and
  the owner stamp persist (one tenure spans the whole conduct
  call).
- The inter-cycle state {GCL HELD, GCA set, phase NotRunning,
  world running, unserved tickets} needs no §3.4 guard: every
  foreign tryLock site FAILS on the held GCL (election winner
  falls to the `:4550-4554` follower wait; the poll returns
  false; CGA2 R7 re-waits). The state is bounded by the
  m_requests check (the conductor runs no JS between cycles,
  §3.7).
- Flags-on only (`useConcurrentSharedGCMarking`): flag-off the
  drain loop runs INSIDE the single stop window (the landed
  shape — GSP/barrier once, resume once), and the F28 arm is
  dead code (CG-I0 byte-for-byte).
- REJECTED alternative — releasing GCL between cycles and
  re-entering via the blocking RE-ENTRY arm: in the resulting
  {GCL free, GCA set, phase NotRunning, unserved tickets} state
  the §3.4 guard's NotRunning pass-through (designed for F20
  wind-down takeover) admits a foreign election winner that
  restamps `m_gcConductorThread` and conducts the remaining
  tickets CONCURRENTLY with the original conductor's queued
  blocking acquire — two threads in the conduct machinery with
  trampled owner identity (breaks the §9.2(4) release-assert and
  CG-I21's stamped-once premise). Distinguishing "wind-down
  takeover" from "drain gap" would need a new discriminator;
  retaining GCL needs none.
- CG-I12 consequence (accepted, latency-only): a §A.3
  conductor's GCL wait worst case extends from one window + one
  marker-pause to + the inter-cycle re-stop + the successor's
  FIRST window — GCL is next free at the successor's first
  non-final close (or its caller release if the successor is
  single-window). CG-T8's F28 sub-arm measures it against the
  30s watchdog margin.

### CGD4.2 — F29/F30: sweeper pause gate, idle liveness, entry race

F29 reachability: incremental sweeping is by construction a
between-cycles activity — `notifyIncrementalSweeper()` fires at
cycle end (`Heap.cpp:2083`, end phase, after
deleteUnmarkedCompiledCode), `startSweeping` schedules the timer
(`IncrementalSweeper.cpp:152`, `scheduleTimer` `:41`), and quanta
run in timer slices while mutators run, i.e. while
`m_currentPhase == NotRunning` (CGD3.2 itself: the sweeper "must
run BETWEEN windows"). The rev 5 ack rode the §9.1(2) ctor pause,
which fires only when `m_currentPhase != NotRunning` — so the
common-case §A.3 stop (between cycles) set no sweeper flag and
awaited no ack: the CGD3.2 race shipped everywhere except the
narrow mid-cycle case, CG-I13's ack clause was unsatisfiable as
specified, and CGS1 row 11's premise was false between cycles.
The phase gate is correct for the MARKER pause (HelperDrain
helpers exist only while a cycle is live) and stays.

F30(1) idle liveness: between quanta the sweeper is parked on
its scheduler with no access and no MSPL; once the sweep list is
exhausted it schedules nothing until the next cycle. An
ack-bit-only predicate ("when ... a sweeper client is
registered") therefore waits on a thread that will never reach a
quantum boundary — with C3 on, every §A.3 stop taken while the
sweeper is idle (the majority state) wedges to the 30s watchdog
(`JSThreadsSafepoint.cpp:401`), and via the `VMManager.cpp:561`
conductor (scope constructed BEFORE `requestStart` is sampled,
`:580` — the CGD2.1 shape) it is a SILENT process hang with GCL
held. Same guaranteed-liveness-failure class as F17, in the
mechanism F26 added.

F30(2) entry race: the rev 5 text governed only a CURRENT
quantum; the sweeper is invisible to the §A.3 fan-out (no lite —
F26's premise) and §A.3 stops set no client-visible heap state
(§9.1(5)), so nothing stopped a timer-woken sweeper from
acquiring access + MSPL and STARTING a fresh quantum inside an
open §A.3 window — freelist/newlyAllocated/directory-bit
mutation concurrent with the window's heap writes, the CGD3.2
race re-opened through the entry edge. There was no
happens-before from the ctor's flag store to an entry-side poll
without requiring the entry check under `m_markingMutex` — the
sweeper analog of CGP1's "ShouldPause gates counter (re-)entry"
clause, which the rev 5 extension lacked.

FIX: the ANNEX CGP1 SWEEPER EXTENSION rewrite (two-bit state
machine, flag-keyed not phase-keyed, gated entry, idle-pass
predicate, dtor-owned ack lifetime, derived bound). CG-T7's
three shapes pin: between-cycles mid-quantum (phase
NotRunning), fully idle (no stall), delayed timer into an open
window (entry refused).

### CGD4.3 — F34: no raced phase read in the visitor lifecycle

Defect: rev 5's two deferral discriminators — §9.3(3) "ACT while
m_currentPhase != NotRunning DEFERS" and §9.2(1) "between cycles
DCT (un)registers directly" — were phase reads by threads holding
neither GCL nor `*m_threadLock`, absent from F22's enumerated
GCL-ordered reader set (an EXIT1 thread has permanently dropped
access and never conducts, CG-I18/§3.7; the ATTACH side's GBL
section orders nothing against the GCL-ordered phase store).
Consequences: (1) the bare load vs `finishChangingPhase`'s
in-window store (`Heap.cpp:2213`) is an unsuppressed TSAN report
— a §12 ladder blocker; (2) a stale-NotRunning read at a cycle
edge lets ACT/DCT mutate `forEachSlotVisitor` DIRECTLY against a
live cycle's first WND-open — racing the in-window walks
(`Heap.cpp:2315-2342`), the CGD3.3(a) skip/UAF shape. (The
stale-Concurrent direction is benign over-deferral.)

NORMATIVE: the predicate is CLOSED, not synchronized — ACT and
DCT never read the phase and never mutate the visitor set.
Registration and unregistration ALWAYS enqueue to the pending
list under `m_parallelSlotVisitorLock` (exit cancels a pending
registration; ownership transfers per F25). The pending list is
applied, under `m_parallelSlotVisitorLock`, at exactly two kinds
of point: (a) every WND-open, by the conductor, BEFORE any
`forEachSlotVisitor` walk; (b) quiesced application by a thread
holding GCL with `m_currentPhase == NotRunning` verified under
`*m_threadLock` (the §10D revert path and server teardown drain
the remainder — both already hold that guard shape per the §3.4
`:5036` row and heap §10D step 1). A pending registration before
application only delays assist eligibility
(`performIncrement` checks registration); a pending
unregistration only delays storage reclamation (owned by the
list). F22's reader enumeration is unchanged — the rewrite
ADDS no phase reader. CG-T9's F34 arm fires ACT/DCT
amplifier-descheduled across the NotRunning -> first-WND-open
edge.

### CGD4.4 — F33: the C1R routing predicate (flag-off ruling)

Defect: §5.2 (CMS reroute), §5.3(3) (fence/threshold consumer
re-point), and §4.1 (didRun fold) were conditioned on ISS and
landed at C0 (CG-2) — flag-off shared mode would stop executing
the landed lock-free append (`Heap.cpp:1479`) and the landed
server reads (`:1434`, `:3324`), taking a per-slow-path lock
instead: a protocol delta AND a plausible bench delta against
the master rule's BYTE-FOR-BYTE bar and the CG-T1 gate, with no
benign ruling (the convention every other flag-off delta
followed: F17 -> CGD2.1, F20 -> CGD3.1).

RULING (option (b); option (a) — benign-rule the reroutes and
soften the master rule to "protocol-equivalent" — REJECTED: it
weakens the spec's strongest gate to save a predicate): all
three routings are gated on C1R := ISS &&
`useConcurrentSharedGCMarking`. Flag-off (= ISS && !flag): the
C0 change may LAND the GCH fields and helper code, but
`addToRememberedSet` keeps `:1479`, the fence/threshold
consumers keep the server pair, and didRun keeps the landed
shared-mode behavior — byte-for-byte. The C1R-conditional
clauses get explicit flag-off arms: the §9.2(1) exit CMS-flush
and dead-publication steps and the §9.3(1) attach
fence-snapshot/FEP-stamp are NO-OPS when !C1R (the copies are
unrouted, unread state; CG-I0). The §5.2 WND-open drain and
§5.3(2) republish loops iterate empty/unrouted state flag-off
and are vacuous by construction. CG-T1 verifies: flags-off
corpus byte-identical AND BENCH.md flags-off delta = 0 with the
C0 infra landed.

### CGD4.5 — F31: conductor-context barrier disposition (full text)

The conduct path itself executes write barriers in-window:
runEndPhase's `iterateExecutingAndCompilingCodeBlocks ->
writeBarrier(codeBlock)` (`Heap.cpp:2036-2039`) and the
finalize-side `m_codeBlocks->iterateCurrentlyExecuting ->
writeBarrier` (`:2085-2088`), reaching `addToRememberedSet`
(`:1427`; fence read `m_mutatorShouldBeFenced` `:1434`). Under
§5.2/§5.3(3) as rev 5 wrote them these route via
`currentThreadClient()` — NULL for the §7.2 C2 standalone
conductor (not a client): both the append routing and the fence
read were undefined (null deref as written). For a C0/C1
client-conductor the appends land in its OWN CMS after that
window's WND-open drain already ran — their fate was unstated.
Neither ANNEX CGA2's "conductorClient/vm() use" pattern nor
CGA1's grep set (the call sites name only `writeBarrier`, an
inline wrapper) caught these sites — both audit patterns are
extended (CGA2 charter; CGA1 A21).

NORMATIVE: (a) NULL-CLIENT (conductor-context) barrier
executions append to the SERVER `m_mutatorMarkStack` (`:1479`)
and read the SERVER fence master/threshold. Soundness: they
occur ONLY in-window (WSAC set — single writer, no concurrent
client appends to the server stack; the server fence pair is the
master §5.3(1) already mutates in-window). Debug assert in
`addToRememberedSet`: `currentThreadClient() == nullptr` implies
WSAC. (b) A CLIENT-conductor's in-window appends go to its own
CMS and are NEXT-CYCLE GREY: drained at the NEXT cycle's
WND-open (§5.2 drain (i)). This matches the landed semantics —
End-phase `m_mutatorMarkStack` appends are deliberately left for
the next cycle's merging constraint
(MarkStackMergingConstraint.cpp:64-68 comment; CGA1 A21) — and
GC correctness never depended on same-cycle consumption of
end-phase barrier appends. (c) CG-I2 carries the matching
exemption (in-window conductor-context appends are not
owner-access-thread appends; WSAC single-writer covers them).
