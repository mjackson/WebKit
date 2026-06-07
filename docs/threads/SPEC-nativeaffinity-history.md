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
          §J.3 park protocol (tokens + heap access KEPT, as at every
          §A.3 park); on wake, run the §A.3.2b post-wake poll BEFORE
          continuing.
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

release(lite):

    1. ASSERT((m_word & ownerMask) == lite->tid
              && lite->m_nativeLockDepth);
    2. if --lite->m_nativeLockDepth return;
    3. old = m_word.exchange(0);          // seq_cst, same word
       if (old & hasParkedBit) ParkingLot::unparkOne(&m_word).

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
            // dead-cheap when ineligible (GIL-on VMs' lites; rev 2:
            // carriers of the gilOff VM ARE eligible per revised
            // NA-I9): one byte test on already-resident lite state;
            // the call sites are themselves on gilOff-mode-split
            // paths, and depth==0 (the overwhelmingly common case)
            // costs one more load.
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

Mandatory instantiation sites (spec NA-I11 funnel; REVISED rev 2,
round-1 finding R1.3 — the rule is CALLEE-defined: EVERY
`vmEntryToJavaScript` / `vmEntryToJavaScriptWith0Arguments` caller,
lint-enforced by NA-T7; the list below is the known caller set at
this rev, NOT the definition):
1. `Interpreter::executeCall` / `executeConstruct` / `executeEval` /
   `executeProgram` JS arms (the `vmEntryToJavaScript` callers
   adjacent to interpreter/Interpreter.cpp:1319/:1409's native arms).
2. `Interpreter::executeCachedCall`
   (interpreter/InterpreterInlines.h:100; direct vmEntryToJavaScript
   at :127). MISSED BY rev 1 and maximally load-bearing: CachedCall
   is the re-entry vehicle of exactly the natives that ship Locked —
   Array.prototype.sort comparators (runtime/ArrayPrototype.cpp:950;
   sort is excluded from the PT1.B seed), String.prototype.replace
   functional replacers (runtime/StringPrototype.cpp:399-400),
   Map/Set/Iterator/WeakMap/RegExp/Promise helpers. Without the
   scope here, a Locked sort comparator runs arbitrary user JS with
   NL held — the exact GIL-regrowth/deadlock NA-T3 exists to catch.
3. The `vmEntryToJavaScriptWith0Arguments` wrapper
   (InterpreterInlines.h:158).
4. `Interpreter::executeModuleProgram` (interpreter/
   Interpreter.cpp:1662; entry at :1728) — reached from the
   moduleLoaderEvaluate host function and embedder (Bun) natives
   that evaluate modules. Also missed by rev 1.
5. The per-lite microtask drain loop entry (SPEC-ungil §E.1/I11) —
   defensive; a native should never pump microtasks while holding NL,
   but the drop scope makes it correct anyway.
6. Embedder/manual: around blocking regions in host bodies (spec
   NA-I13).

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

LOCKED FOR EMPHASIS (not exhaustive — everything unlisted is locked
by NA-I6): ALL Intl* (NA-I7); Date locale/timezone paths (tz cache);
Function constructor / eval / indirect eval (parser+codegen world);
RegExp compile-heavy paths; console/inspector/debugger natives
(main-only family, SD13/SD14); $vm / test natives; Error.captureStackTrace
and stack-trace materialization; Proxy trap helpers; WeakRef/
FinalizationRegistry natives; Wasm natives (spawned-thread Wasm is
refused v1 anyway, SPEC-ungil §I).

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
