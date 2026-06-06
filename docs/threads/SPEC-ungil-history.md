# SPEC-ungil-history.md — change log + overflow rationale

## Rev log

- **rev 1 (2026-06-05).** Initial freeze. Closes UNGIL-PLAN.md items A-J.
  All file:line ground-truth citations re-verified by reading the cited
  code on branch jarred/threads this date (JSLock.cpp, ThreadObject.cpp,
  LockObject.h, ThreadManager.cpp, ThreadAtomics.cpp, AtomicsObject.cpp,
  JSThreadsSafepoint.cpp, VMEntryScope.cpp, OptionsList.h:696, WTF
  SymbolRegistry.cpp) and the cited spec lines (SPEC-vmstate.md:37-48,
  335-349, 418, 443, 490, 529, 551, 572; SPEC-heap.md:26-27, §6 table at
  :109-130; SPEC-api.md:22, 26, 79, 100, 126, 200, 225, 306, 315, 361;
  SPEC-jit.md:233, 278; SPEC-objectmodel.md:359, 377-378; THREAD.md:19,
  :98).

## Overflow rationale (normative intent lives in SPEC-ungil.md; this
## section records WHY, and the alternatives rejected)

### A.1 base-pointer choice: TLS materialization over a pinned GPR
A dedicated callee-saved register for the VMLite base was rejected:
x86_64 register pressure (LLInt already pins PB/metadataTable; Baseline/DFG
allocators would lose a GPR globally for a value needed only at VM-field
touch points), and the ABI churn would invalidate the vmstate Phase A
"accessor signatures frozen, impl replaceable" escape hatch (L4) by leaking
into every tier's frame contract. TLS-read-per-prologue with caching in a
temp costs one load on entry paths and nothing on slot-access fast paths
that already have the lite in a register from the prologue. The
VMEntryRecord::m_vmLite slot exists so OSR exit and unwinding can recover
the base without TLS access in awkward contexts.

### A.3.6 main-carrier decision: why lazy real carriers, not a §6.4(3) view
The alternative (embedder threads share a tid-0 "view" of the VM) is
exactly the configuration JSLock.cpp:136-148 documents as unsound GIL-off:
two threads believing they are TID-0 owners race unlocked flat-butterfly
transitions. Unique lazy TIDs make embedder threads ordinary mutators to
the OM machinery; the cost (TID-space consumption by embedder threads) is
absorbed by Task 13 rebias. api SPEC-api.md:361 already promised "post-GIL:
real TID lazily at first VM entry", so this is a confirmation, not a new
choice.

### E.3 keepalive: why decrement-at-enqueue, not decrement-at-run
Decrementing when the settle task RUNS would make the exit condition
"queues empty && keepalive==0" racy in the other direction: a settler that
appended but not yet decremented blocks exit (fine), but a cancel path
that never enqueues would need a separate decrement anyway, splitting the
accounting across two disciplines. Decrement-at-enqueue under the same
inboxLock critical section as the append gives a single-lock proof of U9:
the predicate (queue emptiness, keepalive) changes only under inboxLock,
and every decrement either appends in the same section or is a cancel
(no append ever follows). The per-ticket m_keepaliveReleased CAS mirrors
the landed m_settled CAS (ThreadManager.cpp:78-81) so the
settle-vs-cancel-vs-close races collapse to one winner.

### E vs DWT: why main keeps DeferredWorkTimer
Spawned threads get bespoke queues rather than per-thread DWT instances
because DWT is RunLoop-coupled and the embedder owns the only real
WTF::RunLoop (Bun's event loop, USE_BUN_EVENT_LOOP). A spawned thread's
"runloop" is the E.2 drain loop — a condition-variable pump — not a
WTF::RunLoop; tickets keep their DWT registration solely for the
shell-liveness (I20/4.6.3) and VM-shutdown cancelPendingWork backstop,
both of which remain process-global concerns.

### F.3 Strong handles: why one locked HandleSet, not per-thread sets
Per-thread HandleSets were rejected because Strong lifetime is not
thread-affine in the landed design: the 5.10 finalizer hook
(ThreadObject.cpp:96-131) and ~AsyncTicket (ThreadManager.cpp:48-59) exist
precisely because last-refs drop on foreign threads (GC finalization,
embedder TLS teardown). Per-thread sets would require cross-thread free
queues with their own epoch problem; a leaf lock on allocate/free is two
uncontended atomic ops on a path that is already not hot (Strong churn is
host-API, not JS-loop). Revisit only if the bench gate shows contention.

### H: why a plain leaf lock, not a sharded/concurrent registry
Symbol.for traffic is orders of magnitude below atomization traffic (which
did get shards). A leaf Lock keeps the WTF diff minimal and the lock-order
proof trivial. The non-goal is recorded in §H.3 with an explicit
bench-evidence reopen condition.

### I: why refuse wasm rather than GIL-serialize wasm on spawned threads
A "wasm runs but takes the GIL" hybrid was rejected: it reintroduces a
global lock with all of §F's deleted semantics for an unaudited subsystem,
creates a priority-inversion channel between JS threads and wasm callers,
and would still be unsound where wasm code calls back into JS (the
re-entry would hold the wasm-GIL across arbitrary JS). The clean TypeError
is honest, testable (U17), and cheap to lift later under a dedicated
charter. Making the gate active under GIL-on too (SD7) buys mode-equal
corpus behavior — a spawned-thread wasm test must not pass GIL-on and
fail GIL-off.

### Task sizing notes
U-T3/U-T4 sizes assume the Group-3 field set's JIT touch points are
mechanical (offset-expression rewrites guarded by the golden-disasm gate);
if FTL's B3 stack-slot interactions with per-lite scratch buffers exceed
budget, split U-T4 into U-T4a (Baseline/DFG) and U-T4b (FTL) at the
implementation workflow's discretion — the spec's freeze is on the
addressing contract, not the task boundary.

### Explicitly NOT redesigned here (deferred with citations)
- OM Task 14 structure splitting: deferral + bench-verdict recording only
  (SPEC-ungil §D.2; SPEC-objectmodel.md:359 decision rule).
- heap concurrent marking / incremental sweep (heap Dev 7 lists them with
  the TLC charter): TLC emission is in scope (U-T7); marking/sweep
  concurrency upgrades are perf work behind the same budget gate and ride
  the heap WS's own charter — no semantic dependency from A-J.
- Inspector/debugger attach to spawned threads: out of scope for the ungil
  milestone (no charter; debugger pause uses the §A.3 stop and observes
  whatever thread is conductor).

- **rev 2 (2026-06-05).** Round-1 adversarial review: 17 findings (6
  blocker, 11 major), ALL verified real against the tree — none refuted.
  Dispositions (finding -> spec change):
  1. A.3.2 heap-access-released stop exemption had no re-acquisition
     gate (JSThreadsStopScope sets no client-visible stop state,
     Heap.cpp; acquireAccess consults only GC stop state) -> new
     normative §A.3.2b: acquire-slow-path poll of the per-lite JSThreads
     stop bit + mandatory post-wake poll at every park site; U4 gains a
     wake-during-stop amplifier arm; E.6/J.3 cross-referenced.
  2. C.1 accessors were unsound both paths (segmented cell-lock CAS does
     not serialize vs lock-free fragment stores; flat grow = CAS+copy
     with no nuke -> lost CAS / double-applied RMW) -> respecified:
     direct seq_cst 64-bit CAS on the fragment word (fragments never
     move); flat path runs the OM §2 foreign-write SW-set DCAS when TID
     mismatches, re-validates per I34, whole-probe restart, RMW never
     re-applied. Cell-locked segmented CAS REJECTED because mandating
     the cell lock for all segmented writes would contradict the frozen
     OM lock-free regime-2 design.
  3. Ordinary shared-promise cross-thread settlement undesigned -> new
     §E.1b: reactions run on the SETTLING thread (option (b) of the
     review's menu); JSPromise internal-state transitions cell-locked
     GIL-off; U22 rewritten; SD10 added. Per-reaction registrant
     tracking REJECTED: a new heap-visible per-reaction structure with
     its own lifetime/teardown races, and the registrant may be dead at
     resolve time — the settling-thread rule needs no new state and is
     I11-clean (enqueuer is the queue owner).
  4. E.3 termination bulk-zero + cancel decrement double-decremented ->
     close now CLAIMS each outstanding registration's
     m_keepaliveReleased flag; the CAS guards ALL decrement sites; rules
     1-3 restated.
  5+11. DWT: m_pendingTickets had no lock (DeferredWorkTimer.h:121) and
     the thread-routed settle path never retired tickets -> new §E.7
     (internal leaf m_pendingLock, asserts re-pointed at the token
     predicate) + E.4 retirement protocol (ThreadTask runs settle, then
     cancelPendingWork under DWT's lock, then clears m_promise);
     dead=>main keeps scheduleWorkSoon; U24 added; DWT files added to IM.
  6. E.2 released heap access INSIDE the inboxLock section
     (releaseAccessSlow can stop the thread, Heap.cpp:2580-2595 — GC vs
     settler cycle) -> normative ordering rule: no heap-access
     transition while holding any api-rank lock; loop rewritten; U20
     extended.
  7. F.2 conflated VM::currentThreadIsHoldingAPILock (VM.cpp:201) with
     JSLock::currentThreadIsHoldingLock -> split: VM predicate = token,
     JSLock predicate = mutex-literal (F.4's DAL no-op and
     m_lockDropDepth LIFO depend on it); spawned unlock() routes to the
     token branch before the mutex assert; U-T8 now requires a full
     ~60-consumer audit table with fixed dispositions for the branch
     sites (sanitizeStackForVM, primitiveGigacageDisabled,
     validateIsNotSweeping, the Heap.cpp ISS-flip exclusion, DWT).
  8. Lazy main/embedder carriers omitted the P5/CS3 butterfly-TID-tag
     TLS install (zero-init correct only for tid 0) -> A.3.6 mandates
     the tag install/clear at lazy carrier install/teardown; U1 and J.7
     assert TLS tag == lite TID.
  9. Group-3 storage location per mode undecided + scratch-buffer
     baked-address mischaracterization -> A.1.3 decides: VM members
     flag-off/GIL-on (golden gate trivially holds), VMLitePrimitives
     GIL-off, JIT switches at codegen time, LLInt branches on the
     JSCConfig gate byte with ONE recorded golden-gate re-baseline; new
     A.1.6: per-lite scratch pool with runtime two-load indirection in
     DFG/FTL (baked addresses GIL-on/flag-off only).
  10. VMEntryScope/VM::entryScope + entry-scope service bits stayed
     VM-global (VMEntryScope.cpp:90/:133; VM.h:298/:373-454) -> new
     A.1.5 per-entry record in the lite; isEntered() over the
     entered-thread set; consumer ruling; VMEntryScope.{h,cpp} IM row
     upgraded from tripwire-deletion-only; U23 added.
  12. A.1.1 TLS scheme platform-unsound as stated; mid-body access
     unspecified -> A.1.1 adopts the frozen jit annex App. R5
     per-platform model verbatim (ELF IE-TLS; Darwin pthread key via
     the M4a JSCConfig slot; Windows already unsupported flag-on per
     App. R5 — no new Windows story owed); A.1.2 fixes the mid-body
     model: rematerialize via loadVMLite at each site, prologue/
     VMEntryRecord caching is optimization-only.
  13. D.1 rebias had no enumeration mechanism or restamp-to-0 soundness
     argument -> D.1 specifies HeapIterationScope full-walk (precise +
     aux) + StructureID-table walk, owning files, cost note, and the
     soundness paragraph: restamped objects become equivalent to
     main-allocated (payload-0/TID-0 regime, OM decode tests payload
     first); restamp ordered BEFORE m_freeTIDs release within one stop,
     which is exactly what the annex false-owner hazard requires.
  14. queueMicrotaskToEventLoop host hook unruled -> E.1 hook clause:
     consulted only on the main/embedder carrier; spawned threads always
     per-lite; corpus test with installed hook.
  15. C.4 ordered deletion of the WRONG gate: ThreadAtomics.cpp:536-541
     is the frozen-api G11 property-wait gate (kept, re-pointed at
     mayBlockSynchronously per §G.2); the sole 4.5-1a gate is
     AtomicsObject.cpp:613-621 (grep isJSThreadCurrent: only hit).
     C.4 rewritten; IM rows corrected.
  16. U19 "unchanged" contradicted C.6/§I both-mode changes -> master
     rule and U19 amended to "unchanged EXCEPT recorded both-mode deltas
     SD6/SD7", which are direct GIL-on corpus edits, removed from the
     per-mode-variant footnote. Option (b) chosen over scoping the D8
     deletion GIL-off-only because per-wait nodes are strictly more
     correct under the GIL too and a mode-conditional D8 gate would
     keep dead machinery alive solely to preserve a wart.
  17. §E silently superseded non-GPO frozen api 4.6.1 "Never waits for
     tkts" / 4.6.2 shell-granularity keepalive -> explicit SUPERSESSION
     block added to the §E preamble citing both sides; recorded for
     INTEGRATE-api.

  Byte-budget note: rev 2 also compressed prose throughout to stay
  under 50000 bytes; all removed rationale lives here. Additional
  rejected alternatives recorded this rev: cell-locked segmented CAS
  (finding 2), per-reaction registrant tracking (finding 3),
  GIL-off-only D8 scoping (finding 16).

## Rev 3 (2026-06-05) — round-3 review dispositions

All eleven distinct round-3 findings verified against the tree and
ACCEPTED (no false positives; duplicates merged: E.3-rule-3 x3, wasm
x2). Dispositions:

1. BLOCKER, E.2 close ran F1/F5 + Strong mutation access-released
   (contradicting B.2/U3; racing the GC handle scan): close now
   re-acquires access (after the §A.3.2b poll) BEFORE DWT retirement,
   residue routing, and F1/F5; access released at the landed T5 point
   after the Strong clears. New normative rule in §F.3: ALL
   Strong/HandleSet mutation requires an entered thread WITH heap
   access — that quiescence (not m_strongLock) is what the
   collection-time handle scan relies on; m_strongLock only guards
   allocate/free/set-slot against concurrent mutators.
2. MAJOR x3, E.3 rule 3 claim step unimplementable (no per-TS
   outstanding-registration set exists; taskQueue residue holds only
   already-settled tickets — verified ThreadManager.h:200-235, no
   forward set, only asyncJoiners + inbox): chose option (a) — claim
   step DROPPED. Proof of exactly-once without it: every decrement
   site is conditioned on (flag-CAS win AND inboxOpen) under
   inboxLock; after close, inboxOpen=false makes all later
   settles/cancels decrement-free (they win the CAS, skip the
   decrement, take the main fallback), and the counter is dead — the
   E.2 exit predicate is only read while the inbox is open, so a
   stale keepalive>0 value can never cause a hang or an early exit.
   U8's race-matrix argument re-derived from rules 1-2 alone.
   Rejected (b) (a per-TS Vector<Ref<AsyncTicket>> under inboxLock):
   adds a field, lock traffic at every registration/settle, and
   ~AsyncTicket interaction, purely to zero a counter nobody reads
   after close.
3. MAJOR, E.1b.2 promise one-liner deadlock-prone (verified
   JSPromise.cpp:341-440: pre-switch status() read, mid-body GC
   allocation of JSSlim/JSFullPromiseReaction + Bun
   InternalFieldTuple, setInlineHandlerReaction fast path): expanded
   to the normative allocate-outside/validate-and-publish-under-lock
   protocol; OM I20 (no GC alloc under 10a) restated as the driver;
   inline-reaction path runs fully under the lock (no alloc);
   markAsHandled/payloadCell/tier-inlined accesses are U-T9 audit
   items.
4. BLOCKER+MAJOR, §I wasm gate bypassable via cached
   jsToWasmICEntrypoint + boxed-callee trampoline (verified
   WebAssemblyFunction.h:75-90/:101-106): enforcement re-specified —
   under useJSThreads jsCallICEntrypoint() returns nullptr AND both
   generated JSToWasm entries (entrypoint thunk + interpreter
   trampoline) emit a spawned-TS prologue check; U17 gains the
   warm-call arm; wasm files added to IM. Rejected check-only-in-slow
   -path (does not gate) and value-visibility refusal (would need an
   object-graph walk).
5. MAJOR, DWT embedder hooks unruled (verified DeferredWorkTimer.h:
   110-112; addPendingWork/scheduleWorkSoon bypass m_pendingTickets
   when installed, .cpp:190-212): new §E.7.4 — hooks fire only on the
   main/embedder carrier; spawned-thread settles to hook-managed
   tickets go through an m_pendingLock-guarded handoff queue flushed
   by the carrier; retirement embedder-owned via onCancelPendingWork.
   Rejected "hooks must be thread-safe": a mid-flight contract change
   for Bun's landed event loop.
6. MAJOR, main/embedder microtask drain unspecified GIL-off (verified
   JSLock.cpp:342-343 is the API-embedder drain point): §F.1 now
   KEEPS drain-on-release GIL-off (drains the CURRENT lite's queue
   via the rerouted drainMicrotasks; well-defined under tokens, I11);
   main-carrier drain points enumerated. No SD entry — behavior
   preserved; U14 covers it.
7. MAJOR, GC-stop vs JSThreads-stop conflation: §D.1 now runs rebias
   world-stopped INSIDE the next full shared-server collection under
   the heap §10 barrier (NOT §A.3; jit R1.h keeps the machineries
   disjoint; mid-walk re-entry blocked by the GC's client-visible
   stop state); U-T12 re-worded. §F.3's Strong scan moved to the heap
   GC stop's handle-scan phase, soundness from the finding-1 rule.
8. MAJOR, A.1.6 scratch indirection underdesigned: concrete design
   added — process-wide ScratchBufferRegistry (leaf lock; monotonic
   index allocator + index->size map), per-lite never-shrinking
   segmented pointer table (lock-free reads), population invariant
   (code install fans out to all lites under the VMLiteRegistry lock;
   lite registration backfills) so the two-load JIT sequence never
   needs a lazy-alloc branch.
9. MAJOR, U-T5's U4 gate unexecutable as sequenced (GIL-off entry
   lands at T8, task appends at T9): staged explicitly — U4 arm LANDS
   with U-T5, FIRST RUNNABLE at U-T9 close-out (a U-T9 exit
   criterion); interim U-T5 gate = GIL-on no-regression +
   N-separate-VMs INTEGRATION-GATE + $vm stop/resume vs
   access-released embedder threads.
10. BLOCKER, A.3.2b gate attached to GIL-on acquire functions
   (verified Heap.h:1557-1565: GCClient::Heap::acquireHeapAccess/
   attachCurrentThread are the per-thread API, and the header invites
   direct AHA/RHA brackets): (i) re-pointed at the per-client layer;
   per-VM acquireAccess/acquireAccessForwardedToMainClient keep the
   poll for GIL-on/forwarded acquires; (ii) park-site polls demoted
   to defense — (i) is the soundness carrier because it covers
   arbitrary embedder brackets no site list can enumerate.
11. BLOCKER, C.1 contradicted OM locked regimes (verified OM I19/L3
   dictionary cell lock, §4.6 AS+SW locked, SPEC-objectmodel.md:46/
   :178/:229; Thread.restrict forces ArrayStorage per api Dev 11; the
   8g chartering record named the locked-dictionary variant,
   history:861): third normative arm added — locked-regime receivers
   do probe+CAS/RMW under the cell lock OM already mandates (plain
   stores there are also locked, so lock-held RMW serializes; and the
   lock is REQUIRED because dictionary mutation is StructureID-
   invisible, making I34 validation blind to quarantined-slot
   deletes). LK note updated; U5 gains dictionary-delete-vs-CAS and
   restricted-object storm arms.

Overflowed rationale (compressed out of the spec this rev):
- F.2 audit per-site reasoning: sanitizeStackForVM's RELEASE_ASSERT
  checks the current stack contains lastStackTop — per-lite storage
  makes token-true per-thread-correct; primitiveGigacageDisabled's
  fast arm means "can fire watchpoints synchronously", which GIL-off
  requires a §A.3 stop, so token-only threads must take the deferred
  arm; the ISS-flip clause-(a) exclusion reasons from mutex exclusion
  tokens do not provide, hence the boot-ordering assert making it
  unreachable GIL-off.
- D.1 restamp-to-0 full argument: payload==0 tested before any TID
  compare (OM decode); TID 0 bit-identical to today; restamped flat
  butterflies behave as main-allocated (foreign writers take the
  SW-set discipline; the only flat-owner claimant is main, a live
  owner under the landed protocol); the annex E4/T1 false-owner
  hazard is about reissue-before-restamp, excluded by ordering
  restamp before the m_freeTIDs release within one stop; parked
  threads' TLS tags belong to live TIDs and are untouched.
- E.2 GC-vs-settler cycle detail: releaseAccessSlow can hand the conn
  over and stop the thread (Heap.cpp:2580-2595); a settler holding
  access blocking on inboxLock while the GC waits on that settler is
  the cycle the ordering rule breaks.

## Rev 4 (2026-06-05) - review round: 4 blockers + 6 majors, all upheld

Every finding was verified against the tree before fixing; none was a
false positive. Dispositions (spec section in parens):

1. BLOCKER - TA Atomics.waitAsync keepalive increment had no decrement
   path (E.3). VERIFIED: WaiterListManager settles outside AsyncTicket
   (WaiterListManager.cpp:291 scheduleWorkSoon; timers on the VM
   runloop :176); TicketData has no registrant/m_keepaliveReleased, so
   neither E.4 rule could ever decrement - a NOTIFIED spawned-thread
   waitAsync would park its thread forever and hang join(). FIX
   (option b): the S4 increment is DELETED; TA waitAsync from spawned
   threads settles main-side and takes NO keepalive (new SD11 +
   corpus variant). Property Atomics.waitAsync IS an AsyncTicket
   (ThreadAtomics.cpp:639) and keeps its increment. Option (a) -
   re-homing TA waitAsync onto AsyncTicket so settlement runs on the
   registering thread - was REJECTED for v1: it requires redesigning
   WaiterListManager registration (per-waiter ticket plumbing through
   the WaiterList/Waiter machinery) for no soundness gain; main-side
   settle is sound under the shared heap (any thread may resolve a
   shared promise per E.1b; U22's "settling thread" rule is satisfied
   with main as the settler). Revisit post-v1 if thread-affine TA
   waitAsync reactions are wanted.
2. BLOCKER/MAJOR (two findings, same root) - inboxOpen never opened.
   VERIFIED: ThreadManager.h:225 initializes false; the declaration is
   the only occurrence in runtime/. FIX: new E.1 "inboxOpen lifecycle"
   clause - opened exactly once on the owning spawned thread in
   threadMain, under inboxLock, after lite/GCClient setup and BEFORE
   fn (hence happens-before any registration against the TS, since
   E.3 increments run on the registering == owning thread; a foreign
   thread cannot register against a TS before that TS's fn has run
   and handed out capabilities). Main/embedder TSs never open theirs;
   settles to them always take the E.4 main path - consistent with
   E.3's main-no-keepalive rule and rule-2's open check (cancel of a
   main registration skips the decrement, correctly, since none was
   taken). New invariant U25; in U-T9 scope.
3. MAJOR - async (signal) VMTraps delivery undesigned. VERIFIED:
   VMTraps.cpp:305 targets vm.ownerThread() (JSLock mutex owner -
   never set GIL-off) and vmIsInactive (:80) reads !entryScope &&
   !ownerThread(), TRUE while N threads run JS; :330 wakes the single
   vm.syncWaiter() that C.6 deletes. FIX: A.2.5 - GIL-off never starts
   the SignalSender (effective polling-trap behavior; the jit-mandated
   poll sites + D9 park quanta carry delivery and U2's bound);
   vmIsInactive re-derived from the lite registry. A.2.6 - the :330
   wake is GIL-on-only; C.6 per-wait TA parks and C.3 property parks
   use D9 10ms quanta polling the current lite's termination bit.
   Alternative (give each lite's VMTraps an owning Thread ref and keep
   signal delivery) rejected for v1: ThreadSuspendLocker-based signal
   install against N concurrently-running mutators multiplies the
   suspension-deadlock surface for marginal latency win over 10ms
   quanta + poll sites.
4. MAJOR - cross-thread entry-scope service requests had no routing
   rule. VERIFIED: requesters with no lite for the target VM exist by
   design (VM.cpp:764 Gigacage callback, :811 SamplingProfiler, :318
   watchdog; CONCURRENT_SAFE overload VM.h:381). FIX: A.1.5 "Service
   routing" fan-out mirroring A.2.3 - VM-level service word + fan into
   every registered lite under the registry lock; token acquisition
   ORs the VM word in; thread-local requests set the current lite;
   classification table due at U-T1. F.2's primitiveGigacageDisabled
   disposition now rides the VM-wide path. U23 extended.
5. MAJOR - multi-VM per thread vs one-carrier-per-thread + J.7 assert.
   VERIFIED: the GIL-on install (JSLock.cpp:131-156) explicitly
   handles multi-VM today; testapi exercises multiple contexts per
   thread. FIX: A.3.6 - carriers are per-(thread,VM); main/embedder
   lock() installs the entered VM's carrier AND swaps the
   butterfly-TID-tag TLS, restoring the prior {lite, tag} pair on
   release (LIFO under nesting). Spawned threads stay single-VM in v1
   (RELEASE_ASSERT; boot-gated) - a spawned Thread has no API surface
   to enter a foreign VM in v1, so this is a tripwire, not a
   behavioral change. J.7/U1 restated against the ENTERED VM.
6. BLOCKER - E.7.4 handoff queue had no main-carrier wakeup under
   USE_BUN_EVENT_LOOP (main parks in the embedder epoll/kqueue loop;
   all listed flush points require main already inside JSC). FIX:
   normative wake - fourth hook onCrossThreadWorkEnqueued, the only
   hook callable off-carrier (no JS; REQUIRED whenever the other three
   are installed, boot-checked); fallback vm.runLoop().dispatch of the
   flush task. RunLoop::dispatch alone was not made primary because
   with Bun hooks installed the VM runloop may not be the loop main
   actually sleeps in; the hook lets the embedder use its native wake
   primitive. U24 + the U-T9 hook arm exercise the parked-main settle.
7. MAJOR - E.7.4 covered only the settle side; registration/cancel
   fired hooks on the calling thread. VERIFIED: addPendingWork invokes
   onAddPendingWork inline (DeferredWorkTimer.cpp:204-205);
   scheduleWorkSoon → onScheduleWorkSoon (:234); cancelPendingWork →
   onCancelPendingWork (:266) - all on the caller. FIX: TicketData
   hookManaged bit set at registration iff hooks installed AND
   registrant is a main/embedder carrier; spawned-TS registrations
   always take the internal m_pendingTickets arm with NO hook call -
   I20 liveness holds from the internal append (registration is live
   from the append, not any flush), and thread keepalive (E.3) is the
   spawned-registrant liveness carrier anyway. Off-carrier
   settle/cancel of EITHER kind goes through the handoff queue and
   re-dispatches on the carrier; onCancelPendingWork therefore only
   ever sees hookManaged tickets, on-carrier, resolving the
   E.4(b)-vs-E.7.4 composition.
8. MAJOR - IM omissions. FIX: rows added for VMTraps.{h,cpp} +
   VMTrapsInlines.h (A.2 / IV), WaiterListManager.{h,cpp} (E.3 note,
   C.6 / IA D4), ConcurrentButterfly.h + Structure* (D.1 restamp /
   IO). Watchdog.cpp consumption is covered by the A.1.5 entryScope
   rehoming under VM.{h,cpp}'s row (watchdog reads the CURRENT lite).
9. BLOCKER - §C could not express indexed property keys. VERIFIED:
   ThreadAtomics routes parseIndex hits through
   canGetIndexQuickly/getIndexQuickly (:77-82) and putDirectIndex
   (:147-148); JSTests/threads/atomics/property-rmw.js exercises
   integer keys. FIX: new indexed arm in the 8g re-freeze -
   atomicElementCompareExchange/ReadModifyWrite(JSObject*, uint32_t)
   dispatching on indexing shape: CoW materializes per OM 4.8/I35
   before any probe; Int32/Double CONVERT to Contiguous at first
   atomic access via the ordinary OM transition discipline (owner
   direct, foreign SW-set-DCAS-first); Contiguous takes the flat
   lock-free arm verbatim; AS/dictionary-indexed take the locked arm.
   Raw int32/double word CAS semantics were REJECTED: an RMW result
   (or Atomics.exchange of a non-number) can force a value-shape
   transition mid-operation - the conversion would have to happen
   inside the atomic step, which the flat protocol cannot express;
   one-time convert-to-Contiguous at first atomic access is the
   landed engines' precedent (shape stays legal for all subsequent
   plain ops). §9.5 named accessors are now explicitly scoped to
   non-index PropertyNames.
10. MAJOR - Task-14 PRE-INT verdict does not exist. VERIFIED:
   INTEGRATE-objectmodel §46 still reads "run the jit Task-13 bench
   ... and record the verdict here" - never done; UNGIL-PLAN.md:250
   binds SPEC-ungil to RECORD, but a docs-only round cannot run a
   bench. FIX: explicit SUPERSESSION in D.2 citing both sides
   (SPEC-objectmodel.md:359 + SPEC-api.md:26 + SPEC-jit.md:278 vs
   D.2): the verdict gate moves to a HARD precondition of U-T10
   ENTRY - the first task a PROMOTE verdict would invalidate (U-T1..
   T9 touch no 8h-dependent design: A/B/E/F/G/H machinery and the
   promise cell-lock protocol are structure-splitting-agnostic).
   Contingency: on PROMOTE, Task 14 lands before U-T10 and §C arm 3
   is re-reviewed pre-implementation. T-list/dependency line updated;
   U-T14 no longer carries the verdict.

Editorial: rev 4 also compressed prose throughout to stay under the
50,000-byte cap; all removed rationale lives in this file. Notably:
- E.1 host-hook rationale (X1.7): consulting queueMicrotaskToEventLoop
  for spawned-thread enqueues would route every spawned reaction to
  the embedder's main loop, violating I11 (owner-only queues) and U22
  (reactions run on the settling thread).
- A.3.2b (i)-vs-(ii): (i) is the soundness carrier because
  GCClient::Heap's acquire/attach header invites arbitrary embedder
  acquire/release brackets that a park-site enumeration (ii) cannot
  cover; (ii) remains as defense in depth.
- D.1 restamp-to-0: full annex false-owner argument as in rev 3.

## Rev 5 (2026-06-05) - review round: 4 blockers + 4 majors; 7 upheld
## (1 partially refuted), all fixed in-spec

1. BLOCKER - property Atomics.wait keeps the landed read-then-enqueue
   shape, reopening I10 GIL-off. VERIFIED: ThreadAtomics.cpp:529-531
   does the SVZ read under the JSLock with "no re-read below";
   :550-552 explicitly credits the JSLock (not the listLock) with
   closing the lost store+notify window; the D9 park loop (:571-576)
   polls only termination/deadline; the waitAsync arming enqueue
   (:646 region) has the same unlocked equality check. GIL-off, §C.1
   atomic stores take NO lock, so [read==expected] -> [store] ->
   [notify sees empty list] -> [enqueue, park] loses the wake forever
   (sync: infinite park; async: never-settled ticket; with old §E.3,
   an unjoinable registrant). FIX (§C.3 rewritten): GIL-off both arms
   enqueue under listLock then RE-VALIDATE SVZ(o[k], expected) via the
   §9.5 atomic load STILL UNDER listLock; mismatch => dequeue +
   "not-equal"; rope re-read => unlock/resolve/restart (no allocation
   under listLock; rank 3 -> 10a nesting is in §LK order). Soundness:
   the notifier acquires listLock after its seq_cst store, so any
   store invisible to the re-validation has its notify ordered after
   our enqueue - exactly WaiterListManager's under-lock check shape.
   U5/U-T11 corpus arms added.

2. BLOCKER - no design for GIL-serialized VM/global caches + lazy
   init. VERIFIED for: VM::numericStrings (VM.h:657; NumericStrings.h
   lockless), stringSplitIndice (VM.h:665, reserveInitialCapacity
   VM.cpp:324), LazyProperty/LazyClassStructure plain uintptr_t state
   machine (LazyProperty.h:117; LazyPropertyInlines.h - no atomics).
   PARTIALLY REFUTED for RegExpCache: RegExpCache.h:79 declares
   Lock m_lock and RegExpCache.cpp takes it in lookup/lookupOrCreate/
   addToStrongCache/deleteAllCode (:43,:52,:63,:78,:100) - the map is
   already internally locked; cited in-spec as the class-2 model. FIX:
   new §K with three rulings (per-lite duplicate / leaf lock / atomic
   double-checked lazy publication with stop-bit-polling spinners),
   a mandatory inventory audit task U-T8b gating U-T9, and the §F.2
   audit taxonomy extended with the third class (EXCLUSIVITY
   CONSUMER must name its §K serializer; reinterpreting the
   token-true-on-N predicate cannot fix such consumers). U26.

3. BLOCKER - asyncJoin keepalive deadlocks mutual/self asyncJoin.
   VERIFIED: join() has a self gate (ThreadObject.cpp:340-342);
   threadProtoFuncAsyncJoin (:398-435) has none; §E.3 rev 4 counted
   asyncJoin in keepalive, and its ticket settles only at the joinee's
   close, so A<->B mutual asyncJoin (or self-asyncJoin) left both
   keepalives pinned at >0 forever - a GIL-on-legal program that hangs
   GIL-off, in no SD entry. Also confirmed UNGIL-PLAN item E's
   mandated keepalive list omits asyncJoin. FIX: asyncJoin removed
   from the increment set (no keepalive); registrant closed by settle
   time => E.4 main fallback (SD11 precedent, I12 dead=>main,
   api:306). New SD12 + mutual/self corpus variants. Rationale for
   keeping the other three: their settles originate outside the
   joinee-close path and represent the registrant's own pending work.

4. BLOCKER - §A.1.6 covered only baked-address scratch. VERIFIED:
   FTLLowerDFGToB3.cpp:300 stores vm.scratchBufferForSize into
   jitCode->common.catchOSREntryBuffer (one buffer per compiled code,
   shared by all entering threads); FTLForOSREntryJITCode.cpp:47
   caches m_entryBuffer identically; DFGOSREntry.cpp:248 calls
   vm.scratchBufferForSize per entry at runtime. Verbatim rev-4
   implementation would leave concurrent catch-/loop-OSR entries
   sharing one staging buffer (silent wrong values). FIX (§A.1.6
   NON-BAKED rule): VM::scratchBufferForSize GIL-off resolves through
   the CURRENT lite's table with a per-size-class registry index
   (covers all runtime C++ callers, bounded index growth); the
   JITCode-resident ScratchBuffer* members become registry INDICES
   resolved against the entering lite at use; amplifier variant
   (concurrent catch-/loop-OSR-entry); U-T4 scope + §IM row extended
   with the FTL/DFG OSR-entry files.

5. MAJOR - DWT no-hooks runloop wake missing. VERIFIED: RunLoop stop
   only inside the DWT timer callback (DeferredWorkTimer.cpp:103-106);
   runRunLoop parks in RunLoop::run() (:185-187); the §E.4(b)
   spawned-thread retirement does cancelPendingWork with no m_tasks
   append/timer arm => jsc shell strands when the LAST pending ticket
   is retired off-carrier. FIX (§E.7.4): any internal-arm cancel/
   retire while m_shouldStopRunLoopWhenAllTicketsFinish dispatches a
   re-check via vm.runLoop().dispatch() (cross-thread-safe); the
   re-check owns the stop decision on-loop; the :103/:186 emptiness
   reads join m_pendingLock coverage; U24 gains the
   last-ticket-retired-off-carrier-while-main-parked shell arm.
   §E.4(b)'s retirement now explicitly fires this wake.

6+8. MAJOR x2 (same defect, two filings) - §A.2.6 kept the VMTraps
   syncWaiter wake "GIL-on-only" while §C.6/SD6 deletes its target in
   BOTH modes. VERIFIED: VMTraps.cpp:329 AND :419 notify
   vm.syncWaiter()->condition(); the landed GIL-on park (waitForSync,
   WaiterListManager.cpp:83-108) blocks in waitUntil and re-checks
   hasTerminationRequest only on wakeups - with per-wait nodes in both
   modes the :329/:419 notifies target a node nobody parks on, making
   GIL-on sync TA waits termination-immune (breaks the U19 oracle +
   U2 in the bisection-baseline mode). FIX (§A.2.6 rewritten + §C.6
   cross-ref): both wakes DELETED with the syncWaiter; per-wait TA
   nodes and §C.3 property sync parks poll termination in D9 10ms
   quanta in BOTH modes (GIL-on: landed VM-wide predicate form;
   GIL-off: CURRENT lite's bit); U2 declared both-mode; U19 gains a
   GIL-on terminate-parked-TA-waiter arm; SD6 entry notes the
   both-mode D9-quanta park + corpus edit.

7. MAJOR - LLInt gate byte wrong + flag-on+GIL-on unassigned in the
   §A.1.3 case split. VERIFIED: the only landed asm gate tests
   OptionsStorage::useJSThreads (LowLevelInterpreter64.asm:1617)
   inside Config::options (JSCConfig.h:104,:107); rev-4's bullets read
   "Flag-off AND GIL-on" vs "GIL-off", leaving flag-on+GIL-on (phase-1
   production AND the U19 oracle) assigned to neither - and a
   useJSThreads-only LLInt branch would route it to per-lite storage
   while J.5/J.6 GIL machinery keeps VM members authoritative
   (split-brain). FIX: bullets relabeled "GIL-on (flag-on OR
   flag-off): VM storage" vs "GIL-off"; LLInt branches on a NEW
   derived gilOff byte in Config::options written at option
   finalization as useJSThreads() && !useThreadGIL(); jit R1.e
   re-pointed; OptionsList/§IM rows note the byte; U-T3/U-T4 updated.

9. BLOCKER - lazy carrier lifetime vs frozen vmstate M6 ~VM sequence.
   VERIFIED: vmstate:486-490 (M6) runs uninstall -> §6.5.1
   no-other-lite assert -> unregister -> destroy m_mainVMLite, with
   I20 "TLS never dangles"; §A.3.6's per-(thread,VM) registered
   carriers make the assert false at ~VM GIL-off, and no supersession
   was recorded (master-rule violation); force-destroying foreign
   carriers without invalidation leaves TLS maps keyed by VM*
   vulnerable to address reuse (UAF on a later VM at the same
   address). FIX (§A.3.6 supersession, both sides cited): ~VM first
   walks the VMLiteRegistry for this VM under its lock; foreign
   carriers must be token-free (RELEASE_ASSERT - entered-elsewhere
   ~VM stays an embedder contract violation, as today) and are
   DCT'd/destroyed/unregistered in the walk; §6.5.1 becomes "registry
   empty for this VM"; VMs carry a process-monotonic 64-bit epoch and
   the TLS map stores {VM*, epoch, carrier} - lock() compares epochs
   BEFORE touching the cached carrier, so a recycled VM* builds a
   fresh carrier and the stale pointer is never dereferenced; I20
   preserved per-thread (a destroyed carrier was token-free, hence
   not CURRENT anywhere). New U27 + teardown-storm test; B.2 teardown
   text updated to reference the walk.

### Rev-5 editorial (byte cap)

To stay under 50000 bytes with ~4.5KB of new normative content, rev 5:
- introduced the citation shorthand vmstate:/api:/om:/jit:/heap: for
  the five frozen SPEC files and IU for INTEGRATE-ungil (legend in the
  header); source-file citations remain full;
- removed markdown bold/backtick markup and reduced list-continuation
  indentation (content unchanged);
- compressed prose throughout; relocated rationale here. Notably:
  - C.3: the full I10 interleaving and the WaiterListManager
    waitSyncImpl analogy (above, item 1);
  - E.3: why asyncHold/cond.asyncWait/property-waitAsync KEEP
    keepalive while asyncJoin does not (item 3);
  - A.2.6: why a GIL-on-only central wake cannot survive SD6 (item
    6+8);
  - A.1.6: the concurrent catch-OSR staging corruption scenario
    (item 4);
  - A.3.6: the VM*-reuse UAF scenario motivating the epoch (item 9);
  - K: NumericStrings/stringSplitIndice/LazyProperty evidence lines
    and the RegExpCache partial refutation (RegExpCache.h:79 +
    RegExpCache.cpp:43-100 lockers) (item 2).

================================================================
## Rev 6 (2026-06-05) - resolves 9 reviewer findings (8 unique; one
duplicate pair), all verified REAL against the tree + frozen specs.
No refutations this round. Full arguments + material compressed out
of the spec to stay under the 50000-byte cap.

### F1 (blocker) E.3/E.4 keepalive underflow on non-counted tickets
Verified: spec rev-5 enumerated the INCREMENT set ("every AsyncTicket
whose registrant is a spawned TS EXCEPT asyncJoin") but rules 1-2's
decrement was gated only on winning the m_keepaliveReleased CAS + the
inbox-open check; nothing initialized the flag released for asyncJoin
tickets (the cited analogue m_settled, ThreadManager.cpp:78-81 /
ThreadManager.h:151, constructs false). Concrete failure: mutual
asyncJoin A<->B with both inboxes OPEN - A closes, F5 settles B's
ticket via E.4 (registrant B, inbox open) -> CAS wins -> rule-1
decrement on a counter B never incremented -> uint64 wraps ->
keepaliveCount==0 never true -> B never closes; SD12's "never
deadlocks" claim was violated by the spec's own mechanics, as was U8.
FIX (normative, §E.3): m_keepaliveReleased is CONSTRUCTED true
(released = safe default); the INCREMENT site ALONE stores false
(armed) before the ticket is visible; rules 1-2 decrement only on
winning the false->true CAS, so never-armed tickets (asyncJoin, TA
waitAsync, main/embedder, any future non-counted registration) can
never decrement. Exactly-once preserved: arm happens-before
visibility, and the CAS is still single-winner. U8 gains the
mutual-asyncJoin-with-OPEN-inboxes corpus arm (the prior mutual/self
variants only covered the closed-registrant path).

### F2+F6 (blocker+major, duplicate) §K.3 lazy-init losers spin
holding heap access polling only §A.3 stop bits
Verified: §A.3 stops and GC stops are disjoint mechanisms - jit R1.h
(SPEC-jit.md:231 "GC does NOT share this") and heap Dev 8/§10
(per-CLIENT access-state barrier); §A.3.2b itself states a JSThreads
stop sets NO client-visible GC stop state. A loser spinning WITH
access while polling only the §A.3 bit never observes a shared-GC
stop request; if the winner's initializer allocates and triggers a
collection, the GC waits on the access-holding loser, the loser waits
on the winner's release-store, the winner is blocked in allocation -
three-way deadlock on a mainline path (two threads first-touch one
JSGlobalObject initLater under allocation pressure). FIX (§K.3):
losers wait PARK-CAPABLE in bounded quanta - release heap access (E.2
ordering, no lock held), poll BOTH stop families (lite §A.3 stop bit
AND heap §10 per-client GC stop state via stopIfNecessary),
re-acquire via the §A.3.2b-gated path, re-test the load-acquire. U26
gains a forced-full-GC-during-winner-initializer arm (liveness, which
the old TSAN one-init check could not catch).

### F3 (major) syncWaiter deletion was flag-off-reachable
Verified: vm.syncWaiter() (VM.h:1174/:1376), the VMTraps.cpp:329/:419
termination wakes, and the waitForSync park (WaiterListManager.cpp:
83-108, full-deadline wait re-checking termination only on wakeups)
are upstream machinery used by EVERY sync TA Atomics.wait incl.
useJSThreads=0 vanilla SAB agents. Rev-5's "deleted, both modes"
language (where "both modes" = both GIL modes under useJSThreads=1,
line 13) left the flag-off arm with its wake mechanism deleted -
verbatim implementation either hangs flag-off
terminate-during-infinite-wait or silently converts flag-off to D9
quanta (an unlicensed flag-off behavioral delta; D9's
jsThreadParkTerminationRequested predicate is JSThreads-only
machinery, LockObject.h:144-156). FIX (§A.2.6): wakes are BYPASSED
under useJSThreads (not deleted); explicit flag-off disposition -
syncWaiter + :329/:419 wakes + landed park stay compiled AND live;
atomicsWaitImpl branches on useJSThreads; the D9 predicate is never
consulted flag-off; "both modes" scoping rule stated in-doc; §T
flag-off golden gates gain terminate-during-infinite-TA-wait. §C.6
re-pointed ("central wakes bypassed flag-on; flag-off keeps them").
Note the D8 single-flight gate (AtomicsObject.cpp:500-511) is itself
on the flag-gated path (per its own comment), so deleting IT both GIL
modes remains sound.

### F4 (major) §E.7.3 hook routing incomplete
Verified: landed scheduleWorkSoon dispatches to onScheduleWorkSoon
UNCONDITIONALLY (DeferredWorkTimer.cpp:232-238) and cancelPendingWork
to onCancelPendingWork unconditionally (:266-269). Rev-5 stated the
hookManaged re-dispatch only for the OFF-carrier handoff queue and
the hookManaged-only rule only for onCancelPendingWork - an
ON-carrier settle/cancel of a spawned-registered (internal) ticket
(E.4 dead=>main fallback on the main carrier; main-side E.4(b)
retirement) would hand Bun a Ticket it never saw via onAddPendingWork
(suppressed for spawned registrants) - lost settle or crash. Also
internal entries re-dispatched to m_tasks depend on DWT's run-loop
timer, which a hook-installing embedder (who owns scheduling) does
not pump - dead-thread-fallback settles would strand;
onCrossThreadWorkEnqueued only drove the flush, which only re-queued.
FIX (§E.7.3): (a) EVERY hook dispatch site consults hookManaged
BEFORE the installed-hook branch - internal tickets take the internal
arm regardless of calling thread; (b) with hooks installed,
internal-arm scheduleWorkSoon entries are NOT timer-scheduled - the
handoff flush + every §F.1 drain point EXECUTE them inline on the
carrier under its token (incl. E.4(b) retire + m_promise clear), so
the wake hook drives them to COMPLETION; (c) U24 Bun arm:
spawned-registered ticket, registrant dead, hooks installed, settle
completes.

### F5 (blocker) GC rooting of GIL-off per-lite Group-3 cells
Verified: heap/Heap.cpp:3585 roots vm.exception()/vm.lastException()
through the VM accessors inside the ConservativeScan/VMExceptions
constraint. Post-§A.1.3 rerouting those accessors resolve through the
CURRENT lite - on a GC visit thread that is no lite or the
conductor's own - so N-1 entered threads' pending
Exception/lastException cells (and cell-holding lazy regexp buffers)
were never rooted: premature collection + UAF on the throw path.
Rev-5 defined registry-walk scanning only for §A.1.6 scratch buffers
and §K.1 cache copies; U-T1's "GC root walk" was a dangling
cross-reference. FIX (§A.1.3 "GC roots", normative): the shared
collection's root/handle visit phase iterates the VMLiteRegistry
under its lock and appends EVERY registered lite's cell fields
(m_exception, m_lastException, scope-verification cells, lazy regexp
match buffers); registry stable because mutators are quiesced by the
heap §10 stop. §IM heap/Heap.* row now lists "A.1.3 root walk
(:3585-class sites)"; U-T1 amplifier: thrower parked pre-catch
survives a forced full collection. (vm.m_terminationException at
Heap.cpp:3592 stays VM-global - the termination exception is
per-VM/singleton, not per-thread Group-3 state.)

### F7 (blocker) "jit R1.e re-points at the new byte" - unsound
reading + missing two-sided supersession
Verified: R1.e (=M4a) is the SPEC-jit gate-byte clause (jit:230/:251)
and the landed ifJSThreadsBranch macro
(LowLevelInterpreter64.asm:1615-1618) gates the §5.4 LLInt cache
disables and §5.5 TID/SW butterfly choke points (:1620+). Reading
rev-5's clause as "R1.e's byte becomes gilOff" would, under
flag-on+GIL-on (phase-1 production / U19 oracle, where gilOff=false),
disable TID/SW dispatch and re-enable the disabled LLInt caches while
butterfly TID tags + segmented spines are still installed - LLInt
would treat a segmented spine payload as a flat butterfly pointer.
FIX (§A.1.3): rewritten - gilOff is a SECOND derived byte ADDED
BESIDE R1.e's useJSThreads byte; R1.e's byte and ALL its landed
ifJSThreadsBranch consumers are UNCHANGED (run whenever
useJSThreads=1, both GIL modes); ONLY the NEW Group-3
storage-selection branches test gilOff. Recorded as an EXTENSION of
jit R1.e with both sides cited (jit:230/:251 M4a vs the clause).

### F8 (major) heap Dev-7 GC-throughput items lacked a disposition
Verified: SPEC-heap.md:26 charters, as WS gating GIL removal, not
only TLC-aware inline emission but also "per-directory handout +
out-of-lock sweep, concurrent marking/incremental sweep" (the Dev-4
carve-outs, heap:23); api:26 composes "heap Dev 7 (incl. per-THREAD
TLC addressing)" into the milestone gate. Rev-5 closed only the TLC
item. FIX (§B.6, SUPERSESSION, both sides heap:26 + api:26 vs §B.6):
the GC-throughput items are DEFERRED to a post-ungil perf milestone;
GIL-off ships on the synchronous conductor-driven heap §10 protocol +
single-MSPL slow path (correctness-complete - the Dev-4 disables are
perf modes only, heap:23); replacement gate = §B.5 (<=10% 1-thread
composite, N-thread scaling recorded); a §B.5 miss pulls the deferred
items forward pre-ship; INTEGRATE-heap records the override.

### F9 (major) §A.1.6 silently displaced vmstate §6.6's frozen
Phase-B scratch contract
Verified: SPEC-vmstate.md:534-539 freezes "Group 5 + ScratchBuffer*
VMLite::scratchBufferForSize(size_t)"; VMLite.h:168-174 carries the
inert Group 5 (scratchBufferLock + Vector<ScratchBuffer*>). Rev-5's
registry+segmented-table design neither cited §6.6 nor disposed of
Group 5/the reserved signature - undirected reconciliation at
U-T1/U-T4. FIX (§A.1.6, re-freeze recorded with both sides
vmstate:534-539 vs the clause): VMLite::scratchBufferForSize(size_t)
IS implemented - over the segmented table by size-class index (the
non-baked arm); Group 5 is REPURPOSED, not dead: the lite's
buffer-ownership list (every buffer installed into this lite's table
appended under scratchBufferLock), backing the jit-R2 registry GC
scan and teardown free. Frozen L1-L5 layout untouched (L4 sanctions
accessor-implementation changes; the list reuses the declared fields
as-is).

### Editorial compressions (content relocated here, normative rules
unchanged in the spec)
- §E.6 Park/unpark merged into §E.2's tail (same rules: access
  released before park; wakeups = task append, stop, termination,
  10ms quantum).
- §J and §LK converted from tables to lists; J.2/J.4/J.5 merged into
  one disposition row. J.7's "tid-0 never installed" folded into U1.
- §E.1b.2 promise-protocol prose tightened; full step list as in rev
  5: performPromiseThen pre-switch status() read is ADVISORY;
  allocation of JSPromiseReaction + Bun InternalFieldTuple context
  (JSPromise.cpp:346-359) strictly outside the cell lock; under the
  lock re-check status, re-read reactionHead, fix next, publish via
  setPackedCell; settled => allocation dropped and the reaction
  enqueued via queueMicrotask after unlock; one uncontended cell-lock
  per op.
- §K.3 deadlock chain abbreviated (full chain: GC waits on the
  access-holding loser; loser waits on the winner's release-store;
  winner blocked in allocation behind the GC).
- U9 mechanics long form (rev 5): increment happens-before any settle
  of the same registration; decrement + append atomic under
  inboxLock; E.2's exit check reads keepalive + emptiness under the
  same lock so no exit can interleave between a decrement and its
  append; decrementer signals runLoopCondition before unlocking.
- §D.1 enumeration/restamp bullets merged; SD "was" clauses
  shortened; INV/T re-tersed; IM rows merged (ThreadObject+
  ThreadManager; ThreadAtomics+AtomicsObject; VMLite+VMTraps); §I
  warm-path rationale shortened (full form rev 5).
- "this spec supplies §§A-K" replaces the chartered-gaps phrasing
  (§K was always in scope as the NEW cache/lazy-init class).

## Rev 7 (2026-06-06) - resolves 4 findings (1 blocker, 3 majors), all
## verified REAL against the tree; no refutations

### F1 (BLOCKER) - builtin cell-internal mutable state beyond JSPromise
Verified: OM SPEC/annex, api/heap/vmstate/jit SPECs and rev-6
SPEC-ungil contain NO ruling for JSMap/JSSet/JSWeakMap storage, rope
resolution/atomization, DateInstance cache, FunctionRareData,
non-promise JSInternalFieldObjectImpl types, or ArrayBuffer detach
(grep across all six docs). OM §9.5/I21 scope = structure/butterfly
property slots; §K.4's audit scope = VM/JSGlobalObject members;
§E.1b's U-T9 audit sentence was promise-scoped. THREAD.md:200 ("no
race should ever lead to a VM crash") and THREAD.md:470 (layout-lock
discussion explicitly naming SparseArrayValueMap and Map/WeakMap/Set
as "too complicated for segmentation" but lockable) show the class
was known and is in-charter. Tree evidence: JSMap is
JSOrderedHashTable-backed (runtime/JSMap.h:32) - set() rehash splices
storage, so a concurrent reader UAFs; JSRopeString::resolveRope*
(JSString.h:637-682) writes fibers/flags in place;
DateInstance::gregorianDateTime caches a multi-word GregorianDateTime
(DateInstance.h:62-75); JSFunction::ensureRareData
(JSFunction.h:136-144) lazily materializes; ArrayBuffer::detach
(ArrayBuffer.h:199/:298) frees contents while racing accesses may
hold the base pointer; DFG inlines MapHash/MapGet/MapSet/WeakMapGet
(DFGNodeType.h:608-629). Fix: NEW §N - (a) defines the class
(multi-word cell-internal state outside §9.5/§K), (b) default
protocol = JSCellLock 10a in the §E.1b allocate-outside/
re-validate-under shape, (c) named rulings for all seven known
members, (d) exhaustive audit U-T8c gating U-T9, U28 invariant +
corpus arms (map.set storm, rope read/atomize race, generator
double-resume, detach-vs-read, Date storm). Design choices:
- Map/Set/WeakMap: cell lock on READS too - JSOrderedHashTable
  rehash/delete splices the backing store, so lock-free probes can
  UAF; DFG/FTL intrinsics disabled GIL-off (call locked native
  bodies); revival is a post-ungil perf item, mirroring §B.6's
  deferral shape.
- Ropes: cell lock REJECTED - strings are read-hot and resolution is
  idempotent; single-flight publish via one release-CAS of the
  fiber0/flags word is sound because losers' buffers are garbage
  (discarded) and readers already branch on isRope before touching
  fibers; resolveRopeToAtomString reuses the shared sharded atom
  table (U0), whose insert is already concurrent. This is also what
  makes §C.3's "resolve then restart" arm safe to run after dequeue.
- DateInstance: the cached GregorianDateTime pair is >8 bytes and
  not CASable; per-call caller-local computation GIL-off is strictly
  correct and only costs the cache win under sharing; m_data
  allocation itself CAS-publishes. vm.dateCache is VM-resident =>
  §K's audit covers it (class 1 or 2).
- FunctionRareData: materialization is exactly the §K.3 lazy-publish
  shape; allocation-profile internals (structure caches,
  watchpoint-bearing fields) mutate under the function's cell lock;
  pure profiling counters fall under jit in-scope item 7 (racy
  profiling tolerated); any cached Structure consumed under I34
  re-validation.
- Generators/iterator helpers: concurrent resume maps onto the
  EXISTING serial "already running" TypeError - the state-field
  re-check just moves under the cell lock, so the race becomes the
  same error a re-entrant serial program sees. Not an SD.
- ArrayBuffer detach: ordering length=0 (seq_cst) before base-clear
  makes racing bounds checks fail safe (length is checked first on
  all access paths); the contents-free is quarantined to the next
  heap §10 stop, the same epoch shape OM §6 uses for deleted
  property offsets - no read-after-detach UAF without adding any
  fast-path cost. Wasm memories are out of scope per §I (wasm
  refused on spawned threads in v1).

### F2 (major) - §J.3 degraded GILDroppedSection kept m_lock held
Verified: GIL-on park sites route JSLock::unlockAllForThreadParking
(JSLock.cpp:389-408), which fully releases m_lock (drain suppressed
via the m_lockDropDepth bump, per the in-tree comment at :393-399).
Rev-6 §J.3 degraded the section GIL-off to "access release + §A.3
park cooperation" only, and §G.3's "parks holding only its token"
never said m_lock is dropped - while §F.1 takes the main/embedder
token BY acquiring m_lock. So a main/embedder thread parked in
join()/cond.wait/property-or-TA Atomics.wait would hold m_lock for
the whole wait: a regression vs GIL-on and an outright deadlock for
the Bun pattern where the notifying store runs on a second embedder
thread that must first enter the VM through JSLock::lock(). Fix:
§J.3 now rules main/embedder park sites MUST release m_lock + token
via the unlockAllForThreadParking shape (drain suppression kept;
GIL-on-only extras skipped per §F.1/§A.3.7/§B.3) and re-acquire on
EVERY wake, re-running the §A.3.6 carrier/tag swap + §F.1
service-word OR before re-checking the wait condition (re-check is
mandatory: the condition may have changed while unlocked). §G.3
re-pointed at §J.3; §F.1 cross-references it; U15 + a C-API corpus
arm (main parks in property Atomics.wait, second embedder thread
enters and notifies) added; U-T11 carries the implementation.
J.2's "dead GIL-off" list amended: unlockAllForThreadParking is NOT
dead - re-derived by J.3.

### F3 (major) - §C.3 rope arm leaked the enqueued waiter
Verified from rev-6 text: the mismatch arm dequeued; the rope arm
said "unlock, resolve, restart" with the Waiting node still on the
PropertyWaiterList. That stale node consumes one FIFO notify(o,k)
count - a genuine waiter loses its wakeup, exactly the I10
lost-notify class §C.3 exists to close (or, with node reuse, a
double-enqueue). Fix (one clause): the rope arm DEQUEUES before
dropping listLock, resolution runs via the §N.2 single-flight
protocol (may allocate - legal, no lock held), and the restart
re-runs from the probe with a FRESH enqueue. Soundness: after
dequeue+restart the waiter is indistinguishable from a first-time
arrival; the notifier-orders-through-listLock argument is unchanged.
The alternative (pre-resolve both comparands before enqueue) was
rejected: the expected value arrives as a JSValue from user code and
can be re-roped between resolution and enqueue only by GC-invisible
user action on OTHER strings; dequeue-first is strictly simpler and
covers the o[k] side too.

### F4 (major) - §I prologue check had no discriminator; TID!=0 wrong
Verified: ThreadManager::isJSThreadCurrent (ThreadManager.cpp:157)
is a WTF::ThreadSpecific - not loadable at a fixed offset from
generated code. The only TLS data generated code has are the
butterfly TID tag (jit App. R5) and the lite via loadVMLite; rev-6
enumerated no isSpawned L2 append, so the natural implementation is
"TID tag != 0" - correct GIL-on (m_mainVMLite tid 0, VMLite.h:160)
but WRONG GIL-off: §A.3.6 gives every main/embedder thread a lazy
carrier with a TM-allocated NONZERO TID, so the check would throw
TypeError on main/embedder wasm (which §I explicitly does NOT
refuse) while still PASSING U17's spawned-positive tests - a
silently shipped break of Bun main-thread wasm. Fix: §I now
normatively defines the discriminator - VMLite gains an L2-append
uint8_t isSpawned, =1 stored at spawned lite registration BEFORE
setCurrent (§B.1 ordering makes it visible before any wasm entry can
run on that thread); main/embedder carriers (incl. GIL-off lazy
carriers) keep 0; emitted check = loadVMLite -> null => fall through
(no lite = not spawned; e.g. compiler threads never execute JSToWasm
entries, but fall-through is also the safe polarity) -> load byte ->
branch-to-throw. The C++ ctor/API gates keep isJSThreadCurrent().
U17 gains the NEGATIVE arm (main/embedder wasm never throws, both
GIL modes) so the broken discriminator can no longer pass the gate;
U-T13 carries thunk+trampoline emission + both U17 arms. L2
legality: vmstate L2 permits appends after Group 6; the byte joins
the other §A appends (threadContext/traps/clientHeap/scratch table).

### Relocated normative annex - §IM full table (spec §IM points here)
Bare names = runtime/. IA/IJ/IV/IH/IO = INTEGRATE-{api,jit,vmstate,
heap,objectmodel}. file = sections (counterpart):
- JSLock.{h,cpp} = F, A.3.6-7, B.3, J.3 (IA D1/D11; IV)
- ThreadObject.cpp + ThreadManager.{h,cpp} = B.1-2, E.1-E.4, D.1 (IA D5)
- DeferredWorkTimer.{h,cpp} = E.4/E.7 (IA D5)
- LockObject/ConditionObject = J.3-5, C.5, C.7/D12 (IA D2/D9/D12)
- ThreadAtomics.cpp + AtomicsObject.cpp = C.2-6 (:613-621 del), G
  (IA D3/D4/D7/D8)
- JSPromise.* + JSGlobalObject = E.1/E.1b, K
- JSMap/JSSet/WeakMapImpl + JSString/JSRopeString + DateInstance +
  FunctionRareData + JSGenerator* + ArrayBuffer* = N (NEW rev 7)
- bytecode/JSThreadsSafepoint.cpp = A.3 stub swap, J.8 (IJ, IO)
- VMEntryScope.{h,cpp} = A.1.5, A.3.4 (IJ M7; IV)
- VM.{h,cpp} + NumericStrings/LazyProperty* = A.1, E.1, F.2, K (IV M4-M6)
- VMLite.* + VMTraps.* = A.1-2, B.4, I isSpawned - L2 appends only (IV)
- WaiterListManager.{h,cpp} = E.3, C.6 (IA D4)
- ConcurrentButterfly.h + Structure* = D.1 (IO)
- VMManager.{h,cpp} = A.3.1-4 (IJ R1; IH)
- heap/Heap.* + HandleSet.* = A.1.3 root walk (:3585-class sites),
  A.3.2b, D.1, F.3 (IH)
- llint/jit/dfg/ftl (+OSR-entry) = A.1 incl. non-baked, B.4, I
  isSpawned check (IJ; gilOff)
- WTF SymbolRegistry.* = H
- wasm/js/* = I (SD7)
- OptionsList.h = U0, J.1, gilOff (all)

### Rationale relocated from the spec body in rev-7 compression
- §A.1.3 gilOff-byte: testing useJSThreads instead of gilOff in the
  NEW Group-3 storage-selection branches would route flag-on+GIL-on
  (phase-1 production) to VMLite storage, breaking the U19 oracle.
- §A.2.6 flag-off cites: vm.syncWaiter() decls VM.h:1174/:1376.
- §E.3 never-armed rationale: without construct-true, an asyncJoin
  settle on an OPEN registrant would decrement a counter that was
  never incremented, wrapping the uint64 and hanging the §E.2 exit
  predicate (reads "keepalive == 0").
- §D.2: the verdict re-times because a docs-only round cannot run
  the construction bench; UNGIL-PLAN.md:250 binds this spec to
  record (not redesign) - the supersession only moves the gate.
- §B.6/§E supersession targets unchanged from rev 6 ("records it" =
  records the override).
- §J.3: "GIL-on releases it here" = GIL-on routes
  unlockAllForThreadParking at the same park sites, so holding
  m_lock GIL-off would be a strict regression.
- §I: "pass U17's positive arm and ship broken" - U17's spawned
  tests cannot distinguish TID!=0 from isSpawned; only the new
  negative arm can.

## Rev 8 (2026-06-06) - whole-design cross-check vs the COMPOSED
six-spec system: 13 findings (3 blockers, 10 majors), all upheld and
resolved in-spec. The five SPECs stay frozen; collisions recorded as
SUPERSESSIONS citing both sides.

1. (blocker) U20/E.2 "no access transition holding any api-rank
   lock" contradicted frozen api 5.9(e)'s sanctioned rank-4 shape
   (NLS::m_lock across GIL reacq, SPEC-api.md:271; landed
   LockObject.cpp:334-380). The U20-compliant alternative (hold
   access, block on m_lock) deadlocks GC: a ParkingLot-blocked
   waiter holding access never polls GSP/stop bits (heap §9
   RHA/AHA contract), cycle GC<-W.access, W<-m_lock, m_lock<-H,
   H<-GC-resume. FIX: §E.2 rank-4 exemption - block on NLS::m_lock
   only token+access-RELEASED, reacquire (gated) while holding it;
   acyclic because every m_lock waiter is access-released and no
   GC/§A.3 conductor acquires NLS::m_lock. U20 lints the order.
2. (major) §J.3 "m_lock+token reacquired on EVERY wake" was
   unimplementable vs kept api 5.4/5.6 loops (condition re-check
   under rank-3 locks; 5.9(e) forbids reacq holding rank 1-3). FIX:
   per-quantum wakes poll ONLY lock-free state (waiter atomic +
   lite trap/stop bits) under the rank-3 lock; full reacquisition
   exactly once at final exit after rank-3 release. U2's
   one-quantum bound re-derived from the lite-bit poll.
3. (major) No merged lock table; rev-7 §LK "Rank 3" collided with
   heap rank 3 (VMManager::m_worldLock); api 5.9 never anchored.
   FIX: §LK rewritten as the merged table - heap 1 < api 1 (TM) <
   api 2 (PWT/affinity) < api 3 group (queueLock/listLock/inboxLock/
   joinLock, mutually unnested) < heap 2-10b < leaves; NLS::m_lock
   = long-hold class outside heap 2-10/api 1-3; negative edges
   normative (no heap 2-9b holder takes api locks; no 10a/10b
   holder takes api<=3; conductors take no api lock; api 1-3
   holders never transition access). ACYCLICITY: every cross edge
   points inward (api->10a only via §C.3; api locks never wrap heap
   2-9b - verified by tree walk: no 10a holder takes listLock;
   notifiers order store-then-LL; api locks never wrap heap 2-9).
4. (major) §A.1.6 install nested ScratchBufferRegistry ->
   VMLiteRegistry::lock -> scratchBufferLock, three "leaves" two
   deep vs vmstate §6.5.1/§7 "no lock while held". FIX: §LK.6
   re-ranks VMLiteRegistry::lock outer-of-leaves with allowed inner
   set {scratchBufferLock, atomic ops, fastMalloc}; SBR outside it;
   SUPERSESSION vs vmstate §6.5.1/§7 recorded. ~VM walk now
   collect-unregister-release THEN client work (no GBL-rank
   transition under the registry lock).
5. (major) §A.3.6 ~VM walk DCT'd FOREIGN threads' GCClients,
   violating heap I4 "lifecycle on the using thread" and dangling
   the owner's §10A.1 TLS slot + machine-thread registration. FIX:
   remote detach SUPERSESSION (heap I4 + §10A.1, both sides): walk
   marks-dead + unregisters (client set + machineThreads); destroy
   deferred to owner TLS death (or owner already dead); heap §10A.1
   TLS slot becomes {client, epoch}, consults epoch-compare first.
6. (major) §C.1 third arm let a foreign atomic CAS/RMW hit an SW=0
   AS object under the cell lock, racing the owner's E2-elided
   UNLOCKED AS fast-path stores (jit §5.5 makes the lock sufficient
   only AFTER SW=1) and skipping OM §4.6's first-foreign-write
   fire+publish. FIX: AS pre-lock stage - SW=0 + foreign TID =>
   OM §4.6 per-event STW (fire PRECEDES lock, I10b), RESTART probe;
   only SW=1/owner enters the locked CAS/RMW. New U5/U28 amplifier:
   owner unlocked AS store storm vs foreign CAS, SW initially 0.
7. (major) Atom-shard + SymbolRegistry leaf locks are reachable
   under MSPL/BVL/9b via in-lock sweep destructors (~JSString ->
   last StringImpl deref -> removeDeadAtom / registered-symbol
   remove), which heap §6 leaf row + vmstate §7 forbade. FIX (the
   cheap arm, matches reality): §LK.8 destructor-leaf class,
   SUPERSESSION vs heap §6 leaf row + vmstate §7 (both sides);
   soundness = fastMalloc-only, acquire-nothing, never-wait
   (vmstate I7 extended to SymbolRegistry). No cycle existed; this
   legalizes the edge so rank-asserts encode it.
8. (blocker) GC-stop park leg for N threads in ONE VM had no owner
   (heap Dev 8 chartered "VMM trap delivery" to Phase B; §A.3 was
   JSThreads-only; landed notifyVMStop is a per-VM state machine
   that double-transitions/asserts with 2 same-VM observers; heap
   §13.5a-g hooks assumed one parked thread per VM). FIX: §A.3.8 -
   GC reason fans out per §A.2.3, per-thread tickets, notifyVMStop
   per-entered-thread (Mode keyed on all-parked/released), heap
   §13.5a/g re-ruled per currentThreadClient() with per-thread
   m_releasedByGCPark pairing; SUPERSESSION vs heap annex hook
   shape + VMManager.cpp recorded; IM row added (IH). New U29 +
   spawned-conductor amplifier.
9. (major) Main/embedder GIL-off entry lost heap-access + ACT after
   §B.3 deleted JSLock forwarding (silently missed stack roots).
   FIX: §F.1 main/embedder bullet now explicit - first entry
   creates carrier lite + GCClient (main reuses original), runs ACT
   (I4(b) addCurrentThread), every lock() runs gated AHA
   (idempotent, heap F8 step 0), depth-0 unlock releases; U27/U-T6
   negative arm: spawned-conductor GC scans an entered embedder's
   stack.
10. (blocker) Wasm-memory escape: §I refuses EXECUTION only; shared
    views over a main-created WebAssembly.Memory reach spawned
    threads as plain TA accesses; rev-7 §N.6 excluded wasm memories
    => grow/detach UAF. FIX: §N.6 now INCLUDES wasm-backed buffers
    - grow's BoundsChecking reallocation publishes length-first and
    quarantines the old BufferMemoryHandle base to the next heap
    §10 stop (epoch shape); detach likewise; racing old-base reads
    stale-but-safe, writes lost (SAB-admissible raciness). U28
    amplifier: spawned TA reader vs main grow storm. §I cross-ref
    fixed.
11. (major) §A.1.3 root/handle walk (and §A.1.6/§K.1 scans,
    §A.1.5/§A.2.3 fan-outs) iterated the process-global registry
    with no per-VM filter - cross-VM rooting/trap injection. FIX:
    all walks filter lite->vm == the target VM; two-VM amplifier
    arm added to U-T1.
12. (major) SD4 self-contradiction: §C.4 deleted the 4.5-1a gate
    unconditionally while the SD footnote kept SD4 per-variant.
    RESOLUTION (a): the AtomicsObject.cpp:613-621 deletion is
    gilOff-conditional; gate KEPT GIL-on; SD4 stays per-variant; NO
    third both-modes delta; master rule intact. (Alternative (b)
    both-modes rejected: would need a GIL-on spawned-park clause
    and a third corpus edit for no behavioral payoff.)
13. (major) §A.3.7 GIL-off atom routing broke vmstate §4.3's 14
    kept atomStringTable asserts ("None relaxed (ex-M5)" frozen).
    FIX: explicit SUPERSESSION - the 14 sites become "gilOff ?
    sharedAtomStringTableEnabled() : tables equal"; GIL-on/flag-off
    unchanged; IU row for Identifier.cpp:77, Completion.cpp:63-287,
    Heap.cpp:2348 et al.

### IM rev-8 delta rows (extends the rev-7 relocated table)
- VMManager.{h,cpp} + heap/Heap.* §13.5a-g hooks = A.3.8 per-thread
  GC parking (IH; IJ R1 cross-ref)
- wasm BufferMemoryHandle/BufferMemory.* = N.6 grow/detach
  quarantine (IH)
- Identifier.cpp + Completion.cpp + Heap.cpp atom-assert sites =
  A.3.7 supersession (IV)
- HandleSet/ScratchBufferRegistry/VMLiteRegistry rank rows = LK
  merged table (IV, IH)

### Relocated normative annex - §E.7.3 full hook mechanics (spec
points here)
Installed hooks onAddPendingWork/onScheduleWorkSoon/
onCancelPendingWork (DeferredWorkTimer.h:110-112) BYPASS
m_pendingTickets and run INLINE on the caller (landed dispatch
unconditional, DeferredWorkTimer.cpp:204/:234/:266-269). hookManaged
is set at addPendingWork iff hooks installed AND registrant
main/embedder; EVERY dispatch site (scheduleWorkSoon,
cancelPendingWork) checks it BEFORE the hook branch; internal
tickets stay internal on ANY calling thread, incl. on-carrier -
hooks never see a ticket that skipped onAddPendingWork. Off-carrier
settle/cancel appends {ticket, task-or-cancel} to the
m_pendingLock-guarded handoff queue; the embedder does NOT pump
DWT's timer, so internal-arm scheduleWorkSoon entries are NOT
timer-scheduled - the handoff flush + every §F.1 drain point EXECUTE
them inline on the carrier under its token (incl. E.4(b) retire +
m_promise clear); onCrossThreadWorkEnqueued (REQUIRED together with
the other three, boot-checked; never runs JS) drives them to
completion; fallback vm.runLoop().dispatch of the flush (else
parked-main settle deadlock; U-T9 hook arm).

### Rationale/cites compressed out of the spec body in rev 8
- §A.3.2b: "(i) carries soundness" because AHA/RHA brackets are
  unenumerable (heap §9 requires bracketing every indefinite
  block); (ii) post-wake polls are defense in depth.
- §A.3.8 cite anchors: notifyVMStop VMManager.cpp:404-590 (per-VM
  active counting, duplicate-dispatch atomic :321,
  RELEASE_ASSERT(m_targetVM==&vm) :218/:580); heap hooks
  gcWillParkInStopTheWorld/didResume :172-179/:435; GC-bit
  keep-parked 5b :354-363; re-check-while-parked 5g :510-557.
- §C.4 gate cite: AtomicsObject.cpp:613-621 isJSThreadCurrent() =>
  throwVMTypeError; ThreadAtomics.cpp:536-541 is the G11 embedder
  gate, kept and re-pointed.
- §E.2 rank-4 cites: releaseAccessSlow Heap.cpp:2580-2595; api
  5.9(e) one-permitted-shape SPEC-api.md:271; landed contended
  hold/cond.wait LockObject.cpp:334-380 (UNGIL-PLAN K5).
- §F.1 ACT cites: JSLock::didAcquireLock forwarding JSLock.cpp:
  159-164 (deleted GIL-off, §B.3); GCClient AHA idempotence heap F8
  step 0; machineThreads addCurrentThread heap I4(b)/I12.
- §I cites: WebAssemblyFunction.h:75-90/:101-106 warm IC dispatch;
  ThreadManager.cpp:157 C++ gate.
- Line-number cites dropped during rev-8 byte compression remain
  valid as of jarred/threads 2026-06-06; authoritative copies in
  the rev 5-7 entries above.

Byte budget: spec at 49996 bytes after compression; §E.7.3
mechanics, LK acyclicity proof, and per-finding rationale relocated
here. GIL-on observable behavior remains unchanged except SD6/SD7
(SD4 explicitly resolved per-variant, finding 12).
