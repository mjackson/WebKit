# SPEC-ungil.md - N-mutator execution model (GIL removal)

Status: FROZEN rev 9 (rev log/overflow/rationale: history =
SPEC-ungil-history.md). Closes every CHARTERED/GAP item A-J in
UNGIL-PLAN.md Part III/IV. Authorities: THREAD.md;
SPEC-{heap,vmstate,objectmodel,jit,api}.md (+annexes);
INTEGRATE-api D1-D13. Verified vs jarred/threads 2026-06-06.
Re-freezes/SUPERSESSIONS cite both sides. vmstate:/api:/om:/jit:/
heap: = the SPEC files; IU = INTEGRATE-ungil.md.

Master rule: GIL-off is a MODE (useThreadGIL=false). Every GIL-on
path stays compiled, the fallback/bisection oracle (§J); GIL-on
observable behavior unchanged EXCEPT both-mode deltas SD6
(§C.6/§A.2.6) and SD7 (§I). "GIL-off" = useJSThreads() &&
!useThreadGIL(); requires useVMLite=1, useSharedAtomStringTable=1,
shared GC server (U0).

## 0. Execution model

Post-GIL, ONE VM may have N concurrently *entered threads*. An
entered thread holds a VM entry token (§F): registered VMLite +
unique TID (vmstate §6.4.4), GCClient::Heap ACT (heap Dev 8),
VMThreadContext/VMTraps (§A.2), microtask + task queues (§E),
per-entry record (§A.1.5). Cross-thread soundness of PROPERTY/shape
storage is the landed OM/jit/heap machinery (UNGIL-PLAN Part IV);
builtin cell-INTERNAL state is NOT - §N rules it. Sections A-N (L/M
unused).

- U0 config gate: GIL-off with {useVMLite=0 |
 useSharedAtomStringTable=0 | non-shared GC server} refused at
 option validation (forced useThreadGIL=1).
- U0b multi-VM (heap I13 KEPT - Heap.cpp:4097-4124's one-sticky-
 shared-server RELEASE_ASSERT stands): GIL-off, exactly ONE VM per
 process - the sticky-shared-server VM - may hold per-thread
 clients. Other VMs: Thread spawn RangeErrors (api 5.1 shape);
 multi-embedder entry keeps the GIL-on single-migrating-client +
 real m_lock protocol (mode per-VM, not per-process; §F.1/§A.3.6
 branch on it). Corpus: second-VM spawn refused; second-VM
 two-embedder entry green beside a shared first VM. IU row.

## A. vmstate Phase B - per-thread execution-state consumption

Charter: vmstate:42-48 (Phase B UNOWNED; r12); api §2; jit R1
freeze scope (jit:233). Phase A's frozen layout consumed
unmodified: VMLite::offsetOfPrimitives()/offsetOfTID() + L1-L5 are
ABI; only accessor implementations change (L4).

### A.1 Pinned base + VM::field rerouting

1. TLS base: t_currentVMLite (vmstate L4). C++: VMLite::current().
 Asm/JIT use a new emitter loadVMLite per jit App. R5 (ELF IE TLS;
 Darwin pthread direct key in a JSCConfig slot; Windows unsupported
 flag-on, inherited). No reserved GPR.
2. Mid-body access rule, all tiers: correctness carrier =
 rematerialization - any site needing the lite re-loads via
 loadVMLite; prologue temps + the new VMEntryRecord::m_vmLite slot
 are OPTIMIZATIONS only, no temp survives the body.
3. Group-3 storage, per mode (DECIDED). THREAD.md:19's Group-3 set
 - topCallFrame, exception state, stack limits (§A.2), scratch
 (rule 6), m_microtaskQueue (§E), lazy regexp stack/match buffers:
 - GIL-on (flag-on OR off): VM storage, emission &vm +
 OBJECT_OFFSETOF - zero codegen delta flag-off (jit I1); the U19
 oracle keeps VM storage - J.5/J.6 authoritative.
 - GIL-off: VMLitePrimitives storage; VM keeps same-name accessors
 branching on mode (vmstate §0 surface). Baseline/DFG/FTL select
 emission AT CODEGEN TIME; LLInt branches at runtime on a SECOND
 derived gilOff byte beside jit R1.e's useJSThreads byte
 (JSCConfig.h:104). R1.e's byte + all landed ifJSThreadsBranch
 consumers UNCHANGED, both GIL modes; ONLY the NEW Group-3
 storage-selection branches test gilOff (else flag-on+GIL-on
 mis-routes). Extends jit R1.e (M4a vs this). Flag-off
 golden-disasm gate RE-BASELINED once, re-frozen.
 GC roots (NEW). Heap.cpp:3585 roots vm.exception()/lastException
 via VM accessors - GIL-off that un-roots other threads' pending
 Exceptions (UAF). Instead the root/handle visit iterates the
 VMLiteRegistry under its lock, appending the cell fields
 (exceptions, scope-verification cells, regexp match buffers) of
 ONLY lites with lite->vm == the collecting VM (registry is
 process-global, multi-VM) - same per-VM filter for the §A.1.6 +
 §K.1 scans and the §A.1.5/§A.2.3 fan-outs; registry stable (heap
 §10 quiesce). U-T1 root walk = this + §A.1.6 + §K.1; amplifiers:
 parked thrower survives a full GC; two-VM arm (no cross-VM
 roots/traps).
4. stackPointerAtVMEntry/lastStackTop become per-entry-token lite
 fields; JSLock.cpp:166's L7 RELEASE_ASSERT is GIL-on-only; the
 GIL-off token ctor asserts the *lite's* slot empty (re-entry uses
 the VMEntryRecord chain).
5. Per-entry record (NEW; entryScope race): the single
 m_vm.entryScope (VMEntryScope.cpp:90/:133), VM::isEntered() +
 service bits are raced GIL-off. NORMATIVE GIL-off: both move into
 the lite (L2 append); VMEntryScope ctor/dtor +
 executeEntryScopeServicesOnExit use the CURRENT lite's slot/bits;
 isEntered() = "§A.3.1 set non-empty"; VM-wide consumers iterate
 the registry under its lock. GIL-on/flag-off unchanged. U23.
 Service routing (mirrors §A.2.3): services classify VM-wide vs
 thread-local (table, U-T1); VM-wide + every CONCURRENT_SAFE
 request (requesters may hold NO lite) set a VM-level service word
 + fan into this VM's lites under the registry lock (token
 acquisition ORs the VM word in); thread-local: current lite only
 (§F.2 gigacage deferred arm = VM-wide).
6. Scratch buffers. scratchBufferForSize ADDRESSES are baked into
 DFG/FTL code - shared by N threads. NORMATIVE GIL-off:
 process-wide ScratchBufferRegistry (§LK rank, outside
 VMLiteRegistry::lock), monotonic index
 allocator + index->size map, never freed; each lite holds (L2
 append) an append-only segmented pointer table (lock-free reads).
 Every baked site becomes loadVMLite -> segment -> [index] (chained
 offsets), per tier, incl. OSR-exit + calleeSaveRegistersBuffer -
 straight-line; a buffer exists at (lite, index) BEFORE the code
 runs (install fans to VM lites; registration backfills);
 install nesting SBR -> VMLiteRegistry::lock -> scratchBufferLock
 LEGAL per the §LK re-rank (SUPERSESSION vs vmstate §6.5.1/§7, both
 sides). Non-baked: VM::scratchBufferForSize resolves through the
 CURRENT lite's table by size-class - IMPLEMENTS reserved
 VMLite::scratchBufferForSize (re-freeze: vmstate:534-539 vs this);
 Group 5 REPURPOSED as the lite's buffer-ownership list (under
 scratchBufferLock; backs jit-R2 scan + teardown free); L1-L5
 untouched. JITCode-RESIDENT members (catchOSREntryBuffer,
 FTLForOSREntryJITCode::m_entryBuffer) become registry indices
 resolved per entering lite (U-T4 amplifier: concurrent catch/loop
 OSR entry, one CodeBlock). GIL-on/flag-off keeps baked addresses;
 per-lite buffers GC-scanned via the registry walk (jit R2).
7. Cross-thread Group-3 READERS (NEW). SamplingProfiler.cpp:391-431
 suspends one m_jscExecutionThread, then reads m_vm.topCallFrame
 from the PROFILER thread; VMInspector/$vm kin. §A.1.3 rerouting
 makes such reads null/asserting GIL-off. NORMATIVE: every
 off-thread reader of a rerouted field (i) resolves the TARGET
 thread's lite via the registry (by suspended-thread identity,
 under the registry lock, target suspended), or (ii) is refused
 GIL-off with a defined error, or (iii) is proven on-thread. v1:
 SamplingProfiler samples ONLY the main/embedder carrier's lite
 via (i) (single-target model kept; spawned threads unsampled;
 --cpu-prof stays useful); N-thread sampling post-ungil. U-T8d
 enumerates off-thread readers per rerouted field, IU table. IM:
 SamplingProfiler.{h,cpp} + VMInspector.cpp.

### A.2 Per-thread VMThreadContext / VMTraps

vmstate §6.8 (per-thread per L2, chained offsets OK):

1. VMLite appends (L2, after Group 6) VMThreadContext threadContext
 + VMTraps traps; generated code reaches lite->traps.m_trapBits via
 the sanctioned chained offset.
2. Stack limits live in the lite's VMThreadContext, set at thread VM
 entry (handoff migration GIL-on-only; vmstate §2 rule 3 preserved
 GIL-on).
3. Trap fan-out. VM keeps a VM-level "process traps" word; raising a
 VM-wide trap (termination/watchdog/debugger, AND the GC stop
 reason - §A.3.8) = under registry lock, set the bit in every lite
 OF THIS VM (§A.1.3 filter) + the VM word (token acquisition ORs it
 in; replaces notifyGrabAllLocks()). Per-thread: one lite.
4. Termination targets all lites (as today); the D9 park-poll
 predicate jsThreadParkTerminationRequested re-points at the
 CURRENT lite's bits.
5. Async (signal) delivery OFF GIL-off (SignalSender targets the
 JSLock mutex owner - unset GIL-off; vmIsInactive reads TRUE
 mid-JS, VMTraps.cpp:305/:80): SignalSender never started; delivery
 = bit fan-out (rule 3) + poll sites + D9 quanta; vmIsInactive =
 "no registered lite entered".
6. Sync-park termination wake BYPASSED under useJSThreads, both GIL
 modes: §C.6's per-wait nodes (SD6) orphan the VMTraps.cpp:329/:419
 vm.syncWaiter() wakes. Replacement: TA nodes + §C.3 property sync
 parks wait in D9 10ms quanta polling termination (GIL-on: landed
 VM-wide rule-4 form; GIL-off: CURRENT lite's bit) - U2's bound;
 U19 terminate-parked arm (SD6). Flag-off: landed vanilla-SAB
 machinery stays COMPILED AND LIVE; atomicsWaitImpl branches on
 useJSThreads. "Both modes"/"deleted" = both GIL modes UNDER
 useJSThreads=1. §T flag-off golden gates (incl. terminate-during-
 infinite-TA-wait).

### A.3 Thread-granular stop-the-world (re-freezes jit R1.c)

Re-freezes jit:233 ("N threads in ONE VM=thread-granular STW"),
both sides. Stub replaced: JSThreadsSafepoint.cpp:244-250
RELEASE_ASSERT; real sequence R1.a-i.

1. Counting unit = entered thread. VMManager tracks per-VM entered
 threads (token holders, §F): forEachEnteredThread(VM&, f) /
 numberOfEnteredThreads, backed by the VMLiteRegistry (stop path
 uses VMManager::m_worldLock, heap rank 3).
2. Per-thread NVS park tickets. A stop request sets the stop bit in
 every target lite (§A.2.3); threads park at poll sites (cooperative
 only, jit R1.f-g) on their own ticket; the conductor proceeds when
 every entered thread of every target VM is parked OR not-entered
 OR access-released - the last sound ONLY with 2b:
 2b. Re-acquisition gate. A JSThreads stop sets NO client-visible
 GC stop state (Heap::JSThreadsStopScope only) - access state alone
 does not block re-acquisition. GIL-off: (i) acquireHeapAccess()/
 attachCurrentThread(), THE GIL-off acquire path, polls the lite's
 stop bit; set => park on its NVS ticket until resume; (ii) every
 park site polls post-wake BEFORE re-acquiring access or running
 JS/JIT. (i) carries soundness; (ii) defense. Tokens kept while
 parked; makes the access-released exemption sound (§A.3.4 gates
 FRESH acquisition). U4.
 SUPERSESSION (heap §10A "Single-client mode unchanged: never
 blocks" + the F8 AHA step list vs this, both sides; IH row):
 GIL-off AHA gains a stop-bit gate; the park follows F8's
 mandatory-revert shape - seq_cst exchange->NoAccess BEFORE the
 NVS park (GC barrier + §A.3 conductor both see NoAccess).
 Ordering (outside the F8 GSP proof): bit fan-out under
 VMLiteRegistry::lock precedes conductor counting; an AHA that
 CASed HasAccess pre-bit is still entered+unparked - the conductor
 waits on it (leg (ii)). GIL-on/flag-off AHA = frozen F8.
3. R1.c re-frozen (conductor release): arbitration releases exactly
 one requesting THREAD as conductor; the park-aware mutex on the
 pending-job slot is keyed by thread; a loser PARKS
 during the winner's stop, then retries; a SAME-VM second thread
 participates fully.
4. Entry during a stop parks: token acquisition (§F)
 checks the stop word, parks on a fresh ticket before completing
 entry. Licensed deletions: JSThreadsSafepoint.cpp stub assert +
 evidence walk + s_stubWorldStoppedDepth; M7 tripwire
 VMEntryScope.cpp:44-70.
5. R1.i GC bracket unchanged (W5): the access-release +
 Heap::JSThreadsStopScope bracket (JSThreadsSafepoint.cpp:252-304)
 is client-scoped, run on the conductor's own client.
6. Main/embedder carriers (vmstate §6.4.4). GIL-off EVERY thread
 uses a real carrier lite with a TM-allocated unique TID, lazily
 installed at first entry; m_mainVMLite (tid 0) is GIL-on-only.
 Carriers are per-(thread,VM) in a TLS VM->carrier map; lock()
 (still per-VM m_lock, §F.1) installs the entered VM's carrier as
 CURRENT lite AND swaps the jit P5/CS3 butterfly-TID-tag TLS to its
 TID, restoring the prior {lite, tag} LIFO on release. Install
 precedes any allocation/OM fast path; tag cleared at teardown;
 never tag 0 or a foreign-VM TID (TTL/§D.1). Spawned Threads
 single-VM in v1 (foreign-VM token RELEASE_ASSERTs). U1: TLS tag ==
 CURRENT lite TID && lite->vm == entered VM. JSLock.cpp:151
 backstop REPLACED (§J.7). Lazy embedder TIDs count against 2^15
 (Dev 10; §D lifts).
 TID SUPERSESSIONS (both sides; IV rows): vmstate §6.7 "Main
 carrier tid stays 0 (§6.4.4)" is GIL-ON-ONLY; api §7
 mainThreadTID=0 + TID note + api 4.1 "(main 0)" re-ruled:
 main/embedder TS.tid STAYS 0 (thr.id/Thread.current.id unchanged
 - no SD); the carrier lite TID is a SEPARATE nonzero TM
 allocation from the same 2^15 space (I17 counts carriers);
 currentTID() GIL-off returns the CARRIER TID; api 5.2's
 lite->tid==ts->tid is SPAWNED-only - main/embedder TS.tid and
 carrier TID diverge by design. OM §2 "TID 0 (main)=>bit-identical
 to today": perf remark only - GIL-off main butterflies carry the
 nonzero carrier tag (both-modes note, not an SD).
 ~VM teardown (SUPERSESSION: vmstate M6 + §6.5.1 assert vs this).
 Foreign carriers may still be REGISTERED at ~VM => M6 replaced:
 ~VM COLLECTS this VM's carriers under the registry lock (each
 token-free, else RELEASE_ASSERT), unregisters them, RELEASES the
 lock before any client work. Remote detach (SUPERSESSION: heap
 I4 "lifecycle on the using thread" + §10A.1, both sides): the walk
 only marks
 each foreign GCClient dead-detached (unregistered from the
 server's client set + machine-thread list); GCClient + lite
 DESTROYED deferred - owner's TLS destructor, or at once if the
 owner is dead. heap §10A.1's TLS slot becomes {client, epoch};
 every consult compares the epoch first - stale => null (no UAF; no
 stale scan: unregistered threads leave machineThreads). §6.5.1
 assert becomes "registry empty for this VM". Stale-TLS: VMs carry
 a process-monotonic epoch; the TLS map stores {VM*, epoch,
 carrier}; lock() compares epochs BEFORE the cached carrier. I20
 holds (dead carriers were token-free, never CURRENT). U27 +
 teardown storm.
7. Atom-table routing (X1). GIL-off, token acquisition points the
 thread at the shared sharded table (U0); the per-handoff swap is
 GIL-on-only. SUPERSESSION (vmstate §4.3 "None
 relaxed (ex-M5)" vs this): without the swap the 14
 vm.atomStringTable()==Thread-table asserts (Identifier.cpp:77;
 Completion.cpp:63-287; Heap.cpp:2348) fire on every spawned/
 carrier path; GIL-off they become "gilOff ?
 sharedAtomStringTableEnabled() : tables equal". GIL-on/flag-off
 unchanged. IU row (the 14 sites).
8. GC-stop parking, N threads one VM (NEW; closes heap Dev 8's
 "VMM trap delivery" Phase-B charter, heap:27; SUPERSESSION vs the
 heap §13.5 annex hooks' one-parked-thread-per-VM shape +
 notifyVMStop's per-VM state machine, VMManager.cpp, both sides).
 The GC stop reason is THREAD-granular like every other: trap bit
 fans out per rule 3; EACH entered thread
 parks on its OWN ticket (§A.3.2 NVS shape); notifyVMStop's per-VM
 duplicate-dispatch flag/active count/m_targetVM asserts become
 per-entered-thread (Mode transitions key on all entered threads
 parked/released/not-entered). heap §13.5a/g re-ruled: willPark/
 didResume operate on currentThreadClient() - one willPark per
 parking THREAD, with its own per-client m_releasedByGCPark;
 5b/5f/5g per thread. Unlike §A.3, the GC stop DOES set
 client-visible stop state (heap §10A/F8 gates re-acquisition). IM:
 VMManager.cpp + heap-§13.5 hooks (IH). Amplifier:
 spawned-conductor shared GC, two same-VM threads mid-JS (one parks
 via trap poll, one via SINFAC).

## B. Per-thread GCClient lifecycle in one VM

Charter: heap Dev 8 (ONE GCClient PER Thread); Dev 7 (full list:
B.6).

1. Create at spawn. threadMain (ThreadObject.cpp:162-176),
 GIL-off: after lite registration/setCurrent + TID-tag handshake,
 BEFORE any allocation, construct the thread's GCClient::Heap
 attached to the shared server (ACT), store clientHeap in the lite
 (L2 append), acquire access (§A.3.2b-gated). The JSLockHolder
 degrades to the §F token.
2. Teardown at exit. In the T5 sequence after the Strong clears +
 unregisterThread: release access, DCT/destroy the client,
 unregisterLite (U3). Lazy carriers own the VM's original client
 (main) or create one at first entry (embedder, §F.1), torn down at
 TLS death or the ~VM walk (§A.3.6).
3. JSLock heap-access forwarding GIL-on-only: didAcquireLock's
 acquire + willReleaseLock's release; GIL-off access is per-client,
 managed by the token (§F.1) and park sites.
4. TLC-aware inline allocation: fast paths address
 lite->clientHeap's TLC table, base = loadVMLite + frozen offsets;
 the §5.3 vm-relative chain stays GIL-on (heap dev 6).
5. Perf budget (heap Dev 7): {useJSThreads=1, sharedGC=1, GIL-off,
 1 thread} composite <=10% geomean vs {1,0} flag-on baseline
 (BENCH.md); {1,0} <=5% gate stays; a miss REQUIRES jit §4.3
 LLInt-cache revival pre-ship. 4-thread alloc microbench >=2.5x
 recorded, not gated.
6. heap Dev-7 GC-throughput items (per-directory handout,
 out-of-lock sweep, concurrent marking/incremental sweep) -
 SUPERSESSION (heap:26 + api:26 vs this): DEFERRED post-ungil;
 GIL-off ships on the synchronous conductor-driven heap §10
 protocol + single-MSPL slow path (correctness-complete). Gate =
 §B.5; a miss pulls them forward pre-ship. INTEGRATE-heap records
 it.

## C. api Dev 12 / OM 8g re-freeze

Charter: api:22; OM 8g; INTEGRATE-api D1/D2/D4/D8/D12. This IS
rev-15 content (IA sign-off).

1. OM §9.5 atomic slot accessors (8g): atomicSlotCompareExchange /
 atomicSlotReadModifyWrite -> JSValue, ONLY plain structure/
 butterfly-backed own NAMED data slots, plus the indexed pair.
 NORMATIVE:
 - Lock-free arms (inline, flat OOL, segmented-fragment slots -
 receivers NOT OM-locked): seq_cst 64-bit CAS/RMW loop on the
 EncodedJSValue slot word; NO cell lock on the segmented arm
 (lock-held RMW would not serialize, U5).
 - Flat-path transition discipline (flat GROW = butterfly-CAS +
 copy, NO nuke - an old-butterfly CAS is silently lost).
 currentButterflyTID() != butterfly tag => FIRST the OM §2
 foreign-write SW-set DCAS, re-validate structureID + butterfly per
 I34, THEN CAS the slot. Validation failure restarts the WHOLE
 probe (I33-bounded); a completed RMW/CAS is NEVER re-applied.
 - Third arm: OM-locked regimes. Dictionary (I19/L3) and AS-shape
 (§4.6; Thread.restrict FORCES AS): probe + CAS/RMW UNDER the
 JSCellLock OM already requires. AS
 PRE-LOCK stage: the cell lock suffices only AFTER SW=1 (jit §5.5
 keeps owner AS fast paths UNLOCKED while SW=0) - if
 butterflySharedWrite==0 and currentButterflyTID()!=tag TID, FIRST
 run the OM §4.6 first-foreign-write protocol (per-event STW,
 fire-then-publish (installerTID,1); I10b: fire PRECEDES any cell
 lock), then RESTART the probe; only SW=1 (or owner) enters the
 locked CAS/RMW. Lock REQUIRED (dictionary delete is I34-blind -
 lock-free CAS could "succeed" on an absent property, U5);
 dictionary-ness re-checked under it. 8g re-freeze.
 U5/U28 amplifier: owner unlocked AS store storm vs foreign CAS,
 same index, SW initially 0.
 - Indexed arm (8g re-freeze), by shape: CoW - materialize per OM
 §4.8/I35 first. Int32/Double - raw-word CAS REJECTED (history):
 first atomic access CONVERTS to Contiguous (owner direct; foreign
 SW-set DCAS first). Contiguous - flat arm verbatim. ArrayStorage/
 dict-indexed - third arm. §C.2 routes parseIndex hits here; one
 arm per shape.
 - Write barrier after success, as §9.5 orders.
2. ThreadAtomics re-homing (UNGIL-PLAN P1): the GIL-step
 atomicity block replaced - bodies call the §9.5 accessors.
 CARRIED: D3 exotic-receiver TypeErrors; D7 writability inside the
 atomic body.
3. PWT arming re-home + I10 re-derivation (F4 GIL-off). The landed
 I10 closure is the JSLock, not the listLock (SVZ read outside it);
 GIL-off the lost store+notify window REOPENS. NORMATIVE GIL-off,
 BOTH arms: (a) the PRE-ENQUEUE validation (api 5.6 step 1 "read
 v=o.k", api:229) itself routes through the §9.5 atomic load -
 NOT a plain read - forcing any CoW materialize / Int32/Double->
 Contiguous conversion (alloc + per-event-STW capable, §C.1
 indexed arm) to complete OUTSIDE listLock. Shape-monotonicity
 lemma: after a §9.5 touch the slot's regime only moves among
 {flat/Contiguous lock-free, AS/dictionary cell-locked} - never
 back to a converting arm - so the under-listLock re-load is
 alloc/STW-free (api 3 -> 10a stays legal). (b) enqueue under
 listLock, RE-VALIDATE SVZ(o[k], expected) via the §9.5 atomic
 load STILL UNDER listLock; mismatch => dequeue, "not-equal"; rope
 re-read OR (defense, lemma violation) convert-needed shape =>
 DEQUEUE TOO (a left-behind node eats one FIFO notify - the I10
 class), unlock, resolve/convert via §9.5, restart with a FRESH
 enqueue (NO alloc/STW under listLock, ever). Notifier orders
 through listLock (§9.5 store happens-before its listLock
 acquire): a missed store notifies AFTER our enqueue. waitAsync
 wakes settle via §E.4; sync parks keep their condition minus
 GILDroppedSection (§J.3). U5/U-T11. Corpus: wait/waitAsync on an
 Int32, Double, and CoW array index (FIRST-ever atomic access)
 racing a notifier. GIL-on unchanged. History rev 5/7/9.
4. 4.5-1a TA gate lifted GIL-OFF ONLY (GPO api:79; I21 :315): the
 SOLE spawned gate, AtomicsObject.cpp:613-621, becomes
 gilOff-conditional - KEPT GIL-on (SD4 stays per-variant; GIL-on
 behavior unchanged per the master rule - NOT a third both-modes
 delta); no twin. ThreadAtomics.cpp:536-541 is NOT 4.5-1a, NOT
 deleted: the G11 embedder gate on property Atomics.wait KEPT,
 re-pointed at mayBlockSynchronously() (§G.2). Post-lift blocking
 is §G-only (deadlock = user error).
5. D2 notify-yield: GIL-off notify() is NOT a yield point -
 jsThreadGILHandoffYield is GIL-on-only (§J.4); no foreign JS in
 notify(); parallel waiters (SD5).
6. D4/D8 lifted together (IA): atomicsWaitImpl's sync path
 allocates a per-wait node instead of the single vm.syncWaiter();
 the D8 single-flight gate deleted in BOTH GIL modes (nodes
 GIL-correct) - SD6. Nodes park per §A.2.6 (D9 quanta; flag-off
 keeps central wakes).
7. D1 ruling - §F.4. D12: grants settle via §E routing on the
 registering thread; uniform (closed).

## D. OM Tasks 13 (TID rebias) + 14

1. Task 13 (om:377, 8c) - IN SCOPE. Rebias runs world-stopped
 INSIDE the next FULL shared collection under the heap §10 GC stop
 barrier - NOT a §A.3 stop (jit R1.h); mutator re-entry blocked by
 the GC's client-visible stop state (§A.3.8).
 Restamps dead TIDs' butterfly tags + structure TIDs to 0; TM
 reissues via m_freeTIDs. Trigger: >=75% of 2^15 arms the next full
 collection; spawn during exhaustion still RangeErrors (api
 5.1/I17) until rebias completes. Lifts Dev 10. Enumeration =
 world-stopped HeapIterationScope walk (ConcurrentButterfly) +
 StructureID-table walk (bench, not gated). Restamp BEFORE
 m_freeTIDs release, one stop; restamp-to-0 soundness: history.
 Two-phase vs §LK (the GC conductor acquires NO api lock,
 TM::m_lock = api 1 included - so it never reads m_threads or
 pushes m_freeTIDs itself): PRE-STOP, a mutator-side pass under
 TM::m_lock snapshots the dead-TID set into a conductor-readable
 buffer (sound: the >=75% RangeError window already blocks spawn
 in THIS VM; lazy carrier creation - possibly for OTHER VMs whose
 threads are not stopped, TM being process-global - only ADDS live
 TIDs, never resurrects a snapshotted-dead one). The conductor
 restamps FROM THE SNAPSHOT world-stopped; m_freeTIDs release runs
 POST-RESUME under TM::m_lock, ordered BEFORE the RangeError gate
 lifts. Amplifier: rebias in VM A while an embedder lazily enters
 VM B, churning TM state.
2. Task 14 (structure splitting, om:378) STAYS DEFERRED pending
 the bench verdict - timing SUPERSESSION (both sides; INTEGRATE-om
 §46 holds NO verdict): the gate re-times to a HARD precondition of
 U-T10 ENTRY (docs-only round cannot bench); record in both
 INTEGRATEs. PROMOTE => Task 14 lands before U-T10, §C third arm
 re-reviewed pre-code. Else 8h ships as landed (OM 8h/L6/I37).

## E. Per-thread event loop + settlement (THREAD.md:98)

Ground truth replaced: one completion drain (api 4.6.1 GPO); all
settlement via vm.deferredWorkTimer. Landed inert: inboxLock,
inbox, inboxOpen, per-lite microtask slot (vmstate §6.6), I11.

SUPERSESSION (both sides): frozen api 4.6.1 "Never waits for tkts"
+ 4.6.2 SHELL-granular keepalive SUPERSEDED GIL-off by §E.2/E.3 -
completion waits for queues-empty + keepalive==0, thread-granular;
GIL-on keeps the old text (SD1 variants); INTEGRATE-api records
it.

### E.1 Queues

Every ThreadState owns, GIL-off:
- Microtask queue: the per-lite MicrotaskQueue (vmstate §6.6),
 enqueued/drained ONLY by its owner (I11); VM::queueMicrotask/
 drainMicrotasks re-route to the CURRENT lite's queue.
- inboxOpen (landed default false): false->true EXACTLY ONCE, on
 the owning spawned thread in threadMain, under inboxLock, after
 lite registration + GCClient attach (§B.1), BEFORE fn -
 happens-before any registration against this TS. Main/embedder
 TSs NEVER open theirs (settles take E.4's main path); increment
 sites assert spawned+OPEN (U25).
- Host hook (X1.7): queueMicrotaskToEventLoop (JSGlobalObject.h:
 1238) consulted ONLY for main/embedder-carrier enqueues; spawned
 enqueues ALWAYS take the per-lite queue, even with an embedder
 hook installed (Bun) - else I11/U22 (history).
- Task (macrotask) queue: new TS fields under the EXISTING
 inboxLock (api rank 3): Deque<ThreadTask> taskQueue, uint64_t
 keepaliveCount, Condition runLoopCondition; ThreadTask packages a
 settle task + its Ref<AsyncTicket>; the landed inbox vector IS the
 task queue.

### E.1b Ordinary shared-promise settlement (NEW)

E.4 routes only AsyncTickets; under the shared heap ANY thread can
resolve an ordinary JSPromise whose .then() registered elsewhere.
NORMATIVE v1:
1. Reaction jobs run on the SETTLING thread: the resolver enqueues
 to ITS OWN per-lite queue via the rerouted VM::queueMicrotask -
 I11; no foreign MicrotaskQueue touched; no per-reaction registrant
 hop in v1 (history). SD10.
2. Concurrent then()/resolve() protocol. GIL-off, JSPromise
 internal-state transitions run under the promise's JSCellLock
 (10a) - internal fields are NOT §9.5 slots. Landed bodies
 RESTRUCTURED per OM I20 (no GC alloc under 10a): allocate
 reactions (+ Bun InternalFieldTuple) OUTSIDE;
 re-check status under the lock (settled => drop allocation,
 queueMicrotask post-unlock; Pending => re-read reactionHead,
 publish via setPackedCell); resolve/reject swap status + extract
 the chain under the lock, enqueue post-unlock. U-T9 audit: every
 other promise internal-field writer/tier-inlined access takes the
 lock or is disabled GIL-off; non-promise types = §N. GIL-on
 unchanged.
3. U22: reactions on the settling thread; AsyncTicket settlements on
 the REGISTERING thread (ThreadTask hops, §E.4).

### E.2 Thread lifecycle (normative drain loop)

threadMain GIL-off, after fn returns/throws (replaces the single
drain):

```
loop:
 drainMicrotasks(own); releaseClientHeapAccess() // pre-inboxLock
 under inboxLock:
   termination trap pending => goto close // §E.5
   task = taskQueue.takeFirst() if any
   else if keepaliveCount == 0: goto close
   else wait runLoopCondition, 10ms quanta, D9 pred (§A.2.4)
 post-wake §A.3.2b poll; reacquireClientHeapAccess()
 run task if any (arbitrary JS, under §F token); loop
close:
 under inboxLock (access-released):
   inboxOpen = false // keepalive DEAD (E.3 r3)
   residue = std::exchange(taskQueue, {})
 drop inboxLock; §A.3.2b poll; reacquireClientHeapAccess()
 retire residue DWT work + route residue to main (E.4 dead rule);
 F1/F5 as landed; access release at the landed T5 point (§B.2, U3)
```

Lock/access rule. Heap-access transitions are NOT leaf
(releaseAccessSlow): NO transition holding any api rank 1-3 lock -
release BEFORE, re-acquire AFTER (ditto §J.3 park sites). RANK-4
EXEMPTION (mirrors api 5.9(e)'s sanctioned rank-4 shape, api:271):
NLS::m_lock/ParkingLot internals MAY be held
across token + access (re)acquisition, order (a) block/quanta-loop
on NLS::m_lock ONLY while token + access RELEASED, (b) on success
(re)acquire token + access (§A.3.2b/§A.3.8-gated) holding m_lock.
Sound: every m_lock waiter is access-released; no GC/§A.3 conductor
acquires NLS::m_lock - edge acyclic (§LK long-hold). Lint U20
checks the ORDER, not the shape.

Thread completes - and join/asyncJoin settle (F5) - ONLY at close
(U7), not fn-return (SD1). Park sites inside fn do NOT service the
task queue. A parked thread released access first - never delays a
conductor; wakeups: task append, stop, termination, quantum.

### E.3 Keepalive accounting

keepaliveCount counts outstanding registrations that may still
enqueue a task here; transitions under the registrant's inboxLock;
exactly-once via per-ticket m_keepaliveReleased, CONSTRUCTED true
(=released, SAFE default). The
INCREMENT site ALONE stores false (=armed) before the ticket is
visible; rules 1-2 decrement ONLY on winning the false->true CAS -
never-armed tickets (asyncJoin, TA waitAsync, main/embedder) lose,
never decrement (else wrap; history). U8 mutual-asyncJoin-OPEN arm.

INCREMENT (+1), once, at registration (I20 addPendingWork), on the
REGISTERING TS: every spawned-TS AsyncTicket EXCEPT asyncJoin -
asyncHold, cond.asyncWait, property Atomics.waitAsync (§C.3).
Main/embedder registrations never touch keepalive (§E.7).
asyncJoin: NO keepalive - its ticket settles only at the JOINEE's
close (F5/§E.2); counting it would deadlock mutual/self asyncJoin
(legal GIL-on; history). Registrant closed by settle => E.4 main
fallback (I12 dead=>main). SD12; mutual/self corpus arms. TA
Atomics.waitAsync: NO keepalive - not an AsyncTicket; WLM settles
via DWT scheduleWorkSoon MAIN-side, the thread may complete first.
SD11; re-home REJECTED v1 (history).

DECREMENT (-1), exactly once - every site first wins the
m_keepaliveReleased CAS; losers do nothing:
1. Settle-enqueue (E.4): decrement in the SAME inboxLock section as
 the append, iff inboxOpen (closed: CAS won, decrement SKIPPED,
 main fallback).
2. Cancel (VM-shutdown cancelPendingWork, api 5.5; D5 bailout):
 decrement iff CAS won AND inbox open, under inboxLock.
3. Inbox-close: NO claim step - inboxOpen=false => counter DEAD;
 close enumerates nothing (no per-TS registration set); a later
 settle/cancel wins its CAS, the open check skips => main fallback.
 Exactly-once (U8) from rules 1-2.

- U9 mechanics: decrement + append atomic under inboxLock; E.2's
 exit check reads both under the same lock; decrementer signals
 before unlocking. Intentional leak: never-notified waitAsync/
 asyncHold keeps keepalive>0 => join hangs (api 4.6.2); §E.5
 termination escapes.

### E.4 Cross-thread ticket settlement routing

Binding api post-GIL surface (api:200). AsyncTicket::settle
GIL-off: CAS m_settled (as landed); cancelled => bail; under
m_registrant->inboxLock: inboxOpen => append ThreadTask, rule-1
decrement (armed only), notifyOne; else FALLBACK to MAIN via the
LANDED scheduleWorkSoon path (keepalive untouched; §E.7.3-4
apply).

DWT retirement on the task-queue path (the spawned-registrant path
BYPASSES the landed scheduleWorkSoon retirement, ThreadManager.cpp:
88-95): ThreadTask body, on the owner under its token: (a) settle,
(b) cancelPendingWork (internal arm - fires §E.7.4's wake), (c)
clear m_promise. Thread keepalive supersedes DWT shell-liveness for
spawned registrants; dead=>main keeps landed retirement. U24.

Satisfies I12 post-GIL + I11. join() parks unchanged in shape;
GILDroppedSection out (§J.3); §G gates the block.

### E.5 Termination

A termination trap observed by the E.2 loop (or during fn) takes
the landed Failed path: close the inbox (E.3 r3), route residue to
main, F1/F5 with Phase::Failed. A terminated thread completes with
keepalive>0; its tickets settle later via main fallback (4.6.2).

### E.7 DeferredWorkTimer under N threads (NEW)

m_pendingTickets is an UncheckedKeyHashSet with NO lock,
JSLock-serialized today. NORMATIVE GIL-off:
1. m_pendingTickets (+ other JSLock-serialized DWT state) gains
 Lock m_pendingLock, rank LEAF (§LK; never across user JS):
 add/cancel/hasPendingWork, hasDependencyInPendingWork,
 scheduleWorkSoon removal, shutdown walk; cross-thread cancel (E.4)
 safe.
2. DWT's API-lock asserts keep the §F.2 token meaning - incl. the
 NEGATIVE assert at runRunLoop.
3. Embedder-hook ruling (USE_BUN_EVENT_LOOP; full mechanics:
 history r8 annex). TicketData hookManaged bit (set at
 addPendingWork iff hooks installed AND registrant main/embedder);
 hooks fire ONLY for hookManaged tickets, ONLY on the carrier;
 spawned registrations ALWAYS internal arm (m_pendingLock; I20
 liveness from append + E.3 keepalive). Off-carrier settle/cancel
 with hooks: m_pendingLock-guarded handoff queue, flushed AND
 EXECUTED inline at §F.1 drain points on the carrier under its
 token (incl. E.4(b) retire). Wake: FOURTH hook
 onCrossThreadWorkEnqueued (off-carrier-callable; no JS;
 boot-checked REQUIRED); fallback
 vm.runLoop().dispatch. U24 Bun arm: dead-registrant settle with
 hooks.
4. No-hooks runloop wake: off-carrier E.4(b) retire would strand a
 parked shell (RunLoop::stop fires only in DWT's timer callback);
 internal-arm cancel/retire while
 m_shouldStopRunLoopWhenAllTicketsFinish dispatches an ON-loop
 re-check via vm.runLoop().dispatch() (owns the stop decision);
 emptiness reads under m_pendingLock. U24 shell arm.
5. Remaining vm.runLoop()-bound paths (api 5.5a schedPump's pump
 task P, G28, + the api 5.6 waitAsync finite-timeout timer): with
 hooks installed BOTH route per rule 3 - m_pendingLock handoff
 queue + onCrossThreadWorkEnqueued + inline execution at §F.1
 drain points on the carrier (rule 3's own premise: a hook-owning
 embedder does not pump the VM RunLoop - api 5.5a-P's "GI" owner
 mark is void under hooks). No hooks => vm.runLoop() dispatch/
 dispatchAfter as landed. Corpus (U-T9/U-T11): two spawned threads
 contend asyncHold while main parks in a permitted sync wait;
 spawned-thread waitAsync finite timeout; both with hooks on.

## F. Post-GIL API-lock contract

1. JSLock GIL-off mode. JSLock::lock() branches on mode + caller:
 - Spawned Thread: NO m_lock acquisition. Installs a per-thread
 entry token {depth, spAtEntry} in the VMLite - records
 sp/lastStackTop (§A.1.4), ORs the VM trap + service words in,
 acquires client heap access (§B.1, §A.3.2b-gated), bumps depth;
 unlock() symmetric; depth 0 releases access. JSLockHolder =
 token.
 - Main/embedder: REAL lock semantics - m_lock still mutually
 excludes embedder threads among THEMSELVES (Bun exclusion kept).
 Acquiring it ALSO takes an entry token (§A.3.1 set uniform) + the
 §A.3.6 carrier+tag swap; GIL-on extras (atom swap, heap
 forwarding) skipped per §§A.3.7/B.3 - REPLACED, not dropped: a
 thread's FIRST entry into a VM creates the carrier lite AND its
 GCClient::Heap (main reuses the VM's original client; embedder
 creates one, §B.2), runs ACT (heap I4(b) addCurrentThread - stack
 conservatively scannable); EVERY lock() runs the §A.3.2b/§A.3.8-
 gated acquireHeapAccess on THAT client (idempotent at depth>0,
 heap F8 step 0); unlock() at depth 0 releases. Spawned-conductor
 GC scans a lock/eval/unlock embedder's stack (U27/U-T6 negative
 arm). Drain-on-release KEPT GIL-off: willReleaseLock drains the
 CURRENT lite's queue - THE drain point for lock/eval/unlock
 embedders (I11); other drain points: embedder runloop/DWT (§E.4),
 explicit drainMicrotasks, §E.7.3 flush. Park sites release m_lock
 per §J.3.
2. Two predicates, split.
 - VM::currentThreadIsHoldingAPILock() REDEFINED GIL-off as
 "current thread holds an entry token for this VM" - the host-call
 assert meaning (DWT §E.7.2).
 - JSLock::currentThreadIsHoldingLock() stays MUTEX-LITERAL -
 §F.4's DAL no-op + m_lockDropDepth LIFO depend on it. Spawned
 unlock() takes the token branch BEFORE the mutex RELEASE_ASSERT.
 - Consumer audit (U-T8): the ~60 consumers of either predicate
 get an IU table - assert (token meaning) vs BRANCH (behavioral) vs
 EXCLUSIVITY CONSUMER (needs a §K serializer). Fixed:
 sanitizeStackForVM - CURRENT lite's lastStackTop;
 primitiveGigacageDisabled - MUTEX predicate + §A.1.5 deferred arm;
 JSCell::validateIsNotSweeping - token + per-CLIENT mutator state;
 ISS-flip clause-(a) - flip pre-dates any GIL-off entry; DWT per
 §E.7.2.
3. Strong-handle discipline (api 5.10). ONE shared HandleSet per
 VM, new leaf HandleSet::m_strongLock inside Strong allocate/free/
 set-slot only (never across user code); per-thread sets would
 orphan foreign last-ref drops. Strong/HandleSet mutation needs an
 entered thread WITH heap access (hence E.2's close re-acquires
 first); GC scans the set under the heap §10 stop (NOT §A.3):
 mutators hold access => quiesced.
4. DropAllLocks GIL-off (IA D1, ruled HERE): DAL scopes ONLY the
 embedder mutex - main/embedder drops m_lock + token; spawned NO-OP
 returning 0 (mutex-literal predicate, JSLock.cpp:423-425), park
 sites use §J.3. m_lockDropDepth strict-LIFO embedder-only; token
 holders invisible. GIL-on unchanged.

## G. Per-thread blocking policy

Replaces the per-VM G11 gate (jsThreadsCanBlockOnCurrentThread).
1. Per-THREAD predicate mayBlockSynchronously(): spawned TS = true;
 main/embedder = embedder policy
 (isAtomicsWaitAllowedOnCurrentThread() as today).
2. Governs ALL sync parks: TA/property Atomics.wait (KEPT G11
 gate, §C.4), join, contended lock.hold, cond.wait; violations
 throw the existing TypeErrors (api I18 intact).
3. D4 GIL-dropped main TA wait machinery is GIL-on-only; GIL-off a
 permitted main sync wait parks per §J.3. D8 per §C.6.

## H. SymbolRegistry / Symbol.for

Closes vmstate Dev 8: WTF::SymbolRegistry's m_table gains Lock
m_lock (destructor-leaf, §LK.8)
- symbolForKey, remove, destructor walk; ~StringImpl's
registered-symbol arm calls remove() under it (legal from any
thread incl. GC/finalizer/in-lock sweep, §LK.8). Registries
destroyed in ~VM after spawned threads exit (U16).

## I. Wasm on spawned threads - REFUSED in v1

Closes UNGIL-PLAN I. Wasm EXECUTION from a spawned Thread throws
TypeError, covering WARM calls (rationale: history).
NORMATIVE (both GIL modes, SD7): (1) WebAssembly.{compile,
instantiate,validate} + Module/Instance/Memory/Table/Tag/Global
ctors throw on a spawned TS; (2) under useJSThreads,
jsCallICEntrypoint() returns nullptr AND every generated JSToWasm
entry (thunk + boxed-callee trampoline) emits a spawned-TS prologue
check. Discriminator: L2-append uint8_t VMLite::isSpawned (=1
BEFORE setCurrent, §B.1; carriers - incl. lazy - keep 0); check =
loadVMLite, null => fall through, else branch-to-throw. NOT "TID
tag != 0" (carriers hold nonzero TIDs; history). U17 NEGATIVE arm:
main/embedder NON-GC wasm never throws, both modes. C++ gates keep
isJSThreadCurrent(). GIL-on corpus edited (SD7). EXECUTION only;
§N.6 rules wasm-buffer grow/detach.
Wasm-GC (SUPERSESSION vs heap §5.5/manifest 11's JS-reachable
RELEASE_ASSERT, JSWebAssemblyInstance.cpp:142, both sides - the
§5.5 never-populate rule itself STANDS): under useJSThreads, both
GIL modes, a hasGCObjectTypes() module fails GRACEFULLY - a guarded
precheck BEFORE instance construction throws WebAssembly.LinkError
(compile-side detection: CompileError); the RELEASE_ASSERT stays
only on non-JS-reachable paths. U17 gains a positive arm:
main-thread wasm-GC instantiation => LinkError, no abort. IU row
(JSWebAssemblyInstance.cpp).

## J. GIL-machinery end state (GIL-on unchanged - oracle)

- J.1 useThreadGIL: KEPT, supported fallback; default flips false
 at the milestone gate.
- J.2/J.4/J.5 jsThreadGILHandoffYield + D2 notify-yield (§C.5),
 GILParkSavedExecutionState + resetForFreshThread: dead GIL-off
 (state per-lite, §A.1); J.5 compiled out.
 unlockAllForThreadParking NOT dead - re-derived in J.3.
- J.3 GILDroppedSection, GIL-off by caller:
 spawned (token-only) = access release + §A.3 park cooperation, E.2
 ordering + §A.3.2b post-wake poll. Main/embedder park sites (join,
 cond.wait, TA/property Atomics.wait) MUST ALSO release m_lock +
 token via the unlockAllForThreadParking shape (drain suppressed;
 GIL-on extras skipped per §F.1) - parking holding m_lock would
 deadlock Bun's second-embedder-thread notifier (history). Wake
 discipline (rules the kept api 5.4/5.6 loops, whose condition
 re-check runs UNDER a rank-3 lock): per-QUANTUM wakes poll ONLY
 lock-free state under the rank-3 lock - waiter-state atomic +
 CURRENT lite trap/stop bits, readable with no token/access (this
 poll carries U2's bound); the FULL m_lock + token + access
 reacquisition (§A.3.6 swap + §F.1 service OR + §A.3.2b post-wake
 poll) happens EXACTLY ONCE, at final exit, AFTER all rank-3 locks
 released (api 5.9(e); NLS::m_lock alone may stay held per §E.2's
 rank-4 exemption). C-API arm: main parks in property Atomics.wait;
 second embedder thread enters + notifies.
- J.6 JSLock handoff body: §F.1 drain + §A.3.6 swap KEPT; rest
 skipped per §§A/B/E/F (GIL-on load-bearing).
- J.7 JSLock.cpp:151 backstop (L1): REPLACED - GIL-off branch
 RELEASE_ASSERTs U1.
- J.8 Stub witnesses W2/W3/W4 + OM stub witness: DELETED at U-T5,
 both modes.

## K. GIL-serialized VM/global caches + lazy init (NEW)

The GIL is today's ONLY serializer for VM-/JSGlobalObject-resident
mutable state outside Group 3; §A.1.3 alone leaves it raced.
Rulings (GIL-on/flag-off unchanged):
1. Per-lite duplicates (L2 appends), hot per-op scratch/caches:
 VM::numericStrings (VM.h:657), stringSplitIndice (:665), dtoa
 scratch and kin. GIL-off accessors route to the CURRENT lite's
 copy; cell-holding copies GC-scanned via the registry walk.
2. Leaf locks: cold/keyed VM caches whose hits must be shared.
 RegExpCache is ALREADY locked (RegExpCache.h:79); unlocked peers
 found by the rule-4 audit get a leaf Lock (§LK).
3. Atomic lazy publication. LazyProperty/LazyClassStructure
 first-touch is a PLAIN uintptr_t state machine (LazyProperty.h:
 117): GIL-off it becomes load-acquire fast path; CAS
 lazyTag->initializing; winner runs the initializer holding NO lock
 (may allocate/GC), release-stores the value. Losers wait
 PARK-CAPABLE in bounded quanta: release heap access, poll BOTH
 stop families - §A.3 bit AND heap §10 GC stop state (DISJOINT;
 polling only one deadlocks, history) - re-acquire §A.3.2b-gated,
 re-test. Covers JSGlobalObject initLater + VM lazy ensure*
 siblings. U26 full-GC-during-init arm.
4. Inventory audit (U-T8b; gates U-T9): enumerate every
 VM/JSGlobalObject member mutated on JS paths whose only serializer
 is the GIL; rule each into class 1/2/3, IU table. Each §F.2
 EXCLUSIVITY CONSUMER must name its serializer there. U26.

## N. Builtin cell-internal mutable state (NEW)

OM §9.5/I21 cover PROPERTY slots; §K covers VM/JSGlobalObject
members. Multi-word C++/internal-field state INSIDE other shareable
cells - GIL-serialized today - was unruled (concurrent GIL-off
sharedMap.set() is corruption). DEFAULT GIL-off protocol
(GIL-on/flag-off unchanged): mutations AND structure-traversing
reads run under the cell's JSCellLock (10a), §E.1b shape - allocate
OUTSIDE, re-validate under, never allocate/park holding it (OM
I20). Rulings (full args: history):
1. JSMap/JSSet (JSOrderedHashTable) + JSWeakMap/Set (WeakMapImpl):
 ALL ops cell-locked, reads too (rehash/delete splice storage =>
 UAF). DFG/FTL map intrinsics DISABLED GIL-off -> locked native bodies;
 revival post-ungil.
2. Rope resolution/atomization (JSString.h:637-682): lock-FREE -
 resolver computes into a fresh buffer, publishes by ONE
 release-CAS of the fiber0/flags word; losers discard + re-read;
 readers load-acquire; resolveRopeToAtomString same vs the shared
 table (U0); JIT rope slow calls land here; §C.3's resolve = this.
3. DateInstance GregorianDateTime cache (DateInstance.h:62-75):
 BYPASSED GIL-off (compute per call); m_data lazy alloc
 CAS-published; vm.dateCache = §K.1/2.
4. FunctionRareData (JSFunction.h:136-144): materialize per §K.3;
 internals mutate under the function's cell lock; profiling-only
 fields racy-tolerated (jit item 7); cached Structures per I34.
5. Non-promise JSInternalFieldObjectImpl (generators, async fns/
 generators, iterator helpers): single-word resume-claim CAS on
 the STATE field (SuspendedX->Running); a racing resume loses the
 CAS and takes the EXISTING "already running" TypeError (serial
 semantics; no SD). While claimed, the at-most-one-resumer
 exclusion keeps frame/field stores PLAIN and tier-inlined
 (PutInternalField stays hot: every await/yield pays one CAS, NOT
 a JSCellLock, and DFG/FTL inlining survives - rev-8's blanket
 cell-lock rule was the largest uncalled-out GIL-off serial cost,
 history r9). Cell lock ONLY for multi-word cases the U-T8c audit
 names. §B.5/BENCH.md gains an async+generator microbench line;
 contingency = this CAS design (already the cheap shape).
6. ArrayBuffer detach/transfer (ArrayBuffer.h:199/:298): publish
 length=0 (seq_cst) BEFORE base clear - racing accesses fail
 bounds-safe; contents NOT freed inline: quarantined, released at
 the next heap §10 stop (OM §6 epoch shape) - no read-after-detach
 UAF. Wasm-backed buffers INCLUDED (§I refuses EXECUTION only; a
 main-created WebAssembly.Memory's buffer/views reach spawned
 threads as ordinary TA accesses): under useJSThreads,
 memory.grow's BoundsChecking reallocation publishes
 {newBase,newLength} length-first and QUARANTINES the old
 BufferMemoryHandle base to the next heap §10 stop (racing old-base
 reads stale-but-safe, writes lost - raciness SAB admits); detach
 of a wasm-backed wrapper likewise. U28 amplifier: spawned TA
 reader vs main memory.grow storm.
7. Audit U-T8c (gates U-T9, beside U-T8b): enumerate EVERY
 shareable JSCell subclass with non-property multi-word mutable
 state; rule each {cell-lock, CAS-publish, racy-tolerated, GIL-off
 TypeError}, IU table; tier-inlined accesses disabled or
 re-pointed. U28.

## LK. Merged process lock table (the ONE order; heap §6 stays
master for heap-internal ranks; api §5.9 ANCHORED here; vmstate §7
leaf rows amended; acyclicity proof: history r8)

Outermost -> innermost:
1. heap rank 1: JSLock::m_lock / entry token / heap access (token
 ordering-inert; "held entering NVS" per thread).
2. api 1: TM::m_lock. 3. api 2: PWT::m_lock / ThreadAffinityTable
 (never both).
4. api 3 group (mutually unnested, api 5.9(d)): NCS::queueLock,
 NLS::m_queueLock, listLock, TS::inboxLock (§E.1), TS::joinLock.
 DISJOINT namespace from heap rank 3 (VMManager::m_worldLock) -
 inboxLock renumbered out of the heap ranks.
5. heap ranks 2-10b as frozen. Cross edges: api 3 -> 10a legal
 (§C.3); api locks NEVER wrap heap ranks 2-9b.
6. VMLiteRegistry::lock - RE-RANKED outer-of-leaves (SUPERSESSION
 vs vmstate §6.5.1/§7 "no lock while held", both sides): allowed
 inner set {VMLite::scratchBufferLock, atomic bit ops, fastMalloc}
 ONLY. ScratchBufferRegistry OUTSIDE it (§A.1.6).
7. Leaves: HandleSet::m_strongLock (F.3); DWT::m_pendingLock (E.7);
 §K class-2 cache locks; VMLite::scratchBufferLock.
8. Destructor-leaf class (SUPERSESSION vs heap §6 leaf row "never
 7-9b" + vmstate §7, both sides): AtomString shards +
 SymbolRegistry::m_lock acquirable UNDER MSPL/BVL/9b - in-lock
 sweep destructors reach them (~JSString -> last StringImpl deref
 -> removeDeadAtom / registered-symbol remove); sound: holders
 fastMalloc-only, acquire nothing, never wait (vmstate I7 extended
 to SymbolRegistry).
- Long-hold: NLS::m_lock is NOT a leaf in the real graph (lock.hold
 runs user JS holding it; held across parks + token/access reacq,
 §E.2 exemption) - ordered OUTSIDE heap 2-10 + api 1-3; acyclic: no
 conductor or heap-2..9b holder acquires it.
- Negative edges (normative): no heap 2-9b holder acquires ANY api
 lock; no 10a/10b holder acquires api rank<=3; GC/§A.3 conductors
 acquire NO api lock; api 1-3 holders never transition heap access
 (§E.2).

§C.1 lock-free arms + §N.2 ropes take NO lock.

## INV. Invariants

- U0 config gate matrix (§0).
- U1 GIL-off JS thread: registered lite for the ENTERED VM, unique
 TID, live token, TLS tag == CURRENT lite TID (§A.3.6/J.7); tid 0
 never installed; multi-VM swap.
- U2 VM-wide trap observed by a parked T within one quantum, both
 GIL modes - carrier = §J.3's lock-free lite-bit poll, NOT token
 reacq; terminate-while-parked (§A.2).
- U3 lifecycle order (§B.1-2/E.2): lite -> ACT -> alloc; Strong
 clears -> access release -> DCT -> unregisterLite.
- U4 §A.3 stop: every entered thread parked/not-entered/
 access-released; entry during stop parks; no access-released
 thread runs JS mid-stop (2b); wake-during-stop amplifier.
- U5/U6 §9.5 atomicity + D3/D7 in-body; CAS-storms all arms;
 dict-delete-vs-CAS; restricted AS; convert-first; SW=0 AS pre-lock
 arm; §C.3 I10 arms.
- U7 completion <=> fn returned && queues empty && keepalive==0, OR
 termination (§E.2/E.5).
- U8/U9 keepalive: at-most-once decrement; no underflow
 (never-armed never decrements; mutual-asyncJoin-OPEN arm); no
 missed shutdown (§E.3).
- U10 settles per §E.4 (registrant iff inboxOpen, else main);
 never a foreign microtask queue (I11).
- U11 join/asyncJoin see Phase!=Running only post-close; join sees
 post-fn macrotask effects.
- U12/U13 nested spawned JSLockHolder depth-counted; APILock
 predicate true on host-call paths GIL-off (§F.2).
- U14 spawned DAL no-op; embedder DAL excludes only embedders;
 embedder C test incl. §F.1 drain.
- U15 §G policy; G11 TypeError preserved (api I18); spawned
 sync-wait OK; main parks release m_lock (§J.3 notifier arm).
- U16 concurrent Symbol.for(one key) => one symbol.
- U17 §I wasm throws from spawned threads, both modes, incl.
 warm-call; NEGATIVE: main/embedder NON-GC wasm never throws;
 POSITIVE: wasm-GC under useJSThreads => LinkError, no abort.
- U18 rebias: no live dead-TID tag post-stop; restamp before
 reissue; spawn-storm past 2^15 (§D.1).
- U19 GIL-on fallback corpus green after every U-task, unchanged
 EXCEPT SD6/SD7 (edited once).
- U29 §A.3.8: GC with >=2 threads entered in one VM - per-thread
 park/release; no per-VM double-transition/assert; per-thread
 willPark/didResume pairing.
- U20 lint: inboxLock/joinLock never nested; leaf locks never
 across user JS; no token/access transition holding any api rank
 1-3 lock; rank-4 across transitions only per §E.2 (a)->(b).
- U21 bench (§B.5).
- U22 reactions on the settling thread; AsyncTicket on the
 REGISTERING thread (dead=>main); queues owner-only (§E.1b/I11).
- U23 per-entry record correct under entry/exit churn; fan-out
 reaches every entered T of THIS VM (§A.1.5).
- U24 DWT: post-settle ticket out of m_pendingTickets, Strong
 cleared; shell exits; hooks hookManaged-only; handoff wake; Bun
 dead-registrant settle (§E).
- U25 inboxOpen once pre-fn, spawned only; increment sites assert
 spawned+open (§E.1).
- U26 §K: concurrent String(0.5)/split/lazy first-touch - one
 init, no race; full GC during winner's init (no deadlock).
- U27 ~VM walk: token-free carriers mark-dead + deferred-destroyed;
 epoch-stale TLS (both maps - carrier + heap §10A.1 client slot)
 never consulted live; teardown storm; spawned-conductor GC scans
 an entered embedder's stack (§A.3.6/§F.1).
- U28 §N: no UAF/torn builtin internal state; map.set + Date
 storms; rope race; generator double-resume;
 detach/grow-vs-read incl. wasm memory (no UAF).

## SD. Semantic deltas vs phase 1 (corpus impact)

- SD1 join settles at close (queues empty + keepalive 0), not
 fn-return (§E).
- SD2 completion drains OWN queues till empty (GPO).
- SD3 tickets settle on the REGISTERING T, dead=>main (§E.4).
- SD4 spawned TA sync wait allowed GIL-OFF ONLY (was TypeError;
 gate kept GIL-on); tests per-variant (§C.4/§G).
- SD5 notify() no yield point; parallel waiters (§C.5).
- SD6 main TA single-flight lifted (was second-wait throw, D8);
 per-wait nodes, D9 quanta, both GIL modes (§C.6/§A.2.6; flag-off
 untouched); GIL-on corpus edited (incl. terminate-parked arm).
- SD7 wasm on spawned threads: TypeError both modes (§I); GIL-on
 corpus edited.
- SD8 terminate parked: Failed completion, residue to main (§E.5).
- SD9 TID exhaustion RangeErrors till next rebias (§D.1).
- SD10 ordinary-promise reactions on the SETTLING thread (§E.1b).
- SD11 spawned TA waitAsync settles main-side, no keepalive (§E.3).
- SD12 asyncJoin: no keepalive; registrant may close first;
 dead=>main; mutual/self never deadlocks (§E.3).

U19 fallback corpus keeps OLD expectations via //@
runThreadsGILOff/GILOn variants for SD1-SD5/SD8-SD12; SD6/SD7
GIL-on expectations change. §N.5's TypeError is NOT an SD.

## IM. Integration manifest (shared hot files)

NORMATIVE ANNEX: the full hot-file -> section table + per-row
counterpart INTEGRATE owners lives in history rev 7, rev-7/8/9
deltas there; diffs land via IU. Rev-8 adds: VMManager + heap
§13.5 hooks = A.3.8 (IH); wasm BufferMemory* = §N.6 (IH); the 14
atom-assert sites = A.3.7 supersession (IV). Rev-9 adds:
SamplingProfiler/VMInspector = A.1.7; JSWebAssemblyInstance.cpp =
§I (IH); Heap.cpp:4097-4124 = U0b; AHA stop-gate = A.3.2b (IH);
TID notes = A.3.6 (IV/IA/IO).

## T. Ordered task list (~1-3k LOC)

- U-T1 §A.1.2-6/A.3.6: mode-split Group-3 + per-VM GC root walk +
 lazy carriers + per-lite scratch/regexp + per-entry record +
 service table. Dark.
- U-T2 §A.2: per-lite VMThreadContext/VMTraps, fan-out,
 SignalSender off, D9/C.6 re-points, stack limits.
- U-T3 §A.1.1-3: loadVMLite; LLInt gilOff byte; VMEntryRecord
 m_vmLite.
- U-T4 §A.1.3/6: Baseline/DFG/FTL emission switch incl. non-baked/
 JITCode-resident scratch.
- U-T5 §A.3: thread-granular STW incl. §A.3.8 per-thread GC
 parking (notifyVMStop + heap §13.5 re-rule); DELETE stubs/
 witnesses/M7 tripwire (J.8). Gate: GIL-on no-regression +
 N-separate-VMs + $vm stop/resume vs access-released embedders; U4
 + §A.3.8 amplifiers.
- U-T6 §B.1-3: per-thread GCClient spawn/teardown + lazy-carrier
 ACT (§F.1), token access, JSLock forwarding GIL-on-only.
- U-T7 §B.4-6: TLC lite-relative addressing, all tiers; U21.
- U-T8 §F/J: tokens, predicate split + audit, DAL ruling,
 HandleSet lock, J.7 replacement.
- U-T8b §K + §N: inventories, rulings, protocols (U26/U28); F.2
 third-class audit rows; ~VM walk + epochs (§A.3.6); U-T8d
 off-thread reader enumeration (§A.1.7). Gates U-T9.
- U-T9 §E: runloop + settlement incl. E.7 hooks, promise protocol,
 termination; corpus SD1-SD3/SD8/SD10-SD12 + hook + §N arms; U4
 one-VM arm.
- U-T10 §C.1-2 (ENTRY GATE: Task-14 verdict recorded, §D.2): §9.5
 accessors all arms incl. AS pre-lock, flat-path SW discipline,
 ThreadAtomics re-home with D3/D7.
- U-T11 §C.3-6/G/J.3: PWT re-home + I10 re-validation, 4.5-1a
 gilOff lift, G11 re-point, D2/D4/D8 (SD6 GIL-on edit), §G
 predicate, GILDroppedSection degradation + main-park m_lock
 release. Corpus SD4-SD6 + §J.3 embedder arm.
- U-T12 §D.1: TID rebias inside a full shared-GC stop (two-phase
 TM snapshot/release); spawn-storm; two-VM TM-churn amplifier.
- U-T13 §H/§I/A.3.7: SymbolRegistry lock; atom-swap GIL-on-only +
 14-assert supersession; wasm isSpawned checks + U17 arms (SD7
 edit) + wasm-GC LinkError precheck; §N.6 wasm-memory quarantine.
- U-T14 close: U0 gate; TSAN + amplifier; U19; default flip; IU
 dispositions.

Deps: T1->{T2,T3,T4}; {T2,T5}->T6; T5 gates T12; {T8,T8b}->T9; T9
gates T11 settle paths; Task-14 verdict gates T10; T14 last. T1-T7
dark. Each task re-runs the flag-off golden gates + U19.
