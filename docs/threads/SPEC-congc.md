# SPEC-congc.md - N-MUTATOR CONCURRENT GC (draft rev 8)

Status: DRAFT rev 8 — freezes after the adversarial pass (history =
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
(§5.3); C1R = routing predicate (§5.2, F33).

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
`mutatorWaitingBit` (asserts `:2354-2384`);
its consumers (`Heap.cpp:2348-2747`) and the single-mutator
periphery carry per-line N-ary dispositions in ANNEX CGA1
(BINDING; indexed §4.3) — the audit of record.

### 2.2 The shared-mode machinery (what we generalize INTO)

Landed per heap §10: ticketing `requestCollectionShared()`
(`Heap.cpp:4479`; conn bit idempotent `:4499`), election
`runSharedGCElection()` (`:4507`; GCL-busy timed wait `:4554`),
poll-conduct `tryConductSharedCollectionForPoll()` (`:4578`),
`conductSharedCollection()` (`:4757`): GSP store (`:4768`), §10.4
GBL barrier (`:4780-4793`), ticket drain (`:4852-4863`),
`runSafepointHooksAndReclaim()` (`:4870`, `:4961-5008`), step-8
resume pass (`:4923-4925`), WSAC/GSP clear + GBC (`:4945-4950`),
VMM resume (`:4955`). Per-client (heap
§10A, cites there): AHA/RHA/SINFAC/park hooks/
`JSThreadsStopScope` (`Heap.cpp:5107-5482`, `:5656-5838`), HCS,
TLC, epoch. Deviation-4 kill switches:
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

Fix-shared-heap-corruption instrumentation (`Heap.cpp:993-1020`,
`:1201-1244`, `:2258-2282`): stop-mode-only; kept behind
`verboseSharedGCHeap`-class options; CG-T11 reuses.

## 3. Architecture: the window model

A shared collection becomes ONE GCA TENURE containing a SEQUENCE
of stop windows (WNDs), not one monolithic stop.

1. WND-open = §10 steps 3-4 + flush, ORDER NORMATIVE: the
   conductor (a) is access-RELEASED (released at the first
   WND-open, stays released all tenure, §3.7); (b) acquires GCL —
   a BLOCKING acquire is legal exactly because the thread is
   access-released (§A.3-fan-out-safe, ungil §A.3 rule 2); the
   HBT4 release-before-GCL order (ungil §A.3.3) EXTENDS to window
   re-entry; (c) seq_cst `GSP=true`;
   (d) `VMManager::requestStopAll(GC)`; (e) GBL barrier until
   every client NoAccess (F8 — `Heap.cpp:4768-4793`);
   set WSAC; per-client flush (§5.2 drain, §6.2 allocator stop,
   today's `stopThePeriphery()` route). Rev 1's GCL-before-release
   order — REJECTED (F9: §A.3 deadlock); CG-T8 arms it.
   FIRST-WINDOW CARVE-OUT (F15): the first WND-open IS the landed
   entry — tryLock access-HELD (`Heap.cpp:4523`, `:4585`;
   non-blocking => §A.3-safe), GSP (`:4768`), THEN the step-3
   release (`:4769-4771`) — flag-off identical (CG-I0); (a)-(c)
   + the blocking acquire govern RE-ENTRY only. TICKET-DRAIN
   SUCCESSOR (F28; ANNEX CGD4.1): a successor cycle of the drain
   loop (`:4852-4863`) RETAINS GCL from the F23 final close; its
   first WND-open = steps (c)-(e) ONLY (ownership transfers;
   access released; GCA/owner stamped). Flags-on only; flag-off
   the drain runs INSIDE the one window (CG-I0).
2. WND-close = §10 steps 8-9: client cache resume pass; ISB
   generation bump when gilOffProcess (EVERY close — each may
   jettison/patch; ISB1.1, `Heap.cpp:4927-4943`); clear WSAC;
   seq_cst `GSP=false`;
   GBC broadcast; `requestResumeAll(GC)`; phase-field publication;
   THEN release GCL (CG-I12) — NON-FINAL closes only. FINAL-CLOSE
   CARVE-OUT (F23): the FINAL close
   (->NotRunning; the conduct loop's m_requests exit
   `Heap.cpp:4852-4863` postdates it) leaves GCL HELD — released
   by the landed CALLER (`:4533` election, `:4600` poll; C2: CGA2
   R7) or TRANSFERRED to an F28 successor's first WND-open —
   flag-off = today's caller-bracketed hold (CG-I0). PHASE-STORE
   ORDER (F22, NORMATIVE under any §13.2 flag):
   finishChangingPhase's phase-field stores (landed `:2213`)
   complete BEFORE the close's GCL release — every reader (§3.4
   guards, §9.1(2) ctor; F34 leaves no other) is GCL-ordered;
   flag-off unaffected (callers hold GCL across it). CG-I4.
   The conductor re-acquires its own access only at the
   landed tail (`Heap.cpp:4955`) after the FINAL WND-close.
   Heap-resume-before-VMM-resume stays normative (heap §10 tail).
3. Between windows: mutators run; marking helpers may run (§7 C1);
   `m_currentPhase` persists, the legacy multi-window shape
   (§2.3(c)). WSAC false between windows; WSAC-gated asserts
   (heap I5) stay correct (§8.1).
4. Tenure: the conductor keeps GCA=true and remains the elected
   §10.2 winner across all windows of the cycle. GCL itself is
   RELEASED between windows and re-acquired at each WND-open
   (CG-I12) — this is what lets a JSThreads stop interleave (§9.1).
   "GCL free && GCA set && phase != NotRunning" thereby becomes a
   STEADY STATE (today only the wind-down instants,
   `Heap.cpp:4534-4537`). EVERY GCL
   tryLock site — landed AND stage-added (CGA2 R7, F24) —
   gets a between-windows disposition (F10/F12). Guard, under `*m_threadLock` after
   tryLock success: `m_gcConductorActive && m_currentPhase !=
   NotRunning` => unlock GCL and back off. Per-site dispositions
   (`:4523` election winner, `:4585` poll, `:5036` §10D revert
   poll/F11/CGD1.2, CGA2 R7/F24): ANNEX CGD6.2 (BINDING; MOVED
   rev 8 under the size cap, content verbatim).
   F28 inter-cycle state {GCL HELD, GCA set, NotRunning, world
   running}: NO guard needed — foreign tryLocks FAIL (election
   falls to `:4550-4554`; poll returns false); bounded by the
   loop's m_requests check.
   WIND-DOWN CLEAR (F20; ruling ANNEX CGD3.1): deferred GCA
   clears postdate the GCL unlock (`:4533/:4536`, `:4600/:4603`).
   NORMATIVE: under `*m_threadLock`, clear GCA +
   `m_gcConductorThread` ONLY if owner == current thread;
   `notifyAll` unconditional; restamp over a non-null owner only
   when NotRunning (debug assert). Flag-off byte-for-byte
   (CGD3.1; CG-I0). CG-I21; CG-T8/T9.
5. Conductor identity: stays `GCConductor::Mutator` running the
   `collectInMutatorThread()` phase loop (heap §10B.2) in stages
   C0-C1; C2 adds a collector-thread conductor (§7.2). GCA gains
   an owner: `m_gcConductorThread` (Thread*, under `*m_threadLock`
   next to GCA, `Heap.h:1290`), stamped/cleared
   with GCA (clear ownership-checked, §3.4 F20). Consumers: the
   §3.4 guards (FOREIGN only), the §9.2 EXIT1 assert, CG-I21.
6. Flag-off degenerate case: all §13.2 flags false => exactly ONE
   window per conduct (`Heap.cpp:1957-1958` fixpoint stays
   stopped) — today's `conductSharedCollection`. CG-I0 by
   inspection + the §12 gates (heap I10 discipline).
7. Conductor tenure contract (NORMATIVE — CG-I19). Conducting is
   a CLOSED LOOP: first WND-open to final WND-close, the conductor
   executes ONLY the phase loop; no heap access all tenure
   (released at WND-open step (a), re-acquired at `Heap.cpp:4955`);
   no JS, no RHA/AHA, no EXIT1 (ungil §B.2 teardown on
   `m_gcConductorThread` mid-cycle release-asserts); NL PIN (F40;
   ANNEX CGD6.1): `m_nativeLockDepth == 0` at conducting entry —
   a Locked-native sync requester reaches the conduct OR follower
   path only through the SPEC-nativeaffinity BL1.8 NL drop scope
   (NA-I13 NARROWED rev 8; both sides).
   BETWEEN windows the conductor's wait is `donateAll()` +
   `waitForTermination(m_scheduler->timeToStop())`
   (`SlotVisitor.cpp:753-758`, `:737-751`) — condvar-only under
   `m_markingMutex` (no lock across the wait body,
   §A.3-compatible), in NEITHER counter, never visitChildren,
   never `drainInParallelPassively`/`drainFromShared` (F13;
   ANNEX CGD1.3). `numberOfGCMarkers()==1`: Concurrent
   NEVER scheduled (one-window degenerate); the legacy Mutator-arm
   (`Heap.cpp:1984-1996`) is NOT used when ISS. Wake-ups: helper
   notifyAll (`:645, :745`) + scheduler timeout. Cost accepted
   (C2 moves conducting off mutators).

## 4. Handshake generalization (charter item 1)

### 4.1 What replaces each legacy bit

- `hasAccessBit` -> per-client `m_accessState` (landed, heap §10A).
- `stoppedBit` -> GSP + the GBL barrier + WSAC (landed, F7/F8);
  the per-window stop cycle is §3.1-3.2. NO new per-client stop
  bit: the F8 Dekker pair gives each client a sound park/revert
  path (AHA steps 0-3, `Heap.cpp:5707-5758`); the GC-park hooks
  pair release/re-acquire per thread (`:5390-5452`, ungil §A.3.8).
- `mutatorWaitingBit` -> the existing F8 blocking in AHA step 3 /
  SINFAC. No ParkingLot on `m_worldState`; clients block on GBC.
- `mutatorHasConnBit` -> GCA + the §10.2 election (landed). Stage
  C2 re-splits it (§7.2): "conn = collector thread" becomes GCA
  held by the collector conductor; the bit itself stays
  set-idempotent (`Heap.cpp:4499`) and the `checkConn` assert
  (`:1683`) keeps its `|| WSAC` form.
- `m_mutatorDidRun` -> per-client `GCH::m_didRunSinceLastWindow`
  (plain byte, owner-thread-only — heap I17). Set in AHA's
  success tail and SINFAC's hot-poll exit; conductor ORs over
  `clientSet().forEach` at each WND-open into the legacy consumer
  (`Heap.cpp:2234-2237` version bump), clears each byte
  in-window. Scheduling-only: relaxed; the window barrier orders
  it (CG-I9). C1R-gated (F33).

### 4.2 Per-client states folded into existing machinery

No new per-client state machine. The per-client record is
exactly: `m_accessState` (landed), `m_didRunSinceLastWindow`
(§4.1), CMS (§5.2), barrier-fence copy + epoch (§5.3), assist
visitor + balance (§7.4), `m_localEpoch` (landed, heap §11).
Conductor iterations over clients run under HCS `m_lock` (rank 6)
or while WSAC (HeapClientSet.h:46-47) — add/remove freeze
in-window per heap I13 (`:54-76`); per-window fold/clear loops
sound vs attach/detach (§9.3).

### 4.3 The "the mutator"-singular audit

ANNEX CGA1 (BINDING) enumerates every singular site in `heap/**`
with disposition ∈ {LANDED-N-ARY, WINDOW-CONFINED, FOLDED
(§4.1/§5), CONDUCTOR-PRIVATE, STAGE-GATED (§7),
VM-SINGULAR-DEFERRED (ungil/vmstate)}.
Implementation consumes the table verbatim; CG-T1
re-greps, failing on unclassified matches.

## 5. Write barrier from N threads (charter item 2)

### 5.1 Inline barrier

Unchanged in shape: `HeapInlines.h:124-135` threshold check +
`writeBarrierSlowPath` (`Heap.cpp:3322`), `mutatorFence()`
(`HeapInlines.h:138`). What changes is (a) where the slow path
appends (§5.2) and (b) where the threshold lives (§5.3).

### 5.2 Per-client mutator mark stack (CMS)

`addToRememberedSet` (`Heap.cpp:1427`) appends to the single
`m_mutatorMarkStack` lock-free (`:1479`) — sound only with one
mutator. Rule: when C1R (:= ISS &&
`useConcurrentSharedGCMarking`; F33/CGD4.4), GCH gains `m_mutatorMarkStack`
(MarkStackArray) + leaf `Lock m_mutatorMarkStackLock`;
`addToRememberedSet` routes via `currentThreadClient()` (heap
§10A.1, `Heap.h:1061, 1095`), appending under the client's lock. Drains: (i) at every WND-open the conductor transfers
every CMS into `m_sharedMutatorMarkStack` under `m_markingMutex`
(`SlotVisitor::donateAll` shape, `SlotVisitor.cpp:753-765`);
(ii) out-of-window (stage C1+), a client whose CMS exceeds NEW
`Options::sharedGCMutatorMarkStackDonationThreshold` cells (§13.2;
default one segment, `GCSegmentedArray.h:116`) donates under
`m_markingMutex` from its own thread. Trigger SITE normative: ONLY
the SINFAC hot poll tail (`Heap.cpp:5107-5149`, after the GSP leg,
access held) — never inside `addToRememberedSet` (7-9b under
`m_markingMutex` barred; CMS lock TERMINAL leaf, legal under
7-9b — CG-I10/F21; SINFAC I6 `:5125-5127` legalizes the site).
Donation is latency-only (WND-open drains give correctness).
`!C1R`: server stack, today's code byte-for-byte (CG-I0).
`m_barriersExecuted++` (`Heap.cpp:1432`) stays a racy diagnostic
counter (benign; relaxed). The cellState CAS protocol
(`:1444-1467`) is already N-safe (single-word CAS;
mutator-count-independent) — CG-T3. CONDUCTOR-CONTEXT barriers
(F31; ANNEX CGD4.5 GOVERNS): runEndPhase's in-window
writeBarrier sites (`Heap.cpp:2036-2039`, `:2085-2088`) with
`currentThreadClient()` NULL (C2) use the SERVER stack + fence
master, in-window only (null => WSAC debug assert; CGA2 R9);
client-conductor in-window CMS appends are NEXT-CYCLE grey; the
relocated `:2032` all-CMS-empty walk stays at the LANDED site,
BEFORE these batches (F37; CGA1 A4). `MarkStackMergingConstraint`
(`MarkStackMergingConstraint.cpp:47, :72`; F32; CGA1 A21)
when C1R covers the SERVER + race stacks only (CMS work
terminates via the WND-open drain, `SlotVisitor.cpp:600-605`);
the §3.1(e) drain PRECEDES the window's first constraint pass
(NORMATIVE).

### 5.3 Fence/threshold versioning (per-client mutatorShouldBeFenced)

Today: single `m_mutatorShouldBeFenced` + `m_barrierThreshold`
(`Heap.h:722-726, 1209`), forced tautological once ISS
(`Heap.cpp:3936-3939`) — every barrier fences forever. Re-enable
rule:

1. Server master pair stays; mutated ONLY in-window
   (`beginMarking` raise `Heap.cpp:1111`; `endMarking` lower
   `:1247`; `setMutatorShouldBeFenced` drops its ISS forcing
   (`:3928-3940`) once `useConcurrentSharedGCMarking` AND
   NOT GIL-off — F19, §5.3(3)) — plus a server
   `Atomic<uint64_t> m_barrierFenceEpoch` (FEP) bumped (release)
   at each in-window mutation.
2. GCH gains `m_mutatorShouldBeFenced` + `m_barrierThreshold` +
   `m_fenceEpochSeen`. The conductor republishes master->client for
   EVERY client inside the SAME window that mutated the master
   (under WSAC, before WND-close), stamping `m_fenceEpochSeen =
   FEP`. Clients never write these fields.
3. Consumers re-point: `mutatorShouldBeFenced()`/
   `barrierThreshold()` read the CURRENT CLIENT's copy when C1R
   (F33; else server, CG-I0). JIT: baked `addressOf*` (`Heap.h:723,
   726`) become per-client addresses (A16-class lite-indexed
   reroute — CHARTERED to jit/ungil owners, §13.3);
   until it lands, GIL-off C1+ keeps the SERVER MASTER
   always-fenced — the §5.3(1) forcing stays when GIL-off (F19;
   emitted code reads ONLY the SERVER pair, ANNEX CGD2.2).
   Master pinned => copies snapshot tautological; FEP
   stays at the raise (CG-I3); CG-T3 arm.
4. Soundness (CG-I3): a RAISE completes for all clients before
   its window closes; a LOWER only in the cycle's final window
   (endMarking), post-termination; over-fenced is always sound.
   Debug assert at every WND-close: `m_fenceEpochSeen == FEP`
   for all clients.
5. `reportExtraMemoryAllocatedPossiblyFromAlreadyMarkedCell`
   (`Heap.cpp:706-740`) and `writeBarrierSlowPath` (`:3322-3333`)
   read the same per-client state; re-whiten reasoning unchanged
   (§5.2).

## 6. Black allocation during marking (charter item 3)

### 6.1 The mechanism is already allocator-local

Allocate-black in Riptide = versioned newlyAllocated, not a
per-cell mark — the §2.3(a) mechanisms, all keyed on
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
   (`Heap.cpp:2217-2256`, banner `:4817-4834`) — N-ary today;
   per-window cadence is the only delta; non-idempotence (`:4822`)
   preserved by exactly-once-per-window pairing with the close's
   resume pass (CG-I6).
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
`runConcurrentPhase` gains an ISS arm (F13): helpers run
`drainFromShared(HelperDrain)` between windows; the conductor runs
the §3.7 donateAll+waitForTermination wait — NEITHER legacy arm is
used when ISS (ANNEX CGD1.3). Requires: §5
(CMS + fence) and §6 complete; §8 audit executed; §9.1 pause
protocol. Conductor remains the §10.2 requester.

### 7.2 C2 — collector continuity + activity-callback collection

Retires: `shouldCollectInCollectorThread` stays-false
(`Heap.cpp:1636-1648`), the `:1686` assert, activity gating
(`:790-792`), the async reroute (`:1595-1600`), the `:4503`
collector-quiesced assert (F38; `m_collectorThreadIsRunning`
fully ruled by CGA2 R10 — `:5051` conjunct KEPT).
Design: the collector thread becomes a STANDALONE-CLASS conductor —
`runSharedGCElection`-equivalent tenure (GCL + GCA) but NEVER
holding heap access (not a client; samples the §10.4 barrier,
licensed by §10B.2).
`stopTheMutator`/`resumeTheMutator` (`:2348`, `:2390`) stay DEAD
(CG-I7); their unreachable RELEASE_ASSERTs remain. The conduct-body refactor surface is ANNEX CGA2 (BINDING):
nullable conductor-client on `conductSharedCollection`
(`Heap.cpp:4757`; per-use rows, F31); the collector run loop
(`Heap.cpp:333-357`) rewired to ticket waits on
`m_threadCondition` (R7 carries the §3.4 guard, F24); the
election VMTraps poll (`:4562-4572`) SKIPPED for the collector
thread (R6). Activity callbacks fire
RCAC tickets and notify via the `m_threadCondition`-class signal;
SINFAC conducting stays as fallback (I15 unchanged). Continuity =
GCA MAY be retained across back-to-back granted tickets
(`:2652-2680` analog) but GCA+GCL MUST drop whenever no ticket is
granted (CG-I12 liveness, §9.1).

### 7.3 C3 — incremental + mutator-concurrent sweeping

Retires: IncrementalSweeper disablement (banner `:4832-4834`).
Pre-req: the T8 BlockDirectoryBits audit EXTENSION (stop-mode
audit exists: `BlockDirectory.cpp:495-519` skip, `:511`, `:556`):
re-audit every BlockDirectoryBits reader/writer for OUT-OF-WINDOW
access. Two structural rules replace per-site reasoning:
1. Bit-vector RESIZE publication: `m_bits` reallocation (heap I5b:
   `addBlock` under BVL+MSPL) additionally RELEASE-publishes the
   new storage; lock-free readers (ANNEX CGB1 rows, BINDING;
   `BlockDirectory.cpp:539-559`)
   acquire-load the descriptor, tolerating a stale (smaller)
   bound — mid-phase blocks are clean by construction
   (newlyAllocated, §6.2(2)). CG-I8.
2. Sweeper identity: the IncrementalSweeper re-homes to a
   dedicated thread registered as a STANDALONE client
   (`markStandalone()`+ACT, heap §12.1 seam) holding access and
   MSPL per quantum (`sweepSynchronously`'s discipline,
   `Heap.cpp:1487-1498`). It participates in
   F8/windows like any client — no sweeper-vs-window special case
   (CG-I13). In-lock allocation-path sweeps unchanged.

### 7.4 C4 — incremental mutator assist

Retires: `performIncrement` ISS early-return (`Heap.cpp:3950-3951`)
and the heap §5.4 world-stopped-only debug-assert. Design:
per-client assist visitor — GCH gains `m_assistVisitor`
(heap-allocated; (un)registered per CG-I14/F34 —
pending-list only) and a per-client
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
at fixpoint windows (Riptide: Concurrent only drains). Likewise
`beginMarking`/`endMarking`, `prepareForMarking`, stack/register
gathering (heap §10.6), `finalize()` (`:2753`), and the §11
reclaim (`runSafepointHooksAndReclaim`, `:4961` — fires once per
cycle in the FINAL window; heap I11's legal-context list reads
"the cycle's last window", CG-I11).

### 8.2 Cell-lock coverage audit (out-of-window draining)

Stage C1 makes `visitChildren` race mutators. The object-model
protocol is FROZEN and sufficient — om §6 butterfly/structure
rules, om I-series shape storage, ungil §N rulings
(N.1/N.2/N.5/N.6), `m_indexingTypeAndMisc` CAS (heap F5). This spec adds the AUDIT
obligation, not new rules: CG-A2 (executed at C1 entry, recorded
as ANNEX CGN1 rows) walks every
`methodTable()->visitChildren`-reachable reader of mutator-mutable
multi-word state and assigns {IN-WINDOW-ONLY, OM-PROTOCOL (cite),
N-PROTOCOL (cite), CELL-LOCKED (10a; tryLock + defer/re-visit,
NEVER block — CGN1 N3; CG-I15), RACY-TOLERATED (justify)}. CELL-LOCK NO-PARK
(CG-I18, NORMATIVE): a thread holding any JSCellLock (10a) must
not release heap access, pass a stop poll (SINFAC/AHA/trap
landing), or enter a conducting path. Grounding + the CG-I15
termination argument: ANNEX CGN1 N3 (SINFAC I6
`Heap.cpp:5125-5127`). CG-A2 classifies every CELL-LOCKED row's MUTATOR side
against CG-I18; debug assert: cell-lock depth == 0 at SINFAC
entry and the AHA park leg (CG-T5 arm). SlotVisitor draining
infrastructure is reused except the §9.1(2) checkpoint/exit
deltas. Visitor-side allocation stays forbidden (markers are not
clients; heap I4(c)).

### 8.3 TID-rebias pin (F35)

ANNEX CGD5.1 (BINDING) GOVERNS. When gilOffProcess, the ungil
§D.1/D1R rebias block (`Heap.cpp:4877-4915`) runs inside the
FINAL window of a conducted FULL cycle — post-reclaim, strictly
before that window's CG-F4 ISB bump and WSAC/GSP clears, under
WSAC; NEVER after a WND-close. Per-cycle predicate when C1R
(replaces the `:4856-4868` aggregate): the FIRST Full cycle with
a Sealed snapshot runs it, single-shot; Eden cycles neither run
nor suppress (Sealed-stays-Sealed safety = CGD5.1(3)). Satisfies
D1/D1R verbatim (CGS1 note); CGA1 grep + CGA2 R3 amended;
CG-I23; CG-T8 F35 sub-arm; flag-off/GIL-on dead/identical
(CG-I0).

## 9. Composition with the JSThreads stop protocol (charter item 6)

### 9.1 GC cycle vs §A.3 stop — the ordering pin

Today both serialize on GCL (`Heap::JSThreadsStopScope`,
`Heap.cpp:5456-5482`; `bytecode/JSThreadsSafepoint.cpp:231-337`);
a concurrent tenure is LONG — untouched it starves §A.3 conductors
into the 30s watchdog (`JSThreadsSafepoint.cpp:401`). PIN
(normative):

1. GCL is held by the GC conductor ONLY in-window or across the
   F28 inter-cycle handoff (§3.1/§3.4); between windows of a cycle
   `JSThreadsStopScope` may interleave.
2. A foreign GCL holder mid-cycle must not race marking helpers:
   `JSThreadsStopScope`'s ctor, after acquiring GCL, when
   `m_currentPhase != NotRunning`, calls new
   `Heap::pauseConcurrentMarkingForForeignStop()` (MARKER pause
   only — the rule-7 sweeper gate is NOT phase-gated, F29); the dtor
   resumes BEFORE releasing GCL (dtor order NORMATIVE: no
   WND-open may interleave with paused markers). CALL SITES
   (F18): the ctor/dtor (`Heap.cpp:5456-5482`, heap-OWNED) are
   the ONLY pause/resume sites — they cover every construction
   (`bytecode/JSThreadsSafepoint.cpp:334-338`;
   `runtime/VMManager.cpp:561`; `SharedHeapTestHarness.cpp:1039+`);
   no foreign row (§13.3(b)). MECHANISM + COUNTER PROTOCOL:
   ANNEX CGP1 (BINDING) GOVERNS (F6/F14/F16/F17 — one batch =
   the CG-I12 bound; flag-off benign CGD2.1; CG-I16/I22). The
   §A.3 window may then jettison/patch (mutators via ITS
   fan-out; markers via THIS hook; C3 sweeper per rule 7); its
   ISB bump (ungil ISB1.1) + rule 4 cover marker-visible frees.
3. Order pin per cycle edge: WND-open (re-entry) is
   access-released -> GCL -> GSP (§3.1(a)-(c) + the first-window
   carve-out); WND-close clears GSP/WSAC BEFORE releasing GCL.
   Hence at most one of {GC window, §A.3 window} open at any
   instant — single-owner GCL is the proof. The HBT4 order EXTENDS to
   window re-entry (the only blocking GCL acquire; election/poll
   stay tryLock-only); the §10.2 GCL-busy rule (`:4542-4554`) is
   unchanged. A GC requester midway through someone else's cycle
   parks as a follower on GCA (set whole-cycle, §3.4), not on GCL.
4. No reclamation outside the cycle's final window: heap I11 +
   jit R4/CS4 refusal stand VERBATIM — a mid-cycle §A.3 stop only
   ENQUEUES an RCAC ticket (heap §13.10a; served by the same
   CONDUCT call's ticket drain, `Heap.cpp:4852-4863` — possibly
   an F28 successor cycle). Memory paused markers can see is
   freed only at epoch reclaim (final window) or via
   epoch/quarantine-routed jettison paths (om §6 bar; contract
   hooks once-per-CYCLE).
5. GC keep-parked vs §A.3 parking stays disjoint per ungil §A.3.8:
   GC stops set client-visible state (GSP/F8); §A.3 stops set none.
   AHA's three gates (GSP `Heap.cpp:5723`, §A.3 word `:5752`,
   mode machine `:5773`) compose unchanged — each re-loops, so a
   client wakes only when NO window of any kind pends.
6. C4 assist visitors vs §A.3 (F14):
   `performIncrementOfDraining` maintains no marker counters; its
   `:578` safepoint takes NO shouldPause checkpoint. An assist
   mutator is bounded by ONE increment (`bytesRequested`), then
   parked by the §A.3 MUTATOR fan-out at its next stop poll.
   CG-T10 arm.
7. C3 sweeper vs §A.3 (F26/F29/F30; derivations CGD3.2/CGD4.2):
   ANNEX CGP1 SWEEPER EXTENSION GOVERNS — flag-keyed
   (`useSharedGCIncrementalSweep` && sweeper registered), NO
   phase gate (sweeping runs BETWEEN cycles); quantum ENTRY
   refused while the pause flag is set (`m_sweeperInQuantum`
   under `m_markingMutex`; access/MSPL only after a gated
   entry); ctor predicate `!m_sweeperInQuantum || acked` (idle
   passes); dtor clears both pre-GCL-release; bound <= one
   quantum. CGS1 row 11; CG-I13; CG-T7 arms.

### 9.2 EXIT1 teardown mid-cycle

A spawned thread's exit (ungil §B.2: RHA -> TEARDOWN mark -> DCT ->
unregister) may land between windows of a live cycle. Rules (the
CMS/fence steps no-op when !C1R — F33, CG-I0):
1. DCT/`~GCClient::Heap` runs the landed teardown FIRST (access
   re-acquire for `lastChanceToFinalize`'s MSPL section, TLC
   `stopAllocatingForGood`), then PERMANENTLY drops access, THEN
   (strictly after its last possible barrier)
   flushes the client's CMS into the shared
   mutator stack (under `m_markingMutex`), then epoch=MAX and
   HCS remove (inside the same GBL/!WSAC section heap I13
   requires). NO dead-state publication (rev 2's clause DELETED —
   F36; rationale = rev 7 log). C4 DELTA
   (F25/F34; CGD3.3/CGD4.3):
   the assist visitor is HEAP-ALLOCATED; ACT/DCT NEVER read
   phase and NEVER mutate the visitor set — they ALWAYS enqueue
   (deferred unregistration with ownership transfer; exit
   CANCELS a pending §9.3(3) registration) under
   `m_parallelSlotVisitorLock`; the pending list is applied only
   at WND-open before any walk, or quiesced per CG-I14 (§10D
   revert, §9.2(5) teardown). Rev 1's flush-first order REJECTED
   (F2); early flush optional; the normative pre-remove flush
   completes while still registered — never a registered client
   with unreachable CMS (CG-I17). CG-T9 arm.
2. HCS `remove` blocking inside windows (heap I13) extends:
   removal between windows is LEGAL once rule 1 ran — the next
   fixpoint window re-converges with one fewer conservative root
   set (refs barriered/CMS-drained, RHA-published and
   root-reachable, or garbage).
3. `~VM` (EXIT1.9 fence) composes: the main client survives all
   spawned exits; §10D ISS reversion already requires
   `m_currentPhase == NotRunning` (heap §10D step 1), so a cycle
   never straddles a protocol switch. That covers the revert
   OUTCOME only — the landed PRE-CHECK (`:5036-5043`) is
   restructured per the §3.4 `:5036` row (F11; CGD1.2).
4. The detaching thread is never the live conductor: §3.7 forbids
   EXIT1 on `m_gcConductorThread` mid-cycle (release assert; the
   closed loop runs no JS); CG-T9 attempts it.
5. SERVER TEARDOWN (F39): ANNEX CGD5.2 (BINDING) GOVERNS. Rule 3
   covers the GIL-on revert OUTCOME only (gilOff: `:5023-5031`
   early return). ~VM after the EXIT1.9 fence, BEFORE any
   access-holding teardown section: disable elections/collector
   wakes; acquire GCL with the §3.4 back-off (teardown ADDED to
   the §3.4 sites) until {GCL held, NotRunning under
   `*m_threadLock`, tickets served-or-refused}; join the
   collector thread; only then the access-holding tail. Teardown
   = the CG-I14 quiesced point (CGD4.3(b) closed). Flag-off
   byte-for-byte. CG-I24; CG-T6 arm.

### 9.3 Mid-cycle client ATTACH

`HeapClientSet::add` blocks only INSIDE windows (I13 add-side:
insert under GBL with !WSAC, `HeapClientSet.h:54-68`), so a
`Thread()` spawn's ACT (ungil §B.1) may land BETWEEN windows of a
live cycle (§10B.4-quiescence alternative REJECTED — F1). Rules
(NORMATIVE):
1. Fence init handshake (!C1R: no-op — copies unrouted, CG-I0;
   F33): inside the same GBL/!WSAC critical
   section that publishes the client in HCS, BEFORE the insert,
   the attaching thread copies the server master
   `m_mutatorShouldBeFenced`/`m_barrierThreshold` into its client
   copies and stamps `m_fenceEpochSeen = FEP`. Happens-before:
   master mutates only in-window (WSAC under GBL); the add runs
   under GBL/!WSAC — snapshot untorn, never stale; a live-marking
   attachee starts RAISED; CG-I3's close assert holds unexempted;
   the §5.3(3) pin subsumes the values, the FEP stamp stays.
2. `m_isMarking` visibility: the client's first AHA performs the
   seq_cst GSP load (`Heap.cpp:5723`) and the GBL section above
   pairs with the in-window flip (§6.2(1)); allocation before ACT
   completes is impossible (ungil §B.1).
3. C4 assist visitor: ACT ALWAYS ENQUEUES registration to the
   pending list (NO phase read — F34), applied per §9.2(1)
   under `m_parallelSlotVisitorLock`. An EXIT1 in the same gap
   CANCELS it (F25). Until applied, the client cannot assist
   (`performIncrement` checks registration); its barriers/CMS are
   live immediately.
4. didRun = false, CMS = empty at ACT (zero-init; benign).
5. CG-T9 attach-storm arm (§12).

## 10. Lock & fence deltas

Lock table (heap §6 master) deltas — additions only, no re-ranks.
BOTH rows are PROPOSED SPEC-ungil §LK amendments (the ONE merged
order, canonical for U20; SPEC-ungil.md:867-925), recorded
SUPERSESSION-PENDING + adoption gate §13.5(1) per the
nativeaffinity §3.5/§9 convention (F41; FULL rows + acyclicity
walks = ANNEX CGS2.1-2, BINDING):
- LK.9c `GCH::m_mutatorMarkStackLock` — TERMINAL leaf, INSIDE
  `m_markingMutex`; MAY be taken with ranks 7-9b held (the §5.2
  append path; CG-I10/F21) — the §LK.8 destructor-leaf-class
  shape, superseding heap §6's leaf-row "never 7-9b" for this
  lock, BOTH sides.
- LK.9d marking-internal group (`m_markingMutex`,
  `m_parallelSlotVisitorLock`, `m_raceMarkStackLock`, visitor
  `m_rightToRun`): ordered INSIDE GCL/GBL; newly MUTATOR-reachable
  out-of-window (§5.2 SINFAC-tail donation; §9.2(1)/§9.3(3)
  pending-list enqueue); disjoint from MSPL-9b except existing
  in-window uses. U20 PROPER extends to both rows — CG-T2 IS that
  U20 extension, not a private lint. The composed
  NL > GCL > `m_markingMutex` > CMS chain (§9.1(2) +
  nativeaffinity BL1.6/LK.1c) is CHARTERED and U20-linted, no
  longer accidental — walk = CGS2.2.
Fences (heap §7 deltas):
- F4/F7/F8 unchanged; F8's Dekker proof is per-WINDOW — cadence
  only.
- CG-F1: FEP store(release) in-window; client copies written by the
  conductor in-window; clients read own copy plain (owner-thread;
  the window barrier synchronizes conductor writes).
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
  access-holding thread under its CMS lock (exempt: in-window
  conductor-context appends — F31, WSAC single-writer); every
  drain happens under `m_markingMutex` (conductor in-window or
  owner out-of-window).
- CG-I3: fence RAISES are window-complete (all client copies
  republished before WND-close); LOWERS occur only in the final
  window after termination; `m_fenceEpochSeen == FEP` for every
  client at every WND-close (debug assert).
- CG-I4: `m_isMarking`, `m_collectionScope`, phase transitions, and
  marking/newlyAllocated version bumps mutate in-window only — for
  the phase fields, in-window = before the closing GCL release
  (§3.2 F22).
- CG-I5: no sweep-to-freelist, steal, or `addBlock` straddles a
  window (MSPL implies access implies barred from open windows).
- CG-I6: per window, each LA is stop-flushed exactly once and
  resumed exactly once (LocalAllocator non-idempotence preserved).
- CG-I7: `stopTheMutator`/`resumeTheMutator`/`m_worldState`
  stop bits stay unreachable when ISS, ALL stages (C2 uses
  windows, never the legacy bits).
- CG-I8: every BlockDirectoryBits access is (a) in-window, (b)
  under BVL, (c) under MSPL, (d) an acquire-read of release-
  published storage tolerating a stale bound (CGB1 rows), or
  (e) `!ISS`.
- CG-I9: `m_didRunSinceLastWindow` is written only by its owning
  access-holding thread; folded+cleared only in-window.
- CG-I10 (F21): (1) `m_markingMutex` is never acquired
  with any rank 7-9b lock held; (2) the CMS lock is a TERMINAL
  leaf (no lock of any rank acquired while holding it) and MAY be
  taken with 7-9b held (the append path — no cycle); (3) order
  `m_markingMutex` > CMS only at the 7-9b-free drain/donation
  sites (WND-open drain: clients parked; SINFAC tail: I6).
- CG-I11: heap I11 verbatim — epoch bump/reclaim only in the cycle's
  FINAL window (legacy `runEndPhase` when `!ISS`); never from a
  §A.3 stop; hooks fire once per cycle.
- CG-I12: GCL never held by the GC conductor between windows of
  a cycle (F28 handoff excepted); GCA without a granted ticket is
  dropped (C2 bound) — a §A.3 conductor's GCL wait <= one window
  + one marker-pause (one batch, §9.1(2)) + (F28 successor) the
  re-stop + its FIRST window (GCL next free at its first
  non-final close); CG-T8 measures both. In-bracket wait BUDGET
  vs the watchdog (incl. the nativeaffinity BL1.6/BL1.8 terms)
  stated ONCE: ANNEX CGS2.3 (F42).
- CG-I13: every sweeping thread is a registered client holding
  access for each quantum, MSPL across each quantum's directory/bit
  mutations; quantum entry is flag-gated and §A.3 stops are acked
  per the §9.1(7) protocol (phase-independent, F29/F30).
- CG-I14: the `forEachSlotVisitor` set mutates only at the
  WND-open pending-list application or quiesced (GCL held + phase
  NotRunning under `*m_threadLock`), all under
  `m_parallelSlotVisitorLock`; ACT/DCT only ENQUEUE (F34); every
  visitor in it has a live owner (client, conductor, helper pool,
  or the pending list).
- CG-I15: marker threads never BLOCK on a JSCellLock (tryLock +
  revisit/race-stack only) and never allocate from the shared heap.
- CG-I16: `pauseConcurrentMarkingForForeignStop()` terminates
  without acquiring any api-rank or heap rank ≥7 lock; paired
  resume in the same `JSThreadsStopScope`.
- CG-I17: a detaching client's CMS is empty by the time it
  leaves HCS; the final flush postdates its last possible
  barrier (§9.2(1)); its assist visitor is out of the visitor
  set — or owned by the pending list — before its storage is
  destroyed (F25).
- CG-I18: no thread releases heap access, passes a stop poll, or
  enters a conducting path while holding a JSCellLock (§8.2);
  debug assert: cell-lock depth == 0 at SINFAC entry / AHA park.
- CG-I19: conductor tenure is a closed loop (§3.7): no JS, no
  RHA/AHA, no EXIT1, access-released throughout; between-window
  waits = donateAll+waitForTermination (condvar, no counter,
  never visitChildren, §A.3-compatible); blocking GCL acquires
  only access-released (§3.1); `m_nativeLockDepth == 0` at
  conducting entry (F40, §3.7 — nativeaffinity BL1.8, both
  sides; debug assert).
- CG-I20: a client attached mid-cycle has its fence/threshold
  copies + `m_fenceEpochSeen` stamped from the master inside the
  publishing GBL section, before its first allocation (§9.3(1)).
- CG-I21: at most one conductor per cycle: every GCL tryLock
  site, landed AND stage-added (§3.4), backs off when
  `GCA && m_currentPhase != NotRunning`; `m_gcConductorThread` is
  stamped once per cycle (restamp legal only in NotRunning
  wind-down takeover) and cleared ONLY by its owner (§3.4 F20;
  both debug-asserted).
- CG-I22: a paused/exited helper is in neither counter (F17 exit
  delta); a paused helper holds no local work (donateAll);
  `didReachTermination` is false while
  `m_pausedParallelMarkers != 0` (§9.1(2)).
- CG-I23: the Sealed->Restamped flip + D1R fires run only inside
  a FULL cycle's FINAL window (post-reclaim,
  pre-ISB-bump/GSP-clear, WSAC), never after a WND-close; one
  rebias per Sealed snapshot (§8.3).
- CG-I24: server teardown destroys no conduct state and runs no
  access-holding section until {wakes disabled, GCL held,
  NotRunning, tickets quiesced, collector joined} (§9.2(5)); the
  EXIT1.9 wait precedes it.

## 12. Verification ladder + TSAN charter (charter item 7)

Per-stage rungs; a flag may default-on only after its rungs AND
all earlier stages' rungs re-run green.

- CG-T1 (C0): ANNEX CGA1 audit executed; audit-pattern grep-lint
  clean; CG-I0 gate — flags-off corpus + `$vm.sharedHeapTest`
  scenarios (heap §12.1) byte-identical; BENCH.md flags-off
  delta = 0 (heap I10 bar). F33: routing C1R-gated — flag-off
  keeps `:1479` + server fence reads.
- CG-T2 (C0): U20 lint EXTENDED to the §10 LK.9c/9d rows (F41;
  gate §13.5(1)) encoding the three F21 clauses
  (barrier-append-under-7-9b lint-clean by rule);
  CMS/markingMutex order litmus + the CGS2.2 chain litmus.
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
- CG-T6 (C2): collector-continuity churn (FULL charter = ANNEX
  CGT1.3, BINDING; MOVED rev 8 under the size cap, verbatim):
  RCAC storms; activity-callback collections; SINFAC fallback;
  R7/F24 guard arm; R10/F38 arm; F39 ~VM-vs-cycle arm.
- CG-T7 (C3): T8-extension audit (ANNEX CGB1) executed (FULL
  charter = ANNEX CGT1.4, BINDING; MOVED rev 8, verbatim): sweep
  quantum vs window race; sweeper attach/detach churn;
  F26/F29/F30 arms (mid-quantum ack, idle liveness, delayed
  timer entry-refusal).
- CG-T8 (all): JSThreads-stop interleaving (FULL charter = ANNEX
  CGT1.1, BINDING; MOVED rev 8 under the size cap, verbatim +
  the F40 arm): haveABadTime/jettison mid-concurrent-cycle arms
  incl. the WND-open GCL acquire, a between-windows parked
  conductor, helpers mid-batch (CG-I22); F17/F18/F20/F22/F28/F35
  sub-arms; NEW F40 sub-arm: NL-holding Locked-native sync
  requester — BL1.8 drop releases NL for the tenure, sibling
  Locked natives PROGRESS mid-cycle, depth-restored reacquire
  after the `:4955` tail; GC-requester storm (CG-I21); GCL wait
  vs the 30s watchdog measured against the CGS2.3 budget;
  `jsThreadsStopVsGCRequester` (heap §12.1) re-run per stage.
- CG-T9 (all): ATTACH/EXIT1 churn during forced concurrent cycles
  (CG-I17/I20; FULL charter = ANNEX CGT1.2, BINDING; MOVED rev 8
  under the size cap, verbatim): attach storm (§9.3(5));
  finalizer-side stores during full marking (§9.2(1));
  F36/F11/F25/F34 arms; conductor-thread exit release-assert
  (§9.2(4)); `clientChurnVsGC` + `issRevertChurn` re-run.
- CG-T10 (C4): assist storm — per-client balances; assist visitor
  vs WND-open rightToRun handoff arm; §A.3 stop injected
  mid-assist-increment (§9.1(6)): fan-out completes within one
  increment, no paused-count overshoot.
- CG-T11 (all): the §2.4 diagnostics as debug asserts gated on the
  stage flags (freelisted-block check per WND-open; endMarking
  root-liveness check); A4-site all-CMS-empty walk exercised at
  C1 with executing CodeBlocks present (F37).
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
   `sharedGCMutatorMarkStackDonationThreshold` (Unsigned; default
   per §5.2(ii)).
3. Chartered out (INTEGRATE rows, not owned): (a) per-client
   barrier-address JIT emission — jit/ungil owners (§5.3(3) F19
   server pin until landed); (b) DELETED (F18) — pause/resume
   live in the heap-owned scope ctor/dtor (§9.1(2));
   (c) VMManager manifest deltas: none — `VMManager.cpp:561`
   covered via the ctor; ZERO VMManager edits.
4. Supersession discipline (REWRITTEN rev 8, F42 — rev 7's
   "nothing frozen is superseded" claim was FALSE for SPEC-ungil):
   the §2.2 kill-switch retires stay flag-gated (the `!flag` arm
   keeps frozen behavior — heap Deviation 4's I10 clause). TWO
   frozen specs are touched: SPEC-heap (list = ANNEX CGS1) and
   SPEC-ungil (the §10/F41 §LK rows; the §9.1(2) ctor/dtor pause
   obligation vs §A.3 rule 5/HBT4.5; the conductor wait bound vs
   U32; the HBT4 extension — list = ANNEX CGS2, every row
   SUPERSESSION-PENDING behind a §13.5 gate, the nativeaffinity
   §9 convention). At freeze CGS1 folds into SPEC-heap-history
   AND CGS2 into SPEC-ungil-history, recorded both sides.
5. ADOPTION GATES (rev 8; rows NOT in force until the named owner
   lands the cross-cite — gate semantics per nativeaffinity §9):
   (1) SPEC-ungil owner lands §LK rows LK.9c/LK.9d + the U20
   extension (CGS2.1-2), both sides — BLOCKS the §5.2 CMS lock
   and stage C1. (2) the §A.3 rule-5/HBT4.5 amendment — the
   §9.1(2) ctor pause obligation + dtor order — plus the
   conductor wait bound vs U32/the watchdog (CGS2.3 ledger,
   shared with nativeaffinity BL1.6/BL1.8), both sides — BLOCKS
   C1. (3) the HBT4 release-before-GCL order EXTENDED to window
   re-entry (CGS2.4), both sides — BLOCKS the §3.1 re-entry
   blocking acquire.
   (4) nativeaffinity rev 8 BL1.8 NL-drop ruling (F40; CG-I19's
   depth==0 assert = this spec's side) — BLOCKS C1 in gilOff
   configs.

## 14. Ordered task list

- CG-1 (C0 infra): window split of `conductSharedCollection`
  (WND-open/close helpers with the §3.1 order incl. the
  first-window carve-out + the F28 successor arm; GCL per-window; GCA tenure +
  `m_gcConductorThread`; §3.7 closed loop; §3.4 phase-gated
  guards at the landed tryLock sites incl. the `:5036`
  restructure); CG-T1/T2.
- CG-2 (C0 infra): CMS + per-client fence/didRun fields + window
  fold/republish loops; consumers re-pointed (C1R-gated, F33); GIL-phase JIT
  address pin (§5.3(3) fail-safe).
- CG-3 (C1): retire the three C1 kill switches behind the flag;
  marker-helper between-window scheduling + the §3.7/§7.1 ISS
  conductor wait; §9.1(2) pause pair/protocol incl. the F17 exit
  delta (F18: ctor/dtor only); §8.3 rebias pin (F35); §9.2 exit
  order + §9.3 attach handshake; donation threshold option;
  CG-A2 audit -> ANNEX CGN1; CG-T3/T4/T5/T8/T9/T11.
- CG-4 (C2): collector-thread conductor; activity-callback RCAC
  routing; continuity bound; CGA2 R10 flag ruling (F38); §9.2(5)
  teardown shutdown (F39); CG-T6 + re-runs.
- CG-5 (C3): ANNEX CGB1 audit; bit-storage release/acquire
  publication; sweeper-client re-home + the §9.1(7) entry-gated
  pause (F29/F30); CG-T7 + re-runs.
- CG-6 (C4): per-client assist visitors/balances; `performIncrement`
  re-enable; CG-T10 + re-runs; BENCH.md stage gates recorded.
- CG-7: freeze pass — ANNEX CGS1 + CGS2 supersessions recorded
  both sides (SPEC-heap-history AND SPEC-ungil-history); §13.5
  gates closed; size-cap check; rev history updated.

Done = every CG-I has a test/assert (§12); flags-off bench and
behavior gates green; all four stage flags green on the ladder;
INTEGRATE-congc.md matches §13 exactly.
