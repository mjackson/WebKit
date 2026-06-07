# SPEC-congc.md - N-MUTATOR CONCURRENT GC (draft rev 4)

Status: DRAFT rev 4 — freezes after the adversarial pass (history =
`SPEC-congc-history.md`; BINDING annexes live there per the
frozen-spec convention). Authorities: THREAD.md;
SPEC-{heap,vmstate,objectmodel,jit,api,ungil}.md (+annexes);
UNGIL-HANDOUT.md rev 32. Charter: SPEC-heap Deviation 4
(SPEC-heap.md:23) deferred the concurrent-GC protocol for N
mutators; shared mode today is synchronous conductor-driven STW.
This spec designs the re-enable. Verified vs tree 2026-06-07
(branch `jarred/threads`). heap:/ungil:/om:/jit: = the SPEC files;
CG-I* = invariants (§11); CG-T* = test charters (§12); ANNEX CG* =
history annexes.

Master rule: every stage is a MODE. Flag-off (all §13.2 options
false) = today's shared §10 protocol BYTE-FOR-BYTE (CG-I0, heap
I10 analog); `!useSharedGCHeap` or `!ISS` = legacy Riptide,
untouched. Nothing here edits GIL-on/flag-off observable behavior.

Notation: inherits heap §8 (SPEC-heap.md:8): WSAC, MSPL, BVL, GCL,
GBL, GBC, GSP, GCA/GEC, ISS, AHA/RHA, SINFAC, CSAC/RCAC, CIND,
ACT/DCT, BD, LA, TLC, HCS, GCH. New: WND = stop window (§3); CMS =
per-client mutator mark stack (§5.2); FEP = `m_barrierFenceEpoch`
(§5.3).

## 1. Scope

Deliverables: re-enable, over the shared server heap (heap §4),
the five features Deviation 4 disabled (SPEC-heap.md:23):
concurrent marking, collector continuity, incremental mutator
assist, activity-callback collection, mutator-concurrent sweeping
— staged per §7. Non-goals: the object-model concurrency protocol
(om: frozen; consumed), the §A.3 stop machinery (ungil: frozen;
composed in §9), TLC layout/JIT addressing (heap §5.3 / ungil
§B.4), the epoch reclamation contract (heap §11), wasm-GC (heap
§5.5 stands).

## 2. Ground truth

### 2.1 The legacy one-mutator handshake

`Heap::m_worldState` (`heap/Heap.h`; scribbled at
`Heap.cpp:521`) packs FOUR bits for exactly ONE mutator:
`hasAccessBit`, `stoppedBit`, `mutatorHasConnBit`,
`mutatorWaitingBit` (state-machine asserts `Heap.cpp:2354-2384`);
its consumer functions (`Heap.cpp:2348-2747`) and the
single-mutator periphery are enumerated site-by-site with per-line
cites and
N-ary dispositions in ANNEX CGA1 (BINDING), indexed in §4.3 —
the annex table is the audit of record.

### 2.2 The shared-mode machinery (what we generalize INTO)

Landed per heap §10: ticketing `requestCollectionShared()`
(`Heap.cpp:4479`; conn bit idempotent `:4499`), election
`runSharedGCElection()` (`:4507`; GCL-busy timed wait `:4554`),
poll-conduct `tryConductSharedCollectionForPoll()` (`:4578`),
`conductSharedCollection()` (`:4757`): GSP store (`:4768`), §10.4
GBL barrier (`:4780-4793`), ticket drain (`:4852-4863`),
`runSafepointHooksAndReclaim()` (`:4870`, `:4961-5008`), step-8
resume pass (`:4923-4925`), WSAC/GSP clear + GBC (`:4945-4950`),
VMM resume (`:4955`). Per-client:
`m_accessState`/`m_accessOwner`, F8 Dekker AHA
(`Heap.cpp:5656-5795`), RHA (`:5797-5838`), SINFAC (`:5107`),
park hooks (`:5390`, `:5431`), `JSThreadsStopScope` (`:5456`),
HCS (`HeapClientSet.h:48-100`), TLC (`GCThreadLocalCache.h:84-91`),
epoch (`GCSafepointEpoch.h:61-125`). Deviation-4 kill switches:
no Concurrent phase when ISS (`Heap.cpp:1957-1958`, assert
`:1979`), collector thread quiesced (`:1636-1648`, `:1686`,
`:2350-2352`, `:2392-2393`), assist off (`:3950-3951`), activity
callbacks off (`:790-792`, reroute `:1595-1600`), always-fenced
(`:3936-3937`), IncrementalSweeper off (banner `:4832-4834`).

### 2.3 The mechanisms that already carry concurrency

(a) Versioned liveness: `MarkedSpace::m_isMarking`
(`MarkedSpace.h:187, 243`), `isLive`'s marking-aware path
(`MarkedBlock.cpp:59-106`), newlyAllocated stamping in
`specializedSweep` when `isMarking` (`MarkedBlockInlines.h:154,
186, 244, 281`), freelist→newlyAllocated conversion in
`stopAllocating` (`MarkedBlock.cpp:201-227`). (b) Mark/barrier
races: `m_raceMarkStack` under `m_raceMarkStackLock`
(`Heap.h:1169, 1184`), `aboutToMarkSlow` (`MarkedBlock.cpp:345`),
the re-whiten CAS protocol (`Heap.cpp:1444-1467`).
(c) Multi-window cycles: `m_currentPhase` persists;
`finishChangingPhase()` (`Heap.cpp:2169-2209`) pairs
`resumeThePeriphery()`/`stopThePeriphery()` (`:2287`, `:2217`);
`resumeThePeriphery`'s rightToRun loop already iterates ALL
visitors (`:2315-2342`). Reused, not redesigned.

### 2.4 Temporary diagnostics

The fix-shared-heap-corruption instrumentation
(`Heap.cpp:993-1020`, `:1201-1244`, `:2258-2282`) is
stop-mode-only; §7 stages may keep it only behind
`verboseSharedGCHeap`-class options; CG-T11 reuses its checks as
debug asserts.

## 3. Architecture: the window model

A shared collection becomes ONE GCA TENURE containing a SEQUENCE
of stop windows (WNDs), not one monolithic window.

1. WND-open = §10 steps 3-4 + flush, ORDER NORMATIVE (rev 2): the
   conductor (a) is access-RELEASED (released at the first
   WND-open, stays released all tenure, §3.7); (b) acquires GCL —
   a BLOCKING acquire is legal exactly because the thread is
   access-released, satisfying any concurrent §A.3 fan-out (ungil
   §A.3 rule 2); the HBT4 release-access-before-GCL order (ungil
   §A.3.3) EXTENDS to window re-entry; (c) seq_cst `GSP=true`;
   (d) `VMManager::requestStopAll(GC)`; (e) GBL barrier until
   every client NoAccess (F8 unchanged — `Heap.cpp:4768-4793`);
   set WSAC; per-client flush (§5.2 drain, §6.2 allocator stop,
   today's `stopThePeriphery()` route). Rev 1's GCL-before-release
   order — REJECTED (rev 2, F9: §A.3 deadlock); CG-T8 arms it. FIRST-WINDOW
   CARVE-OUT
   (rev 3, F15): the first WND-open IS the landed entry — GCL via
   tryLock while still access-HELD (`Heap.cpp:4523`, `:4585`;
   §A.3-safe: non-blocking), then GSP (`:4768`), THEN the step-3
   access release (`:4769-4771`) — flag-off identical (CG-I0).
   (a)->(b)->(c) and the blocking GCL acquire govern window
   RE-ENTRY only; the F9 rejection applies to re-entry.
2. WND-close = §10 steps 8-9: client cache resume pass; ISB
   generation bump when gilOffProcess (`Heap.cpp:4940-4943` —
   REQUIRED at EVERY window close: each window may jettison/patch;
   extends ISB1.1, `:4927-4939`); clear WSAC; seq_cst `GSP=false`;
   GBC broadcast; `requestResumeAll(GC)`; THEN release GCL
   (CG-I12). The conductor re-acquires its own access only at the
   landed tail (`Heap.cpp:4955`) after the FINAL WND-close.
   Heap-resume-before-VMM-resume stays normative (heap §10 tail).
3. Between windows: mutators run; marking helpers may run (§7 C1);
   `m_currentPhase` persists, the legacy multi-window shape
   (§2.3(c)). WSAC is false between windows; WSAC-gated asserts
   (heap I5) remain correct — everything they guard stays
   in-window (§8.1).
4. Tenure: the conductor keeps GCA=true and remains the elected
   §10.2 winner across all windows of the cycle. GCL itself is
   RELEASED between windows and re-acquired at each WND-open
   (CG-I12) — this is what lets a JSThreads stop interleave (§9.1).
   "GCL free && GCA set && phase != NotRunning" thereby becomes a
   STEADY STATE (reachable today only in the wind-down instants,
   `Heap.cpp:4534-4537`, phase already NotRunning). EVERY GCL tryLock site gets a between-windows
   disposition (rev 3, F10/F12). Guard, under `*m_threadLock` after
   tryLock success: `m_gcConductorActive && m_currentPhase !=
   NotRunning` => unlock GCL and back off:
   - `:4523` election winner: fall through to the follower wait
     (`:4550-4554`); else a second `conductSharedCollection` nests
     against the live cycle (interleaving: ANNEX CGD1.1).
   - `:4585` poll: return false; retry next poll.
   - `:5036` §10D revert poll: return, hint stays armed (F11,
     §9.2(3); the landed pre-check `:5040-5043` deadlocks against
     the steady state, ANNEX CGD1.2); the bounded wind-down wait
     is legal only when phase == NotRunning.
   Phase-gating keeps flag-off byte-for-byte (CG-I0): flag-off,
   tryLock success implies phase == NotRunning — the guard is
   unreachable; wind-down keeps today's paths. CG-I21; CG-T8/T9.
5. Conductor identity: stays `GCConductor::Mutator` running the
   `collectInMutatorThread()` phase loop (heap §10B.2) in stages
   C0-C1; C2 adds a collector-thread conductor (§7.2). GCA gains
   an owner: `m_gcConductorThread` (Thread*, under `*m_threadLock`
   next to `m_gcConductorActive`, `Heap.h:1290`), stamped/cleared
   with GCA. Consumers: the §3.4 guards (FOREIGN threads only —
   the conductor never polls mid-cycle, rule 7); the §9.2 EXIT1
   assert; debug asserts (CG-I21).
6. Flag-off degenerate case: all §13.2 flags false => exactly ONE
   window (WND-open, drain to completion at the fixpoint per
   `Heap.cpp:1957-1958`, WND-close) — i.e. today's
   `conductSharedCollection`. CG-I0 checked by inspection + the
   §12 bench/behavior gates (heap I10 discipline).
7. Conductor tenure contract (NORMATIVE — CG-I19). Conducting is
   a CLOSED LOOP: first WND-open to final WND-close, the conductor
   executes ONLY the phase loop; no heap access all tenure
   (released at WND-open step (a), re-acquired at `Heap.cpp:4955`);
   no JS, no RHA/AHA, no EXIT1 (its thread is
   `m_gcConductorThread`; ungil §B.2 teardown on it mid-cycle is a
   release-assert violation).
   BETWEEN windows (Concurrent phase) the conductor's wait is
   `donateAll()` then `waitForTermination(m_scheduler->timeToStop())`
   (`SlotVisitor.cpp:753-758`, `:737-751`) — condvar-only under
   `m_markingMutex`: the conductor is in NEITHER active/waiting
   counter and never executes visitChildren between windows. It
   never calls `drainInParallelPassively` or `drainFromShared`
   (rev 3, F13, superseding rev 2's MainDrain cite — the ACTIVE
   arm; full derivation: ANNEX CGD1.3).
   `numberOfGCMarkers()==1` (zero helpers): Concurrent is NEVER
   scheduled — the cycle degenerates to one window. The legacy
   Mutator-arm (`Heap.cpp:1984-1996`, "served by allocation
   polls") is NOT used when ISS. Wake-ups: helper termination
   `notifyAll` (`SlotVisitor.cpp:645, :745`) + the scheduler
   timeout bound each gap. Every between-window wait is condvar,
   no lock across the wait body, thread access-released —
   §A.3-compatible (the fan-out never needs this thread to poll).
   Cost
   (accepted, C0-C1): the conducting thread is lost to JS for the
   cycle; C2 moves conducting off mutators.

## 4. Handshake generalization (charter item 1)

### 4.1 What replaces each legacy bit

- `hasAccessBit` -> per-client `m_accessState` (landed, heap §10A).
- `stoppedBit` -> GSP + the GBL barrier + WSAC (landed, F7/F8);
  the per-window stop cycle is §3.1-3.2. NO new per-client stop
  bit: the F8 Dekker pair gives each client a sound park/revert
  path (AHA steps 0-3, `Heap.cpp:5707-5758`); the GC-park hooks
  pair release/re-acquire per thread (`:5390-5452`, ungil §A.3.8).
- `mutatorWaitingBit` (mutator waits for the collector to finish a
  window) -> the existing F8 blocking in AHA step 3 / SINFAC. No
  ParkingLot on `m_worldState`; clients block on GBC.
- `mutatorHasConnBit` -> GCA + the §10.2 election (landed). Stage
  C2 re-splits it (§7.2): "conn = collector thread" becomes GCA
  held by the collector-thread conductor; the bit itself stays
  set-idempotent (`Heap.cpp:4499`) and the `checkConn` assert
  (`:1683`) keeps its `|| WSAC` form.
- `m_mutatorDidRun` -> per-client `GCH::m_didRunSinceLastWindow`
  (plain byte, written only by the owning access-holding thread —
  heap I17 discipline). Set in AHA's success tail and SINFAC's
  hot-poll exit; conductor ORs over `clientSet().forEach` at each
  WND-open into the legacy consumer (`Heap.cpp:2234-2237` version
  bump), clears each byte in-window. Scheduling-only: relaxed;
  the window barrier orders it (CG-I9).

### 4.2 Per-client states folded into existing machinery

No new per-client state machine. The per-client record is
exactly: `m_accessState` (landed), `m_didRunSinceLastWindow`
(§4.1), CMS (§5.2), barrier-fence copy + epoch (§5.3), assist
visitor + balance (§7.4), `m_localEpoch` (landed, heap §11).
Conductor iterations over clients run under HCS `m_lock` (rank 6)
or while WSAC (HeapClientSet.h:46-47) — add/remove freeze inside
windows per heap I13 (HeapClientSet.h:54-76), making the
per-window fold/clear loops sound against attach/detach (§9.3).

### 4.3 The "the mutator"-singular audit

ANNEX CGA1 (BINDING) enumerates every singular site in `heap/**`
with disposition ∈ {LANDED-N-ARY, WINDOW-CONFINED, FOLDED
(§4.1/§5), CONDUCTOR-PRIVATE, STAGE-GATED (§7),
VM-SINGULAR-DEFERRED (per-VM not per-thread; ungil/vmstate)}.
Implementation consumes the table verbatim; CG-T1
re-greps and fails on any unclassified match of the annex-header
audit patterns.

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
§10A.1 dispatch, `Heap.h:1061, 1095`) and appends under the
client's own lock. Drains: (i) at every WND-open the conductor transfers
every CMS into `m_sharedMutatorMarkStack` under `m_markingMutex`
(`SlotVisitor::donateAll` shape, `SlotVisitor.cpp:753-765`);
(ii) out-of-window (stage C1+), a client whose CMS exceeds NEW
`Options::sharedGCMutatorMarkStackDonationThreshold` cells (§13.2;
default = one segment, `GCSegmentedArray::s_segmentCapacity`,
`GCSegmentedArray.h:62, :116`) donates directly under
`m_markingMutex` from its own thread. Trigger SITE normative: ONLY
the SINFAC hot poll tail (`Heap.cpp:5107-5149`, after the GSP leg,
access held) — never inside `addToRememberedSet`, whose callers
may hold rank 7-9b allocation locks, forbidden under
`m_markingMutex` (CG-I10: CMS lock is leaf inside
`m_markingMutex`; never either with rank 7-9b held; SINFAC's I6
precondition, `Heap.cpp:5125-5127`, makes the poll site legal). Donation is latency-only
(WND-open drains give correctness; option name fixed rev 2, F8). `!ISS`: server stack, today's code (CG-I0).
`m_barriersExecuted++` (`Heap.cpp:1432`) stays a racy diagnostic
counter (documented benign; relaxed). The cellState CAS protocol
(`:1444-1467`) is already N-safe (single-word CAS;
mutator-count-independent) — CG-T3.

### 5.3 Fence/threshold versioning (per-client mutatorShouldBeFenced)

Today: single `m_mutatorShouldBeFenced` + `m_barrierThreshold`
(`Heap.h:722-726, 1209`), forced tautological once ISS
(`Heap.cpp:3936-3939`) — every barrier fences forever, even between
collections. Re-enable rule:

1. Server master pair stays; mutated ONLY in-window
   (`beginMarking` raise `Heap.cpp:1111`; `endMarking` lower
   `:1247`; `setMutatorShouldBeFenced` drops its ISS forcing
   (`Heap.cpp:3928-3940`) once `useConcurrentSharedGCMarking` AND
   NOT GIL-off — F19, §5.3(3)) — plus a server
   `Atomic<uint64_t> m_barrierFenceEpoch` (FEP) bumped (release)
   at each in-window mutation.
2. GCH gains `m_mutatorShouldBeFenced` + `m_barrierThreshold` +
   `m_fenceEpochSeen`. The conductor republishes master->client for
   EVERY client inside the SAME window that mutated the master
   (under WSAC, before WND-close), stamping `m_fenceEpochSeen =
   FEP`. Clients never write these fields.
3. Consumers re-point: `mutatorShouldBeFenced()`/
   `barrierThreshold()` read the CURRENT CLIENT's copy when ISS
   (else server, CG-I0). JIT: baked `addressOf*` (`Heap.h:723,
   726`) become per-client addresses — GIL phase: the main
   client's; GIL-off: the lite-resolved client's, an A16-class
   lite-indexed reroute — CHARTERED to jit/ungil owners (§13.3);
   until it lands, GIL-off C1+ keeps the SERVER MASTER
   always-fenced — the §5.3(1) forcing stays when GIL-off (rev 4,
   F19: emitted code reads the SERVER pair, never the copies —
   `AssemblyHelpers.h:2045, :2052, :2116`;
   `FTLLowerDFGToB3.cpp:27281, :27323, :27355` — so rev 3's
   copies-only pin under-fenced JIT stores between cycles; ANNEX
   CGD2.2). Master pinned => copies snapshot tautological; FEP
   stays at the raise (CG-I3); CG-T3 arm.
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
per-cell mark: (a) sweep-to-freelist during marking stamps the
live set newlyAllocated (`MarkedBlockInlines.h:244, 281`);
(b) each WND-open's `stopAllocating()` converts in-flight
freelist remainders to newlyAllocated (`MarkedBlock.cpp:201-227`);
(c) `isLive` consults `marksConveyLivenessDuringMarking` +
newlyAllocated versions (`MarkedBlock.cpp:59-106`). All key on
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
   per-window cadence is the only delta. `LocalAllocator`
   non-idempotence (banner `:4822`) is preserved by the
   exactly-once-per-window pairing with the WND-close resume pass
   (`:4923-4925`) (CG-I6).
4. Steal protocol per client: a stolen block re-sweeps under MSPL
   before reuse (heap I8); rule 2 makes its newlyAllocated
   provenance correct under marking. CG-T4 storms steal-vs-marking.
5. Out-of-window allocation during marking is live by
   construction: freelist cells under rule 2 are covered at the
   next WND-open by rule 3; conservative re-scan each fixpoint
   window covers `m_currentBlock` cells (heap I12). The
   `endMarking` snapshot diagnostic (`Heap.cpp:1201-1244`)
   generalizes to CG-T11's debug assert.

## 7. Staged re-enable (charter item 4)

Order NORMATIVE: C1 -> C2 -> C3 -> C4; a stage's flag may be true
only if its predecessors' are (option validation, §13.2). Each
stage retires the §2.2 kill switches it names and NOTHING else.

### 7.1 C1 — concurrent marking

Retires: `Heap.cpp:1957-1958` (fixpoint stays-stopped),
`:1979` (Concurrent-phase assert), the always-fenced forcing
(`:3936`, per §5.3). `runFixpointPhase` may schedule
`CollectorPhase::Concurrent` (only when `numberOfGCMarkers() >= 2`,
§3.7); `finishChangingPhase`'s
periphery pairing (`:2169-2209`) becomes WND-close/WND-open (§3).
`runConcurrentPhase` gains an ISS arm (rev 3, F13): helpers run
`drainFromShared(HelperDrain)` between windows; the conductor runs
the §3.7 donateAll+waitForTermination wait — NEITHER legacy arm is
used when ISS (both mis-branch once shared, §3.7/ANNEX CGD1.3) —
rev 1's "served by SINFAC polls" is superseded by §3.7. Requires: §5
(CMS + fence) and §6 complete; §8 audit executed; §9.1 pause
protocol. Conductor remains the §10.2 requester.

### 7.2 C2 — collector continuity + activity-callback collection

Retires: `shouldCollectInCollectorThread` stays-false
(`Heap.cpp:1636-1648`), the `:1686` assert, activity gating
(`:790-792`), the async reroute (`:1595-1600`).
Design: the collector thread becomes a STANDALONE-CLASS conductor —
`runSharedGCElection`-equivalent tenure (GCL + GCA) but NEVER
holding heap access (not a client; samples the §10.4 barrier like
a VM-less mutator conductor, licensed by §10B.2).
`stopTheMutator`/`resumeTheMutator` (`:2348`, `:2390`) stay DEAD
(CG-I7); their unreachable RELEASE_ASSERTs remain. The
conduct-body refactor surface is ANNEX CGA2 (BINDING):
`conductSharedCollection(GCClient::Heap&)` (`Heap.cpp:4757`) gains
a nullable conductor-client; every conductorClient/vm() use
carries a CGA2 row disposition; the collector run loop
(`Heap.cpp:333-357`) rewires from `shouldCollectInCollectorThread`
to ticket waits on `m_threadCondition`; the `runSharedGCElection`
VMTraps poll (`Heap.cpp:4562-4572`) is SKIPPED for the collector
thread (never enters a VM — CGA2 row R6). Activity callbacks fire
RCAC tickets and notify via the `m_threadCondition`-class signal;
SINFAC conducting stays as fallback (I15 unchanged). Continuity =
GCA MAY be retained across back-to-back granted tickets
(`:2652-2680` analog) but GCA+GCL MUST drop whenever no ticket is
granted (CG-I12 liveness, §9.1).

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
   acquire-load the descriptor and tolerate a stale (smaller)
   bound — blocks added mid-phase are clean by construction
   (newlyAllocated, §6.2(2)). CG-I8; reader table: ANNEX CGB1
   (BINDING).
2. Sweeper identity: the IncrementalSweeper re-homes to a
   dedicated thread registered as a STANDALONE client
   (`markStandalone()`+ACT, heap §12.1 seam) holding access and
   MSPL per quantum (`sweepSynchronously`'s discipline,
   `Heap.cpp:1487-1498`, per-quantum). It participates in
   F8/windows like any client — no sweeper-vs-window special case
   (CG-I13). In-lock allocation-path sweeps unchanged.

### 7.4 C4 — incremental mutator assist

Retires: `performIncrement` ISS early-return (`Heap.cpp:3950-3951`)
and the heap §5.4 world-stopped-only debug-assert. Design:
per-client assist visitor — GCH gains `m_assistVisitor`
(pre-registered in `forEachSlotVisitor` under
`m_parallelSlotVisitorLock` at ACT; unregistered at DCT only
between cycles or in-window, CG-I14) and a per-client
`m_incrementBalance` (the `:1969` reset folds at WND-open).
`performIncrement` routes `didAllocate` bytes (`Heap.cpp:3127,
3140`) to the calling client's visitor;
`performIncrementOfDraining` (`SlotVisitor.cpp:527-585`) runs on
the client's thread WITH access, under its visitor's
`m_rightToRun` — `resumeThePeriphery`'s loop (`Heap.cpp:2315-2342`)
and `updateMutatorIsStopped` (`SlotVisitor.cpp:469-486`) already
handle N visitors. Assist visitors take NO §9.1(2) checkpoint and
live in no marker counter (§9.1(6)). m_opaqueRoots/race-stack
interplay is N-ary-safe today (locked; §2.3(b)).

## 8. Marking vs live mutators (charter item 5)

### 8.1 What stays in-window

Constraint solving stays WINDOW-CONFINED: `executeConvergence`
(`Heap.cpp:1917`) and all root constraints (conservative scan
`:1024-1080`; the `:1037` `ASSERT(!ISS || WSAC)` KEEPS) run only
at fixpoint windows — the Riptide design (Concurrent only
drains), not relaxed. Likewise
`beginMarking`/`endMarking`, `prepareForMarking`, stack/register
gathering (heap §10.6), `finalize()` (`:2753`), and the §11
reclaim (`runSafepointHooksAndReclaim`, `:4961` — fires once per
cycle in the FINAL window, preserving heap I11's legal-context
list verbatim: "§10 step 7" = the cycle's last window, CG-I11).

### 8.2 Cell-lock coverage audit (out-of-window draining)

Stage C1 makes `visitChildren` race mutators. The object-model
protocol is FROZEN and sufficient — om §6 butterfly/structure
rules, om I-series shape storage, ungil §N rulings (N.1
collections cell-locked including reads; N.2 rope
release-CAS/acquire-read; N.5 claim CAS; N.6 buffer torn pairs),
`m_indexingTypeAndMisc` CAS (heap F5). This spec adds the AUDIT
obligation, not new rules: CG-A2 (executed at C1 entry, recorded
as ANNEX CGN1 rows) walks every
`methodTable()->visitChildren`-reachable reader of mutator-mutable
multi-word state and assigns {IN-WINDOW-ONLY, OM-PROTOCOL (cite),
N-PROTOCOL (cite), CELL-LOCKED (10a; visitor MUST tryLock + defer
to race stack / re-visit, NEVER block — a 10a-holding mutator can
be allocation-slow-pathing under MSPL; a blocking marker would
deadlock the next window's flush; CG-I15), RACY-TOLERATED
(justify)}. CELL-LOCK NO-PARK
(CG-I18, NORMATIVE): a thread holding any JSCellLock (10a) must
not release heap access, pass a stop poll (SINFAC/AHA/trap
landing), or enter a conducting path. Grounding: SINFAC's I6
precondition (`Heap.cpp:5125-5127`); F8 revert-and-block parking
lives only in access ACQUISITION (`Heap.cpp:5707-5758`), which a
10a holder (already access-holding) never re-enters; allocation
under 10a is poll-free or drops the lock around the slow path
(the reallocate-butterfly-outside-the-lock idiom). Hence
IN-WINDOW no 10a lock is held, so every visitor tryLock retry at
a fixpoint window succeeds — THIS is the CG-I15 termination
argument (rev 2, F3). CG-A2 classifies every CELL-LOCKED row's MUTATOR side
against CG-I18; debug assert: cell-lock depth == 0 at SINFAC
entry and the AHA park leg (CG-T5 arm). SlotVisitor draining
infrastructure (`drainFromShared` `SlotVisitor.cpp:607`,
safepoints `:522, :578`) is reused except the §9.1(2)
checkpoint/exit deltas. Visitor-side allocation stays forbidden (markers are not
clients; heap I4(c)).

## 9. Composition with the JSThreads stop protocol (charter item 6)

### 9.1 GC cycle vs §A.3 stop — the ordering pin

Today both serialize on GCL: a JSThreads conductor brackets
`Heap::JSThreadsStopScope` (`Heap.cpp:5456-5482`;
`bytecode/JSThreadsSafepoint.cpp:231-337`); a whole-cycle GCL hold
was fine
for one short window. A concurrent tenure is LONG; untouched it
starves §A.3 conductors into the 30s watchdog
(`JSThreadsSafepoint.cpp:401`). PIN (normative):

1. GCL is held by the GC conductor ONLY in-window (§3.4); between
   windows `JSThreadsStopScope` may interleave.
2. A foreign GCL holder mid-cycle must not race marking helpers:
   `JSThreadsStopScope`'s ctor, after acquiring GCL, when
   `m_currentPhase != NotRunning`, calls new
   `Heap::pauseConcurrentMarkingForForeignStop()`; the dtor
   resumes BEFORE releasing GCL (dtor order NORMATIVE: a WND-open
   must not interleave with paused markers). CALL SITES (rev 4, F18): the ctor/dtor
   (`Heap.cpp:5456-5482`, heap-OWNED per §13.1) are the ONLY
   pause/resume sites, covering every scope construction:
   `bytecode/JSThreadsSafepoint.cpp:334-338` (GIL-on stub),
   `runtime/VMManager.cpp:561` (the GIL-off §A.3 conductor —
   every GIL-off jettison's path),
   `SharedHeapTestHarness.cpp:1039+`. No foreign call-site row
   exists (§13.3(b)). MECHANISM (NORMATIVE, rev 3): a NEW pair
   `bool m_parallelMarkersShouldPause` + `unsigned
   m_pausedParallelMarkers`, both guarded by `m_markingMutex`.
   NOT reused: `m_parallelMarkersShouldExit` (one-shot
   cycle-terminate, `Heap.cpp:2027`; `SlotVisitor.cpp:664, :673`)
   and the `:2315-2342` rightToRun loop (history rev 2, F6).
   PARTICIPANT SET (rev 3, F14): the pause pair covers EXACTLY the
   helpers inside `drainFromShared(HelperDrain)` — the counters'
   only maintainers (`SlotVisitor.cpp:620-621, :687-688`). The conductor is in no counter and needs no
   checkpoint (§3.7; window re-open blocks at the GCL acquire,
   §3.1(b), held by the foreign scope); C4 assist visitors take NO
   checkpoint (rule 6). COUNTER PROTOCOL (rev 3, F16): pause = set ShouldPause + `notifyAll`
   `m_markingConditionVariable`, then wait (same mutex/condvar)
   until `m_numberOfActiveParallelMarkers == 0 &&
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
   needs, so the pause terminates (CG-I16). The §A.3 window may
   then jettison/patch: mutators are parked by ITS fan-out;
   markers by THIS hook. The window's ISB generation bump (ungil
   ISB1.1) plus rule 4 covers marker-visible code/object frees.
3. Order pin per cycle edge: WND-open (re-entry) is
   access-released -> GCL -> GSP (§3.1(a)-(c) + the first-window
   carve-out); WND-close clears GSP/WSAC BEFORE releasing GCL.
   Hence at most one of {GC window, §A.3 window} open at any
   instant — single-owner GCL is the proof. The HBT4 order
   (release access -> arbitration -> GCL; ungil §A.3.3) EXTENDS to
   window re-entry (the only blocking GCL acquire; election/poll
   stay tryLock-only, `Heap.cpp:4523, :4585`); the §10.2 GCL-busy
   rule (timed 1ms follower wait, `Heap.cpp:4542-4554`) is
   unchanged. A GC requester midway through someone else's cycle
   parks as a follower on GCA (set whole-cycle, §3.4), not on GCL.
4. No reclamation outside the cycle's final window: heap I11 +
   jit R4/CS4 refusal stand VERBATIM — a mid-cycle §A.3 stop only
   ENQUEUES an RCAC ticket (heap §13.10a; served by the SAME
   cycle's ticket drain, `Heap.cpp:4852-4863`). Memory the paused
   markers can see is freed only at epoch reclaim (final window)
   or via jettison paths already routed through epoch/quarantine
   (om §6 bar; §9 contract hooks fire once-per-CYCLE, final
   window).
5. GC keep-parked vs §A.3 parking stays disjoint per ungil §A.3.8:
   GC stops set client-visible state (GSP/F8); §A.3 stops set none.
   AHA's three gates (GSP `Heap.cpp:5723`, §A.3 word `:5752`,
   mode machine `:5773`) compose unchanged — each re-loops, so a
   client wakes only when NO window of any kind pends.
6. C4 assist visitors vs §A.3 (rev 3, F14):
   `performIncrementOfDraining` (`SlotVisitor.cpp:527-585`)
   maintains no marker counters; its `:578` safepoint takes NO
   shouldPause checkpoint (rationale: history F14). An assist
   mutator is
   bounded by ONE increment (`bytesRequested`), then parked by the
   §A.3 MUTATOR fan-out at its next stop poll. CG-T10 arm.

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
   (inside the same GBL/!WSAC section heap I13 requires). Rev 1's
   flush-first order lost barriers via `lastChanceToFinalize`
   stores (rev 2, F2; interleaving in history). An OPTIONAL
   early flush stays legal; the pre-remove flush is normative. It
   completes while still registered, so a WND-open concurrent with
   exit either sees the client (and drains it) or doesn't (already
   drained) — never a registered client with an unreachable CMS
   (CG-I17). CG-T9 arm: exit with finalizer-side stores during
   forced full concurrent marking.
2. HCS `remove` blocking inside windows (heap I13) extends:
   removal between windows is LEGAL once rule 1 ran — the next
   fixpoint window re-converges with one fewer conservative root
   set, the same soundness argument as a legacy mutator dropping
   references between increments (refs barriered/CMS-drained,
   RHA-published and root-reachable, or garbage).
3. `~VM` (EXIT1.9 fence) composes: the main client survives all
   spawned exits; §10D ISS reversion already requires
   `m_currentPhase == NotRunning` (heap §10D step 1), so a cycle
   never straddles a protocol switch. That covers the revert
   OUTCOME only — the landed PRE-CHECK (`pollIssRevertIfNeeded`,
   `Heap.cpp:5036-5043`) is restructured per the §3.4 `:5036` row
   (rev 3, F11): the main client's next SINFAC poll must not wait
   for GCA under GCL against a live cycle (ANNEX CGD1.2).
4. The detaching thread is never the live conductor: §3.7 forbids
   EXIT1 on `m_gcConductorThread` mid-cycle (release assert) —
   the conductor is in its closed loop, not running JS, so its
   thread cannot reach ungil §B.2 teardown; CG-T9 attempts it.

### 9.3 Mid-cycle client ATTACH (rev 2; cited but unwritten in rev 1)

`HeapClientSet::add` blocks only INSIDE windows (I13 add-side:
insert under GBL with !WSAC, `HeapClientSet.h:54-68`), so a
`Thread()` spawn's ACT (ungil §B.1) may land BETWEEN windows of a
live cycle. Requiring §10B.4 quiescence instead is REJECTED
(blocks spawn for whole cycles; starves vs continuous RCAC).
Rules (NORMATIVE):
1. Fence init handshake: inside the same GBL/!WSAC critical
   section that publishes the client in HCS, BEFORE the insert,
   the attaching thread copies the server master
   `m_mutatorShouldBeFenced`/`m_barrierThreshold` into its client
   copies and stamps `m_fenceEpochSeen = FEP`. Happens-before: the
   master mutates only in-window (§5.3(1), WSAC set under GBL);
   the add runs under GBL with !WSAC — GBL mutual exclusion makes
   the snapshot untorn and never stale across a window edge. An
   attachee during live marking starts RAISED and misses no
   barrier; CG-I3's WND-close assert holds with NO exemption. The
   §5.3(3) always-fenced pin subsumes the copy values while it
   stands; the FEP stamp stays required.
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
- `GCH::m_mutatorMarkStackLock` — leaf, INSIDE `m_markingMutex`
  (CG-I10); never with ranks 7-9b held.
- `m_markingMutex`, `m_parallelSlotVisitorLock`,
  `m_raceMarkStackLock`, visitor `m_rightToRun`: existing
  unranked marking-internal locks; CG-T2 lint adds them as a group
  ordered inside GCL/GBL, disjoint from MSPL-9b except existing
  in-window uses.
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
  marker-pause (one drained batch, §9.1(2); CG-T8 measures).
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
  no RHA/AHA, no EXIT1, access-released throughout (§3.1), every
  between-window wait
  = donateAll+waitForTermination — condvar-parked, in NO marker
  counter, never visitChildren, §A.3-compatible; the blocking GCL
  acquire (re-entry only) happens only access-released (§3.1).
- CG-I20: a client attached while `m_currentPhase != NotRunning`
  has its fence/threshold copies + `m_fenceEpochSeen` stamped from
  the master inside the publishing GBL section, before its first
  allocation (§9.3(1)).
- CG-I21: at most one conductor per cycle: every GCL tryLock site
  (§3.4: `:4523`, `:4585`, `:5036`) backs off when
  `GCA && m_currentPhase != NotRunning`; `m_gcConductorThread` is
  stamped exactly once per cycle (debug assert).
- CG-I22: a paused or exited helper is in neither counter (a
  property CREATED by the F17 exit delta, not landed); a paused
  helper holds no local work (mandatory donateAll);
  `didReachTermination` is false while
  `m_pausedParallelMarkers != 0` (§9.1(2)).

## 12. Verification ladder + TSAN charter (charter item 7)

Per-stage rungs; a flag may default-on only after its rungs AND
all earlier stages' rungs re-run green.

- CG-T1 (C0): ANNEX CGA1 audit executed; audit-pattern grep-lint
  clean; CG-I0 gate — flags-off corpus + `$vm.sharedHeapTest`
  scenarios (heap §12.1) byte-identical; BENCH.md flags-off
  delta = 0 (heap I10 bar).
- CG-T2 (C0): lock-lint (U20-class) + §10 rows; CMS/markingMutex
  order litmus.
- CG-T3 (C1): barrier storm — N threads store-heavy during forced
  concurrent marking; TSAN no-JIT (TSAN.md target) over the
  corpus; amplifier arms (AMPLIFIER.md hooks) at: WND-open barrier
  entry, CMS donate, fence republish, `m_isMarking` resume edge,
  steal-vs-mark. F19 sub-arm: GIL-off C1, two full cycles, then a
  fence storm asserting the SERVER pair stays tautological.
- CG-T4 (C1): allocation/steal storm during marking
  (`allocationStorm`+marking variant); endMarking liveness debug
  assert (§6.2(5)) enabled.
- CG-T5 (C1): CG-A2 cell-lock audit executed (ANNEX CGN1 rows
  complete); per-row arm for every CELL-LOCKED and N-PROTOCOL row;
  CG-I18 storm (map/set mutation vs forced fixpoint windows) with
  the cell-lock-depth debug asserts enabled.
- CG-T6 (C2): collector-continuity churn — RCAC storms, zero
  mutator polls; activity-callback collections fire; SINFAC
  fallback conducts when the collector thread is disabled.
- CG-T7 (C3): T8-extension audit (ANNEX CGB1) executed; sweep
  quantum vs window race arm; sweeper-thread attach/detach churn.
- CG-T8 (all): JSThreads-stop interleaving — haveABadTime/jettison
  (`$vm`) fired mid-concurrent-cycle from a sibling thread,
  including injected exactly at the WND-open GCL acquire (§3.1),
  against a between-windows condvar-parked conductor (§3.7), and
  while helpers are mid-batch with non-empty local stacks
  (CG-I22); F17 sub-arm: stop injected into a SECOND cycle after
  one completed cycle; F18 sub-arm: >=1 jettison arm drives the GIL-off
  `VMManager.cpp:561` conductor, not only the stub;
  GC-requester storm (sync election + poll) from sibling
  threads against a between-windows cycle — assert one
  `m_gcConductorThread` per cycle (CG-I21); measures GCL wait vs
  the 30s watchdog (`JSThreadsSafepoint.cpp:401, :403`);
  `jsThreadsStopVsGCRequester` (heap §12.1) re-run per stage.
- CG-T9 (all): ATTACH/EXIT1 churn — spawn/exit threads continuously
  during forced concurrent cycles (CG-I17/I20); attach storm with
  the §6.2(5) liveness assert (§9.3(5)); exit with finalizer-side
  stores during forced full marking (§9.2(1)); conductor-thread
  exit attempt (must release-assert, §9.2(4)); spawn+exit arming
  `m_issRevertPending` DURING a forced concurrent cycle followed
  by a main-client SINFAC poll storm — assert cycle completion
  (F11); `clientChurnVsGC` +
  `issRevertChurn` re-run (cycle must quiesce before revert).
- CG-T10 (C4): assist storm — per-client balances; assist visitor
  vs WND-open rightToRun handoff arm; §A.3 stop injected
  mid-assist-increment (§9.1(6)): fan-out completes within one
  increment, no paused-count overshoot.
- CG-T11 (all): the §2.4 diagnostics as debug asserts gated on the
  stage flags (freelisted-block check per WND-open; endMarking
  root-liveness check).
- TSAN charter: every CG-T3..T10 arm runs under the TSAN no-JIT
  target; suppressions limited to documented RACY-TOLERATED rows
  (CGA1/CGN1); any other report is a stage blocker. GIL-off rungs
  use the pinned ladder commands (UNGIL-HANDOUT) + the stage flag.

## 13. Files, options, integration

1. Owned (this spec's implementation): `heap/**` only, same set as
   heap §12 + `GCThreadLocalCache`/`HeapClientSet`/
   `GCSafepointEpoch` files; `JSTests/threads/congc-*.js`;
   `docs/threads/INTEGRATE-congc.md` (manifest, written first).
2. Options (`runtime/OptionsList.h` via manifest, heap §13.2
   shape): `useConcurrentSharedGCMarking` (C1),
   `useSharedGCCollectorThread` (C2),
   `useSharedGCIncrementalSweep` (C3), `useSharedGCMutatorAssist`
   (C4) — default false; validation enforces the §7 prefix rule +
   `useSharedGCHeap` prerequisite. Plus
   `sharedGCMutatorMarkStackDonationThreshold` (Unsigned;
   §5.2(ii); default `s_segmentCapacity`, `GCSegmentedArray.h:116`).
3. Chartered out (INTEGRATE rows, not owned): (a) per-client
   barrier-address JIT emission (§5.3(3)) — jit/ungil owners;
   until landed, GIL-off C1+ pins the SERVER master always-fenced
   (F19); (b) DELETED (rev 4, F18): no foreign marker-pause row —
   pause/resume live in the heap-owned scope ctor/dtor (§9.1(2);
   rev 3 miscited a USE site, `:334-337`, as the ctor);
   (c) VMManager manifest deltas: none — `VMManager.cpp:561` is
   covered via the ctor, ZERO VMManager edits (windows reuse the
   manifest-5 hunks).
4. Supersession discipline: nothing frozen is superseded yet; the
   §2.2 kill-switch retires are flag-gated (the `!flag` arm keeps
   frozen behavior, satisfying heap Deviation 4's I10 clause and
   the shared-mode flag-off analog). At freeze, the heap-spec
   "once shared" clauses (SPEC-heap.md:23, §5.4
   `performIncrement`, §10B(7)) gain two-sided supersessions
   pointing here; list = ANNEX CGS1, folded into
   SPEC-heap-history at freeze.

## 14. Ordered task list

- CG-1 (C0 infra): window split of `conductSharedCollection`
  (WND-open/close helpers with the §3.1 order incl. the
  first-window carve-out; GCL per-window; GCA tenure +
  `m_gcConductorThread`; §3.7 closed loop; §3.4 phase-gated guards
  at ALL THREE tryLock sites incl. the `pollIssRevertIfNeeded`
  restructure); CG-T1/T2.
- CG-2 (C0 infra): CMS + per-client fence/didRun fields + window
  fold/republish loops; consumers re-pointed; GIL-phase JIT
  address pin (§5.3(3) fail-safe).
- CG-3 (C1): retire the three C1 kill switches behind the flag;
  marker-helper between-window scheduling + the §3.7/§7.1 ISS
  conductor wait; §9.1(2) pause pair/protocol incl. the F17 exit
  delta (call sites = scope ctor/dtor only, F18 — no foreign
  integration row); §9.2 exit order + §9.3
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

Done = every CG-I has a test/assert (§12); flags-off bench and
behavior gates green; all four stage flags green on the ladder;
INTEGRATE-congc.md matches §13 exactly.
