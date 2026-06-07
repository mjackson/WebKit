# SPEC-congc.md - N-MUTATOR CONCURRENT GC (draft rev 2)

Status: DRAFT rev 2 — freezes after the adversarial pass (history =
`SPEC-congc-history.md`; BINDING annexes live there per the
frozen-spec convention). Authorities: THREAD.md;
SPEC-{heap,vmstate,objectmodel,jit,api,ungil}.md (+annexes);
UNGIL-HANDOUT.md rev 32. Charter: SPEC-heap Deviation 4
(SPEC-heap.md:23) deferred the concurrent-GC protocol for N
mutators; shared mode today is synchronous, conductor-driven STW
with parallel marking inside the stop. This spec designs the
re-enable. Verified vs tree 2026-06-07 (branch `jarred/threads`).
heap:/ungil:/om:/jit: = the SPEC files; CG-I* = invariants (§11);
CG-T* = test charters (§12); ANNEX CG* = history annexes.

Master rule: every stage is a MODE. Flag-off (all §13.2 options
false) = today's shared §10 protocol BYTE-FOR-BYTE (CG-I0, the
heap I10 analog one level up); `!useSharedGCHeap` or `!ISS` =
today's legacy Riptide protocol, untouched. Nothing here edits
GIL-on/flag-off observable behavior.

Notation: inherits heap §8 (SPEC-heap.md:8): WSAC, MSPL, BVL, GCL,
GBL, GBC, GSP, GCA/GEC, ISS, AHA/RHA, SINFAC, CSAC/RCAC, CIND,
ACT/DCT, BD, LA, TLC, HCS, GCH. New: WND = a conducted stop window
(§3); CMS = per-client mutator mark stack (§5.2); FEP =
`m_barrierFenceEpoch` (§5.3).

## 1. Scope

Deliverables: design for re-enabling, over the shared server heap
(heap §4), the five features Deviation 4 disabled
(SPEC-heap.md:23): concurrent marking, collector continuity,
incremental mutator assist, activity-callback collection,
mutator-concurrent sweeping — staged per §7. Non-goals: changes to
the object-model concurrency protocol (om: frozen; consumed),
the §A.3 thread-granular stop machinery (ungil: frozen; composed
with in §9), TLC layout/JIT addressing (heap §5.3 / ungil §B.4
frozen), the epoch reclamation contract (heap §11; composed with),
wasm-GC (heap §5.5 stands).

## 2. Ground truth

### 2.1 The legacy one-mutator handshake (what must generalize)

`Heap::m_worldState` (`heap/Heap.h`; scribbled at
`Heap.cpp:521`) packs FOUR bits for exactly ONE mutator:
`hasAccessBit`, `stoppedBit`, `mutatorHasConnBit`,
`mutatorWaitingBit` (state-machine asserts `Heap.cpp:2354-2384`).
Consumers: `stopTheMutator()` (`Heap.cpp:2348`),
`resumeTheMutator()` (`:2390`), `stopIfNecessarySlow()` (`:2421`),
`collectInMutatorThread()` (`:2461`), `waitForCollector()`
(`:2497`), `acquireAccessSlow()` (`:2534`), `releaseAccessSlow()`
(`:2601`), conn relinquish (`:2652-2680`), `handleNeedFinalize()`
(`:2688`), `notifyThreadStopping()` (`:2747`). Single-mutator
periphery: `m_mutatorDidRun` (`:2234-2237`, `:2433`, `:2519`,
`:2594`), the ONE `m_mutatorSlotVisitor` (`Heap.h:1182`), the ONE
unsynchronized `m_mutatorMarkStack` (`Heap.h:1183`; appended
lock-free at `Heap.cpp:1479`), single server
`m_mutatorShouldBeFenced`/`m_barrierThreshold`
(`Heap.h:722-726, 1209`), `SlotVisitor::m_mutatorIsStopped` +
`m_rightToRun` (`SlotVisitor.h:166-168, 236-239`),
`sanitizeStackForVM(vm())` (`Heap.cpp:1704`, `:2206`, `:2675`),
shadow-chicken update (`:2253-2254`). Full audit of every
"the mutator"-singular site with its N-ary disposition: ANNEX CGA1
(BINDING), indexed in §4.3.

### 2.2 The shared-mode machinery (what we generalize INTO)

Landed per heap §10: ticketing `requestCollectionShared()`
(`Heap.cpp:4479`; conn bit set idempotently `:4499`), election
`runSharedGCElection()` (`:4507`; GCL-busy timed wait `:4554`),
poll-conduct `tryConductSharedCollectionForPoll()` (`:4578`),
`conductSharedCollection()` (`:4757`): GSP store (`:4768`), §10.4
access barrier under GBL sampling every client seq_cst
(`:4780-4793`), ticket-drain loop (`:4852-4863`),
`runSafepointHooksAndReclaim()` (`:4870`, `:4961-5008`), step-8
client resume pass (`:4923-4925`), WSAC/GSP clear + GBC broadcast
(`:4945-4950`), VMM resume (`:4955`). Per-client state:
`m_accessState`/`m_accessOwner` with F8 Dekker AHA
(`Heap.cpp:5656-5795`) and RHA (`:5797-5838`), SINFAC (`:5107`),
park hooks (`:5390`, `:5431`), `JSThreadsStopScope` (`:5456`),
HCS (`HeapClientSet.h:48-100`), TLC stop/resume/teardown
(`GCThreadLocalCache.h:84-91`), epoch (`GCSafepointEpoch.h:61-125`).
Deviation-4 kill switches in the tree: no Concurrent phase when ISS
(`Heap.cpp:1957-1958`, assert `:1979`), collector thread quiesced
(`:1636-1648`, `:1686`, `:2350-2352`, `:2392-2393`), assist off
(`:3950-3951`), activity callbacks off (`:790-792`, async reroute
`:1595-1600`), always-fenced (`:3936-3937`), IncrementalSweeper off
(conduct banner `:4832-4834`).

### 2.3 The mechanisms that already carry concurrency

(a) Versioned liveness: `MarkedSpace::m_isMarking`
(`MarkedSpace.h:187, 243`), `isLive`'s marking-aware path
(`MarkedBlock.cpp:59-106`), newlyAllocated stamping in
`specializedSweep` when `isMarking` (`MarkedBlockInlines.h:154,
186, 244, 281`), freelist→newlyAllocated conversion in
`MarkedBlock::Handle::stopAllocating` (`MarkedBlock.cpp:201-227`).
(b) Mark/barrier races: `m_raceMarkStack` under
`m_raceMarkStackLock` (`Heap.h:1169, 1184`), `aboutToMarkSlow`
(asserts `isMarking`, `MarkedBlock.cpp:345`), the re-whiten CAS
protocol (`Heap.cpp:1444-1467`). (c) Multi-window cycles:
`m_currentPhase` persists across windows;
`finishChangingPhase()` (`Heap.cpp:2169-2209`) pairs
`resumeThePeriphery()`/`stopThePeriphery()` (`:2287`, `:2217`) on
phase-concurrency edges; `resumeThePeriphery`'s rightToRun loop
already iterates ALL visitors (`:2315-2342`). These are reused, not
redesigned.

### 2.4 Temporary diagnostics

The fix-shared-heap-corruption instrumentation
(`Heap.cpp:993-1020`, `:1201-1244`, `:2258-2282`) is stop-mode-only
(O(blocks)/O(roots) walks inside the window). §7 stages may keep
them only behind `verboseSharedGCHeap`-class options; CG-T11 reuses
their checks as debug asserts.

## 3. Architecture: the window model

A shared collection becomes ONE GCL TENURE containing a SEQUENCE of
stop windows (WNDs) instead of one monolithic window.

1. WND-open = §10 steps 3-4 + flush, ORDER NORMATIVE (rev 2): the
   conductor (a) is access-RELEASED (released at the first
   WND-open, stays released all tenure, §3.7); (b) acquires GCL —
   a BLOCKING acquire is legal exactly because the thread is
   access-released, so it satisfies any concurrent §A.3 fan-out
   (ungil §A.3 rule 2) and the HBT4 release-access-before-GCL
   order (ungil §A.3.3) EXTENDS to window re-entry; (c) seq_cst
   `GSP=true`; (d) `VMManager::requestStopAll(GC)`; (e) GBL barrier
   until every client NoAccess (F8 unchanged —
   `Heap.cpp:4768-4793`); set WSAC; per-client flush (§5.2 drain,
   §6.2 allocator stop via the directory lists, exactly today's
   `stopThePeriphery()` route). Rev 1's GCL-before-release order
   deadlocked against an interleaved `JSThreadsStopScope` —
   REJECTED (full interleaving: history rev 2, finding F9);
   (a)-(c) is the pin; CG-T8 arms it.
2. WND-close = §10 steps 8-9: client cache resume pass; ISB
   generation bump when gilOffProcess (`Heap.cpp:4940-4943` —
   REQUIRED at EVERY window close, not once per cycle: each window
   may jettison/patch; extends the ISB1.1 cite at `:4927-4939`);
   clear WSAC; seq_cst `GSP=false`; GBC broadcast;
   `requestResumeAll(GC)`; THEN release GCL (CG-I12). The conductor
   does NOT re-acquire its own access at non-final WND-closes; the
   landed tail re-acquire (`Heap.cpp:4955`) runs only after the
   cycle's FINAL WND-close. Heap-resume-before-VMM-resume stays
   normative (heap §10 tail).
3. Between windows: mutators run; marking helpers may run (§7 C1);
   `m_currentPhase ∈ {Fixpoint, Concurrent, Reloop, End...}`
   persists, exactly the legacy multi-window shape (§2.3(c)).
   `m_worldIsStoppedForAllClients` is false between windows;
   WSAC-gated asserts (heap I5) remain correct because everything
   they guard stays in-window (§8.1).
4. Tenure: the conductor keeps GCA=true and remains the elected
   §10.2 winner across all windows of the cycle. GCL itself is
   RELEASED between windows and re-acquired at each WND-open
   (CG-I12) — this is what lets a JSThreads stop interleave (§9.1).
   Other GC requesters never start a second cycle: election
   followers key on GCA (`Heap.cpp:4550-4554`), and
   `tryConductSharedCollectionForPoll` re-checks granted-vs-served
   under `*m_threadLock` with GCA visible (`:4589-4593`); a
   GCA-true tryLock winner is impossible mid-window (conductor
   holds GCL) and between windows finds GCA set and backs off
   (one-line guard added to `:4590`).
5. Conductor identity: stays `GCConductor::Mutator` running the
   `collectInMutatorThread()` phase loop (heap §10B.2) in stages
   C0-C1; stage C2 introduces a collector-thread conductor (§7.2).
   GCA gains an owner: `m_gcConductorThread` (Thread*, guarded by
   `*m_threadLock` next to the bare `m_gcConductorActive` bool,
   `Heap.h:1290`), stamped/cleared with GCA. Consumers: the §3.4
   `:4590` guard reads "GCA set => back off" for FOREIGN threads
   (the conductor itself never polls mid-cycle, rule 7); the §9.2
   EXIT1 assert (a detaching client's thread is never the live
   conductor); debug asserts.
6. Flag-off degenerate case: with all §13.2 flags false the cycle
   has exactly ONE window (WND-open, drain to completion at the
   fixpoint per `Heap.cpp:1957-1958`, WND-close) — i.e. it IS
   today's `conductSharedCollection`. CG-I0 is checked by code
   inspection + the §12 bench/behavior gates, mirroring heap I10's
   discipline.
7. Conductor tenure contract (NORMATIVE, rev 2 — CG-I19).
   Conducting is a CLOSED LOOP: from the first WND-open to the
   final WND-close the conductor thread executes ONLY the phase
   loop. It holds no heap access for the whole tenure (released at
   the first WND-open step (a); re-acquired only at the landed tail,
   `Heap.cpp:4955`); it runs no JS, performs no RHA/AHA, and may
   not EXIT1 (its thread is `m_gcConductorThread`; ungil §B.2
   teardown on it is a release-assert violation mid-cycle).
   BETWEEN windows it waits exactly where the legacy collector-conn
   waits: the Concurrent phase runs the COLLECTOR-arm shape
   (`drainInParallelPassively`, `Heap.cpp:1997-2002` —
   MainDrain wait on `m_markingConditionVariable` under
   `m_markingMutex` with the `m_scheduler->timeToStop()` timeout,
   `SlotVisitor.cpp:623-636`) EVEN though its conn class is
   `GCConductor::Mutator`; the legacy Mutator-arm
   (`Heap.cpp:1984-1996`, "served by allocation polls") is NOT used
   when ISS — there is no SINFAC-resume machinery to invent, no
   poll predicate, and no foreign wake-up dependency. Progress
   wake-ups are the landed ones: helpers reaching termination
   `notifyAll` `m_markingConditionVariable`
   (`SlotVisitor.cpp:629, :645`), and the scheduler timeout
   bounds each gap. Every between-window wait is on a condvar with
   no lock held across the wait body and the thread access-released
   — i.e. §A.3-compatible: the fan-out never needs this thread to
   poll (resolves the §9.1 liveness pin's conductor case). Cost
   (accepted, C0-C1): the conducting mutator thread is unavailable
   to JS for the cycle; C2 moves conducting off mutators.

## 4. Handshake generalization (charter item 1)

### 4.1 What replaces each legacy bit

- `hasAccessBit` -> per-client `m_accessState` (landed, heap §10A).
- `stoppedBit` -> GSP + the GBL barrier + WSAC (landed, F7/F8).
  The N-ary "stop requested / all out / resume" cycle per window is
  §3.1-3.2; NO new per-client stop bit is introduced — the F8
  Dekker pair already gives each client an individually sound
  park/revert path (AHA steps 0-3, `Heap.cpp:5707-5758`), and the
  GC-park hooks already pair release/re-acquire per thread
  (`:5390-5452`, ungil §A.3.8).
- `mutatorWaitingBit` (mutator waits for the collector to finish a
  window) -> the existing F8 blocking in AHA step 3 / SINFAC. No
  ParkingLot on `m_worldState`; clients block on GBC.
- `mutatorHasConnBit` -> GCA + the §10.2 election (landed). Stage
  C2 re-splits it (§7.2): "conn = collector thread" becomes GCA
  held by the collector-thread conductor; the bit itself stays
  set-idempotent (`Heap.cpp:4499`) and the `checkConn` assert
  (`:1683`) keeps its `|| WSAC` form.
- `m_mutatorDidRun` -> per-client `GCH::m_didRunSinceLastWindow`
  (plain byte, written only by the owning thread while it holds
  access — same discipline as heap I17's per-client deferral
  depth). Set in AHA's success tail and in SINFAC's hot-poll exit.
  Conductor folds with OR over `clientSet().forEach` at each
  WND-open into the legacy `m_mutatorDidRun` consumer
  (`Heap.cpp:2234-2237` version bump), then clears each byte
  in-window. Scheduling-only state: relaxed is sufficient; the
  window barrier orders it (CG-I9).

### 4.2 Per-client states folded into existing machinery

No new per-client state machine is introduced. The per-client
record for concurrency is exactly: `m_accessState` (landed),
`m_didRunSinceLastWindow` (§4.1), CMS (§5.2), barrier-fence copy +
epoch (§5.3), per-client assist visitor + balance (§7.4),
`m_localEpoch` (landed, heap §11). All conductor iterations over
clients run either under HCS `m_lock` (rank 6) or while WSAC
(HeapClientSet.h:46-47) — add/remove freeze inside windows per
heap I13 (HeapClientSet.h:54-76), which is what makes the per-window
fold/clear loops sound against attach/detach (§9.3).

### 4.3 The "the mutator"-singular audit

ANNEX CGA1 (BINDING) enumerates every singular site found in
`heap/**` with disposition ∈ {LANDED-N-ARY (already per-client),
WINDOW-CONFINED (touched only in-window; no change), FOLDED
(per-client copy + window fold; §4.1/§5), CONDUCTOR-PRIVATE
(conductor-thread state), STAGE-GATED (per stage §7),
VM-SINGULAR-DEFERRED (per-VM not per-thread; tracked by
ungil/vmstate, e.g. `sanitizeStackForVM(vm())` `Heap.cpp:1704`,
shadow chicken `:2253`)}. Implementation consumes the table
verbatim; CG-T1 re-greps the tree and fails on any unclassified
match of the audit patterns (listed in the annex header).

## 5. Write barrier from N threads (charter item 2)

### 5.1 Inline barrier

Unchanged in shape: `HeapInlines.h:124-135` threshold check +
`writeBarrierSlowPath` (`Heap.cpp:3322`), `mutatorFence()`
(`HeapInlines.h:138`). What changes is (a) where the slow path
appends (§5.2) and (b) where the threshold lives (§5.3).

### 5.2 Per-client mutator mark stack (CMS)

`addToRememberedSet` (`Heap.cpp:1427`) appends to the single
`m_mutatorMarkStack` lock-free (`:1479`) — sound only with one
mutator. Rule: when ISS, GCH gains `m_mutatorMarkStack`
(MarkStackArray) + leaf `Lock m_mutatorMarkStackLock`;
`addToRememberedSet` routes via `currentThreadClient()` (heap
§10A.1; same dispatch as `deferralDepthSlot()`/`mutatorStateSlot()`,
`Heap.h:1061, 1095`) and appends under the client's own lock
(uncontended in steady state: the only other acquirer is a drain).
Drains: (i) at every WND-open the conductor transfers every CMS
into `m_sharedMutatorMarkStack` under `m_markingMutex`
(`SlotVisitor::donateAll` shape, `SlotVisitor.cpp:753-761`);
(ii) out-of-window (stage C1+), a client whose CMS exceeds NEW
`Options::sharedGCMutatorMarkStackDonationThreshold` cells (§13.2;
default = one segment, `GCSegmentedArray::s_segmentCapacity` over
4KB blocks, `GCSegmentedArray.h:62, :116`) donates directly under
`m_markingMutex` from its own thread. Trigger SITE is normative:
the check runs ONLY at the SINFAC hot poll tail
(`Heap.cpp:5107-5149`, after the GSP leg, client holding access) —
never inside `addToRememberedSet` itself, whose callers may hold
rank 7-9b allocation locks, which CG-I10 forbids under
`m_markingMutex` (lock order: client CMS lock is leaf, taken
inside `m_markingMutex`; m_markingMutex > CMS lock; never both
with any rank 7-9b lock held; SINFAC's I6 precondition "no rank
>= 4 lock held", `Heap.cpp:5125-5127`, makes the poll site legal
by construction). Donation is latency-only (WND-open drains give
correctness); rev 1's `minimumNumberOfCellsToKeep` cite was a
nonexistent option (history rev 2, F8). `!ISS`:
server stack, today's code (CG-I0). `m_barriersExecuted++`
(`Heap.cpp:1432`) stays a racy diagnostic counter (documented
benign; relaxed). The cellState CAS protocol (`:1444-1467`) is
already N-safe (single-word CAS vs collector; the comment's
monotonicity argument is mutator-count-independent) — CG-T3
TSAN-exercises it.

### 5.3 Fence/threshold versioning (per-client mutatorShouldBeFenced)

Today: single `m_mutatorShouldBeFenced` + `m_barrierThreshold`
(`Heap.h:722-726, 1209`), forced tautological once ISS
(`Heap.cpp:3936-3939`) — every barrier fences forever, even between
collections. Re-enable rule:

1. Server master pair stays; it is mutated ONLY inside a window
   (`beginMarking` raise `Heap.cpp:1111`; `endMarking` lower
   `:1247`; `setMutatorShouldBeFenced` drops its ISS forcing once
   `useConcurrentSharedGCMarking` — the §2.2 kill switch this spec
   retires) — plus a server `Atomic<uint64_t> m_barrierFenceEpoch`
   (FEP) bumped (release) at each in-window mutation.
2. GCH gains `m_mutatorShouldBeFenced` + `m_barrierThreshold` +
   `m_fenceEpochSeen`. The conductor republishes master->client for
   EVERY client inside the SAME window that mutated the master
   (under WSAC, before WND-close), stamping `m_fenceEpochSeen =
   FEP`. Clients never write these fields.
3. Consumers re-point: `mutatorShouldBeFenced()`/
   `barrierThreshold()` read the CURRENT CLIENT's copy when ISS
   (else server, CG-I0). JIT: baked
   `addressOfBarrierThreshold()`/`addressOfMutatorShouldBeFenced()`
   (`Heap.h:723, 726`) become per-client addresses — GIL phase: the
   main client's (one mutator, identical values); GIL-off: the
   lite-resolved client's, an A16-class lite-indexed reroute —
   CHARTERED to jit/ungil owners, recorded in the integration row
   (§13.3); until it lands, GIL-off stages C1+ keep clients
   ALWAYS-FENCED (copies pinned tautological) so stale baked
   addresses cannot under-fence (fail-safe direction).
4. Soundness direction (CG-I3): a RAISE (black->tautological) is
   complete for all clients before the window that performed it
   closes — i.e. before any marking visit can run concurrently with
   a mutator. A LOWER may only happen in the cycle's final window
   (endMarking), after marking terminated; a client running fenced
   "too long" is always sound. Debug assert at every WND-close:
   `client.m_fenceEpochSeen == FEP` for all clients.
5. `reportExtraMemoryAllocatedPossiblyFromAlreadyMarkedCell`
   (`Heap.cpp:706-740`) and `writeBarrierSlowPath` (`:3322-3333`)
   read the same per-client state; their full-collection re-whiten
   reasoning is unchanged (§5.2).

## 6. Black allocation during marking (charter item 3)

### 6.1 The mechanism is already allocator-local

Allocate-black in Riptide = versioned newlyAllocated, not a
per-cell mark at allocation: (a) sweep-to-freelist during marking
stamps the block's live set newlyAllocated
(`MarkedBlockInlines.h:244, 281`); (b) each WND-open's
`stopAllocating()` converts every in-flight freelist remainder to
newlyAllocated (`MarkedBlock.cpp:201-227`), making cells handed out
since the sweep live; (c) `isLive` consults
`marksConveyLivenessDuringMarking` + newlyAllocated versions
(`MarkedBlock.cpp:59-106`). All of (a)-(c) key on
`MarkedSpace::isMarking()` (`MarkedSpace.h:187`).

### 6.2 N-client rules

1. `m_isMarking` flips ONLY inside a window (`beginMarking`
   `Heap.cpp:1103-1112` / `endMarking` `:1190-1248` already run
   in-window). The window's F8 resume edge (seq_cst GSP clear ->
   client's seq_cst GSP load in AHA) publishes the flip to every
   client before it can allocate again — no additional fence
   (CG-I4).
2. Every sweep-to-freelist and steal stays under MSPL (heap
   §5.2/I8; `assertSharedAllocatorMutationIsSafe`,
   `LocalAllocator.cpp:46`); since MSPL holders hold access, no
   sweep straddles a window, so each sweep observes a stable
   `isMarking` (CG-I5).
3. Per-client flush at EVERY WND-open: the conductor's
   `m_objectSpace.stopAllocating()` route reaches every client's
   LAs via the shared directories' `m_localAllocators` lists
   (`Heap.cpp:2217-2256` + banner `:4817-4834`) — N-ary today;
   the per-window (not per-cycle) cadence is the only delta.
   `LocalAllocator::stopAllocating` non-idempotence (banner
   `:4822`) is preserved by the exactly-once-per-window pairing
   with the WND-close resume pass (`:4923-4925`) (CG-I6).
4. Steal protocol per client: a stolen block re-sweeps under MSPL
   before reuse (heap I8); rule 2 makes its newlyAllocated
   provenance correct under marking. CG-T4 storms steal-vs-marking.
5. Out-of-window allocation during marking is then live by
   construction: cells from a freelist created under rule 2 are
   covered at the next WND-open by rule 3; conservative re-scan
   each fixpoint window covers `m_currentBlock` cells (heap I12).
   The `endMarking` snapshot diagnostic (`Heap.cpp:1201-1244`)
   generalizes to CG-T11's debug assert.

## 7. Staged re-enable (charter item 4)

Order is NORMATIVE: C1 -> C2 -> C3 -> C4; a stage's flag may be
true only if its predecessors' flags are true (option validation,
§13.2). Each stage retires the §2.2 kill switches it names and
NOTHING else.

### 7.1 C1 — concurrent marking

Retires: `Heap.cpp:1957-1958` (fixpoint stays-stopped),
`:1979` (Concurrent-phase assert), the always-fenced forcing
(`:3936`, per §5.3). `runFixpointPhase` may schedule
`CollectorPhase::Concurrent`; `finishChangingPhase`'s
periphery pairing (`:2169-2209`) becomes WND-close/WND-open (§3).
`runConcurrentPhase`'s collector arm (`:1997-2002`,
`drainInParallelPassively`) runs helpers BETWEEN windows and is
the arm the ISS conductor itself executes per §3.7 (closed loop);
the legacy mutator-conn arm (`:1984-1996`) stays `!ISS`-only —
rev 1's "served by SINFAC polls" is superseded by §3.7 (no
poll-resume machinery exists or is introduced). Requires: §5
(CMS + fence) and §6 complete; §8 audit executed; §9.1 pause
protocol. Conductor remains the §10.2 requester.

### 7.2 C2 — collector continuity + activity-callback collection

Retires: `shouldCollectInCollectorThread` stays-false
(`Heap.cpp:1636-1648`), the `:1686` quiesced assert, activity
gating (`:790-792`) and the async reroute (`:1595-1600`).
Design: the collector thread becomes a STANDALONE-CLASS conductor —
it runs `runSharedGCElection`-equivalent tenure (GCL + GCA) but
NEVER holds heap access (it is not a client; it samples the §10.4
barrier exactly like a VM-less mutator conductor, which §10B.2
already licenses). `stopTheMutator`/`resumeTheMutator`
(`:2348`, `:2390`) stay DEAD (CG-I7) — the collector thread uses
WND-open/close, not the m_worldState bits; their unreachable
RELEASE_ASSERTs remain. The conduct-body refactor surface for a
client-less conductor is ANNEX CGA2 (BINDING; history file):
`conductSharedCollection(GCClient::Heap&)` (`Heap.cpp:4757`)
gains a nullable conductor-client; every conductorClient/vm() use
in the conduct path carries a CGA2 row disposition (skip /
in-window-main-VM / loop-over-clients); the collector run loop
(`Heap.cpp:333-357`) rewires from `shouldCollectInCollectorThread`
to ticket waits on `m_threadCondition`; the `runSharedGCElection`
VMTraps poll (`Heap.cpp:4562-4572`) is SKIPPED for the collector
thread (it never enters a VM — history open item 1 RESOLVED,
CGA2 row R6). Activity callbacks fire RCAC tickets
(`requestCollectionShared`) and notify the collector thread via the
existing `m_threadCondition`-class signal; mutator-driven SINFAC
conducting stays as fallback (I15 routing unchanged). Continuity =
the collector thread MAY retain GCA across back-to-back granted
tickets (the legacy "conn retention", `:2652-2680` analog) but MUST
drop GCA+GCL whenever no ticket is granted (CG-I12 liveness for
JSThreads stops, §9.1).

### 7.3 C3 — incremental + mutator-concurrent sweeping

Retires: IncrementalSweeper disablement (banner `:4832-4834`).
Pre-req: the T8 BlockDirectoryBits audit EXTENSION (the stop-mode
audit exists — `BlockDirectory.cpp:495-519` skip,
`assertIsMutatorOrMutatorIsStopped` sites, e.g. `:511`, `:556`):
re-audit every BlockDirectoryBits reader/writer for OUT-OF-WINDOW
access. Two structural rules replace per-site reasoning:
1. Bit-vector RESIZE publication: `m_bits` reallocation (heap I5b:
   `addBlock` under BVL+MSPL) additionally RELEASE-publishes the
   new storage; lock-free helper-side readers (e.g.
   `parallelNotEmptyBlockSource`, `BlockDirectory.cpp:539-559`,
   whose in-window assert C3 RELAXES for marker helpers)
   acquire-load the vector descriptor and tolerate a stale (smaller)
   bound — blocks added mid-phase are clean by construction
   (their cells are newlyAllocated, §6.2(2)). CG-I8; full reader
   table: ANNEX CGB1 (BINDING).
2. Sweeper identity: the IncrementalSweeper re-homes to a
   dedicated thread registered as a STANDALONE client
   (`markStandalone()`+ACT, heap §12.1 seam) holding access only
   per quantum, sweeping under MSPL per quantum
   (`sweepSynchronously`'s MSPL discipline, `Heap.cpp:1487-1498`,
   becomes per-quantum). It therefore participates in F8/windows
   like any client — no sweeper-vs-window special case (CG-I13).
   In-lock allocation-path sweeps are unchanged.

### 7.4 C4 — incremental mutator assist

Retires: `performIncrement` ISS early-return (`Heap.cpp:3950-3951`)
and the heap §5.4 debug-assert that the mutator SlotVisitor is
world-stopped-only. Design: per-client assist visitor — GCH gains
`m_assistVisitor` (a SlotVisitor pre-registered in the
`forEachSlotVisitor` set under `m_parallelSlotVisitorLock` at ACT;
unregistered at DCT only between cycles or in-window, CG-I14) and a
per-client `m_incrementBalance` (the `:1969` reset folds at
WND-open). `performIncrement` routes `didAllocate` bytes
(`Heap.cpp:3127, 3140`) to the calling client's visitor;
`performIncrementOfDraining` (`SlotVisitor.cpp:527-583`) runs on
the client's thread WITH access, under its visitor's `m_rightToRun`
— `resumeThePeriphery`'s acknowledge loop (`Heap.cpp:2315-2342`)
and `updateMutatorIsStopped` (`SlotVisitor.cpp:469-486`) already
handle N visitors. m_opaqueRoots/race-stack interplay is
N-ary-safe today (locked; §2.3(b)).

## 8. Marking vs live mutators (charter item 5)

### 8.1 What stays in-window

Constraint solving stays WINDOW-CONFINED: `executeConvergence`
(`Heap.cpp:1917`) and all root constraints (conservative scan
`:1024-1080`, whose `ASSERT(!ISS || WSAC)` at `:1037` KEEPS) run
only at fixpoint windows — this is the Riptide design (the
Concurrent phase only drains) and we do not relax it. Likewise
`beginMarking`/`endMarking`, `prepareForMarking`, stack/register
gathering (heap §10.6), `finalize()` (`:2753`), and the §11
reclaim (`runSafepointHooksAndReclaim`, `:4961` — fires once per
cycle in the FINAL window, preserving heap I11's legal-context
list verbatim: "§10 step 7" = the cycle's last window, CG-I11).

### 8.2 Cell-lock coverage audit (out-of-window draining)

Stage C1 makes `visitChildren` race mutators. The object-model
protocol is FROZEN and sufficient — om §6 butterfly/structure
rules, om I-series for shape storage, ungil §N rulings for builtin
internals (N.1 collections cell-locked including reads; N.2 rope
release-CAS/acquire-read; N.5 claim CAS; N.6 buffer torn-pair
rules), `m_indexingTypeAndMisc` CAS (heap F5). What this spec adds
is the AUDIT obligation, not new rules: CG-A2 (executed at C1
entry, recorded as ANNEX CGN1 rows) walks every
`methodTable()->visitChildren`-reachable reader of
mutator-mutable multi-word state and assigns each row
{IN-WINDOW-ONLY, OM-PROTOCOL (cite om §), N-PROTOCOL (cite ungil §N
row), CELL-LOCKED (10a; visitor side MUST use tryLock + defer to
the race stack / re-visit, NEVER block on a cell lock — a mutator
holding 10a can be allocation-slow-pathing under MSPL and a
blocking marker would deadlock the next window's flush; CG-I15),
RACY-TOLERATED (profiling-class, justify)}. CELL-LOCK NO-PARK
(CG-I18, NORMATIVE): a thread holding any JSCellLock (10a) must
not release heap access, pass a stop poll (SINFAC/AHA/trap
landing), or enter a conducting path. Grounding: SINFAC's I6
precondition forbids polling with any rank >= 4 lock held
(`Heap.cpp:5125-5127`); F8 revert-and-block parking lives only in
access ACQUISITION (`Heap.cpp:5707-5758`), which a 10a holder
(already access-holding) never re-enters; allocation under 10a is
restricted to poll-free paths, else the lock is dropped around
the slow path (JSC's reallocate-butterfly-outside-the-lock
idiom). Hence IN-WINDOW no 10a lock is held, so every visitor
tryLock retry at a fixpoint window succeeds — THIS is the CG-I15
termination argument (rev 1's inverted N3 sentence amended,
history rev 2, F3). CG-A2 classifies every CELL-LOCKED row's
MUTATOR side against CG-I18; debug assert: cell-lock depth == 0
at SINFAC entry and the AHA park leg (CG-T5 arm). SlotVisitor draining
infrastructure (`drainFromShared` `SlotVisitor.cpp:607`,
`m_rightToRun` safepoints `:522, :578`) is reused unmodified.
Visitor-side allocation stays forbidden (markers are not clients;
heap I4(c)).

## 9. Composition with the JSThreads stop protocol (charter item 6)

### 9.1 GC cycle vs §A.3 stop — the ordering pin

Today both serialize on GCL: a JSThreads conductor brackets
`Heap::JSThreadsStopScope` (`Heap.cpp:5456-5482`;
`JSThreadsSafepoint.cpp:231-337`), and a whole-cycle GCL hold was
fine because the cycle was one short window. A concurrent cycle's
tenure is LONG; an untouched design would starve §A.3 conductors
into the 30s watchdog (`JSThreadsSafepoint.cpp:401`). PIN
(normative):

1. GCL is held by the GC conductor ONLY in-window (§3.4). Between
   windows GCL is free; `JSThreadsStopScope` may interleave between
   any two windows of a cycle.
2. A foreign GCL holder mid-cycle must not race marking helpers:
   `JSThreadsStopScope`'s ctor, after acquiring GCL, when
   `m_currentPhase != NotRunning`, calls new
   `Heap::pauseConcurrentMarkingForForeignStop()`; the dtor
   resumes. MECHANISM (NORMATIVE, rev 2): a NEW pair
   `bool m_parallelMarkersShouldPause` + `unsigned
   m_pausedParallelMarkers`, both guarded by `m_markingMutex`.
   NOT reused: `m_parallelMarkersShouldExit` (one-shot
   cycle-terminate, `Heap.cpp:2027`; `SlotVisitor.cpp:664, :673`)
   and the `:2315-2342` rightToRun loop (rightToRun visitors only;
   acknowledge round-trip) — rev 1 cited both and chose neither
   (history rev 2, F6). Pause: set ShouldPause + `notifyAll`
   `m_markingConditionVariable`, then wait (same mutex/condvar)
   until `m_pausedParallelMarkers == m_numberOfActiveParallelMarkers
   + waiting helpers in drain`. Checkpoints: (a) the
   `drainFromShared` helper wait `isReady` lambda
   (`SlotVisitor.cpp:661-667`) gains `|| shouldPause`; a woken
   helper increments the paused count, `notifyAll`s, and waits for
   `!shouldPause` before touching work; (b) the in-drain
   safepoints at each cell batch (`m_rightToRun.safepoint()` +
   donate, `SlotVisitor.cpp:522, :578`) gain a shouldPause check —
   granularity = one drained batch, which is what makes the
   CG-I12/CG-T8 pause-latency bound derivable. No lost wakeup:
   flag writes, count writes, and every wait share
   `m_markingMutex`; the existing `didReachTermination` waits
   re-evaluate their predicates under the same mutex after any
   notifyAll, and a paused helper counts as waiting for
   termination purposes only after re-checking `hasWork` post
   resume. The GC conductor's own between-window MainDrain wait
   (§3.7) needs no pause check: re-opening a window blocks at the
   GCL acquire (§3.1(b)), which the foreign scope holds. Resume:
   clear flag + `notifyAll`. Helpers hold no lock a §A.3 window
   needs (`m_markingMutex`/block-internal only), so the pause
   terminates (CG-I16). The §A.3 window may then jettison/patch:
   mutators are parked by ITS fan-out; markers are paused by THIS
   hook. The window's ISB generation bump (ungil ISB1.1) plus
   rule 4 below covers marker-visible code/object frees.
3. Order pin per cycle edge: WND-open is access-released -> GCL ->
   GSP (§3.1(a)-(c) — rev 2 fix; the rev 1 GCL-before-GSP-before-
   release order is REJECTED there); WND-close clears GSP/WSAC
   BEFORE releasing GCL. Hence at every instant at most one of
   {GC window open, §A.3 window open} — single-owner GCL is the
   proof. The HBT4 conductor order (release access -> arbitration
   -> GCL; ungil §A.3.3) now EXTENDS to GC-conductor window
   re-entry (the only blocking GCL acquire in the system; election
   and poll paths stay tryLock-only, `Heap.cpp:4523, :4585`) and
   the §10.2 GCL-busy rule (timed 1ms follower wait,
   `Heap.cpp:4542-4554`) is unchanged; both also cover
   "GC requester vs §A.3 stop midway through someone else's
   cycle": the requester parks as a follower on GCA (set for the
   whole cycle), not on GCL.
4. No reclamation outside the cycle's final window: heap I11 +
   jit R4/CS4 refusal stand VERBATIM — a §A.3 stop interleaved
   mid-cycle still only ENQUEUES an RCAC ticket (heap §13.10a;
   processed by the SAME cycle's remaining drain via the §10B.1
   all-granted-tickets rule, `Heap.cpp:4852-4863`). Memory the
   paused markers can see is freed only at epoch reclaim (final
   window) or by jettison paths that already route through the
   epoch/quarantine machinery (om §6 quarantine bar; the §9
   contract notes' hook firing stays once-per-CYCLE, final window).
5. GC keep-parked vs §A.3 parking stays disjoint per ungil §A.3.8:
   GC stops set client-visible state (GSP/F8); §A.3 stops set none
   (`JSThreadsStopScope` only). AHA's three gates (GSP leg
   `Heap.cpp:5723`, §A.3 word leg `:5752`, mode-machine leg
   `:5773`) compose unchanged — each re-loops to step 1, so a
   client wakes only when NO window of any kind pends.

### 9.2 EXIT1 teardown mid-cycle

A spawned thread's exit (ungil §B.2: RHA -> TEARDOWN mark -> DCT ->
unregister) may land between windows of a live cycle. Rules:
1. DCT/`~GCClient::Heap` runs the landed teardown FIRST (access
   re-acquire for `lastChanceToFinalize`'s MSPL section, TLC
   `stopAllocatingForGood`), then PERMANENTLY drops access, THEN —
   strictly AFTER the last point at which this thread can execute
   a write barrier — flushes the client's CMS into the shared
   mutator stack (under `m_markingMutex`) and publishes its
   fence/didRun state as dead, then epoch=MAX and HCS remove
   (inside the same GBL/!WSAC section heap I13 requires for
   remove). Rev 1's flush-first order lost barriers:
   `lastChanceToFinalize` can store into surviving old-gen objects
   and hit `addToRememberedSet` (`Heap.cpp:1427`) — appends after
   an early flush died with the client (full interleaving: history
   rev 2, F2). An OPTIONAL early flush for bounding stays legal;
   the pre-remove flush is the normative one. It completes while
   still registered, so
   a WND-open concurrent with exit either sees the client (and
   drains it) or doesn't (already drained) — never a registered
   client with an unreachable CMS (CG-I17). CG-T9 arm: exit with
   finalizer-side stores during forced full concurrent marking.
2. HCS `remove` blocking inside windows (heap I13) extends
   naturally: removal between windows of a cycle is LEGAL once
   rule 1 ran — the next fixpoint window's convergence simply
   re-runs with one fewer conservative root set, which is the same
   soundness argument as a legacy mutator dropping references
   between increments (its retained refs were either barriered
   (CMS, drained) or published by the RHA seq_cst edge and reachable
   from surviving roots, or garbage).
3. `~VM` (EXIT1.9 fence) composes: the main client survives all
   spawned exits; §10D ISS reversion already requires
   `m_currentPhase == NotRunning` (heap §10D step 1), so a cycle
   never straddles a protocol switch.
4. The detaching thread is never the live conductor: §3.7 forbids
   EXIT1 on `m_gcConductorThread` mid-cycle (release assert) —
   the conductor is in its closed loop, not running JS, so its
   thread cannot reach ungil §B.2 teardown; CG-T9 attempts it.

### 9.3 Mid-cycle client ATTACH (rev 2; cited but unwritten in rev 1)

`HeapClientSet::add` blocks only INSIDE windows (I13 add-side:
insert under GBL with !WSAC, `HeapClientSet.h:54-68`), so a
`Thread()` spawn's ACT (ungil §B.1) may land BETWEEN windows of a
live cycle (`m_currentPhase != NotRunning`). Requiring the §10B.4
quiescence loop instead is REJECTED: it would block spawn for
whole cycles and starve against continuous RCAC tickets. Rules
(NORMATIVE):
1. Fence init handshake: inside the same GBL/!WSAC critical
   section that publishes the client in HCS, BEFORE the insert,
   the attaching thread copies the server master
   `m_mutatorShouldBeFenced`/`m_barrierThreshold` into its client
   copies and stamps `m_fenceEpochSeen = FEP`. Happens-before: the
   master mutates only in-window (§5.3(1), WSAC set under GBL) and
   the add runs under GBL with !WSAC, so GBL mutual exclusion
   orders the snapshot against every master mutation — the
   snapshot is never torn and never stale across a window edge.
   A client attaching during live marking thus starts RAISED and
   misses no barrier; CG-I3's WND-close assert holds for attachees
   with NO exemption. The §5.3(3) always-fenced pin subsumes the
   copy values while it stands, but the FEP stamp stays required.
2. `m_isMarking` visibility: the client's first AHA performs the
   seq_cst GSP load (`Heap.cpp:5723`) and the GBL section above
   pairs with the in-window flip (§6.2(1)); allocation before ACT
   completes is impossible (ungil §B.1: ACT precedes any
   allocation).
3. C4 assist visitor: an ACT while `m_currentPhase != NotRunning`
   DEFERS `forEachSlotVisitor` registration to the next WND-open
   (conductor registers pending visitors in-window under
   `m_parallelSlotVisitorLock`) — CG-I14 amended to {in-window,
   between cycles, deferred-from-attach applied in-window}. Until
   registered, the client cannot assist (`performIncrement` checks
   registration); its barriers/CMS are live immediately.
4. `m_didRunSinceLastWindow` = false and CMS = empty at ACT
   (zero-init; both benign — stated for completeness).
5. CG-T9 arm: attach storm during forced concurrent marking with
   the §6.2(5)/CG-T11 endMarking liveness assert enabled.

## 10. Lock & fence deltas

Lock table (heap §6 master) deltas — additions only, no re-ranks:
- `GCH::m_mutatorMarkStackLock` — leaf-class, INSIDE
  `m_markingMutex` (CG-I10); never with ranks 7-9b held.
- `m_markingMutex`, `m_parallelSlotVisitorLock`,
  `m_raceMarkStackLock`, visitor `m_rightToRun`: existing,
  unranked-in-§6 marking-internal locks; CG-T2 lint adds them as a
  group ordered inside GCL/GBL and disjoint from MSPL-9b except the
  existing in-window uses.
Fences (heap §7 deltas):
- F4/F7/F8 unchanged; F8's Dekker proof is per-WINDOW (GSP
  set/clear per window) — no change to the proof, only to its
  cadence.
- CG-F1: FEP store(release) in-window; client copies written by the
  conductor in-window; clients read own copy plain (owner-thread,
  conductor writes only while that client is parked — the window
  barrier is the synchronization).
- CG-F2: `m_isMarking` flips in-window only; published by the F8
  resume edge (§6.2(1)).
- CG-F3: directory-bit storage descriptor release-published on
  resize / acquire-loaded by out-of-window readers (§7.3(1)).
- CG-F4: ISB generation bump at EVERY WND-close when gilOffProcess
  (§3.2), before the GSP clear, mirroring `Heap.cpp:4927-4943`.

## 11. Invariants (numbered; each needs a test or assert — §12)

- CG-I0: all §13.2 flags off => shared-mode behavior identical to
  today's §10 protocol (one window per cycle, §3.6); `!ISS` =>
  legacy protocol byte-for-byte (heap I10 untouched).
- CG-I1: at most one GC window OR one §A.3 window is open at any
  instant (single-owner GCL, §9.1(3)); heap §10's step ordering
  (heap-resume before VMM-resume) holds per window.
- CG-I2: every CMS append happens on its owning client's
  access-holding thread under its CMS lock; every drain happens
  under `m_markingMutex` (in-window by the conductor, or
  out-of-window by the owning thread).
- CG-I3: fence RAISES are window-complete (all client copies
  republished before WND-close); LOWERS occur only in the final
  window after termination; `m_fenceEpochSeen == FEP` for every
  client at every WND-close (debug assert).
- CG-I4: `m_isMarking`, `m_collectionScope`, phase transitions, and
  marking/newlyAllocated version bumps mutate in-window only.
- CG-I5: no sweep-to-freelist, steal, or `addBlock` straddles a
  window (MSPL implies access implies barred from open windows).
- CG-I6: per window, each LA is stop-flushed exactly once and
  resumed exactly once (LocalAllocator non-idempotence preserved).
- CG-I7: `stopTheMutator`/`resumeTheMutator`/`m_worldState`
  stop bits stay unreachable when ISS, ALL stages (the C2 collector
  conductor uses windows, never the legacy bits).
- CG-I8: every BlockDirectoryBits access is (a) in-window, (b)
  under BVL, (c) under MSPL, (d) an acquire-read of release-
  published storage tolerating a stale bound (marker/sweeper
  readers, ANNEX CGB1 rows), or (e) `!ISS`.
- CG-I9: `m_didRunSinceLastWindow` is written only by its owning
  access-holding thread; folded+cleared only in-window.
- CG-I10: lock order `m_markingMutex` > CMS lock; neither taken
  with ranks 7-9b held; CMS lock is leaf.
- CG-I11: heap I11 verbatim — epoch bump/reclaim only in the
  cycle's FINAL window (or legacy `runEndPhase` when `!ISS`);
  never from a §A.3 stop; hooks fire once per cycle.
- CG-I12: GCL is never held by the GC conductor between windows;
  GCA without a granted ticket is dropped (C2 continuity bound) —
  a §A.3 conductor's GCL wait is bounded by one window + one
  marker-pause (pause granularity per §9.1(2): one drained batch;
  watchdog headroom: CG-T8 measures).
- CG-I13: every sweeping thread is a registered client holding
  access for each quantum, MSPL across each quantum's directory/bit
  mutations.
- CG-I14: the `forEachSlotVisitor` set mutates only in-window,
  between cycles, or as a deferred-from-attach registration
  applied in-window (§9.3(3)), all under
  `m_parallelSlotVisitorLock`; every visitor in it has a live
  owner (client or conductor or helper pool).
- CG-I15: marker threads never BLOCK on a JSCellLock (tryLock +
  revisit/race-stack only) and never allocate from the shared heap.
- CG-I16: `pauseConcurrentMarkingForForeignStop()` terminates
  without acquiring any api-rank or heap rank ≥7 lock; paired
  resume in the same `JSThreadsStopScope`.
- CG-I17: a detaching client's CMS is empty (flushed) by the time
  it leaves HCS, and the final flush postdates the client's last
  possible barrier (§9.2(1) order); its assist visitor is out of
  the visitor set before destruction (CG-I14 timing).
- CG-I18: no thread releases heap access, passes a stop poll, or
  enters a conducting path while holding a JSCellLock (§8.2);
  debug assert: cell-lock depth == 0 at SINFAC entry / AHA park.
- CG-I19: the conductor's tenure is a closed loop (§3.7): no JS,
  no RHA/AHA, no EXIT1, access-released throughout, every
  between-window wait condvar-parked and §A.3-compatible; the
  blocking GCL acquire happens only access-released (§3.1).
- CG-I20: a client attached while `m_currentPhase != NotRunning`
  has its fence/threshold copies + `m_fenceEpochSeen` stamped from
  the master inside the publishing GBL section, before its first
  allocation (§9.3(1)).

## 12. Verification ladder + TSAN charter (charter item 7)

Per-stage rungs; a stage's flag may default-on only after its rungs
are green AND all earlier stages' rungs re-run green.

- CG-T1 (C0): ANNEX CGA1 audit executed; grep-lint for the audit
  patterns clean; CG-I0 behavior gate — flags-off corpus +
  `$vm.sharedHeapTest` scenarios (heap §12.1 list) byte-identical;
  BENCH.md flags-off delta = 0 (same bar as heap I10).
- CG-T2 (C0): lock-lint (U20-class) extended with §10 rows;
  CMS/markingMutex order litmus.
- CG-T3 (C1): barrier storm — N threads store-heavy during forced
  concurrent marking; TSAN no-JIT (TSAN.md target) over the
  corpus; amplifier arms (AMPLIFIER.md hooks) at: WND-open barrier
  entry, CMS donate, fence republish, `m_isMarking` resume edge,
  steal-vs-mark.
- CG-T4 (C1): allocation/steal storm during marking
  (`allocationStorm`+marking variant); endMarking liveness debug
  assert (§6.2(5)) enabled.
- CG-T5 (C1): CG-A2 cell-lock audit executed (ANNEX CGN1 rows
  complete); per-row arm for every CELL-LOCKED and N-PROTOCOL row;
  CG-I18 storm (map/set mutation vs forced fixpoint windows) with
  the cell-lock-depth debug asserts enabled.
- CG-T6 (C2): collector-continuity churn — RCAC storms with zero
  mutator polls; activity-callback collections fire; SINFAC
  fallback still conducts when the collector thread is disabled.
- CG-T7 (C3): T8-extension audit (ANNEX CGB1) executed; sweep
  quantum vs window race arm; sweeper-thread attach/detach churn.
- CG-T8 (all): JSThreads-stop interleaving — haveABadTime/jettison
  (`$vm`) fired mid-concurrent-cycle from a sibling thread,
  including injected exactly at the WND-open GCL acquire (§3.1)
  and against a between-windows condvar-parked conductor (§3.7);
  measures GCL wait vs the 30s watchdog
  (`JSThreadsSafepoint.cpp:403`); `jsThreadsStopVsGCRequester`
  (heap §12.1) re-run per stage.
- CG-T9 (all): ATTACH/EXIT1 churn — spawn/exit threads continuously
  during forced concurrent cycles (CG-I17/I20); attach storm with
  the §6.2(5) liveness assert (§9.3(5)); exit with finalizer-side
  stores during forced full marking (§9.2(1)); conductor-thread
  exit attempt (must release-assert, §9.2(4)); `clientChurnVsGC` +
  `issRevertChurn` re-run (cycle must quiesce before revert).
- CG-T10 (C4): assist storm — per-client balances; assist visitor
  vs WND-open rightToRun handoff arm.
- CG-T11 (all): the §2.4 diagnostics re-expressed as
  debug-build asserts gated on the stage flags (freelisted-block
  check per WND-open; endMarking root-liveness check).
- TSAN charter: every CG-T3..T10 arm runs under the TSAN no-JIT
  target with suppressions limited to documented RACY-TOLERATED
  rows (CGA1/CGN1); any other report is a stage blocker. GIL-off
  rungs use the pinned ladder commands (UNGIL-HANDOUT
  verification section) with the stage flag appended.

## 13. Files, options, integration

1. Owned (this spec's implementation): `heap/**` only, same set as
   heap §12 + `GCThreadLocalCache`/`HeapClientSet`/
   `GCSafepointEpoch` files; `JSTests/threads/congc-*.js`;
   `docs/threads/INTEGRATE-congc.md` (manifest, written first).
2. Options (`runtime/OptionsList.h` via manifest, heap §13.2
   shape): `useConcurrentSharedGCMarking` (C1),
   `useSharedGCCollectorThread` (C2),
   `useSharedGCIncrementalSweep` (C3),
   `useSharedGCMutatorAssist` (C4) — all default false; option
   validation enforces the §7 prefix rule and `useSharedGCHeap`
   prerequisite. Plus `sharedGCMutatorMarkStackDonationThreshold`
   (Unsigned; §5.2(ii); default = `s_segmentCapacity`,
   `GCSegmentedArray.h:116`).
3. Chartered out (recorded as INTEGRATE rows, not owned):
   (a) per-client barrier-address JIT emission (§5.3(3)) — jit/
   ungil owners (A16-class lite reroute); until landed, GIL-off
   C1+ pins clients always-fenced; (b) `JSThreadsSafepoint.cpp`
   marker-pause call site (§9.1(2)) — the scope ctor lives there
   (`:334-337`), heap exports the pause/resume pair; (c) VMManager
   manifest deltas: none expected (window open/close reuses the
   manifest-5 hunks verbatim, per-window).
4. Supersession discipline: this spec SUPERSEDES nothing frozen
   yet; the §2.2 kill switches it retires are flag-gated retires
   (the `!flag` arm keeps the frozen behavior, satisfying heap
   Deviation 4's "option off => today's fully concurrent protocol"
   I10 clause and its shared-mode "flag-off = today's §10" analog).
   At freeze time, the heap-spec clauses that say "once shared"
   unconditionally (e.g. SPEC-heap.md:23 deviation 4 itself,
   §5.4 `performIncrement`, §10B(7)) gain a recorded two-sided
   supersession pointing here; the list is ANNEX CGS1 and MUST be
   folded into SPEC-heap-history at freeze.

## 14. Ordered task list

- CG-1 (C0 infra): window split of `conductSharedCollection`
  (WND-open/close helpers with the §3.1 order; GCL per-window;
  GCA tenure + `m_gcConductorThread`; §3.7 closed loop; §3.4 guard
  in `tryConductSharedCollectionForPoll`); CG-T1/T2.
- CG-2 (C0 infra): CMS + per-client fence/didRun fields + window
  fold/republish loops; consumers re-pointed (`addToRememberedSet`,
  `mutatorShouldBeFenced`, `barrierThreshold`); GIL-phase JIT
  address pin (§5.3(3) fail-safe).
- CG-3 (C1): retire the three C1 kill switches behind the flag;
  marker-helper between-window scheduling; §9.1(2) pause pair
  (ShouldPause flag + paused count + checkpoints) + the
  JSThreadsSafepoint integration row; §9.2 exit order + §9.3
  attach handshake; donation threshold option; CG-A2 audit ->
  ANNEX CGN1; CG-T3/T4/T5/T8/T9/T11.
- CG-4 (C2): collector-thread conductor; activity-callback RCAC
  routing; continuity bound; CG-T6 + re-runs.
- CG-5 (C3): ANNEX CGB1 audit; bit-storage release/acquire
  publication; sweeper-client re-home; CG-T7 + re-runs.
- CG-6 (C4): per-client assist visitors/balances; `performIncrement`
  re-enable; CG-T10 + re-runs; BENCH.md stage gates recorded.
- CG-7: freeze pass — ANNEX CGS1 supersessions recorded both
  sides; size-cap check; rev history updated.

Done = every CG-I has a test/assert (§12 map); flags-off bench and
behavior gates green; all four stage flags individually green on
the ladder; INTEGRATE-congc.md matches §13 exactly.
