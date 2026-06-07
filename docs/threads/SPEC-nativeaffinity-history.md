# SPEC-nativeaffinity — history + BINDING annexes

## PART A / PART B boundary (read this first)

Same structure as the SPEC-objectmodel-history family:

**PART A — BINDING NORMATIVE ANNEXES.** Full text of clauses the spec
body indexes (size-cap overflow). Implementers treat PART A exactly
like the spec; the spec body's index entries cite these by id.

**PART B — non-normative audit trail.** Revision log and review
rounds; new rounds append here ONLY.

---

# PART A — BINDING annexes

## ANNEX NL1 — NativeSerialLock protocol (BINDING; spec §3.2 index)
## (REWRITTEN rev 2 — round-1 findings R1.2/R1.8/R1.9; rev-1 text
## preserved in PART B round 1 for the record)

State: one per VM (the single `m_gilOff` VM, UNGIL-HANDOUT U0b/U0c).

    struct NativeSerialLock {
        // ONE word, WTF LockAlgorithm shape (R1.8): bits 0-15 =
        // owning lite's TID (0 = free), bit 16 = hasParkedBit.
        // Owner TIDs are nonzero for EVERY NL-eligible lite:
        // spawned lites carry TM-allocated TIDs (VMLite.h:211) and
        // GIL-off CARRIER lites carry TM-allocated nonzero carrier
        // TIDs (JSLock.cpp:350 carrier->tid = allocateCarrierTID();
        // :522 RELEASE_ASSERT(lite->tid) — tid 0 never installed
        // GIL-off). No carrier-special encoding is needed.
        Atomic<uint32_t> m_word;        // ALL transitions seq_cst
    };
    static constexpr uint32_t hasParkedBit = 1u << 16;
    // per-lite (L2 append, beside nativeLockEligible):
    //   uint32_t m_nativeLockDepth; // owned-thread-only writes

acquire(lite):

    1. if lite->m_nativeLockDepth { ++depth; return; }   // reentrant
    2. loop:
       a. STOP POLL FIRST (spec §3.2.2 / SPEC-ungil §A.3.2-2b,
          SPEC-ungil.md:216-218): if this lite's stop/trap bit is
          set, park on the lite's OWN NVS ticket per the standard
          §J.3 park protocol (tokens KEPT; heap access kept at §A.3
          JSThreads stops, but a rule-8 GC stop's NVS park performs
          the F8 MANDATORY revert + gated re-acquisition,
          SPEC-ungil.md:289-298 — legal with NL held, spec NA-I13
          rev-3 exemption; rev-2's "heap access KEPT, as at every
          §A.3 park" was wrong for GC stops, R2.6); on wake, run the
          §A.3.2b post-wake poll BEFORE continuing.
       b. w = m_word.load(); if (w & ownerMask) == 0 and
          m_word.compareExchangeWeak(w, lite->tid | (w & hasParkedBit)):
          CAS WON. Re-poll the stop bit (it may have been set since
          (a)); if set, park on the NVS ticket WHILE HOLDING NL —
          explicitly legal, no conductor ever acquires NL (spec
          NA-I10) — and run the §A.3.2b post-wake poll before
          proceeding. Then lite->m_nativeLockDepth = 1; return.
          INVARIANT (R1.2): acquire() NEVER returns without a stop
          poll after its final park of either kind; a waiter woken
          by the holder's mid-stop-window release cannot run the
          native body inside the window.
       c. CAS FAILED (held): ensure hasParkedBit is set
          (compareExchangeWeak w -> w | hasParkedBit; if the word
          went free meanwhile, goto a); then
          ParkingLot::parkConditionally(&m_word,
              validation = [w] { return m_word.load() ==
                  (w | hasParkedBit); },   // still held + flagged
              beforeSleep = nop,
              deadline = now + quantum).
          NOTE (R1.9): ParkingLot::compareAndPark takes NO timeout —
          it hard-codes Time::infinity() (wtf/ParkingLot.h:91-102);
          parkConditionally with an explicit deadline is the ONLY
          primitive that supports the bounded quantum, and the
          quantum is LOAD-BEARING (below).
       d. on wake — unpark OR deadline — goto a. (The stop poll in
          (a) therefore runs after EVERY wake, before any CAS.)

release(lite):   (REWRITTEN rev 3, R2.3 — the rev-2 exchange(0) +
                  plain unparkOne lost hasParkedBit for every waiter
                  beyond the first: the woken waiter's acquire CAS
                  re-installed `tid | (w & hasParkedBit)` over w==0,
                  so remaining waiters slept to the deadline — a
                  10ms-stepped convoy falsifying NA-T4's
                  unpark-bounded handoff arm. Rev-2 text of record
                  in PART B round 2.)

    1. ASSERT((m_word & ownerMask) == lite->tid
              && lite->m_nativeLockDepth);
    2. if --lite->m_nativeLockDepth return;
    3. w = m_word.load();
       if !(w & hasParkedBit) and m_word.compareExchangeWeak(w, 0):
           return;                       // fast path: no waiters
       // Slow path — the LockAlgorithm::unlockSlow handoff shape
       // (wtf/LockAlgorithmInlines.h:207-241: unparkOne WITH
       // callback; the callback runs holding the ParkingLot bucket
       // lock, which is what orders the bit republication against
       // concurrent step-(c) parks):
       ParkingLot::unparkOne(&m_word, [&](UnparkResult result) {
           m_word.store(result.mayHaveMoreThreads
                            ? hasParkedBit : 0);   // seq_cst
           return 0; // token unused
       });
       // The plain unparkOne(address) form DISCARDS
       // UnparkResult::mayHaveMoreThreads (wtf/ParkingLot.h:104-119;
       // the :130-135 comment notes WTF::Lock uses the callback
       // form for exactly this) and MUST NOT be used here.

Memory order / lost-wakeup (R1.8): rev 1 kept a SEPARATE relaxed
m_waiters count; on weakly-ordered hardware (ARM64, a shipping Bun
target) the releaser's relaxed load had no happens-before edge to a
waiter's relaxed increment, so a release could legitimately read a
stale 0 and skip unparkOne — a lost wakeup costing up to a full
quantum per miss. The parked indication now lives IN the owner word
and every transition (acquire CAS, parked-bit CAS, release exchange)
is a seq_cst RMW on m_word; ParkingLot's bucket lock orders the
parked-bit set + validation against the releaser's exchange (the
WTF::Lock hasParkedBit protocol — wtf/LockAlgorithm.h shape; the
rev-1 objection to WTF::Lock was ONLY its non-polling park, which
step (a) replaces). The m_word RMW chain is the synchronization edge
between successive critical sections; LIVENESS additionally depends
on (i) the parked bit sharing the released word and (ii) the bounded
deadline existing.

Quantum (R1.9, normative): the park deadline is bounded and MUST be
strictly less than the §A.3 stop-the-world watchdog interval
(`stopTheWorldWatchdogTimeout` = 30s,
bytecode/JSThreadsSafepoint.cpp:379) by a comfortable margin;
default 10ms. It is NOT a tunable whose value is correctness-free:
it bounds (1) lost-progress recovery if an unpark is missed for any
residual reason, and (2) the conductor-visibility window below.

Conductor-side (R1.9 — honest restatement; rev 1 claimed a
(c)-parked waiter "counts as parked at a poll site", but no §A.3.2
conductor mechanism samples foreign ParkingLot buckets): NL appears
in NO conductor code path (spec NA-I10). A waiter parked in (c) is
parked on the NL bucket, NOT on its NVS ticket, and is therefore
CONDUCTOR-INVISIBLE FOR AT MOST ONE QUANTUM: if the NL holder is
itself safepoint-stopped while holding NL (permitted, NA-I10/§3.2.3)
no release ever arrives, the deadline fires, the waiter re-runs (a),
observes the stop bit, and parks compliantly on its NVS ticket.
Worst-case added stop latency per NL waiter = one quantum +
scheduling; with the 10ms default this is invisible against the 30s
watchdog (NA-T4 bounds it).

Teardown: ~VM blocks until VM-empty (SPEC-ungil EXIT1.9); an empty VM
has no entered lites, hence m_word == 0 — destructor asserts it. A
thread exiting (EXIT1) with m_nativeLockDepth != 0 is a structural
bug: close (§E.2) asserts depth == 0 (drop scopes are stack-strict,
ANNEX EX1).

## ANNEX EX1 — re-entry drop scope (BINDING; spec §3.3 index)

    class NativeLockDropScope {  // also the public embedder type
    public:
        ALWAYS_INLINE NativeLockDropScope(VMLite* lite)
            : m_lite(lite), m_savedDepth(0)
        {
            // REV 4 (R3.11): level-0 gate FIRST. The call sites
            // (items 1-6 below) are COMMON-PATH C++ — they execute
            // identically in every configuration (rev 3's "the
            // call sites are themselves on gilOff-mode-split
            // paths" was false). Flag-off/GIL-on processes
            // therefore pay exactly one predictable global-byte
            // branch here and never reach the lite/depth loads
            // (NA-I1 as restated rev 4).
            if (!g_jscConfig.gilOffProcess) [[likely]] return;
            // dead-cheap when ineligible (GIL-on VMs' lites
            // coexisting in a gilOff process; rev 2: carriers of
            // the gilOff VM ARE eligible per revised NA-I9): one
            // byte test on already-resident lite state; depth==0
            // (the overwhelmingly common case) costs one more load.
            if (!lite || !lite->nativeLockEligible) [[likely]] return;
            if (uint32_t d = lite->m_nativeLockDepth) {
                m_savedDepth = d;
                lite->m_nativeLockDepth = 1;     // collapse
                nativeSerialLock(lite).release(lite); // to zero
            }
        }
        ~NativeLockDropScope()
        {
            if (!m_savedDepth) [[likely]] return;
            // Park-capable reacquire (ANNEX NL1 loop). Runs on EVERY
            // exit path, including exception-pending (spec NA-I12):
            // JSC host exceptions propagate by return value +
            // per-lite m_exception word, never by C++ unwinding
            // through host frames, so this destructor is reached on
            // the throw path too; it MUST NOT inspect or clear the
            // pending exception.
            nativeSerialLock(*m_lite).acquire(*m_lite);
            m_lite->m_nativeLockDepth = m_savedDepth;
        }
    private:
        VMLite* m_lite;
        uint32_t m_savedDepth;
    };

Mandatory instantiation sites (spec NA-I11 funnel; REVISED rev 3,
round-2 finding R2.2 — the rule is CALLEE-defined over the WHOLE
`vmEntryToJavaScript*` family, NORMATIVELY the prefix-matched
symbol set of the llint/LLIntThunks.h:39-52 extern declaration
block (bare + With0..With6Arguments today); rev 2's enumeration of
two exact symbols left With1..6 callers outside the funnel — the
R1.3 bug shape one level down. Lint-enforced by NA-T7 with a
GENERATED symbol list; the list below is the known caller set at
this rev, NOT the definition):
1. `Interpreter::executeCall` / `executeConstruct` / `executeEval` /
   `executeProgram` JS arms (the `vmEntryToJavaScript` callers
   adjacent to interpreter/Interpreter.cpp:1319/:1409's native arms).
2. `Interpreter::executeCachedCall`
   (interpreter/InterpreterInlines.h:100; direct vmEntryToJavaScript
   at :127). MISSED BY rev 1: CachedCall is the re-entry vehicle of
   exactly the natives that ship Locked.
3. `Interpreter::tryCallWithArguments` (InterpreterInlines.h:132-171,
   With0..With6) — ONE scope here covers all seven arity arms.
   MISSED BY rev 2 (it cited only the With0 arm at :158) and
   maximally load-bearing: `CachedCall::callWithArguments`
   (interpreter/CachedCallInlines.h:38-66) routes through it on
   CPU(ARM64)/CPU(X86_64) whenever argumentCountIncludingThis <= 7,
   i.e. the COMMON case on both shipping targets — Array.prototype.
   sort comparators enter via `cachedCall.callWithArguments(
   globalObject, jsUndefined(), left, right)`
   (runtime/ArrayPrototype.cpp:955 -> With2Arguments), NOT via
   executeCachedCall; the TypedArray forEach/map/filter/reduce/sort
   callback family and String.prototype.replace replacers ride the
   same symbols.
4. `MicrotaskCall::tryCallWithArguments`
   (interpreter/MicrotaskCallInlines.h:40-86, With0..With6) and the
   runJSMicrotask direct arms (runtime/JSMicrotask.cpp:159-171
   With0..6 + :198 bare). Also missed by rev 2's two-symbol list.
5. `Interpreter::executeModuleProgram` (interpreter/
   Interpreter.cpp:1662; entry at :1728) — reached from the
   moduleLoaderEvaluate host function and embedder (Bun) natives
   that evaluate modules. Missed by rev 1.
6. The per-lite microtask drain loop entry (SPEC-ungil §E.1/I11) —
   defensive; a native should never pump microtasks while holding NL,
   but the drop scope makes it correct anyway (item 4 covers the
   per-call entries regardless).
7. Embedder/manual: around blocking regions in host bodies (spec
   NA-I13).
8. The §F.5 nested foreign-VM entry funnel, at its F8
   mandatory-revert point, keyed on the THREAD's gilOff-VM lite —
   spec NA-I21 (rev 3, R2.1): inside the nested VM every §3.3 caller
   passes VM B's lites (nativeLockEligible 0 for a GIL-on B, and in
   any case a DIFFERENT lock), so no inner-VM scope can release VM
   A's NL; this is the one callee-defined site keyed on a lite OTHER
   than the entered VM's.

Strictness: scopes are stack-nested (LIFO) by construction (C++
automatic storage); a scope NEVER outlives its native frame. The
collapse-to-1-then-release in the ctor (rather than looping release)
keeps the NL1 owner-word traffic at exactly one CAS per drop
regardless of depth.

Interaction with §A.3 stops: the destructor's reacquire is an NL1
acquire and therefore a compliant park site; a stop arriving between
release (ctor) and reacquire (dtor) sees this thread parked inside JS
machinery as usual — no NL-specific conductor logic exists. rev 2:
the reacquire inherits the rewritten NL1 loop verbatim, including
the R1.2 post-wake/post-CAS stop-poll-before-return invariant (the
rev-1 wake-mid-stop hole applied to this reacquire too).

Interaction with termination (SPEC-ungil §A.2.4, VM-wide v1): a
termination trap observed during the dtor's reacquire parks/polls per
NL1 step (b) and then COMPLETES the reacquire (spec NA-I12); the
exception then unwinds the native frames above, each releasing
through the §4 brackets to depth 0 before the thread reaches its §E.2
close (which asserts depth 0, NL1 teardown rule).

## ANNEX PT1 — seed policy table (BINDING; spec §2.2 index)

Grouping = the unit a §5 audit row covers. "Registration" = where the
ConcurrentOk marker lands (lut marker or getHostFunction parameter,
spec §2.1). Every group below still requires its NA1 rows executed
before useJSThreads ships default-on (spec §5.2); this table is the
APPROVED CANDIDATE LIST, not a waiver of evidence.

PT1.A object/property hot core (runtime/ObjectPrototype.cpp,
runtime/ObjectConstructor.cpp):
  hasOwnProperty, propertyIsEnumerable, isPrototypeOf,
  Object.getPrototypeOf, Object.keys/values/entries fast paths,
  Object.is, Object.create(null|proto) fast path.
  Rationale: bodies are Structure/butterfly walks ruled by
  SPEC-objectmodel; enumeration caches are §K-ruled (K4 rows).

PT1.B array hot core (runtime/ArrayPrototype.cpp):
  push, pop, shift fast paths, indexOf/lastIndexOf/includes, join
  fast path (rope-safe), slice, fill, at, isArray.
  EXCLUDED from seed: sort (comparator JS re-entry + scratch
  buffers), species-creation-heavy paths (concat/splice/flat) until
  their NA1 rows separately argue the species lookup caches.

PT1.C string hot core (runtime/StringPrototype.cpp):
  charCodeAt/codePointAt/charAt/at, indexOf/includes/startsWith/
  endsWith, slice/substring, toLowerCase/toUpperCase ASCII-only
  paths, fromCharCode.
  EXCLUDED: every locale-sensitive variant (NA-I7), replace/match
  family (RegExp side ruled separately via K4 m_regExpGlobalData
  per-lite row AUD1.K2/SD19 — replace/match join the seed ONLY once
  their NA1 rows cite that machinery end-to-end).

PT1.D Math (runtime/MathObject.cpp): all of Math.*; Math.random's
  m_weakRandom is per-lite per K4 AUD1.K4/VIII.10.

PT1.E JSON (runtime/JSONObject.cpp): parse, stringify. Stringify's
  toJSON/replacer JS re-entry is governed by the core machinery
  (concurrent-ok natives may re-enter JS freely; the §3.3 drop rule
  is a LOCKED-path concern).

PT1.F Atomics (runtime/AtomicsObject.cpp): all of Atomics.*; the
  waiter list is process-global and already lock-disciplined.

PT1.G threads API (NEW rev 3, R2.5 — spec NA-I22): the SPEC-api
  Thread/Lock/Condition/ThreadLocal natives, INCLUDING the blocking
  gate set G11 (SPEC-api.md:15: join(), lock.hold(), cond.wait())
  and lock.asyncHold/cond.asyncWait/notify/spawn/postMessage.
  Rationale: their bodies are exactly the lock-disciplined NLS/NCS/
  TS state machine SPEC-api §5.9 (SPEC-api.md:260-275) already
  audited rank-by-rank; if they shipped Locked (the NA-I6 default),
  (a) a joiner would hold NL across an indefinite block while the
  joined thread's Locked natives need NL — mutator deadlock; (b)
  every contended hold/wait would block on NLS::m_lock holding NL —
  the §3.5 forbidden NL > NLS edge; (c) their
  release-access-before-blocking discipline (api 5.9(a1-a3)/(e))
  would trip the NA-I13 assert. NA1 rows (group G) still required
  before useJSThreads default-on, like every seed group.

LOCKED FOR EMPHASIS (not exhaustive — everything unlisted is locked
by NA-I6; rev 3: the threads-API natives are NOT in this bucket —
they are PT1.G above, per NA-I22): ALL Intl* (NA-I7); Date
locale/timezone paths (tz cache);
Function constructor / eval / indirect eval (parser+codegen world);
RegExp compile-heavy paths; console/inspector/debugger natives
(main-only family, SD13/SD14); $vm / test natives; Error.captureStackTrace
and stack-trace materialization; Proxy trap helpers; WeakRef/
FinalizationRegistry natives; Wasm natives (spawned-thread Wasm is
refused v1 anyway, SPEC-ungil §I).

## ANNEX BL1 — blocking, nesting, and the NL liveness scope
## (BINDING; spec §3.4/§3.5 index; NEW rev 3, R2.1/R2.4/R2.5/R2.6)

FULL text behind the spec body's compressed §3.4/§3.5 clauses.

BL1.1 NA-I13 assert scope. The VOLUNTARY access-transition paths
(§A.3.4-gated fresh acquisitions; explicit native-side
release/re-acquire) debug-assert `m_nativeLockDepth == 0`. EXEMPT:
the §J.3 park-site MANDATORY reverts — at a rule-8 GC stop
(SPEC-ungil.md:289-298) the NVS park performs the F8 revert
(§13.5a willPark / per-client m_releasedByGCPark) and the gated
re-acquisition, legally WITH NL held: the GC conductor never
acquires NL (NA-I10), so no deadlock; rev 2's unqualified assert
fired on every GC stop that caught any thread inside a Locked
native body at a poll site (R2.6).

BL1.2 NA-I21 cross-VM nesting (R2.1). Topology licensed by
UNGIL-HANDOUT §F.5 (:2262-2290): a Bun host function in gilOff VM A
on a CARRIER (only — spawned nesting RELEASE_ASSERTs, SPEC-ungil
§F.6(e), SPEC-ungil.md:668-670) enters VM B mid-body; under NA-I6
that host function defaults Locked and holds NL at that point. The
§F.5 funnel mandates the F8 revert of A's access BEFORE installing
B's carrier (:2276-2277) — the exact transition NA-I13 polices —
and inside B every §3.3 drop-scope caller passes B's lites
(`nativeLockEligible` computed from VM B; 0 if B is GIL-on, and in
any case a DIFFERENT VM's lock), so A's NL would stay held across
the ENTIRE nested window: unbounded foreign-VM JS with every Locked
native of every VM-A thread serialized behind it, and a
constructible no-conductor deadlock (B's JS Atomics.waits on a
notify owed by a Locked VM-A native on a spawned thread, which
blocks on NL held by the nesting carrier — NA-I10 is irrelevant to
mutator-vs-mutator cycles, no watchdog fires). THEREFORE: the §F.5
funnel, at its F8-revert point, instantiates `NativeLockDropScope`
keyed on the THREAD's gilOff-VM (VM A) lite — the one
callee-defined drop site keyed on a lite other than the entered
VM's — releasing A's NL for the nested window, reacquiring at the
LIFO restore. Backstop: RELEASE_ASSERT (not debug) that the gilOff
lite's `m_nativeLockDepth == 0` inside the nested window. Gate
§9.6: SUPERSESSION-PENDING with the SPEC-ungil/UNGIL-HANDOUT owner
(§F.5/§F.6(e)/A36C; the §F.6 IU embedder-checklist
"JSContext-inside-host-call" row gains the NL-depth obligation).

BL1.3 NA-I22 engine blocking natives (R2.5). G11 (SPEC-api.md:15:
join(), lock.hold(), cond.wait()) and the rest of the
Lock/Condition/Thread/ThreadLocal native family are seeded
ConcurrentOk as PT1.G. If they shipped Locked (NA-I6 default): (a)
a joiner would hold NL across an indefinite block while the joined
thread — which completes only at §E.2 close (SPEC-ungil U7/SD1) —
needs NL for any Locked native on its exit path: total mutator
deadlock; (b) contended lock.hold would block on NLS::m_lock
holding NL — the BL1.4 forbidden edge; (c) all three release heap
access before blocking (SPEC-api.md:267-271, 5.9(a1-a3)/(e)),
firing the NA-I13 assert on every contended call in debug builds.
Demotion rule: a G11 native may only be Locked with internal
`NativeLockDropScope`s around every blocking region and every
NLS::m_lock acquisition.

BL1.4 The long-hold cycle (R2.4). Forward direction (legal,
structural): NLS::m_lock > NL — lock.hold runs user JS holding
NLS::m_lock (SPEC-ungil §LK long-hold row, SPEC-ungil.md:902-907);
that JS may call Locked natives (acquiring NL), and the ANNEX EX1
dtor reacquires NL inside fn's epilogue while NLS::m_lock is held.
Reverse direction (forbidden, was constructible in rev 2): a
Locked lock.hold body blocking on NLS::m_lock while holding NL.
Cycle witness: T1 in Locked lock.hold holds NL, blocks on
NLS::m_lock; T2 holds NLS::m_lock running fn's user JS, which
calls any Locked native and blocks on NL. No stop in progress —
§3.2 polls never fire; NA-I10 (conductor exclusion) is irrelevant.
Pinned LK.1c addition: NLS::m_lock OUTER to NL; negative edge "no
NL holder blocks on NLS::m_lock or any G11 blocking primitive"
(discharged by PT1.G); SPEC-api §5.9 companion row permitting the
EX1-dtor-inside-fn-epilogue NL reacquire (the legal direction) and
forbidding the reverse — all inside the §9.1 SUPERSESSION-PENDING
scope.

BL1.5 Liveness scope (supersedes rev 2 §3.4's unscoped release-build
"never deadlock — by NA-I10 nobody the conductor needs is behind
NL", which was sound ONLY for conductor liveness). CONDUCTOR
liveness: NA-I10. MUTATOR liveness: NA-I11 (no JS under NL) +
NA-I21 (no nested window under NL) + NA-I22 (no G11 block under
NL) + BL1.4's negative edge (no NLS::m_lock block under NL); each
violation has a constructible deadlock above.

## ANNEX SC1 — rev-4 surface closures (BINDING; spec §2.6/§4.4/§6 index)
## (NEW rev 4 — round-3 findings R3.1/R3.3/R3.4/R3.6)

SC1.1 DOMJIT signature dispatch (NA-I16 member d; spec §4.4.1d
index). When a NativeExecutable carries a `DOMJIT::Signature`, the
DFG parser takes `callee.signatureFor(specializationKind)` ->
`handleDOMJITCall` immediately AFTER the handleIntrinsicCall arm
(dfg/DFGByteCodeParser.cpp:2106-2114; function at :5244, emits a
Call node carrying OpInfo(signature)) and the node lowers to a
DIRECT call of `signature->functionWithoutTypeCheck` with no thunk
and no bracket — SpeculativeJIT::compileCallDOM
(dfg/DFGSpeculativeJIT.cpp:11603) and FTL compileCallDOM
(ftl/FTLLowerDFGToB3.cpp:22055). Because CallDOM calls the
WithoutTypeCheck variant, the bypass ALSO skips the type check the
generic path performs. The dispatch keys on the signature, not an
Intrinsic, so rev 3's NA-T6 sets (handleIntrinsicCall intrinsics,
VM.cpp:1283 switch, classInfo list, the three custom-accessor node
kinds) could not see it, and the emitted call is not
HostFunctionPtrTag-tagged so no rev-3 NA-T7 family matched.
Signatures enter through the exact §2.1 registration funnels:
VM::getHostFunction's `const DOMJIT::Signature*` parameter
(runtime/VM.cpp:1429, declared VM.h:1082) and
JITThunks::hostFunctionStub's NativeDOMJITCode arm
(jit/JITThunks.cpp:275-276) — i.e. the embedder API invites it —
and the in-tree $vm natives register two signatures today
(tools/JSDollarVM.cpp:1479 DOMJITFunctionObjectSignature, :1542
DOMJITCheckJSCastObjectSignature), both PT1-LOCKED ("$vm / test
natives"). RULE (mirrors §4.4.1b): in gilOff configs
getHostFunction passes nullptr for the DOMJIT::Signature unless the
executable's policy is ConcurrentOk — this simultaneously kills the
NativeDOMJITCode jit-code arm and starves `signatureFor` so
handleDOMJITCall never fires (locally verifiable, like NA-I23's
parser-side choice). NA-I23's CallDOMGetter disposition does NOT
cover CallDOM. The two compileCallDOM lowering files are
NA-T7-exempt WITH the NA-T6 cross-reference. (Filed twice in round
3; deduplicated.)

SC1.2 Intrinsic GETTER inlining (NA-I16 members e/f; spec §4.4.1e/f
index). Two parallel pipelines execute NativeExecutable-backed
getter semantics with no call: (e) DFG
`ByteCodeParser::handleIntrinsicGetter`
(dfg/DFGByteCodeParser.cpp:5263, dispatched from the GetById path
at :6743 — two hundred lines from the :6647 custom-accessor status
site rev 3 cited) replaces the getter call with plain graph nodes
(DataViewByteLength family, typed-array length family, etc.); (f)
baseline/IC `IntrinsicGetterAccessCase`
(bytecode/IntrinsicGetterAccessCase.cpp:37-48), admitted by
`InlineCacheCompiler::canEmitIntrinsicGetter`
(bytecode/InlineCacheCompiler.cpp:4473, used at
bytecode/Repatch.cpp:692), compiled via
`InlineCacheCompiler::emitIntrinsicGetter` (InlineCacheCompiler.cpp
:3575 dispatch, :4536 definition) inlines the same semantics into
IC stubs. The getters are host functions registered with
intrinsics; none appear in ANNEX PT1, so they are Locked by NA-I6 —
yet their semantics would execute on N threads with no bracket and
(rev 3) no lint row. RULE: each member's inlined semantics must be
OM/heap-ruled member-by-member and the set lint-pinned by NA-T6
(lint source: the handleIntrinsicGetter switch cases + the
canEmitIntrinsicGetter set), OR intrinsic-getter inlining is
disabled in gilOff configs alongside the §4.4.1b suppression.

SC1.3 Raw methodTable/host-hook pointers (NA-I24; spec §6 index).
The `GlobalObjectMethodTable` family
(runtime/GlobalObjectMethodTable.h:58-71) consists of raw function
pointers invoked as direct member-pointer calls from VM internals:
`promiseRejectionTracker` at runtime/VM.cpp:2265 (the §F.1 carrier
drain) and :2304; `reportUncaughtExceptionAtEventLoop` at
runtime/MicrotaskQueue.cpp:66, runtime/DeferredWorkTimer.cpp:284,
runtime/ThreadManager.cpp:933. These calls mint no NativeExecutable
(so "Locked" is not even REPRESENTABLE — no §1.2 byte exists),
reach no §4 emitter (they are direct C++ calls, not
HostFunctionPtrTag/vmEntryToNative/trampoline sites), are not §2.6
accessor funnels, and match no NA-T7 token family. NORMATIVE
consequences: (1) they receive NO NL coverage — their N-mutator
safety rests SOLELY on the SPEC-ungil §E.1b.4 / U-T8e disposition
audit, and §0.4 says so; (2) anti-laundering (inverse of NA-I19): a
U-T8e INLINE disposition for a methodTable hook MUST NOT be granted
or justified on the strength of NL serialization, because none
exists for this family — rev 3's §6 sentences ("an
inline-disposition Locked hook runs under NL"; "a Locked
carrier-queued hook simply runs under NL") asserted serialization
with no mechanism and are superseded (text of record in PART B
round 3); (3) a hook implemented by REGISTERING a
NativeExecutable-backed host function (rather than installing a raw
pointer) is covered by the bit machinery normally; (4) a future
bracket at the `methodTable()->X(...)` call-expression funnels
(an NA-I20-style seventh dispatch surface) is a scope extension,
NA-X5, not v1.

SC1.4 gilOff IC custom-accessor suppression — full grounding
(NA-I20; spec §2.6 index). Tag/symbol enumeration: the C++ dispatch
funnels carry GetValueFuncPtrTag (runtime/PropertySlot.h:97),
PutValueFuncPtrTag (:100), CustomAccessorPtrTag
(runtime/PutPropertySlot.h:37); the IC emission arms additionally
carry, on the JITCage arms, `setupArguments<GetValueFuncWithPtr/
PutValueFuncWithPtr>` (tags GetValueFuncWithPtrPtrTag /
PutValueFuncWithPtrPtrTag, PropertySlot.h:98/:101; tag block
runtime/JSCPtrTag.h:50-58) + `callOperation<OperationPtrTag>
(vmEntryCustomGetter/Setter)` (symbols llint/LLIntThunks.h:41-42,
annotated LLIntThunks.cpp:66-71; call sites
bytecode/InlineCacheCompiler.cpp:3474-3496, :5556, :6051) — all
five tags + both symbols are NA-T7 token families (rev 4, R3.6).
Suppression point: AccessCase CREATION in bytecode/Repatch.cpp —
gilOff mode does not create the four kinds CustomValueGetter /
CustomAccessorGetter (kind chosen at :711-715 inside tryCacheGetBy,
:475) and CustomValueSetter / CustomAccessorSetter (:1251 inside
tryCachePutBy, :1040). Post-suppression IC state: the access joins
the slow-path-only set the same way unsupported kinds already do —
give-up, no repatch retry, hence no regeneration livelock.
InlineCacheCompiler therefore never sees the kinds in gilOff mode;
the existing :3462 gilOff arm (`emitPublishTopCallFrameForHostCall`
with the UNGIL §A.1.3 comment) becomes unreachable in gilOff mode
and is RETAINED byte-identical for GIL-on processes — disposition:
superseded by suppression, not ripped out.

## ANNEX TC1 — test charters, FULL text (BINDING; spec §8 index)
## (MOVED here rev 4 under the size cap; content normative,
## carried forward from rev 3 + rev-4 amendments)

- NA-T1 Serialization witness. A Locked test native (test-only
  registration, e.g. via the jsc shell's `$vm`/test natives)
  increments-checks-decrements an unsynchronized global; N spawned
  threads hammer it. Bit=0: zero observed overlap (NL serializes).
  Same body registered bit=1 under the amplifier: overlap observed
  (proves the bracket, not luck, provides the serialization).
  TSAN arm: bit=0 body is TSAN-clean BECAUSE of NL. Carrier arm
  (rev 2, NA-I9): the MAIN thread hammers the Locked witness while
  spawned threads do — zero overlap (carrier-vs-spawned
  serialization).
- NA-T2 Tier coverage matrix. The NA-T1 witness driven through each
  entry: LLInt-only (`--useJIT=false`), thunk (baseline), DFG/FTL
  hot loop (forced tier-up), C++ entry (API call from a spawned
  thread, §4.5), microtask job with a native callee
  (queueMicrotask(nativeFn), §4.5 rev 4, R3.2), InternalFunction
  `new` (NA-I8), and the construct kind via `new` over a host
  function (NA-I4). Each cell must serialize.
- NA-T3 Re-entry drop. A Locked native calls `toString` on an
  argument whose `toString` (a) blocks on a condition variable
  released only after a SECOND thread completes a different Locked
  native, and (b) throws after resume. (a) passes only if NL was
  dropped (NA-I11 — else deadlock-by-timeout); (b) verifies
  exception-path reacquisition (NA-I12: the outer native observes
  the pending exception with NL held; instrumented assert).
  CachedCall arm (rev 2, R1.3): the same shape driven through a
  Locked sort comparator (`Interpreter::executeCachedCall`,
  InterpreterInlines.h:100) and through module evaluation
  (`executeModuleProgram`, Interpreter.cpp:1662) — the rev-1 funnel
  missed both. Small-arity arm (rev 3, R2.2): the comparator arm
  runs on x86_64/ARM64 so `callWithArguments` ->
  `vmEntryToJavaScriptWith2Arguments` (NOT executeCachedCall) is
  the exercised path; plus a microtask-runner arm (With0..6).
- NA-T4 Conductor liveness. While one thread is parked WAITING on NL
  and another HOLDS NL inside a spinning host body, a third forces
  GC and an §A.3 stop (jettison path): both complete within the
  watchdog (NA-I10 + §3.2.2 compliant parking; arms the SPEC-ungil
  U4 litmus family against NL). Wake-mid-stop arm (rev 2, R1.2): the
  conductor initiates a stop while T2 is parked on the NL bucket and
  T1 holds NL; T1 releases inside the window (host-call return); T2
  MUST NOT execute the native body inside the window — instrumented
  assert that acquire() ran a stop poll after its final park.
  Lost-wakeup arm (rev 2, R1.8): release-vs-park race hammered on
  ARM64; NL handoff latency bounded by unpark, never by the park
  deadline expiring. Multi-waiter arm (rev 3, R2.3): THREE
  contenders; every handoff unpark-bounded — the third waiter wakes
  via unpark, not deadline (exercises NL1's callback-form release).
  GC-stop-with-NL arm (rev 3, R2.6): rule-8 GC stop lands while a
  thread holds NL at a poll site — no NA-I13 assert; F8 revert,
  re-acquisition and GC complete; the native resumes with NL held.
- NA-T11 Nesting + G11 liveness (rev 3, R2.1/R2.5). (a) Carrier in
  a Locked VM-A native enters VM B (§F.5); B's JS Atomics.waits on
  a notify owed by a Locked VM-A native on a spawned thread: must
  complete (NA-I21); assert depth 0 across the nested window.
  (b) `t.join()` while the joined thread's exit path runs Locked
  natives: completes (NA-I22). (c) Contended `lock.hold`/
  `cond.wait` in debug: no NA-I13 assert.
- NA-T5 Flip campaign template. Per NA1 row: amplifier-scheduled
  N-thread corpus hitting the candidate native from all tier cells
  (NA-T2 matrix), TSAN no-JIT + ASAN JIT configs, plus a thread-fuzz
  session whose profile includes the candidate; evidence ids land in
  the row (NA-I18).
- NA-T6 Inline-bypass lint (rev 2). Build-time/test-time
  cross-check over the §4.4.1 UNION: Intrinsics reachable from
  `handleIntrinsicCall`, cases of the `thunkGeneratorForIntrinsic`
  switch (VM.cpp:1283), and the DFGByteCodeParser constant-
  InternalFunction classInfo list — minus gilOff-disabled minus
  concurrent-ok == empty (NA-I16). Includes the
  BoundFunctionCallIntrinsic disposition. rev 3 (R2.8): verifies
  the gilOff parser builds NO CallCustomAccessorGetter/Setter/
  CallDOMGetter nodes (NA-I23). rev 4 (R3.3/R3.4): + the
  signature-bearing-executable set (greppable: DOMJIT::Signature
  constructions + getHostFunction calls with non-null signature) —
  asserts gilOff builds NO Call-with-signature/CallDOM nodes — and
  the intrinsic-getter sets (handleIntrinsicGetter switch +
  canEmitIntrinsicGetter), each member ruled or disabled.
- NA-T7 Surface-closure lint (rev 4). Source scan: every site in
  the EIGHT token families of §4.6 (HostFunctionPtrTag,
  cloopCallNative, vmEntryToNative callers, the three accessor
  tags, the accessor call-expression tokens, the
  vmEntryCustomGetter/Setter symbol callers, the two WithPtr tags)
  is bracketed/exempt-cited (NA-I17, NA-I20, NA-I23); every caller
  of the `vmEntryToJavaScript*` family instantiates the NA-I11 drop
  scope. SELF-TEST (R2.2): the JS-entry symbol list is generated
  from the LLIntThunks.h:39-52 block at lint runtime; a symbol
  absent from the previous snapshot FAILS the lint without a spec
  rev (a new With7 forces a conscious update, never silent escape).
- NA-T8 Policy-conflict assert (rev 2). Registering one function
  pointer twice with conflicting NativeConcurrency RELEASE_ASSERTs
  (NA-I5) via the strong side table; covers the JITThunks path, the
  no-JIT path, AND the collected-then-re-registered sequence (force
  GC of the first executable between registrations — the assert
  must still fire).
- NA-T9 Mode-cost oracle (RECHARTERED rev 4, R3.10 — rev 3's
  whole-binary byte-compare was unsatisfiable as written: §4.2 adds
  instructions to the statically-assembled shared trampoline, and
  SPEC-ungil §J is a machinery-end-state discipline, not a
  byte-compare). (a) Byte-compare the GIL-on-generated per-VM thunk
  against a pre-change build; (b) arm-level LLInt diff: new
  trampoline bytes reachable ONLY via branchIfGilOffGroup3* arms
  (jit R1.e executed-path discipline, UNGIL-HANDOUT:172-179);
  (c) non-GILOFF builds byte-identical outright — NA-I1 as
  restated.
- NA-T10 U-T8e non-interference (rev 2). The carrier-queued
  promiseRejectionTracker corpus (SPEC-ungil §E.1b.4) runs unchanged
  with the bit machinery active; carrier NL acquisitions occur ONLY
  for Locked NativeExecutable-backed bodies and NEVER for the
  raw-pointer hook invocations themselves (rev 4, NA-I24 — rev 3
  implied the queued tracker runs under NL; superseded), and
  ordering/identity of the queued hooks is unchanged. (rev 1
  asserted ZERO carrier NL acquisitions; superseded with NA-I9.)

## ANNEX AT1 — NA1 audit row template (BINDING; spec §5.1 index)

File: docs/threads/SPEC-nativeaffinity-audit-NA1.md, style of
SPEC-ungil-audit-K4.md (status header stating the tree/branch the
audit executed against; classification key; rows consumed verbatim;
row ids NA1.<group>.<n>, groups mirror PT1 letters + E for embedder).

Row fields (all REQUIRED for a Locked->ConcurrentOk flip; "n/a" must
be argued, not bare):

| field | content |
|---|---|
| natives | symbol(s) + registration site file:line (one row may cover a PT1 sub-group that shares a body pattern) |
| kinds | call / construct / both (spec NA-I4: construct column mandatory even when it is callHostFunctionAsConstructor — say so) |
| shared state | every VM-/global-/process-resident datum touched -> ruling id (K4.x.y / N7-xx / NA1 row / OM-heap § cite); "none" allowed only with the grep recipe used to establish it |
| cell state | §N/OM rulings for cell-internal touches |
| JS re-entry | every re-entry site in the body (coercions, callbacks); for ConcurrentOk these need no NL note, but the column forces the auditor to FIND them |
| ICU | Intl gate (spec NA-I7): every ICU API touched + the per-API thread-safety argument; non-Intl rows: "none" |
| TSAN | run id, config (TSAN no-JIT target per docs/threads/TSAN.md), corpus, amplifier schedule, result (zero races attributable) |
| fuzzer | campaign id, profile, wall hours, coverage proof that the native executed on spawned threads, result |
| disposition | ConcurrentOk \| Locked-keep \| Locked-blocked-on <row/spec §> |
| revocations | initially empty; a ConcurrentOk->Locked regression flip appends here with date + incident/bug cite (spec §5.2) |

---

# PART B — non-normative audit trail

## Revision log

### rev 1 (2026-06-07) — initial draft

Drafted per the thread-specs2 charter: (1) per-NativeExecutable
concurrent-ok bit (function identity, not PropertyAttribute); (2)
park-capable, safepoint-polling native serial lock taken on spawned
threads only, released around JS re-entry; (3) load8+branch check
shape per tier following the gilOff()/group3Primitives() mode-split
pattern; (4) TSAN+fuzzer-evidenced ungating audit extending the
K4/N7 style; (5) U-T8e complement rule; (6) test charter NA-T1..T10.

Grounding pass executed against the live tree:
runtime/NativeExecutable.{h,cpp}, runtime/JSFunction.cpp,
runtime/VM.cpp getHostFunction funnel, jit/JITThunks.cpp
hostFunctionStub + HostFunctionKey weak map,
jit/ThunkGenerators.cpp nativeForGenerator (including its existing
gilOff arms), llint/LowLevelInterpreter64.asm nativeCallTrampoline /
internalFunctionCallTrampoline (existing AB-1 gilOff arms),
llint/LowLevelInterpreter.asm two-level discriminator macros,
runtime/VMLite.h L2 append region + gilOff byte,
interpreter/Interpreter.cpp vmEntryToNative arms,
dfg/DFGByteCodeParser.cpp handleIntrinsicCall;
SPEC-ungil §A.3/§LK/§E.1b.4/§K/§N + audits K4/N7 + UNGIL-HANDOUT
rev 32 §A.1.3/U0b/U0c.

Design decisions of record (rationale not in the spec body):

- One lock per VM, not per native or per group. Per-native locks
  would let two un-audited natives that share hidden state (the
  whole reason they are locked) race each other; the single lock is
  the GIL's safety argument scoped to locked-native bodies on
  spawned threads. Granularity (lock striping by audit group) is a
  post-v1 optimization that must NOT precede group-level audits —
  striping IS an audit claim ("group X and group Y share no state").
- The bit defaults Locked even for natives the K4/N7 audits already
  touch: those audits ruled specific DATA, not whole BODIES; §0.4.
- Owner word uses TID (16-bit, VMLite.h:211) rather than a lite
  pointer: avoids any question of lite-pointer caching across
  samples (EXIT1 forbids caching lite pointers; a TID is a value,
  and NL ownership cannot survive thread exit by the EX1/NL1
  depth-0-at-close assert).
- ParkingLot directly rather than WTF::Lock: WTF::Lock's slow path
  parks WITHOUT polling the lite stop bit; NL1 step (b) is the whole
  point. (WTF::Lock under the hood is the same compareAndPark shape,
  so cost is identical.)
- The InternalFunction conservative rule (NA-I8) was chosen over
  mirroring the byte because InternalFunction lacks the single
  interning funnel NativeExecutable has (JITThunks weak map);
  policy-consistency (NA-I5) would need a second mechanism. Small
  set, cold path, v1 simplicity wins.

Known open items carried into review (also spec §9):
- §LK row LK.1c both-sides supersession (SPEC-ungil owner).
- SPEC-jit R1.e-family extension note for the new emitted branches.
- SPEC-api one-liner: C API mints Locked.
- vmstate L2 append ratification for nativeLockEligible +
  m_nativeLockDepth.
- Whether the §4.5 C++-entry bracket should ALSO cover
  `JSObjectCallAsFunction` C-API paths on spawned threads, or
  whether SPEC-api's carrier-binding of the C API makes that
  unreachable (believed unreachable; needs an api-owner confirm).

Adversarial review rounds: append below.

## Review round 1 (2026-06-07) -> rev 2

Twelve reviewer findings received; deduplicated to nine items (the
carrier-exemption blocker, the NL1 wake-ordering blocker, and the
specialized-thunk bypass were each filed twice/thrice). Every
file:line citation was re-verified against the live tree before
acceptance. Dispositions:

R1.1 ACCEPTED (blocker; filed 2x): carrier-side calls to Locked
natives were unserialized. Rev-1 NA-I9 set nativeLockEligible only
on spawned lites, justified by "carrier semantics are the GIL-on
semantics already shipping" — unsound, because under SPEC-ungil
GIL-off carriers run JS in parallel with spawned threads
(SPEC-ungil.md:272 "GIL-off EVERY thread uses a real carrier lite";
verified). An un-audited Locked body (any ICU native) could run
concurrently on main + spawned, falsifying §0.3 and voiding NA-I7's
Intl serialization in the normal Bun topology. FIX: NA-I9 revised to
`nativeLockEligible = vm.m_gilOff` (all lites of the gilOff VM);
§0.3, NA-I1/NA-I15 cost claims, §6 carrier-queued bullet, NA-T1
(carrier arm) and NA-T10 (carrier NL acquisitions now legal)
amended. Rev-1 NA-I9 text of record: "nativeLockEligible =
vm.m_gilOff && (lite is a SPAWNED thread's lite ...); main/embedder
CARRIER lites emit 0. Main-thread/carrier calls to locked natives
take NOTHING."
  SUB-CLAIM REFUTED: the reviewer asserted "carrier tid is 0,
  VMLite.h:211 — owner encoding must distinguish 'free' from 'main
  thread holds', e.g. tid+1". FALSE for the configuration where NL
  exists: GIL-off carrier lites receive TM-allocated NONZERO carrier
  TIDs — runtime/JSLock.cpp:350 (`carrier->tid =
  allocateCarrierTID(); // unique nonzero TM allocation`) and :522
  (`RELEASE_ASSERT(lite->tid); // tid 0 never installed GIL-off`).
  VMLite.h:211's "0 = main thread" describes the GIL-on default,
  which never reaches NL (level-0 discriminator). No encoding change
  made; NL1 owner-word comment now cites these lines.

R1.2 ACCEPTED (blocker; filed 3x incl. the EX1-destructor variant):
rev-1 ANNEX NL1 ordered the contended loop CAS-first ((a) CAS,
(b) stop poll, (c) park, (d) goto a). A waiter parked in (c) and
woken by the holder's release INSIDE a stop window would CAS, win,
and RETURN into the host body with no post-wake stop poll —
violating SPEC-ungil §A.3.2b(ii) (SPEC-ungil.md:216-218, verified)
and contradicting the spec body §3.2.2, which was poll-first. FIX:
NL1 rewritten poll-first with two invariants: every wake (unpark or
deadline) loops to the stop poll before any CAS, and a winning CAS
re-polls — if the stop bit is set, the thread parks on its NVS
ticket WHILE HOLDING NL (legal per NA-I10) and completes the
post-wake poll before returning. Body §3.2.2 and annex now state the
identical order; EX1 destructor inherits it; NA-T4 gains the
wake-mid-stop arm. Rev-1 NL1 acquire text of record: "2. loop: a. if
m_owner.compareExchangeWeak(0, lite->tid) { depth=1; return; }
b. STOP POLL ... c. m_waiters++; ParkingLot::compareAndPark(&m_owner,
snapshot, timeout = bounded quantum); m_waiters--. d. goto a."

R1.3 ACCEPTED (blocker): the NA-I11 four-function re-entry funnel
was falsified by the tree. Verified: `Interpreter::executeCachedCall`
(interpreter/InterpreterInlines.h:100) calls vmEntryToJavaScript
directly at :127 and serves exactly the Locked natives' callbacks
(sort comparator ArrayPrototype.cpp:950, replace replacer
StringPrototype.cpp:399-400); `Interpreter::executeModuleProgram`
(Interpreter.cpp:1662, entry :1728) likewise bypasses the four.
FIX: funnel restated callee-defined (every vmEntryToJavaScript /
vmEntryToJavaScriptWith0Arguments caller), EX1 site list extended
(items 2-4), NA-T7 greps the caller set, NA-T3 gains the
CachedCall/module arms.

R1.4 ACCEPTED (major; merged with the no-JIT-leg finding): NA-I5's
RELEASE_ASSERT was anchored to the JITThunks weak map — verified
weak (`Weak<NativeExecutable>` entries with dead-entry override,
jit/JITThunks.cpp:262-268/:285-300), so a collected Locked entry
followed by a ConcurrentOk re-registration of the same key would
assert against nothing (GC-timing-dependent policy, transient
Locked/ConcurrentOk twins); and the no-JIT arm
(runtime/VM.cpp:1440-1442) creates fresh executables every call with
no interning, so the chartered assert had no mechanism there at all.
FIX: §1.3 now specifies a per-VM STRONG append-only
HashMap<HostFunctionKey, NativeConcurrency> side table (leaf-rank
lock) consulted by BOTH funnels; the weak map is explicitly NOT the
enforcement structure; NA-T8 gains the collected-then-re-registered
sequence.

R1.5 ACCEPTED (part of the closure finding): DFG lowers
constant-callee InternalFunction constructions to plain graph nodes
with no call — verified at dfg/DFGByteCodeParser.cpp:6080-6140
(SymbolConstructor -> NewSymbol, ObjectConstructor ->
NewObject/CallObjectConstructor) — contradicting rev-1 NA-I8's
"EVERY InternalFunction native call takes NL". FIX: §2.5 caveat
scoping NA-I8 to actual calls; the inline set joins the NA-T6 lint
(§4.4.1c).

R1.6 ACCEPTED (blocker): custom accessors were in the declared scope
(§0.1 "every static-table getter thunk") but had no bit carrier, no
bracket, and were invisible to NA-T7 (CustomAccessorPtrTag sites in
bytecode/InlineCacheCompiler.cpp, GetterSetterAccessCase.cpp,
Repatch.cpp, GetByVariant.cpp, PutByVariant.cpp — verified; 105
JSC_DEFINE_CUSTOM_GETTER sites in runtime/*.cpp — verified). FIX:
new §2.6 + NA-I20 (unconditionally Locked v1; gilOff IC compilation
slow-paths custom-accessor cases through an NL-bracketed operation);
NA-T7 greps CustomAccessorPtrTag; NA-X4 charts the post-v1 marker.

R1.7 ACCEPTED (major; filed 2x): specialized intrinsic thunks bypass
the §4.3 bracket. Verified: VM::getHostFunction passes
thunkGeneratorForIntrinsic (runtime/VM.cpp:1283 switch, used at
:1435); hostFunctionStub installs the generator's code as the
executable's CALL code (jit/JITThunks.cpp:273-275 DirectJITCode);
only the SpecializedThunkJIT::finalize fallback
(jit/SpecializedThunkJIT.h:173) reaches the bracketed generic thunk;
BoundFunctionCallIntrinsic (VM.cpp:1444-1458) is not PT1-seeded.
FIX: NA-I16/NA-T6 extended to the three-surface UNION; gilOff
getHostFunction suppresses the generator for Locked executables;
NA-I17/NA-T7 name the bypass surfaces as lint-governed exempts;
BoundFunctionCallIntrinsic explicitly flagged.

R1.8 ACCEPTED (major): the rev-1 split owner-word/relaxed-m_waiters
design had a lost-wakeup window on weakly-ordered hardware (no
happens-before from the waiter's relaxed increment to the releaser's
relaxed load; the annex's own "sole synchronization edge" clause
guaranteed the absence of the needed edge). FIX: NL1 rewritten to a
single word with hasParkedBit (WTF LockAlgorithm shape), all
transitions seq_cst RMW on m_word; the memory-order clause now
states what liveness depends on; NA-T4 gains the lost-wakeup arm.

R1.9 ACCEPTED (major): rev-1 NL1 step (c) cited
ParkingLot::compareAndPark with a timeout — verified that
compareAndPark takes none and hard-codes Time::infinity()
(wtf/ParkingLot.h:91-102) — and claimed (c)-parked waiters "count as
parked at a poll site" with no conductor mechanism behind the claim,
while calling the quantum correctness-free. FIX: NL1 specifies
parkConditionally with an explicit bounded deadline, normatively <
stopTheWorldWatchdogTimeout (30s,
bytecode/JSThreadsSafepoint.cpp:379), default 10ms; the
conductor-side paragraph now states honestly that an NL waiter is
conductor-invisible for at most one quantum and records the
worst-case stop-latency bound.

Supersessions recorded this side: rev-1 NA-I9/NA-I15/NA-T10 carrier
exemption (R1.1), rev-1 NL1 loop order + m_waiters + compareAndPark
+ conductor claim (R1.2/R1.8/R1.9), rev-1 NA-I11 funnel list (R1.3),
rev-1 NA-I5 weak-map enforcement (R1.4), rev-1 NA-I8 universal-call
claim (R1.5), rev-1 NA-I16/NA-I17/NA-T6/NA-T7 closure set
(R1.6/R1.7). No counterparty spec text changed: the §9 adoption
gates (SPEC-ungil §LK row, SPEC-jit note, SPEC-api note, vmstate L2
ratification) remain PENDING and unmodified; R1.1 does not alter the
proposed LK.1c edge set (NL stays inner to heap rank 1, outer to api
1-3 / heap 2-10b; only WHO acquires it widened to carrier lites,
which adds no conductor edge — NA-I10 unchanged).

## Review round 2 (2026-06-07) -> rev 3

Ten reviewer findings received; deduplicated to eight items (the
NL1 hasParkedBit release loss was filed 3x; the vmEntryToJavaScript
With1..6 funnel escape 2x). Every file:line citation re-verified
against the live tree and the counterparty specs before acceptance.
No finding refuted this round. Dispositions:

R2.1 ACCEPTED (blocker): NL held across licensed carrier cross-VM
nesting. Verified: UNGIL-HANDOUT §F.5 (UNGIL-HANDOUT.md:2262-2290)
licenses the carrier-side "Bun JSContext-inside-host-call" pattern
(:2270) and mandates "lock() on VM B while holding any other VM A's
token FIRST releases A's client heap access (F8 mandatory-revert)"
(:2276-2277); SPEC-ungil §F.6(e) (SPEC-ungil.md:668-670) forbids
nesting only for SPAWNED threads. A Locked VM-A native on the
carrier entering VM B therefore (1) performed the exact transition
rev-2 NA-I13 forbade, with only a debug assert behind it; (2) kept
NL across the whole nested window — every §3.3 drop-scope caller
inside B passes B's lites, which cannot release A's NL; (3) made a
mutual-wait deadlock constructible (B's JS Atomics.waits on a
notify owed by a Locked VM-A native on a spawned thread that blocks
on NL) with no conductor involved, falsifying rev-2 §3.4's
unscoped release-build "never deadlock" sentence (which was sound
only for conductor liveness). FIX: NA-I21 — the §F.5 funnel
instantiates NativeLockDropScope keyed on the thread's gilOff-VM
lite at its F8-revert point, RELEASE_ASSERT depth==0 backstop;
EX1 site 8; §3.4 liveness-scope paragraph rewritten (conductor vs
mutator liveness split); adoption gate §9.6 (SUPERSESSION-PENDING
with the SPEC-ungil/UNGIL-HANDOUT owner: §F.5/§F.6(e)/A36C + the
§F.6 IU embedder-checklist NL-depth row); NA-T11(a).

R2.2 ACCEPTED (blocker; filed 2x): the rev-2 NA-I11 funnel named
two exact symbols while the tree's JS-entry family is
vmEntryToJavaScript + With0..With6Arguments
(llint/LLIntThunks.h:39, :46-52; asm at
llint/LowLevelInterpreter.asm:2194-2329). Verified escaped callers:
Interpreter::tryCallWithArguments (interpreter/
InterpreterInlines.h:132-171, all seven arms), reached from
CachedCall::callWithArguments (interpreter/CachedCallInlines.h:38-66)
on CPU(ARM64)/CPU(X86_64) whenever argumentCountIncludingThis <= 7
— so the spec's own poster child, the Array.prototype.sort
comparator (runtime/ArrayPrototype.cpp:955, two args ->
With2Arguments), bypassed the funnel on both shipping targets (rev
2's :950/executeCachedCall story covered only the >7-arg fallback);
MicrotaskCall::tryCallWithArguments
(interpreter/MicrotaskCallInlines.h:40-86); runJSMicrotask arms
(runtime/JSMicrotask.cpp:159-171, :198). The R1.3 bug shape
recurring one level down, as filed. FIX: funnel restated as the
prefix-matched family defined by the LLIntThunks.h:39-52
declaration block (a future With7 cannot silently escape); EX1
sites 3-4 added and item 3's cite corrected from the With0 arm to
the whole wrapper; NA-T7 symbol list generated from LLIntThunks.h +
self-test; NA-T3 small-arity comparator arm pinned to
x86_64/ARM64 so tryCallWithArguments (not executeCachedCall) is
exercised.

R2.3 ACCEPTED (major; filed 3x): ANNEX NL1 release lost
hasParkedBit on multi-waiter handoff. Verified: the rev-2 release
(`old = m_word.exchange(0); if (old & hasParkedBit) unparkOne`)
cleared the bit for ALL waiters while waking ONE; the woken
waiter's acquire CAS preserved only `w & hasParkedBit` with w == 0,
so remaining waiters slept to the bounded deadline — one full
quantum (default 10ms) per subsequent handoff under contention
depth >= 2, directly failing NA-T4's own "handoff latency bounded
by unpark" arm; the plain unparkOne(address) form discards
UnparkResult::mayHaveMoreThreads (wtf/ParkingLot.h:104-119, :130);
WTF::LockAlgorithm::unlockSlow uses the CALLBACK form and
republishes isParkedBit iff mayHaveMoreThreads
(wtf/LockAlgorithmInlines.h:207-241) — the one load-bearing piece
of the "LockAlgorithm shape" rev 2 omitted. FIX: NL1 release step 3
rewritten to the callback-form handoff (bucket-locked store of
hasParkedBit iff mayHaveMoreThreads, else 0; fast path stays a bare
CAS to 0 when the bit is clear); memory-order note updated (the
bucket-locked callback store replaces the bare exchange as the
release-side transition); NA-T4 multi-waiter handoff arm added
(third waiter wakes via unpark). Rev-2 release text of record:
"3. old = m_word.exchange(0); // seq_cst, same word / if (old &
hasParkedBit) ParkingLot::unparkOne(&m_word)."

R2.4 ACCEPTED (blocker): no order existed between the two
long-hold locks (NL and NLS::m_lock), and a mutator-vs-mutator
AB/BA cycle was constructible from normative text: SPEC-ungil §LK
long-hold row (SPEC-ungil.md:902-907) — lock.hold runs user JS
holding NLS::m_lock, that JS may call a Locked native (NLS > NL,
also via the EX1 dtor reacquire in fn's epilogue); while a Locked
lock.hold body (host native, absent from rev-2 PT1, hence Locked
by NA-I6) blocks on NLS::m_lock holding NL (SPEC-api.md:57-59
contended hold blocks) — NL > NLS. §3.5's acyclicity argument was
silent on long-hold-vs-long-hold. FIX: LK.1c gains the pinned edge
NLS::m_lock OUTER to NL plus the negative edge "no NL holder
blocks on NLS::m_lock or any G11 blocking primitive" (discharged
operationally by R2.5's PT1.G seeding); SPEC-api §5.9 companion
row (the EX1-dtor-inside-fn-epilogue case is the LEGAL direction)
added to the §9.1 SUPERSESSION-PENDING scope.

R2.5 ACCEPTED (blocker): the G11 blocking natives (SPEC-api.md:15:
join(), lock.hold(), cond.wait()) defaulted Locked and would block
holding NL: joiner-vs-joined-thread total deadlock (the joined
thread completes only at §E.2 close and its Locked natives need
NL); and all three release heap access before blocking
(SPEC-api.md:267-271, 5.9(a1-a3)/(e)), tripping rev-2 NA-I13's
assert on every contended call. The engine's own threads API — the
project's deliverable — was unaddressed by §3.4's embedder-only
remedy. FIX: NA-I22 + ANNEX PT1.G (threads-API natives seeded
ConcurrentOk; their bodies are the api-§5.9-audited lock
discipline; NA1 rows still required); demotion rule (a G11 native
may only be Locked with internal drop scopes); PT1
LOCKED-FOR-EMPHASIS amended; gate §9.3(b); NA-T11(b)/(c).

R2.6 ACCEPTED (major): rev-2 NA-I13's unqualified
release-path assert fired on a legal, spec-mandated path — rule-8
GC stops fan through the per-lite trap bit and their NVS park
performs the F8 MANDATORY access revert (§13.5a willPark /
per-client m_releasedByGCPark; SPEC-ungil.md:289-298 "the GC stop
DOES set client-visible stop state"), so any thread holding NL at
an NL1 stop poll during a GC stop would assert in debug builds;
ANNEX NL1's "tokens + heap access KEPT, as at every §A.3 park"
parenthetical was true only for §A.3 JSThreads stops. FIX: NA-I13
scoped to VOLUNTARY transitions with an explicit exemption for
§J.3 park-site mandatory reverts; §3.2.4 and the NL1 step-(a)
parenthetical corrected (kept at §A.3 stops, F8-reverted at rule-8
GC stops); NA-T4 GC-stop-with-NL arm added.

R2.7 ACCEPTED (major): NA-I20's bracket was ungrounded — the C++
dispatch carries GetValueFuncPtrTag (runtime/PropertySlot.h:97) /
PutValueFuncPtrTag (PropertySlot.h:100) / CustomAccessorPtrTag
(runtime/PutPropertySlot.h:37), three distinct tags
(runtime/JSCPtrTag.h:53-56), and the invocation sites are untagged
call expressions (PropertySlot::customGetter's
m_data.custom.getValue, runtime/PropertySlot.cpp:36-48; put-side
customSetter invocations, runtime/JSObject.cpp:1449-1493) that a
CustomAccessorPtrTag grep cannot find; and "the slow-path
operation gains the bracket" was ambiguous-to-unimplementable
(custom-ness is unknown until after lookup). FIX: NA-I20
regrounded — the bracket wraps the FunctionPtr INVOCATION at the
named dispatch funnels; NA-T7 token families extended to all three
tags + the call-expression tokens.

R2.8 ACCEPTED (major): DFG/FTL direct custom-accessor calls had no
disposition in NA-I17's closure. Verified: CallCustomAccessorGetter
/Setter and CallDOMGetter lower to direct calls of the retagged
accessor pointer (dfg/DFGSpeculativeJIT.cpp:11689/:11813/:11840;
ftl/FTLLowerDFGToB3.cpp:15961-15989, :22171-22187
"bypassedFunction"), built from GetByStatus/PutByStatus variants at
dfg/DFGByteCodeParser.cpp:6647/:7076/:5559 — reaching neither the
§2.6 IC mechanism nor any C++ funnel. FIX: NA-I23 (§4.4.3) — gilOff
byte-code parser does not build the three node kinds (parser-side
suppression chosen over the unstated starvation argument because
it is locally verifiable); NA-T6 extended; the lowering files are
NA-T7 exempt with the NA-T6 cross-reference.

Supersessions recorded this side: rev-2 NA-I11 two-symbol funnel +
EX1 item 3 (R2.2), rev-2 NL1 release step 3 + "heap access KEPT"
parenthetical (R2.3/R2.6), rev-2 NA-I13 unqualified assert +
unscoped "never deadlock" sentence (R2.1/R2.5/R2.6), rev-2 §3.5
edge set silence on NLS::m_lock (R2.4), rev-2 NA-I20
slow-path-operation wording + single-tag lint (R2.7), rev-2
NA-I17 surface set (R2.8). Counterparty-spec obligations are
gates, not edits made here: §9.1 (extended LK.1c + api §5.9 row),
§9.3(b) (PT1.G api-owner ack), §9.6 (§F.5/§F.6 NL-drop
obligation) — all SUPERSESSION-PENDING; no text outside
SPEC-nativeaffinity{,-history}.md was modified.

## Review round 3 (2026-06-07) -> rev 4

Eleven reviewer findings received; deduplicated to ten items (the
DOMJIT/CallDOM bypass was filed twice). Every file:line citation
re-verified against the live tree before acceptance (one path
correction: "Repatch.cpp" lives at bytecode/Repatch.cpp in this
tree, not jit/ — kinds at :711-715/:1251, tryCacheGetBy :475,
tryCachePutBy :1040). No finding refuted this round; one refined
(R3.10, GILOFF-build scope). Numbering: the two DOMJIT filings
took R3.3 and R3.5 on intake; R3.5 merged into R3.3, id retired —
the R3.5 gap below is deliberate. Size-cap action: spec body was at
49,966/50,000 pre-round; the §8 charter full text moved to ANNEX
TC1 and §1.3/§3.1/§3.2.2 rationale prose compressed against
existing history records to absorb the rev-4 additions.
Dispositions:

R3.1 ACCEPTED (blocker): §6 asserted NL serialization for
globalObjectMethodTable hooks with no mechanism. Verified: the
hooks are raw member pointers (GlobalObjectMethodTable.h:58-71)
invoked directly — promiseRejectionTracker at runtime/VM.cpp:2265
(the §F.1 drain; rev 3's own carrier-queued example) and :2304;
reportUncaughtExceptionAtEventLoop at runtime/MicrotaskQueue.cpp:66,
runtime/DeferredWorkTimer.cpp:284, runtime/ThreadManager.cpp:933.
No NativeExecutable is minted ("Locked" unrepresentable), no §4
emitter or §2.6 funnel is reached, no NA-T7 family matches — rev
3's "an inline-disposition Locked hook runs under NL" and "a Locked
carrier-queued hook simply runs under NL" claimed serialization the
design does not provide (R1.1 severity class), inviting U-T8e
dispositions laundered ON the nonexistent NL — the inverse of
NA-I19. FIX (reviewer option (a), the smaller honest fix): NA-I24 +
ANNEX SC1.3 — §6 scoped to NativeExecutable-backed natives; raw
hooks get NO NL coverage, safety rests SOLELY on the U-T8e audit;
anti-laundering clause (no INLINE disposition on the strength of
NL); §0.3/§0.4 cross-notes; NA-T10 stops implying the queued
tracker is NL-serialized. Option (b) (a methodTable bracket) is
charted as NA-X5 in SC1.3, not v1. Rev-3 §6 bullets text of record:
"An inline-disposition hook that is Locked is legal — it runs on
the spawned thread under NL"; "under revised NA-I9 the carrier IS
NL-eligible, so a Locked carrier-queued hook simply runs under NL
like any other Locked native — correct, ordered".

R3.2 ACCEPTED (major): §4.5's "both sites" falsified — verified
THIRD vmEntryToNative caller at runtime/JSMicrotask.cpp:206 (the
CallData::Type::Native arm, reachable from plain JS via
queueMicrotask(nativeFn) on a spawned thread); rev 3 cited this
function's JS arms (:159-171, :198) for the NA-I11 DROP funnel
while missing the ACQUIRE-side native arm below them — the
R1.3/R2.2 bug shape a third level down. FIX: §4.5 enumerates three
sites and DEFINES the set by the NA-T7 vmEntryToNative token
family; wasm arm (:204) exempt-cited (spawned Wasm refused,
SPEC-ungil §I); NA-T2 gains the microtask-native-callee cell;
NA-I17 names the family as definitional.

R3.3 ACCEPTED (major; filed 2x): DOMJIT signature dispatch is a
fourth inline-bypass surface — verified end-to-end
(DFGByteCodeParser.cpp:2106-2114 signatureFor -> handleDOMJITCall
:5244; CallDOM direct call of functionWithoutTypeCheck,
DFGSpeculativeJIT.cpp:11603, FTLLowerDFGToB3.cpp:22055; funnels
VM.cpp:1429 + JITThunks.cpp:275-276 NativeDOMJITCode; $vm
signatures JSDollarVM.cpp:1479/:1542, PT1-LOCKED), invisible to
rev-3 NA-T6/NA-T7 and additionally type-check-skipping. R1.7/R2.8
class, recurring on the signature channel. FIX: NA-I16 member (d) +
ANNEX SC1.1 — gilOff getHostFunction passes nullptr signature
unless ConcurrentOk (kills the jit-code arm AND starves
signatureFor); NA-T6 gains the signature-bearing-executable set;
§4.4's "no new emitter" sentence corrected; compileCallDOM files
NA-T7-exempt with NA-T6 cross-ref; recorded that NA-I23's
CallDOMGetter row does not cover CallDOM.

R3.4 ACCEPTED (major): intrinsic GETTER inlining (DFG
handleIntrinsicGetter, DFGByteCodeParser.cpp:5263/:6743; IC
IntrinsicGetterAccessCase + emitIntrinsicGetter,
IntrinsicGetterAccessCase.cpp:37-48, InlineCacheCompiler.cpp
:3575/:4536, admission canEmitIntrinsicGetter :4473 used at
bytecode/Repatch.cpp:692) executes Locked-by-default getter
semantics outside every bracket and outside the rev-3 NA-T6 sets.
FIX: NA-I16 members (e)/(f) + ANNEX SC1.2, §2.5-style treatment —
each member OM/heap-ruled and lint-pinned, or gilOff-disabled.

R3.6 ACCEPTED (major; reviewer's two-part IC finding): (1) NA-I20's
gilOff IC suppression named no mechanism while the live tree's IC
emission already carries a gilOff arm (InlineCacheCompiler.cpp:3462
emitPublishTopCallFrameForHostCall). FIX: suppression GROUNDED at
AccessCase creation in bytecode/Repatch.cpp (four kinds named;
slow-path-only give-up state — no regeneration livelock; the :3462
arm's disposition recorded: unreachable gilOff, retained GIL-on).
(2) The JITCage emission arm calls via vmEntryCustomGetter/Setter
(LLIntThunks.h:41-42) under GetValueFuncWithPtrPtrTag/
PutValueFuncWithPtrPtrTag (PropertySlot.h:98/:101) — none in rev
3's six NA-T7 families, and NA-I20's "three tags" undercounted.
FIX: NA-T7 extended to EIGHT families; §2.6 enumerates five tags +
two symbols; full grounding = ANNEX SC1.4.

R3.7 ACCEPTED (major): negative-edge rank range inconsistent inside
the proposed LK.1c row — §3.2.3/NA-I10 said heap 2-10b, §3.5's
acyclicity bullet said 2-9b (copied from the narrower NLS row,
SPEC-ungil.md:903-906 "no conductor or heap-2..9b holder ACQUIRES
it"). Since NL is declared outer to 2-10b and SPEC-heap L4's
allocation back-edge shows 10a/10b sections are not lock-terminal,
the exclusion must be stated. FIX: 2-10b pinned VERBATIM in §3.5,
matching NA-I10; the §9.1 gate scope records the range verbatim.

R3.8 ACCEPTED (major): §3.5's "needs no §E.2 rank-4 exemption" was
stale after R2.6 — BL1.1/NL1 license holding NL across the F8
MANDATORY revert + gated re-acquisition at rule-8 GC stops, exactly
the shape §E.2's park-site clause polices (SPEC-ungil.md:489-495,
"release BEFORE, re-acquire AFTER (ditto §J.3 park sites)") and for
which NLS::m_lock carries the recorded RANK-4 EXEMPTION; U20 lints
the order. Recording/composition defect, not unsoundness
(deadlock-freedom argued by NA-I10/BL1.1). FIX: §3.5 grants NL an
NLS-style carve-out LIMITED to §J.3 park-site MANDATORY F8
reverts/re-acquisitions; voluntary transitions remain forbidden
(NA-I13); the carve-out joins the LK.1c row text and §9.1 scope.

R3.9 ACCEPTED (major): three counterparty obligations lacked gates.
(1) §9.4 ratified only nativeLockEligible while ANNEX NL1 appends
m_nativeLockDepth to L2 (the rev-1 open-item list named both). FIX:
§9.4 names both fields. (2) NL1's teardown asserts edit
SPEC-ungil-owned lifecycle text (§E.2 close depth==0; ~VM m_word==0
ordered against EXIT1.9) with no supersession row. FIX: added to
the §9.1 SUPERSESSION-PENDING scope. (3) §2.6 cited "gate §9.2"
whose stated scope ("codegen note for the §4.3/§4.4 arms") did not
bind the IC suppression. FIX: §9.2 reworded to name, explicitly,
the bracket arms + the §2.6 Repatch suppression + the NA-I23
parser suppression + the §4.4.1b/d/e/f suppressions.

R3.10 ACCEPTED (major, REFINED): NA-I1's "byte-for-byte" and
NA-T9's whole-binary byte-compare are unsatisfiable for
GILOFF-enabled builds — §4.2 adds instructions inside
nativeCallTrampoline's `.liteStoreTopCallFrame` arm
(LowLevelInterpreter64.asm:3161-3219) and LLInt is assembled once
at build time. REFINEMENT (verified): the arm lives inside the
`if GILOFF_TLS` assembler conditional, so non-GILOFF builds DO
remain byte-identical; the claim fails only for GILOFF-enabled
binaries running flag-off — which is the shipping Bun
configuration, so the finding stands. The family convention is
executed-path identity (jit R1.e, UNGIL-HANDOUT:172-179), not
binary identity; SPEC-ungil §J does not rescue the literal charter.
FIX: NA-I1 restated (executed sequences unchanged; new LLInt bytes
confined to branchIfGilOffGroup3*-guarded arms; GIL-on per-VM
thunks byte-identical; non-GILOFF builds byte-identical outright);
NA-T9 rechartered to thunk byte-compare + arm-level LLInt diff.

R3.11 ACCEPTED (major): body §3.3 and BINDING ANNEX EX1
contradicted each other on the drop scope's mode gating — §3.3
claimed a §4.1-style mode test plus mode-split call sites; the EX1
ctor had only the lite test, and the mandatory sites
(InterpreterInlines.h:100-171, JSMicrotask.cpp, etc.) are
common-path code executing identically in every configuration. Both
texts normative => conflicting instructions, and the "dead code
GIL-on" cost story was wrong. FIX (shape chosen: add the gate, keep
the cost claim honest): EX1 ctor gains the level-0
`g_jscConfig.gilOffProcess` test FIRST; §3.3 rewritten — flag-off/
GIL-on processes pay one predictable global-byte branch per
JS-entry funnel, the lite/depth loads never execute; the false
"call sites are themselves on gilOff-mode-split paths" sentence
superseded (text of record: "the C++ check is one TLS-adjacent
load, and it is additionally gated on the same mode test as §4.1,
making it dead code GIL-on").

Supersessions recorded this side: rev-3 §6 hook-NL bullets + NA-T10
implication (R3.1, NA-I24), rev-3 §4.5 two-site enumeration (R3.2),
rev-3 NA-I16 three-surface union + §4.4 "no new emitter" + NA-I17
surface set + NA-T6/NA-T7 lint sets (R3.3/R3.4/R3.6), rev-3 NA-I20
"three tags" + ungrounded IC suppression (R3.6), rev-3 §3.5 2-9b
range + "needs no §E.2 rank-4 exemption" (R3.7/R3.8), rev-3
§9.2/§9.4 gate wording (R3.9), rev-3 NA-I1/NA-I15/NA-T9
"byte-for-byte" (R3.10), rev-3 §3.3 gating sentence + EX1 ctor
(R3.11). Size-cap supersessions (content unchanged, location
moved): §8 full charters -> ANNEX TC1; §1.3/§3.1/§3.2.2 rationale
prose -> already-recorded history rounds. Counterparty-spec
obligations remain gates: §9.1 (rev-4-extended LK.1c scope), §9.2
(reworded), §9.3, §9.4 (both fields), §9.6 — all
SUPERSESSION-PENDING; no text outside
SPEC-nativeaffinity{,-history}.md was modified.
