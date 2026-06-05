# SPEC-ungil.md - N-mutator execution model (GIL removal)

Status: FROZEN rev 6 (rev log/overflow: SPEC-ungil-history.md). Closes
every CHARTERED/GAP item A-J in UNGIL-PLAN.md Part III/IV. Authorities:
THREAD.md; SPEC-{heap,vmstate,objectmodel,jit,api}.md (+annexes);
INTEGRATE-api D1-D13. Verified against jarred/threads 2026-06-05.
Re-freezes/SUPERSESSIONS cite both sides. Shorthand: vmstate:/api:/om:/
jit:/heap: = the SPEC-*.md files; IU = INTEGRATE-ungil.md.

Master rule: GIL-off is a MODE (useThreadGIL=false, OptionsList.h:696).
Every GIL-on path stays compiled, the fallback/bisection oracle (§J);
GIL-on observable behavior unchanged EXCEPT both-mode deltas SD6
(§C.6/§A.2.6) and SD7 (§I). "GIL-off" = useJSThreads() &&
!useThreadGIL(); also requires useVMLite=1,
useSharedAtomStringTable=1, shared GC server (U0).

## 0. Execution model

Post-GIL, ONE VM may have N concurrently *entered threads*. An entered
thread holds a VM entry token (§F): registered VMLite + unique TID
(vmstate §6.4.4/§6.7), GCClient::Heap attached ACT (heap Dev 8),
VMThreadContext/VMTraps (§A.2), microtask + task queues (§E), per-entry
record (§A.1.5). Cross-thread JS-heap soundness is the landed OM/jit/heap
machinery (UNGIL-PLAN Part IV); this spec supplies the chartered gaps
§§A-J + the §K cache/lazy-init class.

- U0 (config gate). useJSThreads=1 && useThreadGIL=0 with
 any of {useVMLite=0, useSharedAtomStringTable=0, non-shared GC
 server} is refused at option validation (RELEASE log + forced
 useThreadGIL=1).

## A. vmstate Phase B - per-thread execution-state consumption

Charter: vmstate:42-48 (Phase B UNOWNED; r12); api §2
(api:26); jit R1 freeze scope (jit:233). Phase A's frozen
layout is consumed unmodified: VMLite::offsetOfPrimitives()/
offsetOfTID() + L1-L5 (:335-349) are the ABI; only accessor
implementations change (L4 sanctions this).

### A.1 Pinned base + VM::field rerouting

1. TLS base. t_currentVMLite (vmstate L4) is the single base. C++:
 VMLite::current(). Asm/JIT use a new macro/emitter loadVMLite per the
 frozen jit annex App. R5 model: ELF = initial-exec TLS; Darwin =
 pthread direct key in a JSCConfig slot (beside butterflyTIDTagTLSKey);
 Windows = unsupported flag-on, inherited. No reserved GPR.
2. Mid-body access rule, all tiers. Correctness carrier =
 rematerialization: any site needing the lite (slow paths,
 exception/trap checks, OSR exit, scratch) re-loads it via loadVMLite.
 Prologue temps + the new VMEntryRecord::m_vmLite slot (unwinding) are
 OPTIMIZATIONS - no codegen may rely on a temp surviving the body.
3. Group-3 storage, per mode (DECIDED). The THREAD.md:19 Group-3 set -
 topCallFrame, exception state (m_exception, m_lastException,
 scope-verification chain), stack limits (§A.2), scratch state (rule 6),
 m_microtaskQueue (§E), lazy regexp stack/match buffers:
 - GIL-on (flag-on OR flag-off): VM storage, emission &vm +
 OBJECT_OFFSETOF(VM, field) - zero codegen delta flag-off (golden
 gate jit I1; Phase A rules 1-2); flag-on+GIL-on (phase-1
 production; the U19 oracle) keeps VM storage too - J.5/J.6 stay
 authoritative.
 - GIL-off: VMLitePrimitives storage; VM keeps same-name accessors
 branching on mode (vmstate §0 surface). Baseline/DFG/FTL select
 emission AT CODEGEN TIME; LLInt branches at runtime on a SECOND
 derived gilOff byte ADDED BESIDE jit R1.e's useJSThreads byte in
 Config::options (JSCConfig.h:104), written at option finalization
 as useJSThreads() && !useThreadGIL(). R1.e's byte and ALL its
 landed consumers - the jit §5.4 cache disables + §5.5 TID/SW choke
 points via ifJSThreadsBranch (LowLevelInterpreter64.asm:1615-1625)
 - are UNCHANGED (they run whenever useJSThreads=1, both GIL modes);
 ONLY the NEW Group-3 storage-selection branches test gilOff
 (testing useJSThreads there would mis-route flag-on+GIL-on to
 per-lite storage). Extension of jit R1.e, both sides: jit:230/:251
 (M4a) vs this clause. Flag-off golden-disasm gate RE-BASELINED
 once, then re-frozen.
 GC roots (NEW, normative). Shared collections root Group-3 cells
 through VM accessors today (Heap.cpp:3585 appendUnbarriered(vm
 .exception())/lastException) - GIL-off that resolves only to the
 visiting thread's lite, un-rooting N-1 threads' pending Exceptions
 (UAF). Instead the root/handle visit phase iterates the
 VMLiteRegistry under its lock and appends EVERY registered lite's
 cell fields: m_exception, m_lastException, scope-verification
 cells, lazy regexp match buffers (registry-walk model of
 §A.1.6/§K.1; registry stable - mutators quiesced per heap §10).
 U-T1's "GC root walk" = this + §A.1.6 + §K.1; amplifier: A throws
 and parks pre-catch, B forces a full collection, A's Exception
 survives. §IM heap/Heap.* row.
4. stackPointerAtVMEntry / lastStackTop become per-entry-token fields
 in the lite; JSLock.cpp:166's L7 RELEASE_ASSERT is GIL-on-only; the
 GIL-off token ctor asserts the *lite's* slot empty (re-entry uses the
 VMEntryRecord chain).
5. Per-entry record (NEW; closes the entryScope race). VMEntryScope
 writes the single m_vm.entryScope at outermost entry (VMEntryScope
 .cpp:90, nulled :133); VM::isEntered() reads it (VM.h:298);
 entry-scope service bits (VM.h:373-454) likewise raced VM-globals.
 NORMATIVE GIL-off: both move into the lite (L2 append); VMEntryScope
 ctor/dtor + executeEntryScopeServicesOnExit use the CURRENT lite's
 slot/bits; isEntered() = "§A.3.1 set non-empty"; entryScope consumers
 read the CURRENT lite; VM-wide consumers iterate the registry under its
 lock. GIL-on/flag-off unchanged. U23.
 Service routing (fan-out; mirrors §A.2.3). Services classify
 VM-wide vs thread-local (table, U-T1). VM-wide + every CONCURRENT_SAFE
 request (requesters may hold NO lite: Gigacage VM.cpp:764,
 SamplingProfiler :811, watchdog :318, ConcurrentEntryScopeService
 VM.h:381): set a VM-level service word + fan into every registered lite
 under the registry lock; token acquisition ORs the VM word in.
 Thread-local: current lite only (§F.2's gigacage deferred arm =
 VM-wide).
6. Scratch buffers (per-lite, registry + segmented table).
 scratchBufferForSize ADDRESSES are baked into DFG/FTL code
 (DFGSpeculativeJIT.cpp:7023, :9751) - shared by N threads in one
 code block. NORMATIVE GIL-off: process-wide ScratchBufferRegistry
 (leaf lock, §LK) - monotonic index allocator + index->size map,
 never freed. Each lite holds (L2 append) a never-shrinking segmented
 pointer table (append-only; lock-free reads); every baked-address
 site becomes loadVMLite -> segment -> [index] (chained offsets,
 §A.2.1), per tier, incl. OSR-exit + calleeSaveRegistersBuffer -
 straight-line, no lazy branch. A buffer exists at (lite, index)
 BEFORE the code can run: install fans the index to every lite;
 registration backfills. NON-BAKED sharing too:
 VM::scratchBufferForSize GIL-off resolves through the CURRENT
 lite's table (index per size-class; OSR-entry staging,
 DFGOSREntry.cpp:248, becomes per-thread); JITCode-RESIDENT members
 (common.catchOSREntryBuffer, FTLLowerDFGToB3.cpp:300;
 FTLForOSREntryJITCode::m_entryBuffer :47) become registry INDICES
 resolved against the entering lite; amplifier: concurrent
 catch-/loop-OSR-entry of one CodeBlock (U-T4). GIL-on/flag-off
 keeps baked addresses (golden gate). Per-lite buffers GC-scanned
 via the registry walk (jit R2).
 Re-freezes vmstate §6.6 (vmstate:534-539). The reserved
 ScratchBuffer* VMLite::scratchBufferForSize(size_t) IS implemented
 - over the segmented table (the non-baked arm). Group 5
 (VMLite.h:168-174) is REPURPOSED, not dead: the lite's
 buffer-ownership list (each installed buffer appended under
 scratchBufferLock), backing the jit-R2 GC scan + teardown free.
 Frozen L1-L5 layout untouched.

### A.2 Per-thread VMThreadContext / VMTraps

vmstate §6.8 (vmstate:572; per-thread per L2, chained offsets OK):

1. VMLite appends (L2, after Group 6) VMThreadContext threadContext +
 VMTraps traps; generated code reaches lite->traps.m_trapBits via
 the sanctioned chained offset.
2. Stack limits live in the lite's VMThreadContext, set at thread VM
 entry (JSLock.cpp:157/§6.1.3 handoff migration GIL-on-only; vmstate §2
 rule 3 preserved GIL-on).
3. Trap fan-out. VM keeps a VM-level "process traps" word. Raising a
 VM-wide trap (termination/watchdog/debugger) = under registry lock,
 set the bit in EVERY registered lite's VMTraps + the VM word (token
 acquisition ORs the VM word in; replaces notifyGrabAllLocks(),
 JSLock.cpp:179). Per-thread: one lite.
4. Termination targets all lites (as today). The D9 park-poll
 predicate jsThreadParkTerminationRequested (LockObject.h:144-156) is
 re-pointed at the CURRENT lite's bits.
5. Async (signal) delivery OFF GIL-off. SignalSender targets
 vm.ownerThread() (VMTraps.cpp:305) = the JSLock mutex owner - unset
 GIL-off - and vmIsInactive (VMTraps.cpp:80) reads TRUE while N
 threads run JS. GIL-off: SignalSender never started; delivery = bit
 fan-out (rule 3) + poll sites on lite->traps.m_trapBits + D9 park
 quanta; vmIsInactive = "no registered lite is entered" (registry).
6. Sync-park termination wake BYPASSED under useJSThreads, both GIL
 modes. §C.6's per-wait nodes (SD6) leave the VMTraps.cpp:329/:419
 vm.syncWaiter() wakes pointing at a node nobody parks on, in EITHER
 GIL mode (a "GIL-on-only" wake would hang GIL-on
 terminate-during-sync-wait: the landed park, WaiterListManager.cpp:
 83-108, re-checks termination only on wakeups). Replacement, both
 GIL modes: per-wait TA nodes + §C.3 property sync parks wait in D9
 10ms quanta polling termination (GIL-on: landed VM-wide rule-4 form;
 GIL-off: CURRENT lite's bit) - U2's bound; U19 GIL-on
 terminate-parked-TA-waiter arm (SD6).
 Flag-off (useJSThreads=0) DISPOSITION: vm.syncWaiter() (VM.h:1174/
 :1376), the :329/:419 wakes, and the landed waitForSync park stay
 COMPILED AND LIVE - upstream machinery for vanilla SAB agents;
 atomicsWaitImpl branches on useJSThreads (per-wait nodes + D9
 quanta flag-on only - the D9 predicate is JSThreads machinery,
 never consulted flag-off). "Both modes"/"deleted" throughout this
 doc = both GIL modes UNDER useJSThreads=1 (line 13). §T's flag-off
 golden gates include terminate-during-infinite-TA-Atomics.wait.

### A.3 Thread-granular stop-the-world (re-freezes jit R1.c)

Both sides: jit:233 ("N threads in ONE VM=thread-granular STW") -
re-frozen HERE. Stub replaced:
JSThreadsSafepoint.cpp:244-250 RELEASE_ASSERT(enteredVMs <= 1); real
sequence R1.a-i :208-221.

1. Counting unit = entered thread. VMManager tracks, per VM, the set
 of entered threads (token holders, §F): forEachEnteredThread(VM&, f)
 / numberOfEnteredThreads, backed by the VMLiteRegistry (its lock =
 5.9-legal leaf; stop path uses VMManager::m_worldLock, rank 3).
2. Per-thread NVS park tickets. A stop request sets the stop bit in
 every target lite (§A.2.3); threads park at poll sites (cooperative
 only, jit R1.f-g) on their own ticket; the conductor proceeds when
 every entered thread of every target VM is parked OR not-entered OR
 access-released - the last sound ONLY with 2b:
 2b. Re-acquisition gate. A JSThreads stop sets NO
 client-visible GC stop state (Heap::JSThreadsStopScope only, Heap.h:
 483; JSThreadsSafepoint.cpp:299), so access state alone does not block
 re-acquisition. GIL-off: (i) GCClient::Heap::acquireHeapAccess()/
 attachCurrentThread() (Heap.h:1557-1565), THE GIL-off acquire path,
 polls the lite's JSThreads stop bit; if set, parks on its NVS ticket
 until resume. (ii) Every park site on an entered thread polls
 post-wake, BEFORE re-acquiring access or running JS/JIT. (i) carries
 soundness (embedder AHA/RHA brackets unenumerable); (ii) is defense.
 Tokens kept while parked; this gate, not the token, makes the
 exemption sound (§A.3.4 gates FRESH acquisition). U4.
3. R1.c re-frozen (conductor release). Arbitration releases exactly
 one requesting THREAD as conductor; the park-aware mutex on the
 pending-job slot (jit App. R1) is keyed by thread; a loser PARKS
 (= stopped) during the winner's stop, then retries; a SAME-VM second
 thread is a full participant.
4. Entry during a stop parks (was crash): token acquisition (§F)
 checks the stop word, parks on a fresh ticket before completing
 entry. Licensed deletions: JSThreadsSafepoint.cpp:250 stub
 assert; :126-154 evidence walk; :97 s_stubWorldStoppedDepth (->
 per-thread parked state + conductor stop scope); M7 tripwire
 VMEntryScope.cpp:44-70 (§A.1.5).
5. R1.i GC bracket unchanged (W5): the access-release +
 Heap::JSThreadsStopScope bracket (JSThreadsSafepoint.cpp:252-304) is
 client-scoped; runs on the conductor's own client.
6. Main/embedder carriers (vmstate §6.4.4 "Phase B decides", :490).
 GIL-off, EVERY thread - incl. main/embedder - uses a real carrier lite
 with a TM-allocated unique TID, lazily installed at first entry
 (vmstate :551; api:361). m_mainVMLite (tid 0) is GIL-on-only.
 Carriers are per-(thread,VM) - a lite binds one VM; multi-VM per
 thread is ordinary C-API usage. Each main/embedder thread keeps a TLS
 VM->carrier map; lock() (still per-VM m_lock, §F.1) installs the
 entered VM's carrier as CURRENT lite AND swaps the jit P5/CS3
 butterfly-TID-tag TLS to its TID, restoring the prior {lite, tag}
 pair on release, LIFO under nesting (JSLock.cpp:356-364). Install
 precedes any allocation/OM fast path; tag cleared at teardown
 (ThreadObject.cpp:166/:248); never tag 0 or a foreign-VM TID (TTL
 ownership/§D.1). Spawned Threads are single-VM in v1: a §F token
 for another VM RELEASE_ASSERTs. U1 asserts TLS tag == CURRENT lite
 TID && lite->vm == entered VM. JSLock.cpp:151 backstop REPLACED per
 §J.7. Lazy embedder TIDs count against 2^15 (Dev 10; §D lifts).
 ~VM teardown (SUPERSESSION, both sides: vmstate M6 + §6.5.1
 no-other-lite assert, vmstate:486-490, vs this). Foreign embedder
 carriers are legitimately still REGISTERED at ~VM, so M6 is
 replaced: ~VM walks the VMLiteRegistry for this VM under its lock;
 every foreign carrier must be token-free (else embedder contract
 violation - RELEASE_ASSERT) and is DCT'd/destroyed/unregistered;
 the §6.5.1 assert becomes "registry empty for this VM". Stale-TLS
 safety: VMs carry a process-monotonic 64-bit epoch; the TLS map
 stores {VM*, epoch, carrier}; lock() compares epochs BEFORE
 touching the cached carrier - a recycled VM* mismatches => fresh
 carrier. I20 holds (a destroyed carrier was token-free, never
 anyone's CURRENT lite). U27 + teardown storm.
7. Atom-table routing (X1). GIL-off, token acquisition points the
 thread at the shared sharded table permanently (U0); the per-handoff
 swap (JSLock.cpp:126) is GIL-on-only.

## B. Per-thread GCClient lifecycle in one VM

Charter: heap Dev 8 (heap:27 - ONE GCClient::Heap PER Thread); Dev 7
(:26 - TLC-aware inline emission, per-THREAD).

1. Create at spawn. threadMain (ThreadObject.cpp:162-176), GIL-off:
 after lite registration/setCurrent + TID-tag handshake, BEFORE any
 allocation, construct the thread's own GCClient::Heap attached to
 the shared server (ACT), store clientHeap in the lite (L2 append),
 acquire access (§A.3.2b-gated). The JSLockHolder at :170 degrades to
 the §F token.
2. Teardown at exit. In the T5 sequence (ThreadObject.cpp:233-248)
 after the Strong clears + unregisterThread: release access,
 DCT/destroy the client, unregisterLite (U3). Lazy carriers (§A.3.6)
 own the VM's original client (main) or create one (embedder), torn
 down at TLS death or the ~VM walk (§A.3.6).
3. JSLock heap-access forwarding GIL-on-only. didAcquireLock's
 acquire (JSLock.cpp:159-164) + willReleaseLock's release (:351-352)
 are GIL-on-only; GIL-off, access is per-client, managed by the token
 and park sites (ClientHeapAccessReleaseScope).
4. TLC-aware inline allocation. Inline alloc fast paths address
 lite->clientHeap's TLC table, base = loadVMLite + frozen offsets
 (chained loads, §A.2.1); the §5.3 vm-relative chain stays GIL-on
 (heap dev 6).
5. Perf budget (heap Dev 7). {useJSThreads=1, sharedGC=1, GIL-off, 1
 thread} composite <=10% geomean vs {1,0} flag-on baseline (BENCH.md);
 the {1,0} <=5% gate stays; a {1,0} miss still REQUIRES jit §4.3
 LLInt-cache revival pre-ship. 4-thread alloc microbench >=2.5x
 recorded, not gated.
6. heap Dev-7 GC-throughput items - SUPERSESSION (both sides:
 heap:26 + api:26 "incl." vs this). heap:26 also charters
 per-directory handout + out-of-lock sweep and concurrent marking/
 incremental sweep (the heap Dev-4 perf carve-outs, heap:23) as WS
 gating GIL removal. DEFERRED to a post-ungil perf milestone:
 GIL-off ships on the synchronous conductor-driven heap §10 protocol
 + single-MSPL slow path - correctness-complete at N clients (Dev 4
 disables are perf modes only). Replacement gate = §B.5 (<=10%
 1-thread; N-thread scaling RECORDED at sign-off); a §B.5 miss
 pulls the deferred items forward pre-ship. INTEGRATE-heap records
 the override.

## C. api Dev 12 / OM 8g re-freeze (rev-15 bundle)

Charter: api:22 ("5.6/4.5 post-GIL re-freeze ... UNOWNED"); OM
8g; api §2; INTEGRATE-api D1/D2/D4/D8/D12. This IS the rev-15 content
(INTEGRATE-api sign-off).

1. OM §9.5 atomic slot accessors (8g). Added to frozen §9.5:
 atomicSlotCompareExchange(JSObject*, PropertyOffset,
 expected, newValue) / atomicSlotReadModifyWrite(..., Op, operand),
 both -> JSValue, ONLY for plain structure/butterfly-backed own NAMED
 data slots (non-index PropertyNames), plus the indexed pair below (api
 4.5 admits indices; ThreadAtomics.cpp:77-82/:147-148).
 NORMATIVE:
 - Lock-free arms (inline, flat OOL, segmented-fragment slots -
 receivers NOT OM-locked): seq_cst 64-bit CAS/RMW loop on the
 EncodedJSValue slot word. NO cell lock on the segmented arm:
 segmented writes are lock-free fragment stores (THREAD.md regime 2;
 a lock-held RMW would not serialize, U5).
 - Flat-path transition discipline. A flat GROW is butterfly-CAS +
 copy, NO nuke (THREAD.md Arrays): a CAS landing in the old butterfly
 is silently lost. When currentButterflyTID() != butterfly tag, the
 accessor FIRST does the OM §2 foreign-write SW-set DCAS, re-validates
 structureID + butterfly per I34, THEN CASes the slot. Validation
 failure restarts the WHOLE probe (I33-bounded); a completed RMW/CAS
 is NEVER re-applied (retry point before the slot CAS).
 - Third arm: OM-locked regimes. Dictionary (I19/L3,
 om:178/:229) and AS-shape-with-any-SW (§4.6, :46;
 Thread.restrict FORCES AS, api Dev 11): probe + CAS/RMW UNDER the
 JSCellLock OM already requires there. Lock REQUIRED: dictionary
 delete keeps the StructureID, I34-blind (OM §6/I18) - a lock-free CAS
 could "succeed" on an absent property (api 4.5/U5). Dictionary-ness
 re-checked under the lock. 8g re-freeze (om-history:861).
 - Indexed arm (8g re-freeze). atomicElementCompareExchange/
 ReadModifyWrite(JSObject*, uint32_t) by shape: CoW -
 materialize per OM §4.8/I35 BEFORE any probe. Int32/Double -
 raw words; raw-word CAS REJECTED (history): first atomic access
 CONVERTS to Contiguous via the ordinary transition discipline
 (owner direct; foreign SW-set DCAS first). Contiguous - the
 lock-free flat arm verbatim (grows butterfly-CAS, same lost-CAS
 hazard). ArrayStorage/dictionary-indexed - third arm. §C.2
 routes parseIndex hits here; each shape lands in exactly one
 arm.
 - Write barrier after success, as §9.5 orders.
2. ThreadAtomics re-homing (replaces UNGIL-PLAN P1). The GIL-step
 atomicity block (ThreadAtomics.cpp:109-133) is replaced: bodies call
 the §9.5 accessors. CARRIED: D3 exotic-receiver TypeErrors (:115-130);
 D7 writability enforced inside the atomic body.
3. PWT arming re-home + I10 re-derivation (F4 GIL-off). The landed
 I10 closure is the JSLock, not the listLock: the SVZ read runs OUTSIDE
 it (ThreadAtomics.cpp:529-531; :550-552 credits the JSLock); GIL-off
 the lost store+notify window REOPENS (§C.1 stores take no lock; the D9
 loop never re-reads; waitAsync arming :646 identical). NORMATIVE
 GIL-off, BOTH arms: enqueue under listLock, then RE-VALIDATE
 SVZ(o[k], expected) via the §9.5 atomic load STILL UNDER listLock
 (rank 3 -> 10a nesting legal); mismatch => dequeue, return/settle
 "not-equal"; rope re-read => unlock, resolve, restart (no alloc under
 listLock). The notifier orders through listLock (its §9.5 seq_cst
 store happens-before its listLock acquire): a store the re-validation
 misses notifies AFTER our enqueue. waitAsync wake delivery settles via
 §E.4; sync parks keep their condition (:560-575) minus
 GILDroppedSection (§J.3). I10 GIL-off arms in U5/U-T11. GIL-on
 unchanged. History rev 5.
4. 4.5-1a TA gate lifted (GPO clause api:79; I21 :315). The
 SOLE 4.5-1a spawned gate is AtomicsObject.cpp:613-621
 (isJSThreadCurrent() => throwVMTypeError) - deleted; no twin.
 ThreadAtomics.cpp:536-541 is NOT 4.5-1a, NOT deleted: it is the G11
 embedder gate on property Atomics.wait, REQUIRED by api 4.5/I18
 (api:309), KEPT - re-pointed at mayBlockSynchronously()
 (§G.2). Post-lift blocking is §G-only (deadlock = user error).
5. D2 notify-yield re-derivation. GIL-off, notify() is NOT a yield
 point: jsThreadGILHandoffYield (LockObject.h:159-174) is GIL-on-only
 (§J.4). No foreign JS in notify(); parallel waiters (SD5).
6. D4/D8 lifted together (INTEGRATE-api). atomicsWaitImpl's sync
 path allocates a per-wait node instead of the single vm.syncWaiter();
 the D8 single-flight gate (AtomicsObject.cpp:500-511, flag-gated path) is
 deleted in BOTH GIL modes (nodes GIL-correct) - both-mode delta SD6.
 Nodes park per §A.2.6: D9 quanta both GIL modes; :329/:419 central
 wakes bypassed flag-on (flag-off keeps them, §A.2.6).
7. D1 ruling - §F.4. D12 grant-runner asymmetry - grants settle
 via §E routing on the registering thread; D12 uniform (closed).

## D. OM Task 13 (TID rebias); Task 14

1. Task 13 (om:377, 8c; api Dev 10) - IN SCOPE.
 Rebias runs world-stopped INSIDE the next FULL shared-server
 collection under the heap §10 GC stop barrier - NOT a §A.3 stop (jit
 R1.h, jit:231); mutator re-entry blocked by the GC's
 client-visible stop state (heap §10A/F8). Restamps dead TIDs'
 butterfly tags + structure TIDs to 0; TM reissues via m_freeTIDs
 (ThreadManager.h:308). Trigger: >=75% of 2^15 arms the next full
 collection; spawn during exhaustion still RangeErrors (api 5.1/I17)
 until rebias completes. Lifts Deviation 10 (ThreadObject.cpp:243).
 - Enumeration. World-stopped HeapIterationScope walk
 over ALL live cells (butterfly headers via ConcurrentButterfly) + a
 StructureID-table walk. Cost: one full-heap walk in an armed FULL
 stop (bench, not gated).
 - Restamp-to-0 soundness. OM decode (SPEC-objectmodel
 .md:27): payload==0 precedes any TID compare => {0, SW preserved}
 reads main-allocated. Annex:175-176 false-owner hazard: restamp
 ORDERED BEFORE m_freeTIDs release, one stop. Spines untouched; dead
 threads cleared TLS tags. History.
2. Task 14 (structure splitting, om:378) STAYS
 DEFERRED pending the bench verdict - timing SUPERSESSION (both
 sides). Frozen rule: promotion "DECIDED PRE-INT on jit Task-13's
 GIL-stub construction bench" (om:359; api:26;
 jit:278); INTEGRATE-objectmodel §46 (:1290-1301) holds NO
 verdict (bench never run); UNGIL-PLAN.md:250 binds this spec to
 record, not redesign. SUPERSEDED: the verdict gate re-times to a HARD
 precondition of U-T10 ENTRY (docs-only round cannot bench; U-T1..T9
 touch no 8h-dependent design): run the bench; record in both
 INTEGRATEs. On PROMOTE, Task 14 lands before U-T10, §C's third arm
 re-reviewed pre-code. Else 8h ships as landed (OM 8h/L6/I37).

## E. Per-thread event loop + settlement (MANDATED, THREAD.md:98)

Ground truth replaced: one completion drain (ThreadObject.cpp:205-208;
api 4.6.1 GPO); all settlement via vm.deferredWorkTimer (ThreadManager
.cpp:88). Landed inert: inboxLock /*rank 3*/; inbox; inboxOpen
(api:126); vmstate §6.6 reserves the per-lite microtask slot
(:529) + I11.

SUPERSESSION (both sides). Frozen api 4.6.1 (un-GPO'd): "Never waits
for tkts"; 4.6.2 fixes keepalive at SHELL granularity. §E.2/E.3 SUPERSEDE
both GIL-off: completion waits for queues-empty + keepalive==0,
thread-granular (old api:100/:102). GIL-on keeps the old text
(SD1 variants); INTEGRATE-api records the override.

### E.1 Queues

Every ThreadState owns, GIL-off:
- Microtask queue: the per-lite MicrotaskQueue (vmstate §6.6),
 enqueued/drained ONLY by its owner (I11); VM::queueMicrotask/
 drainMicrotasks re-route to the CURRENT lite's queue (:529).
- inboxOpen lifecycle (landed default false, ThreadManager.h:225). false->true EXACTLY ONCE, on the owning spawned thread in
 threadMain, under inboxLock, after lite registration + GCClient
 attach (§B.1), BEFORE fn - happens-before any registration against
 this TS. Main/embedder TSs NEVER open theirs: settles to them take
 E.4's main path. Increment sites assert a spawned, OPEN registrant
 (U25).
- Host hook ruling (X1.7). queueMicrotaskToEventLoop
 (JSGlobalObject.h:1238) is consulted ONLY when the enqueuer is the
 main/embedder carrier; spawned-thread enqueues ALWAYS take the per-lite
 queue, even with an embedder hook installed (Bun) - else I11/U22
 violated (history).
- Task (macrotask) queue: new ThreadState fields under the
 EXISTING inboxLock (rank 3): Deque<ThreadTask> taskQueue; uint64_t
 keepaliveCount; Condition runLoopCondition; - ThreadTask =
 Function<void(VM&)> packaging a settle task + its Ref<AsyncTicket>.
 The landed inbox vector IS the task queue.

### E.1b Ordinary shared-promise settlement (NEW)

E.4 routes only AsyncTickets (keyed on m_registrant, ThreadManager.cpp:
41-47/:73); under the shared heap ANY thread can resolve an ordinary
JSPromise whose .then() registered elsewhere. NORMATIVE v1:
1. Reaction jobs run on the SETTLING thread: the resolver enqueues to
 ITS OWN per-lite queue via the rerouted VM::queueMicrotask - I11
 satisfied; no foreign MicrotaskQueue touched. No per-reaction
 registrant hop in v1 (rejected; history). SD10.
2. Concurrent then()/resolve() protocol. GIL-off, JSPromise
 internal-state transitions run under the promise's JSCellLock (10a) -
 internal fields are NOT §9.5 slots. Landed bodies (JSPromise.cpp:
 341-440) RESTRUCTURED: OM I20 forbids GC allocation under 10a.
 performPromiseThen: pre-switch status() read ADVISORY; allocate the
 reaction (+ Bun InternalFieldTuple context, :346-359) OUTSIDE the
 lock; under it RE-CHECK status - settled => drop allocation,
 queueMicrotask after unlock; Pending => re-read reactionHead, fix
 next, publish via setPackedCell (setInlineHandlerReaction: same lock
 + re-check). resolve/reject: status swap + chain extraction under the
 lock; reactions enqueued after unlock. U-T9 audit: every other
 internal-field writer and tier-inlined access takes the lock or is
 disabled GIL-off. One uncontended cell-lock per op; GIL-on unchanged.
3. U22 restated: reactions on the settling thread; AsyncTicket
 settlements on the REGISTERING thread (ThreadTask hops, §E.4).

### E.2 Thread lifecycle (normative drain loop)

threadMain GIL-off, after fn returns/throws (replaces ThreadObject
.cpp:205-208's single drain):

```
loop:
 drainMicrotasks(ownQueue)
 releaseClientHeapAccess()  // BEFORE inboxLock (rule below)
 under inboxLock:
   if termination trap pending: goto close   // §E.5
   if !taskQueue.isEmpty(): task = taskQueue.takeFirst()
   else if keepaliveCount == 0: goto close
   else: wait on runLoopCondition, 10ms quanta, D9 pred (§A.2.4)
 post-wake §A.3.2b stop poll; reacquireClientHeapAccess()
 if task: run task (arbitrary JS, under §F token)
 loop
close:
 under inboxLock (access-released):
   inboxOpen = false           // keepalive DEAD (E.3 r3)
   residue = std::exchange(taskQueue, {})
 drop inboxLock; §A.3.2b poll; reacquireClientHeapAccess()
 // WITH access (Strong mutation, §F.3):
 retire residue DWT work + route residue to main TS (E.4 dead
 rule); then F1/F5 EXACTLY as landed (ThreadObject.cpp:212-231);
 access release at the landed T5 point (§B.2, U3)
```

Lock/access ordering rule. Heap-access transitions are
NOT leaf (releaseAccessSlow, Heap.cpp:2580-2595) - holding access while
blocked on inboxLock cycles against GC. NO heap-access transition holding
inboxLock/joinLock/listLock/any api-rank lock - release BEFORE,
re-acquire AFTER (the loop above; ditto every §J.3-degraded park site).
Lint U20.

Thread completes - and join/asyncJoin settle (F5) - ONLY at close (U7);
join settles then, not at fn-return (SD1). Park sites inside fn do NOT
service the task queue; tasks queue.

### E.3 Keepalive accounting

keepaliveCount counts outstanding registrations that may still enqueue
a task here. Transitions under the registrant's inboxLock; exactly-once
via per-ticket m_keepaliveReleased flag - CONSTRUCTED true (=already
released, the SAFE default; deliberately UNLIKE m_settled,
ThreadManager.cpp:78-81, which constructs false). The INCREMENT site
ALONE stores false (=armed) before the ticket is visible to any
settle/cancel; rules 1-2 decrement ONLY on winning the false->true
CAS. A never-armed ticket - asyncJoin, TA waitAsync, main/embedder
registrant, any future non-counted registration - loses the CAS and
never decrements. (A decrement against a counter the registrant never
incremented would wrap uint64 and hang the §E.2 exit predicate -
U8's no-underflow holds by construction; mutual-asyncJoin-with-OPEN-
inboxes corpus variant added to U8. History rev 6.)

INCREMENT (+1), once, at registration (the I20 addPendingWork point,
ThreadManager.cpp:67-72), on the REGISTERING TS: every
AsyncTicket whose registrant is a spawned TS EXCEPT asyncJoin -
asyncHold, cond.asyncWait, property Atomics.waitAsync (AsyncTicket:
ThreadAtomics.cpp:639; §C.3). Main/embedder registrations do NOT touch
keepalive (DWT shell-liveness, §E.7).
asyncJoin takes NO keepalive. Its ticket settles only at
the JOINEE's close (F5/§E.2); counting it would deadlock mutual asyncJoin
(A<->B: each close waits on the other's) and self-asyncJoin (no self gate
- only join() has one, ThreadObject.cpp:340-342) - both legal GIL-on
(4.6.1 never waits). Registrant closed by settle time => E.4 main
fallback (SD11 pattern; I12 dead=>main, api:306). SD12;
mutual/self corpus variants. History rev 5.
TA Atomics.waitAsync takes NO keepalive. Not an
AsyncTicket: WaiterListManager settles via the VM's
deferredWorkTimer->scheduleWorkSoon (WaiterListManager.cpp:291; timers
on the VM runloop, :176) - no registrant, no decrement site. Spawned TA
waitAsync settles MAIN-side; the thread may complete first. SD11;
corpus variant. (Re-home REJECTED v1; history.)

DECREMENT (-1), exactly once - every site first wins the
m_keepaliveReleased CAS; a loser does nothing:
1. Settle-enqueue (E.4): flag CAS, then decrement in the SAME
 inboxLock section as the append, iff inboxOpen (closed: CAS wins,
 decrement SKIPPED, main fallback).
2. Cancel (VM-shutdown cancelPendingWork, api 5.5; D5 bailout):
 decrement iff CAS won AND inbox open, under inboxLock (unconditional
 would double-count at termination).
3. Inbox-close: NO claim step. Once inboxOpen=false the counter is
 DEAD (E.2's exit predicate is read only while open); close enumerates
 nothing (no per-TS registration set, ThreadManager.h:200-235), touches
 no flags. A later settle/cancel wins its CAS; rules 1-2's open check
 skips the decrement => main fallback. Exactly-once (U8) from rules 1-2
 alone.

- U9 mechanics: increment happens-before any settle of the same
 registration; decrement + append atomic under inboxLock; E.2's exit
 check reads keepalive + emptiness under the same lock - no exit between
 a decrement and its append; decrementer signals before unlocking.
- Leak note (intentional): never-notified property-waitAsync/
 asyncHold keeps keepalive>0 => join hangs (api 4.6.2); termination
 (§E.5) is the escape.

### E.4 Cross-thread ticket settlement routing

Binding api post-GIL surface (api:200). AsyncTicket::settle
GIL-off: CAS m_settled (as landed); cancelled => bail (rule-2
decrement attempt); under m_registrant->inboxLock: inboxOpen => append
ThreadTask, rule-1 decrement (armed tickets only, §E.3), notifyOne;
else FALLBACK: schedule on MAIN
via the LANDED scheduleWorkSoon path (ThreadManager.cpp:88; keepalive
untouched; §E.7.3-4 apply).

DWT ticket retirement on the task-queue path. The landed
settle wraps the task in scheduleWorkSoon, whose run retires the ticket
and clears m_promise (ThreadManager.cpp:88-95); the spawned-registrant
path BYPASSES it. So the ThreadTask body, on the owner under its token:
(a) settle task, (b) cancelPendingWork(ticket) (internal arm, §E.7 -
fires §E.7.4's runloop wake), (c) clear m_promise. Thread keepalive
supersedes DWT shell-liveness for spawned registrants; dead=>main keeps
the landed retirement. U24: post-settle the ticket is out of
m_pendingTickets, Strong cleared.

Satisfies I12 post-GIL (api:306) + I11. join() parks
(ThreadObject.cpp:337-358) unchanged in shape; GILDroppedSection out
(§J.3); §G gates the block.

### E.5 Termination

A termination trap observed by the E.2 loop (or during fn) takes the
landed Failed path: close the inbox (E.3 rule 3), route residue to main,
F1/F5 with Phase::Failed. A terminated thread completes with
keepalive>0; its tickets settle later via the main fallback (4.6.2).

### E.6 Park/unpark

A parked thread released access first - never delays a conductor.
Wakeups: task append, stop, termination, 10ms quantum.

### E.7 DeferredWorkTimer under N threads (NEW)

m_pendingTickets is an UncheckedKeyHashSet with NO lock
(DeferredWorkTimer.h:121; only m_tasks is guarded) - JSLock-serialized
today; add/hasPendingWork assert the API lock (DeferredWorkTimer.cpp:
191/:219). NORMATIVE GIL-off:
1. m_pendingTickets (+ other JSLock-serialized DWT state) gains
 internal Lock m_pendingLock, rank LEAF (§LK; never held across user
 JS): add/cancel/hasPendingWork, hasDependencyInPendingWork,
 scheduleWorkSoon removal, shutdown walk. Cross-thread cancel (E.4)
 safe.
2. DWT's API-lock asserts keep the §F.2 token meaning - incl. the
 NEGATIVE assert at runRunLoop (:182).
3. Embedder-hook ruling (USE_BUN_EVENT_LOOP). Installed hooks
 onAddPendingWork/onScheduleWorkSoon/onCancelPendingWork
 (DeferredWorkTimer.h:110-112) BYPASS m_pendingTickets, run INLINE on
 the caller (DeferredWorkTimer.cpp:204-205, :234, :266). NORMATIVE:
 hooks fire ONLY on the main/embedder carrier:
 - Registration. TicketData gains a hookManaged bit, set at
 addPendingWork iff hooks installed AND registrant is main/embedder.
 Spawned-TS registrations ALWAYS take the internal arm
 (m_pendingTickets under m_pendingLock) - onAddPendingWork NOT
 fired; I20 liveness from the internal append (+ E.3 keepalive).
 - Dispatch rule, EVERY site. The landed bodies dispatch to an
 installed hook UNCONDITIONALLY (DeferredWorkTimer.cpp:234
 onScheduleWorkSoon; :266-269 onCancelPendingWork). NORMATIVE:
 scheduleWorkSoon/cancelPendingWork consult hookManaged BEFORE the
 installed-hook branch - an internal (spawned-registered) ticket
 takes the internal arm regardless of CALLING thread, incl.
 on-carrier calls (E.4 dead=>main fallback, main-side E.4(b)
 retirement): hooks must never receive a ticket they never saw via
 onAddPendingWork.
 - Settle/cancel. Off-carrier scheduleWorkSoon/cancelPendingWork
 with hooks appends {ticket, task-or-cancel} to a
 m_pendingLock-guarded handoff queue, flushed at §F.1 drain points;
 entries re-dispatch per the rule above. onCancelPendingWork fires
 only for hookManaged tickets, on-carrier; E.4(b) retirement of
 spawned-registered tickets is internal-only.
 - Internal-arm EXECUTION with hooks installed. m_tasks is drained
 by DWT's run-loop timer, which a hook-installing embedder does NOT
 pump - internal entries would strand. NORMATIVE: hooks installed
 => internal-arm scheduleWorkSoon entries are NOT timer-scheduled;
 the handoff flush and every §F.1 drain point EXECUTE them inline
 on the carrier under its token (incl. E.4(b) retire + m_promise
 clear), so onCrossThreadWorkEnqueued drives them to COMPLETION,
 not merely into m_tasks. U24 Bun arm: spawned-registered ticket,
 registrant dead, hooks installed - settle completes.
 - Wake. The enqueue wakes the parked carrier via a FOURTH hook
 onCrossThreadWorkEnqueued - the ONLY off-carrier-callable hook (no
 JS; REQUIRED with the other three, boot-checked); fallback:
 vm.runLoop().dispatch of the flush; else parked-main settle
 deadlock (U-T9 hook arm).
4. No-hooks runloop wake. RunLoop::stop runs only in
 DWT's timer callback (DeferredWorkTimer.cpp:103-106); runRunLoop
 parks in RunLoop::run() (:185-187) - an E.4(b) off-carrier retire
 emptying m_pendingTickets would strand a parked jsc shell. Any
 internal-arm cancel/retire while
 m_shouldStopRunLoopWhenAllTicketsFinish is set dispatches a re-check
 via vm.runLoop().dispatch(); the re-check owns the stop decision
 ON-loop. Emptiness reads (:103, :186) join rule 1's m_pendingLock.
 U24: last-ticket-retired-off-carrier-while-main-parked shell arm.

## F. Post-GIL API-lock contract (closes UNGIL-PLAN F)

1. JSLock GIL-off mode. JSLock::lock() (JSLock.cpp:84-116) branches
 on mode + caller:
 - Spawned Thread: NO m_lock acquisition. Installs a per-thread
 entry token { unsigned depth; void* spAtEntry; } in the VMLite -
 records sp/lastStackTop (§A.1.4), ORs the VM trap + service words in
 (§A.2.3/§A.1.5), acquires client heap access (§B.1, §A.3.2b-gated),
 bumps depth; unlock() symmetric; depth 0 releases access.
 JSLockHolder = token.
 - Main/embedder: REAL lock semantics - m_lock still mutually
 excludes embedder threads among THEMSELVES (Bun exclusion kept).
 Acquiring it ALSO takes an entry token (§A.3.1 set uniform) + runs
 the §A.3.6 carrier+tag swap; GIL-on extras (atom swap, heap
 forwarding) skipped per §§A.3.7, B.3. Drain-on-release KEPT GIL-off
 (JSLock.cpp:342-343): willReleaseLock drains the CURRENT lite's
 queue (rerouted drainMicrotasks) - THE drain point for
 lock/eval/unlock embedders (I11). Main-carrier drain points = this +
 embedder runloop/DWT (§E.4) + explicit drainMicrotasks + §E.7.3
 flush.
2. Two predicates, split.
 - VM::currentThreadIsHoldingAPILock() (VM.cpp:201) REDEFINED GIL-off
 as "current thread holds an entry token for this VM": what host-call
 asserts use (~AsyncTicket, ThreadManager.cpp:57;
 stopTheWorldAndRun, JSThreadsSafepoint.cpp:225; DWT, §E.7.2).
 - JSLock::currentThreadIsHoldingLock() stays MUTEX-LITERAL - §F.4's
 DAL no-op and the m_lockDropDepth LIFO depend on it. Spawned
 unlock() takes the token branch BEFORE the mutex RELEASE_ASSERT
 (JSLock.cpp:305-311).
 - Consumer audit (REQUIRED at U-T8). The ~60 consumers of either
 predicate get an IU table: assert (token meaning) vs
 BRANCH (behavioral) vs EXCLUSIVITY CONSUMER (needs a §K serializer).
 Fixed: sanitizeStackForVM (VM.cpp:1608-1616) - CURRENT lite's
 lastStackTop; primitiveGigacageDisabled (VM.cpp:755-760) - MUTEX
 predicate + §A.1.5 VM-wide deferred arm;
 JSCell::validateIsNotSweeping (JSCell.cpp:178-182) - token +
 per-CLIENT mutator state; ISS-flip clause-(a) (Heap.cpp:4186-4196) -
 flip completes before any GIL-off entry; DWT per §E.7.2.
3. Strong-handle discipline (api 5.10 under N threads). ONE shared
 HandleSet per VM, new leaf HandleSet::m_strongLock inside Strong
 allocate/free/set-slot only (never across user code);
 per-thread sets would orphan foreign last-ref drops (5.10
 finalizer-hook pattern, ThreadObject.cpp:96-131). All Strong/HandleSet
 mutation requires an entered thread WITH heap access (hence E.2's
 close re-acquires first). GC scans the set in each shared collection's
 handle-scan phase (heap §10 stop - NOT §A.3; jit R1.h): Strong
 mutators hold access => quiesced.
4. DropAllLocks GIL-off (IA D1, ruled HERE): DAL scopes
 ONLY the embedder mutex. Main/embedder: drops m_lock + token.
 Spawned: NO-OP returning 0 - falls out of the mutex-literal predicate
 (JSLock.cpp:423-425); park sites use §J.3. m_lockDropDepth
 strict-LIFO (:449-453) is embedder-only; token holders invisible to it
 (LockObject.h:236-244). GIL-on unchanged.

## G. Per-thread blocking policy

Replaces per-VM G11 gate consumption
(jsThreadsCanBlockOnCurrentThread, ThreadObject.cpp:87-90).

1. New per-THREAD predicate mayBlockSynchronously(): spawned TS = true
 unconditionally; main/embedder = the embedder policy
 (isAtomicsWaitAllowedOnCurrentThread() as today).
2. Governs ALL sync parks: TA/property Atomics.wait (KEPT G11 gate,
 §C.4), join, contended lock.hold, cond.wait. Violations throw
 the existing TypeErrors - api I18 intact for main/embedder.
3. D4 GIL-dropped main TA wait machinery is GIL-on-only; GIL-off a
 permitted main sync wait parks holding only its token (access released
 per E.2). D8 deleted per §C.6.

## H. SymbolRegistry / Symbol.for

Closes vmstate Dev 8 (vmstate:40/:57). WTF::SymbolRegistry's
m_table is JSLock-serialized today. It gains Lock m_lock (rank
LEAF, §LK): symbolForKey, remove, destructor walk. ~StringImpl's
registered-symbol arm calls remove() under it - legal from any
thread incl. GC/finalizer (Dev 8). Registries destroyed in ~VM after
spawned threads exit (U16). One uncontended leaf lock per op; no sharding.

## I. Wasm on spawned threads - REFUSED in v1

Closes UNGIL-PLAN I. Wasm from a spawned Thread throws TypeError,
covering WARM call paths: WebAssemblyFunction caches
m_jsToWasmICJITCode; a trampoline reads m_boxedJSToWasmCallee
(WebAssemblyFunction.h:75-90, :101-106) - ICs warmed on main dispatch a
spawned thread straight in; a slow-path-only check does not gate.
NORMATIVE (both modes, SD7): (1) WebAssembly.{compile,instantiate,
validate} + Module/Instance/Memory/Table/Tag/Global ctors throw if
currentThreadStateIfExists() is a spawned TS; (2) under useJSThreads,
jsCallICEntrypoint() returns nullptr AND every generated JSToWasm entry
- the ensureJSToWasmCallee().entrypoint() thunk and the boxed-callee
trampoline - emits a spawned-TS prologue check (TLS load +
branch-to-throw): all call shapes funnel through a checked entry.
Files: §IM; U17 warm-call variant. GIL-on corpus edited (SD7).

## J. GIL-machinery end state (disposition)

| Artifact | GIL-off disposition (GIL-on unchanged - the oracle) |
|---|---|
| J.1 useThreadGIL (OptionsList.h:696) | KEPT as supported fallback; default flips false at the milestone gate |
| J.2 unlockAllForThreadParking (JSLock.cpp:389-408); J.4 jsThreadGILHandoffYield + D2 notify-yield | never called (J.4: §C.5) |
| J.3 GILDroppedSection (LockObject.h:249-258) | degrades to heap-access release + §A.3 park cooperation, E.2 ordering + §A.3.2b post-wake poll |
| J.5 GILParkSavedExecutionState + resetForFreshThread (ThreadObject.cpp:175) | dead (state per-lite, §A.1); compiled out GIL-off |
| J.6 JSLock handoff body (didAcquireLock/willReleaseLock extras; §F.1 drain + §A.3.6 swap KEPT) | rest skipped per §§A/B/E/F (GIL-on load-bearing, vmstate §6.1) |
| J.7 JSLock.cpp:151 backstop (L1) | REPLACED: GIL-off branch RELEASE_ASSERTs the Phase-B invariant (carrier registered for ENTERED VM, unique TID, TLS tag == CURRENT lite TID); tid-0 never installed |
| J.8 Stub witnesses W2/W3/W4 + OM stub witness (JSThreadsSafepoint.cpp:40-52) | DELETED at U-T5, both modes |

## K. GIL-serialized VM/global caches + lazy init (NEW)

The GIL is today's ONLY serializer for VM-/JSGlobalObject-resident
mutable state outside THREAD.md:19's Group 3; §A.1.3 alone leaves it
raced. Rulings (GIL-on/flag-off unchanged):
1. Per-lite duplicates (L2 appends). Hot per-op scratch/caches:
 VM::numericStrings (VM.h:657; NumericStrings.h has NO lock),
 stringSplitIndice (VM.h:665), dtoa scratch and kin. GIL-off
 accessors route to the CURRENT lite's copy; cell-holding copies
 GC-scanned via the registry walk.
2. Leaf locks. Cold/keyed VM caches whose hits must be shared.
 RegExpCache is ALREADY locked (RegExpCache.h:79 guards its maps -
 reviewer-flagged race refuted; the class-2 model). Unlocked peers
 found by the rule-4 audit get a leaf Lock (§LK).
3. Atomic lazy publication. LazyProperty/LazyClassStructure
 first-touch is a PLAIN uintptr_t state machine (LazyProperty.h:117; no
 atomics): GIL-off it becomes load-acquire fast path; CAS
 lazyTag->initializing; winner runs the initializer holding NO lock
 (may allocate/GC), release-stores the value. Losers wait
 PARK-CAPABLE, in bounded quanta: release heap access (E.2 ordering;
 no lock held), poll BOTH stop families - the lite's §A.3 stop bit
 AND the client's heap §10 GC stop state (stopIfNecessary) - then
 re-acquire via the §A.3.2b-gated path and re-test the load-acquire.
 §A.3 and GC stops are DISJOINT (jit R1.h; heap Dev 8 per-client
 barrier): a loser spinning WITH access polling only §A.3 bits
 deadlocks any collection the winner's initializer triggers (GC waits
 on the access-holding loser; loser waits on the winner; winner
 blocked in allocation). Covers JSGlobalObject initLater + VM lazy
 ensure* siblings. U26 adds a forced-full-GC-during-winner-
 initializer arm.
4. Inventory audit (U-T8b; gates U-T9, the first GIL-off corpus
 runner). Enumerate every VM/JSGlobalObject member mutated on JS
 paths whose only serializer is the GIL; rule each into class 1/2/3 in
 an IU table. Each §F.2 EXCLUSIVITY CONSUMER (treats the
 token-true-on-N predicate as exclusivity proof) must name its
 class-1/2/3 serializer there (reinterpretation cannot fix it). U26.

## LK. Lock-order additions (extends SPEC-heap §6)

| Rank | Lock | Where |
|---|---|---|
| 1 | per-thread VM entry token (§F) - ordering-inert; rank 1's "held entering NVS" role per thread | JSLock GIL-off |
| 3 | ThreadState::inboxLock - guards taskQueue/keepalive/runLoopCondition (§E.1); never nested with joinLock; no heap-access transition held (E.2) | ThreadManager.h |
| 10a | JSPromise internal-state cell lock (§E.1b.2) - existing JSCellLock row | runtime |
| leaf | HandleSet::m_strongLock (F.3); SymbolRegistry::m_lock (H); DeferredWorkTimer::m_pendingLock (E.7 + handoff); ScratchBufferRegistry (A.1.6); §K class-2 cache locks | beside AtomString shards |

No rank changes. §C.1 lock-free arms take NO lock.

## INV. Invariants

- U0 Config gate (§0); boot matrix.
- U1 GIL-off JS thread: registered lite for the ENTERED VM, unique TID,
 live token, TLS tag == CURRENT lite TID (§A.3.6/J.7); tid 0 never
 installed; multi-VM swap test.
- U2 VM-wide trap observed by a parked T within one quantum, BOTH modes
 (§A.2.3-6); terminate-while-parked.
- U3 Order: lite -> ACT -> first alloc; last alloc -> Strong clears ->
 access release -> DCT -> unregisterLite (§B.1-2, E.2).
- U4 §A.3 stop: every entered thread of every target VM parked or
 access-released before the closure; entry during a stop parks; no
 access-released thread runs JS mid-stop (§A.3.2b); INTEGRATION-GATE +
 wake-during-stop amplifier.
- U5/U6 §9.5: successful CAS => no other write between its read and
 write (api 4.5); D3/D7 exclusions hold in the atomic bodies.
 CAS-storms all arms vs grows + plain stores; dict-delete-vs-CAS;
 restricted AS; convert-first; §C.3 I10 GIL-off arms.
- U7 Completion <=> fn returned && queues empty && keepalive==0, OR
 termination (§E.2/E.5).
- U8/U9 Keepalive: at-most-once decrement (armed-flag CAS + open
 check), no underflow - never-armed tickets never decrement;
 mutual-asyncJoin-with-OPEN-inboxes arm; no missed shutdown (§E.3).
- U10 Settles route to the registrant's task queue iff inboxOpen, else
 main; never a foreign microtask queue (I11; SD3).
- U11 join/asyncJoin observe Phase!=Running only after inbox close (F5
 at close); join sees post-fn macrotask effects.
- U12 Nested spawned JSLockHolder: depth-counted, no deadlock, no
 double access acquire.
- U13 APILock predicate true on every host-call path GIL-off (§F.2);
 U-T8 audit.
- U14 Spawned DAL no-op; embedder DAL excludes only embedder threads
 (§F.4); embedder C test incl. §F.1 drain.
- U15 §G: spawned may sync-wait (SD4); main obeys embedder policy;
 G11 TypeError preserved (api I18).
- U16 Concurrent Symbol.for(same key) => one symbol (TSAN).
- U17 Every §I wasm entry throws from a spawned thread, both modes,
 incl. warm-call (main warms, spawned calls).
- U18 Rebias (§D.1): no live dead-TID tag post-stop; restamp before
 reissue; spawn-storm past 2^15.
- U19 GIL-on fallback corpus passes after every U-task, unchanged EXCEPT
 SD6/SD7 (edited once).
- U20 Lint: inboxLock/joinLock never nested; leaf locks never across
 user JS; no heap-access transition holding api-rank locks (E.2).
- U21 Bench: {1,0}<=5%; GIL-off 1-thread <=10% vs {1,0} (§B.5);
 N-thread scaling recorded.
- U22 Reactions on the settling thread (§E.1b.3); AsyncTicket settles on
 the REGISTERING thread (dead=>main); queues owner-only (I11).
- U23 Per-entry record (§A.1.5): entry/exit storm - own service bits
 only; isEntered() correct under churn; VM-wide fan-out reaches
 every entered T.
- U24 DWT (§E.4/E.7): post-settle ticket out of m_pendingTickets,
 Strong cleared; shell exits incl. last-ticket-retired-off-carrier
 while main parked (§E.7.4); hooks on-carrier only; handoff wake.
- U25 inboxOpen opened exactly once pre-fn on spawned TSs, never on
 main/embedder (§E.1); increment sites assert spawned+open.
- U26 §K: concurrent String(0.5)/split/first-touch of one lazy global
 from 2 threads - no race (TSAN), one init; + forced full GC during
 the winner's initializer (losers park, no deadlock, §K.3).
- U27 ~VM (§A.3.6): foreign token-free carriers destroyed in the walk;
 epoch-stale TLS entry never installed; teardown storm.

## SD. Semantic deltas vs phase 1 (corpus impact)

- SD1 Join timing: was fn-return settle (ThreadObject.cpp:205-231;
 4.6.1 SUPERSEDED GIL-off, §E); now queues-empty + keepalive==0.
- SD2 Completion drain: was one SHARED-queue drain (4.6.1 GPO); now own
 queues till empty.
- SD3 Ticket settling thread: was unspecified (I12 GPO); now the
 REGISTERING thread, dead=>main (§E.4; U10).
- SD4 Spawned TA sync wait: was TypeError (I21); now allowed (§C.4/§G);
 gate tests inverted.
- SD5 notify() yield: was GIL yield point (D2); now no yield, parallel
 waiters (§C.5).
- SD6 Main TA sync single-flight: was second-wait throw (D8); now
 allowed, per-wait nodes parking in D9 quanta both modes (§C.6/§A.2.6);
 GIL-on corpus edited (incl. terminate-parked arm).
- SD7 Wasm on spawned threads: was runs; now TypeError BOTH modes (§I);
 GIL-on corpus edited.
- SD8 Terminate parked thread: new - Failed completion, residue to main
 (§E.5).
- SD9 TID exhaustion: was RangeError forever (Dev 10); now until
 next rebias (§D.1).
- SD10 Ordinary-promise reaction thread: new - SETTLING thread
 (§E.1b); U22.
- SD11 Spawned-thread TA waitAsync: settles main-side, no keepalive
 (registrant may complete first, §E.3).
- SD12 asyncJoin: no keepalive - registrant may close first; settle
 dead=>main (§E.3); mutual/self-asyncJoin never deadlocks.

U19 fallback corpus keeps OLD expectations via //@ runThreadsGILOff/
GILOn variants for SD1-SD5, SD8-SD12; SD6/SD7 GIL-on expectations
change.

## IM. Integration-manifest entries (shared hot files)

Diffs land via IU. IA/IJ/IV/IH/IO = INTEGRATE-
{api,jit,vmstate,heap,objectmodel}; bare names = runtime/.

| File | Sections (counterpart) |
|---|---|
| JSLock.{h,cpp} | F, A.3.6-7, B.3, J (IA D1/D11; IV) |
| ThreadObject.cpp | B.1-2, E.2, D.1 (IA) |
| ThreadManager.{h,cpp} | E.1/E.3/E.4, D.1 (IA D5) |
| DeferredWorkTimer.{h,cpp} | E.4/E.7 (IA D5) |
| LockObject/ConditionObject | J.3-5, C.5, C.7/D12 (IA D2/D9/D12) |
| ThreadAtomics.cpp | C.2-3, G (IA D3/D4/D7) |
| AtomicsObject.cpp | C.4 (:613-621 del), C.6, G (IA D4/D8) |
| JSPromise.* + JSGlobalObject | E.1/E.1b, K (IA) |
| bytecode/JSThreadsSafepoint.cpp | A.3 stub swap, J.8 (IJ, IO) |
| VMEntryScope.{h,cpp} | A.1.5, A.3.4 (IJ M7; IV) |
| VM.{h,cpp} + NumericStrings/LazyProperty* | A.1, E.1, F.2, K (IV M4-M6) |
| VMLite.* | A.1-2, B.4 - L2 appends only (IV) |
| VMTraps.* | A.2 (IV) |
| WaiterListManager.{h,cpp} | E.3, C.6 (IA D4) |
| ConcurrentButterfly.h + Structure* | D.1 (IO) |
| VMManager.{h,cpp} | A.3.1-4 (IJ R1; IH) |
| heap/Heap.* + HandleSet.* | A.1.3 root walk (:3585-class visit sites), A.3.2b, D.1, F.3 (IH) |
| llint/jit/dfg/ftl (+FTL/DFG OSR-entry) | A.1 incl. non-baked, B.4 (IJ; gilOff byte) |
| WTF SymbolRegistry.* | H (vmstate Dev 8) |
| wasm/js/* (WebAssemblyFunction, JSToWasm emitters, ctors) | I (SD7) |
| OptionsList.h | U0, J.1, gilOff (all) |

## T. Ordered task list (~1-3k LOC each)

- U-T1. Mode-split Group-3 storage/accessors + lazy per-(thread,VM)
 carriers (P5/CS3 tag swap) + per-lite scratch/regexp + per-entry
 record + service table + GC root walk (§A.1.2-6, A.3.6). Dark.
- U-T2. Per-lite VMThreadContext/VMTraps, fan-out, SignalSender off,
 D9/C.6 re-points, stack limits (§A.2).
- U-T3. loadVMLite (App. R5); LLInt mode-gating on the gilOff byte;
 VMEntryRecord m_vmLite (§A.1.1-3).
- U-T4. Baseline/DFG/FTL emission switch: Group 3, traps, scratch
 indirection incl. non-baked/JITCode-resident (§A.1.3/6).
- U-T5. Thread-granular STW: entered-thread counting, NVS tickets,
 conductor arbitration, park-on-entry, §A.3.2b gates; DELETE stub
 asserts/witnesses/M7 tripwire (§A.3, J.8). Gate: GIL-on no-regression
 + N-separate-VMs INTEGRATION-GATE + $vm stop/resume vs access-released
 embedders. U4 amplifier LANDS here, FIRST RUNNABLE at U-T9.
- U-T6. Per-thread GCClient spawn/teardown, token access, JSLock
 forwarding GIL-on-only (§B.1-3).
- U-T7. TLC lite-relative allocator addressing, all tiers; U21 (§B.4-5).
- U-T8. JSLock GIL-off mode: tokens, predicate split + audit (U13), DAL
 ruling, HandleSet lock, J.7 replacement (§F, J).
- U-T8b. §K inventory + rulings; F.2 third-class audit rows; ~VM walk +
 epoch (§A.3.6). Gates U-T9.
- U-T9. Per-thread runloop + settlement: inboxOpen, task queue/
 keepalive/close, settle routing + DWT retirement/lock + §E.7.3-4 hook
 ruling + wakes, promise protocol, host-hook ruling, termination,
 microtask re-routing (§E); corpus SD1-SD3/SD8/
 SD10-SD12 + hook arm; runs U4's one-VM arm.
- U-T10. ENTRY GATE: Task-14 bench verdict recorded (§D.2). Then §9.5
 accessors (all arms) + flat-path SW discipline + ThreadAtomics re-home
 with D3/D7 (§C.1-2).
- U-T11. PWT inbox re-home + I10 re-validation, 4.5-1a lift, G11
 re-point, D2/D4/D8 lifts (SD6 GIL-on edit), §G predicate,
 GILDroppedSection degradation w/ E.2 ordering (§C.3-6, G, J.3). Corpus
 SD4-SD6.
- U-T12. TID rebias inside a full shared-GC stop (§D.1): enumeration,
 restamp-then-reissue, Dev-10 lift; spawn-storm.
- U-T13. SymbolRegistry lock; atom-swap GIL-on-only; wasm gates incl.
 thunk/trampoline checks (SD7 edit) (§H, §I, A.3.7).
- U-T14. Close: U0 gate; TSAN + amplifier; U19 corpus; default flip;
 IU dispositions.

Dependencies: T1->{T2,T3,T4}; {T2,T5}->T6; T5 gates T12; {T8,T8b}->T9;
T9 gates T11's settle paths; Task-14 verdict gates T10; T14 last. T1-T7
land dark. Each task re-runs the flag-off golden gates and U19.

Full rationale, rejected alternatives, rev log: history.
