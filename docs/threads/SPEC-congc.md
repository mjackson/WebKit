# SPEC-congc.md - N-MUTATOR CONCURRENT GC (draft rev 6)

Status: DRAFT rev 6 — freezes after the adversarial pass (history =
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
`mutatorWaitingBit` (state-machine asserts `Heap.cpp:2354-2384`);
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
§10A): AHA/RHA/SINFAC/park hooks/`JSThreadsStopScope`
(`Heap.cpp:5107-5482`, `:5656-5838`),
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
   every client NoAccess (F8 unchanged — `Heap.cpp:4768-4793`);
   set WSAC; per-client flush (§5.2 drain, §6.2 allocator stop,
   today's `stopThePeriphery()` route). Rev 1's GCL-before-release
   order — REJECTED (F9: §A.3 deadlock); CG-T8 arms it. FIRST-WINDOW
   CARVE-OUT (F15): the first WND-open IS the landed
   entry — GCL tryLock while access-HELD (`Heap.cpp:4523`,
   `:4585`; non-blocking => §A.3-safe), then GSP (`:4768`), THEN
   the step-3 release (`:4769-4771`) — flag-off identical (CG-I0).
   (a)->(b)->(c) and the blocking acquire govern RE-ENTRY only
   (F9 likewise). TICKET-DRAIN SUCCESSOR (F28; full text
   ANNEX CGD4.1): a successor cycle of the conduct loop
   (`:4852-4863`) RETAINS GCL from the predecessor's F23 final
   close; its first WND-open = steps (c)-(e) ONLY (no GCL
   acquire — ownership transfers; access stays released;
   GCA/owner stay stamped). Flags-on only
   (`useConcurrentSharedGCMarking`); flag-off the drain runs
   INSIDE the one window (CG-I0).
2. WND-close = §10 steps 8-9: client cache resume pass; ISB
   generation bump when gilOffProcess (REQUIRED at EVERY window
   close — each window may jettison/patch; extends ISB1.1,
   `Heap.cpp:4927-4943`); clear WSAC; seq_cst `GSP=false`;
   GBC broadcast; `requestResumeAll(GC)`; phase-field publication;
   THEN release GCL (CG-I12) — NON-FINAL closes only. FINAL-CLOSE
   CARVE-OUT (F23; symmetric to F15): the FINAL close
   (->NotRunning; the conduct loop's m_requests exit
   `Heap.cpp:4852-4863` postdates it) leaves GCL HELD — released
   by the landed CALLER (`:4533` election, `:4600` poll; C2: CGA2
   R7) or TRANSFERRED to an F28 successor's first WND-open —
   flag-off = today's caller-bracketed hold (CG-I0;
   per-close release would double-unlock). PHASE-STORE ORDER
   (F22, NORMATIVE under any §13.2 flag):
   finishChangingPhase's phase-field updates (`m_currentPhase`/
   `m_lastPhase`/`m_nextPhase`/`m_phaseVersion`; the landed
   `:2213` store postdates `:2187` resumeThePeriphery) complete
   BEFORE the close's GCL release, so every reader (§3.4 guards,
   §9.1(2) ctor — the F34 rewrite leaves NO other phase reader)
   is GCL-ordered; flag-off unaffected (callers hold
   GCL across the store today). Cited by CG-I4.
   The conductor re-acquires its own access only at the
   landed tail (`Heap.cpp:4955`) after the FINAL WND-close.
   Heap-resume-before-VMM-resume stays normative (heap §10 tail).
3. Between windows: mutators run; marking helpers may run (§7 C1);
   `m_currentPhase` persists, the legacy multi-window shape
   (§2.3(c)). WSAC false between windows; WSAC-gated asserts
   (heap I5) stay correct (guarded work stays in-window, §8.1).
4. Tenure: the conductor keeps GCA=true and remains the elected
   §10.2 winner across all windows of the cycle. GCL itself is
   RELEASED between windows and re-acquired at each WND-open
   (CG-I12) — this is what lets a JSThreads stop interleave (§9.1).
   "GCL free && GCA set && phase != NotRunning" thereby becomes a
   STEADY STATE (today only the wind-down instants,
   `Heap.cpp:4534-4537`, phase already NotRunning). EVERY GCL
   tryLock site — landed AND stage-added (CGA2 R7, F24) —
   gets a between-windows disposition (F10/F12). Guard, under `*m_threadLock` after
   tryLock success: `m_gcConductorActive && m_currentPhase !=
   NotRunning` => unlock GCL and back off:
   - `:4523` election winner: fall through to the follower wait
     (`:4550-4554`) (else CGD1.1 nesting).
   - `:4585` poll: return false; retry next poll.
   - `:5036` §10D revert poll: return, hint stays armed (F11,
     §9.2(3); the landed `:5040-5043` pre-check deadlocks, ANNEX
     CGD1.2); bounded wind-down wait legal only when NotRunning.
   - CGA2 R7 (C2): unlock GCL, re-wait on the ticket signal (F24;
     CG-T6 arm).
   F28 inter-cycle state {GCL HELD, GCA set, NotRunning, world
   running}: NO guard needed — foreign tryLocks FAIL (election
   falls to `:4550-4554`; poll returns false); bounded by the
   loop's m_requests check.
   WIND-DOWN CLEAR (F20; interleaving + benign ruling:
   ANNEX CGD3.1): the landed deferred GCA clears postdate the GCL
   unlock (`Heap.cpp:4533`/`:4536`; `:4600`/`:4603`) and can land
   mid-successor-cycle. NORMATIVE: under `*m_threadLock`, clear
   GCA + `m_gcConductorThread` ONLY if owner == current thread;
   `notifyAll` unconditional; restamp over a non-null owner legal
   only when phase == NotRunning (debug assert). Flag-off: the
   guard is unreachable (tryLock success implies NotRunning), the
   clear delta benign (CGD3.1) — byte-for-byte (CG-I0). CG-I21;
   CG-T8/T9.
5. Conductor identity: stays `GCConductor::Mutator` running the
   `collectInMutatorThread()` phase loop (heap §10B.2) in stages
   C0-C1; C2 adds a collector-thread conductor (§7.2). GCA gains
   an owner: `m_gcConductorThread` (Thread*, under `*m_threadLock`
   next to GCA, `Heap.h:1290`), stamped/cleared
   with GCA (clear ownership-checked, §3.4 F20). Consumers: the §3.4 guards (FOREIGN only;
   the conductor never polls mid-cycle, rule 7), the §9.2 EXIT1
   assert, CG-I21.
6. Flag-off degenerate case: all §13.2 flags false => exactly ONE
   window per conduct (`Heap.cpp:1957-1958` fixpoint stays
   stopped) — today's `conductSharedCollection`. CG-I0 by
   inspection + the §12 gates (heap I10 discipline).
7. Conductor tenure contract (NORMATIVE — CG-I19). Conducting is
   a CLOSED LOOP: first WND-open to final WND-close, the conductor
   executes ONLY the phase loop; no heap access all tenure
   (released at WND-open step (a), re-acquired at `Heap.cpp:4955`);
   no JS, no RHA/AHA, no EXIT1 (ungil §B.2 teardown on
   `m_gcConductorThread` mid-cycle release-asserts).
   BETWEEN windows the conductor's wait is `donateAll()` +
   `waitForTermination(m_scheduler->timeToStop())`
   (`SlotVisitor.cpp:753-758`, `:737-751`) — condvar-only under
   `m_markingMutex` (no lock across the wait body,
   access-released, §A.3-compatible), in NEITHER counter, never
   visitChildren,
   never `drainInParallelPassively`/`drainFromShared` (F13;
   ANNEX CGD1.3). `numberOfGCMarkers()==1`: Concurrent
   NEVER scheduled (one-window degenerate); the legacy Mutator-arm
   (`Heap.cpp:1984-1996`) is NOT used when ISS. Wake-ups: helper
   termination notifyAll (`SlotVisitor.cpp:645, :745`) + scheduler
   timeout. Cost (accepted): the conducting thread is lost to JS
   until C2 moves conducting off mutators.

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
  held by the collector conductor; the bit itself stays
  set-idempotent (`Heap.cpp:4499`) and the `checkConn` assert
  (`:1683`) keeps its `|| WSAC` form.
- `m_mutatorDidRun` -> per-client `GCH::m_didRunSinceLastWindow`
  (plain byte, written only by the owning access-holding thread —
  heap I17 discipline). Set in AHA's success tail and SINFAC's
  hot-poll exit; conductor ORs over `clientSet().forEach` at each
  WND-open into the legacy consumer (`Heap.cpp:2234-2237` version
  bump), clears each byte in-window. Scheduling-only: relaxed;
  the window barrier orders it (CG-I9). C1R-gated (F33): flag-off
  keeps the landed shared-mode behavior.

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
mutator. Rule: when C1R (:= ISS && `useConcurrentSharedGCMarking`;
F33 — ISS-keyed routing broke the flag-off BYTE-FOR-BYTE bar;
ruling ANNEX CGD4.4), GCH gains `m_mutatorMarkStack`
(MarkStackArray) + leaf `Lock m_mutatorMarkStackLock`;
`addToRememberedSet` routes via `currentThreadClient()` (heap
§10A.1, `Heap.h:1061, 1095`), appending under the client's lock. Drains: (i) at every WND-open the conductor transfers
every CMS into `m_sharedMutatorMarkStack` under `m_markingMutex`
(`SlotVisitor::donateAll` shape, `SlotVisitor.cpp:753-765`);
(ii) out-of-window (stage C1+), a client whose CMS exceeds NEW
`Options::sharedGCMutatorMarkStackDonationThreshold` cells (§13.2;
default = one segment, `GCSegmentedArray::s_segmentCapacity`,
`GCSegmentedArray.h:116`) donates under
`m_markingMutex` from its own thread. Trigger SITE normative: ONLY
the SINFAC hot poll tail (`Heap.cpp:5107-5149`, after the GSP leg,
access held) — never inside `addToRememberedSet` (callers may hold
rank 7-9b allocation locks, barred under `m_markingMutex`; the CMS
lock is a TERMINAL leaf, legal under 7-9b — CG-I10/F21; SINFAC's
I6 precondition `Heap.cpp:5125-5127` legalizes the poll site).
Donation is latency-only (WND-open drains give correctness).
`!C1R`: server stack, today's code byte-for-byte (CG-I0).
`m_barriersExecuted++` (`Heap.cpp:1432`) stays a racy diagnostic
counter (documented benign; relaxed). The cellState CAS protocol
(`:1444-1467`) is already N-safe (single-word CAS;
mutator-count-independent) — CG-T3. CONDUCTOR-CONTEXT barriers
(F31; FULL text ANNEX CGD4.5, GOVERNS): runEndPhase's in-window
writeBarrier sites (`Heap.cpp:2036-2039`, `:2085-2088`) with
`currentThreadClient()` NULL (C2) use the SERVER stack (`:1479`)
+ SERVER fence master, in-window only (debug assert: null =>
WSAC; CGA2 R9); a client-conductor's in-window CMS appends are
NEXT-CYCLE grey. `MarkStackMergingConstraint`
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
   always-fenced — the §5.3(1) forcing stays when GIL-off (rev 4,
   F19: emitted code reads ONLY the SERVER pair —
   reader table: ANNEX CGD2.2). Master pinned => copies snapshot tautological; FEP
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
per-cell mark — the §2.3(a) mechanisms (sweep-time stamping,
stopAllocating conversion, marking-aware `isLive`), all keyed on
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
   per-window cadence is the only delta; non-idempotence (banner
   `:4822`) preserved by exactly-once-per-window pairing with the
   WND-close resume pass (`:4923-4925`) (CG-I6).
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
(`:790-792`), the async reroute (`:1595-1600`).
Design: the collector thread becomes a STANDALONE-CLASS conductor —
`runSharedGCElection`-equivalent tenure (GCL + GCA) but NEVER
holding heap access (not a client; samples the §10.4 barrier,
licensed by §10B.2).
`stopTheMutator`/`resumeTheMutator` (`:2348`, `:2390`) stay DEAD
(CG-I7); their unreachable RELEASE_ASSERTs remain. The
conduct-body refactor surface is ANNEX CGA2 (BINDING):
`conductSharedCollection(GCClient::Heap&)` (`Heap.cpp:4757`) gains
a nullable conductor-client; every conductorClient/vm()/
TLS-client use carries a CGA2 row disposition (F31); the collector run loop
(`Heap.cpp:333-357`) rewires from `shouldCollectInCollectorThread`
to ticket waits on `m_threadCondition` (R7's tryLock carries the
§3.4 guard — rev 5, F24); the `runSharedGCElection`
VMTraps poll (`Heap.cpp:4562-4572`) is SKIPPED for the collector
thread (CGA2 R6). Activity callbacks fire
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
   new storage; lock-free readers (ANNEX CGB1 rows, BINDING; e.g.
   `parallelNotEmptyBlockSource`, `BlockDirectory.cpp:539-559`)
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
N-PROTOCOL (cite), CELL-LOCKED (10a; tryLock + defer to race
stack / re-visit, NEVER block — deadlock derivation ANNEX CGN1
N3; CG-I15), RACY-TOLERATED
(justify)}. CELL-LOCK NO-PARK
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
   WND-open may interleave with paused markers). CALL SITES (rev 4,
   F18): the ctor/dtor (`Heap.cpp:5456-5482`, heap-OWNED per
   §13.1) are the ONLY pause/resume sites, covering every
   construction (`bytecode/JSThreadsSafepoint.cpp:334-338`;
   `runtime/VMManager.cpp:561` — every GIL-off jettison;
   `SharedHeapTestHarness.cpp:1039+`). No foreign call-site row
   exists (§13.3(b)). MECHANISM + COUNTER PROTOCOL: ANNEX CGP1
   (BINDING; relocated VERBATIM rev 5) GOVERNS — pause pair
   `m_parallelMarkersShouldPause` + `m_pausedParallelMarkers`
   under `m_markingMutex` (F6); participants = EXACTLY the
   `drainFromShared(HelperDrain)` helpers (F14; conductor §3.7
   and C4 assist rule 6 take NO checkpoint); a pausing helper
   donateAll()s, leaves its counter, paused++ (F16; one batch =
   the CG-I12 bound); EXIT DELTA (F17): every drainFromShared
   return does waiting-- (flag-off delta benign, CGD2.1); pauser
   predicate = active==0 && waiting==0; didReachTermination
   requires paused==0 (CG-I22); pause terminates (CG-I16).
   The §A.3 window may
   then jettison/patch: mutators are parked by ITS fan-out;
   markers by THIS hook; the C3 sweeper per rule 7. The window's
   ISB bump (ungil ISB1.1) plus rule 4 covers marker-visible
   code/object frees.
3. Order pin per cycle edge: WND-open (re-entry) is
   access-released -> GCL -> GSP (§3.1(a)-(c) + the first-window
   carve-out); WND-close clears GSP/WSAC BEFORE releasing GCL.
   Hence at most one of {GC window, §A.3 window} open at any
   instant — single-owner GCL is the proof. The HBT4 order EXTENDS to
   window re-entry (the only blocking GCL acquire; election/poll
   stay tryLock-only); the §10.2 GCL-busy rule (timed 1ms follower
   wait, `Heap.cpp:4542-4554`) is unchanged. A GC requester midway through someone else's cycle
   parks as a follower on GCA (set whole-cycle, §3.4), not on GCL.
4. No reclamation outside the cycle's final window: heap I11 +
   jit R4/CS4 refusal stand VERBATIM — a mid-cycle §A.3 stop only
   ENQUEUES an RCAC ticket (heap §13.10a; served by the same
   CONDUCT call's ticket drain, `Heap.cpp:4852-4863` — possibly
   an F28 successor cycle). Memory the paused
   markers can see is freed only at epoch reclaim (final window)
   or via jettison paths already routed through epoch/quarantine
   (om §6 bar; §9 contract hooks fire once-per-CYCLE, final
   window).
5. GC keep-parked vs §A.3 parking stays disjoint per ungil §A.3.8:
   GC stops set client-visible state (GSP/F8); §A.3 stops set none.
   AHA's three gates (GSP `Heap.cpp:5723`, §A.3 word `:5752`,
   mode machine `:5773`) compose unchanged — each re-loops, so a
   client wakes only when NO window of any kind pends.
6. C4 assist visitors vs §A.3 (F14):
   `performIncrementOfDraining` (`SlotVisitor.cpp:527-585`)
   maintains no marker counters; its `:578` safepoint takes NO
   shouldPause checkpoint. An assist
   mutator is
   bounded by ONE increment (`bytesRequested`), then parked by the
   §A.3 MUTATOR fan-out at its next stop poll. CG-T10 arm.
7. C3 sweeper vs §A.3 (F26; REWRITTEN F29/F30 — the rev 5 ack
   was phase-gated yet sweeping runs BETWEEN cycles
   (`Heap.cpp:2083`; `IncrementalSweeper.cpp:152`, `:41`), and an
   idle sweeper never reaches a quantum boundary to ack;
   CGD3.2/CGD4.2; ANNEX CGP1 GOVERNS): the sweeper is invisible
   to the §A.3 fan-out and the
   rule-2 pause (F14) — unruled, a §A.3 window's heap writes
   (heap §10A jit-R1.i exemption) race live sweep quanta.
   NORMATIVE: when `useSharedGCIncrementalSweep` AND a sweeper
   client is registered, the scope ctor sets the sweeper-pause
   flag under `m_markingMutex` with NO phase gate; sweeper
   quantum ENTRY is REFUSED while the flag is set
   (`m_sweeperInQuantum` set/cleared under the same mutex at
   entry/exit; access/MSPL acquired only after a gated entry);
   ctor wait predicate = `!m_sweeperInQuantum || acked` (idle sweeper
   passes); dtor clears flag + ack before the
   GCL release. Bound: one quantum when in-quantum, else zero.
   CGS1 row 11; CG-I13 extended; CG-T7 arms.

### 9.2 EXIT1 teardown mid-cycle

A spawned thread's exit (ungil §B.2: RHA -> TEARDOWN mark -> DCT ->
unregister) may land between windows of a live cycle. Rules (the
CMS/fence steps no-op when !C1R — F33, CG-I0):
1. DCT/`~GCClient::Heap` runs the landed teardown FIRST (access
   re-acquire for `lastChanceToFinalize`'s MSPL section, TLC
   `stopAllocatingForGood`), then PERMANENTLY drops access, THEN
   (strictly after its last possible barrier)
   flushes the client's CMS into the shared
   mutator stack (under `m_markingMutex`) and publishes its
   fence/didRun state as dead, then epoch=MAX and HCS remove
   (inside the same GBL/!WSAC section heap I13 requires). C4 DELTA
   (F25, ANNEX CGD3.3; rev 6, F34, CGD4.3 — the "between
   cycles ... directly" arm keyed on a phase read outside F22's
   GCL-ordered reader set; stale-NotRunning recreated CGD3.3(a)):
   the assist visitor is HEAP-ALLOCATED; ACT/DCT NEVER read phase
   and NEVER mutate the visitor set directly — teardown ALWAYS
   enqueues a DEFERRED UNREGISTRATION (ownership to the pending
   list) and CANCELS any pending §9.3(3) registration, under
   `m_parallelSlotVisitorLock`; the pending list is applied ONLY
   at WND-open BEFORE any `forEachSlotVisitor` walk, or quiesced
   (GCL held + phase NotRunning under `*m_threadLock`: §10D
   revert, server teardown) (CG-I14). Rev 1's flush-first order
   REJECTED (F2); early flush optional, the pre-remove flush
   normative; it completes while still registered — a concurrent
   WND-open sees the client (drains it) or doesn't (already
   drained); never a registered client with unreachable CMS
   (CG-I17). CG-T9 arm.
2. HCS `remove` blocking inside windows (heap I13) extends:
   removal between windows is LEGAL once rule 1 ran — the next
   fixpoint window re-converges with one fewer conservative root
   set (refs barriered/CMS-drained, RHA-published and
   root-reachable, or garbage).
3. `~VM` (EXIT1.9 fence) composes: the main client survives all
   spawned exits; §10D ISS reversion already requires
   `m_currentPhase == NotRunning` (heap §10D step 1), so a cycle
   never straddles a protocol switch. That covers the revert
   OUTCOME only — the landed PRE-CHECK (`pollIssRevertIfNeeded`,
   `Heap.cpp:5036-5043`) is restructured per the §3.4 `:5036` row
   (F11; CGD1.2).
4. The detaching thread is never the live conductor: §3.7 forbids
   EXIT1 on `m_gcConductorThread` mid-cycle (release assert; the
   closed loop runs no JS); CG-T9 attempts it.

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
   copies and stamps `m_fenceEpochSeen = FEP`. Happens-before: the
   master mutates only in-window (WSAC under GBL); the add runs
   under GBL with !WSAC — snapshot untorn, never stale. An
   attachee during live marking starts RAISED; CG-I3's WND-close
   assert holds unexempted. The §5.3(3) pin subsumes the values;
   the FEP stamp stays required.
2. `m_isMarking` visibility: the client's first AHA performs the
   seq_cst GSP load (`Heap.cpp:5723`) and the GBL section above
   pairs with the in-window flip (§6.2(1)); allocation before ACT
   completes is impossible (ungil §B.1).
3. C4 assist visitor: ACT ALWAYS ENQUEUES registration to the
   pending list (NO phase read — rev 6, F34), applied per §9.2(1)
   under `m_parallelSlotVisitorLock`. An EXIT1 in the same gap
   CANCELS it (F25). Until applied, the client cannot assist
   (`performIncrement` checks registration); its barriers/CMS are
   live immediately.
4. didRun = false, CMS = empty at ACT (zero-init; benign).
5. CG-T9 attach-storm arm (§12).

## 10. Lock & fence deltas

Lock table (heap §6 master) deltas — additions only, no re-ranks:
- `GCH::m_mutatorMarkStackLock` — TERMINAL leaf, INSIDE
  `m_markingMutex`; MAY be taken with ranks 7-9b held (the §5.2
  append path; CG-I10/F21).
- `m_markingMutex`, `m_parallelSlotVisitorLock`,
  `m_raceMarkStackLock`, visitor `m_rightToRun`: existing
  unranked marking-internal locks; CG-T2 lint groups them inside
  GCL/GBL, disjoint from MSPL-9b except existing in-window uses.
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
  drain happens under `m_markingMutex` (in-window by the
  conductor, or out-of-window by the owning thread).
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
  published storage tolerating a stale bound (marker/sweeper
  readers, ANNEX CGB1 rows), or (e) `!ISS`.
- CG-I9: `m_didRunSinceLastWindow` is written only by its owning
  access-holding thread; folded+cleared only in-window.
- CG-I10 (restated rev 5, F21): (1) `m_markingMutex` is never acquired
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
  non-final close); CG-T8 measures both.
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
- CG-I17: a detaching client's CMS is empty (flushed) by the time
  it leaves HCS, and the final flush postdates the client's last
  possible barrier (§9.2(1) order); its assist visitor is out of
  the visitor set — or owned by the §9.2(1) pending list — before
  its storage is destroyed (F25).
- CG-I18: no thread releases heap access, passes a stop poll, or
  enters a conducting path while holding a JSCellLock (§8.2);
  debug assert: cell-lock depth == 0 at SINFAC entry / AHA park.
- CG-I19: conductor tenure is a closed loop (§3.7): no JS, no
  RHA/AHA, no EXIT1, access-released throughout; between-window
  waits = donateAll+waitForTermination (condvar, no counter,
  never visitChildren, §A.3-compatible); blocking GCL acquires
  only access-released (§3.1).
- CG-I20: a client attached mid-cycle has its fence/threshold
  copies + `m_fenceEpochSeen` stamped from the master inside the
  publishing GBL section, before its first allocation (§9.3(1)).
- CG-I21: at most one conductor per cycle: every GCL tryLock
  site, INCLUDING stage-added ones (§3.4: `:4523`, `:4585`,
  `:5036`; CGA2 R7), backs off when
  `GCA && m_currentPhase != NotRunning`; `m_gcConductorThread` is
  stamped once per cycle (restamp legal only in NotRunning
  wind-down takeover) and cleared ONLY by its owner (§3.4 F20;
  both debug-asserted).
- CG-I22: a paused/exited helper is in neither counter (F17 exit
  delta); a paused helper holds no local work (donateAll);
  `didReachTermination` is false while
  `m_pausedParallelMarkers != 0` (§9.1(2)).

## 12. Verification ladder + TSAN charter (charter item 7)

Per-stage rungs; a flag may default-on only after its rungs AND
all earlier stages' rungs re-run green.

- CG-T1 (C0): ANNEX CGA1 audit executed; audit-pattern grep-lint
  clean; CG-I0 gate — flags-off corpus + `$vm.sharedHeapTest`
  scenarios (heap §12.1) byte-identical; BENCH.md flags-off
  delta = 0 (heap I10 bar). F33: routing is C1R-gated — flag-off
  keeps `Heap.cpp:1479` + the server fence reads.
- CG-T2 (C0): lock-lint (U20-class) + §10 rows encoding the three
  F21 clauses (barrier-append-under-7-9b lint-clean by rule, not
  suppression); CMS/markingMutex order litmus.
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
  fallback conducts when the collector thread is disabled; R7
  guard arm (F24): collector wake on an RCAC ticket between
  windows of a forced C1 cycle; assert one conductor.
- CG-T7 (C3): T8-extension audit (ANNEX CGB1) executed; sweep
  quantum vs window race arm; sweeper-thread attach/detach churn;
  F26/F29/F30 arms (C3 on): §A.3 jettison/haveABadTime (a)
  mid-quantum, phase PINNED NotRunning (between cycles, dominant;
  ack <= one quantum); (b) sweeper fully IDLE — no watchdog
  stall; (c) sweeper timer amplifier-delayed into the open §A.3
  window — quantum entry refused.
- CG-T8 (all): JSThreads-stop interleaving — haveABadTime/jettison
  mid-concurrent-cycle from a sibling, incl. at the
  WND-open GCL acquire (§3.1), vs a between-windows parked
  conductor (§3.7), and with helpers mid-batch (CG-I22); F17
  sub-arm: stop injected into a SECOND cycle; F18 sub-arm: >=1
  jettison arm drives the GIL-off `VMManager.cpp:561` conductor;
  F20 sub-arm: conduct return descheduled between
  GCL unlock and GCA clear + second-cycle between-windows
  requester storm; F22 sub-arm: stop-scope ctor just after a
  non-final WND-close; F28 sub-arm: §A.3 stop while a second
  granted ticket forces a back-to-back drain cycle — no deadlock
  at the successor's WND-open; extended CG-I12 wait measured; GC-requester storm (sync election + poll)
  vs a between-windows cycle — assert one `m_gcConductorThread`
  per cycle (CG-I21); measures GCL wait vs the 30s watchdog
  (`JSThreadsSafepoint.cpp:401`);
  `jsThreadsStopVsGCRequester` (heap §12.1) re-run per stage.
- CG-T9 (all): ATTACH/EXIT1 churn during forced concurrent cycles
  (CG-I17/I20); attach storm with the §6.2(5) liveness assert
  (§9.3(5)); exit with finalizer-side stores during full marking
  (§9.2(1)); conductor-thread exit attempt (release-assert,
  §9.2(4)); spawn+exit arming `m_issRevertPending` mid-cycle +
  main-client SINFAC poll storm — assert cycle completion (F11);
  attach-then-exit inside one between-windows gap (F25); ACT/DCT
  amplifier-descheduled across the NotRunning -> first-WND-open
  edge (F34);
  `clientChurnVsGC` + `issRevertChurn` re-run (cycle must quiesce
  before revert).
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
   `sharedGCMutatorMarkStackDonationThreshold` (Unsigned; default
   per §5.2(ii)).
3. Chartered out (INTEGRATE rows, not owned): (a) per-client
   barrier-address JIT emission (§5.3(3)) — jit/ungil owners;
   until landed, GIL-off C1+ pins the SERVER master always-fenced
   (F19); (b) DELETED (F18): no foreign marker-pause row —
   pause/resume live in the heap-owned scope ctor/dtor (§9.1(2));
   (c) VMManager manifest deltas: none — `VMManager.cpp:561`
   covered via the ctor; ZERO VMManager edits.
4. Supersession discipline: nothing frozen is superseded yet; the
   §2.2 kill-switch retires are flag-gated (the `!flag` arm keeps
   frozen behavior — heap Deviation 4's I10 clause + the flag-off
   analog). At freeze the affected heap-spec clauses gain
   two-sided supersessions; list = ANNEX CGS1, folded into
   SPEC-heap-history at freeze.

## 14. Ordered task list

- CG-1 (C0 infra): window split of `conductSharedCollection`
  (WND-open/close helpers with the §3.1 order incl. the
  first-window carve-out + the F28 successor arm; GCL per-window; GCA tenure +
  `m_gcConductorThread`; §3.7 closed loop; §3.4 phase-gated guards
  at ALL THREE tryLock sites incl. the `pollIssRevertIfNeeded`
  restructure); CG-T1/T2.
- CG-2 (C0 infra): CMS + per-client fence/didRun fields + window
  fold/republish loops; consumers re-pointed (C1R-gated, F33); GIL-phase JIT
  address pin (§5.3(3) fail-safe).
- CG-3 (C1): retire the three C1 kill switches behind the flag;
  marker-helper between-window scheduling + the §3.7/§7.1 ISS
  conductor wait; §9.1(2) pause pair/protocol incl. the F17 exit
  delta (F18: ctor/dtor only); §9.2 exit order + §9.3
  attach handshake; donation threshold option; CG-A2 audit ->
  ANNEX CGN1; CG-T3/T4/T5/T8/T9/T11.
- CG-4 (C2): collector-thread conductor; activity-callback RCAC
  routing; continuity bound; CG-T6 + re-runs.
- CG-5 (C3): ANNEX CGB1 audit; bit-storage release/acquire
  publication; sweeper-client re-home + the §9.1(7) entry-gated
  pause (F29/F30); CG-T7 + re-runs.
- CG-6 (C4): per-client assist visitors/balances; `performIncrement`
  re-enable; CG-T10 + re-runs; BENCH.md stage gates recorded.
- CG-7: freeze pass — ANNEX CGS1 supersessions recorded both
  sides; size-cap check; rev history updated.

Done = every CG-I has a test/assert (§12); flags-off bench and
behavior gates green; all four stage flags green on the ladder;
INTEGRATE-congc.md matches §13 exactly.
