# SPEC-nativeaffinity — per-NativeExecutable concurrency bit + native serial lock

Status: DRAFT rev 2 (2026-06-07; review round 1 applied — history
PART B). Frozen-spec conventions per the
SPEC-{heap,vmstate,objectmodel,jit,api,ungil}.md family: normative
clauses cite `file:line` (tree at branch jarred/threads as of this
rev) or a `SPEC-* §`; FULL-text overflow lives in
`docs/threads/SPEC-nativeaffinity-history.md` PART A as BINDING
annexes (ANNEX NL1/PT1/EX1/AT1); supersessions are recorded BOTH
sides — where the other side is a frozen spec this document does not
own, the row is marked SUPERSESSION-PENDING and is an ADOPTION GATE
(§9), not yet in force. Invariants NA-I*, test charters NA-T*,
extensions NA-X*. Size cap 50000 bytes; history file uncapped.

Reading order: SPEC-ungil.md §A.3 (stop protocol), §LK (lock table),
§E.1b.4/U-T8e (hook dispositions), §K/§N + audits K4/N7 (audit
style), UNGIL-HANDOUT.md §A.1.3 (two-level gilOff discriminator),
then this file.

## 0. Scope, motivation, threat model

0.1 PROBLEM. GIL removal (SPEC-ungil) makes JS-visible engine state
safe under N mutators via §K (VM/global caches), §N (cell-internal
state), §A.3 (stops). Host/native functions are a long tail the §K/§N
audits cannot exhaustively close: every `JSC_DEFINE_HOST_FUNCTION`
body, every static-table getter thunk, every embedder (Bun) native is
arbitrary C++ that today runs with the GIL as its implicit
serializer. The K4 audit (SPEC-ungil-audit-K4.md) covers VM-resident
state those natives touch, but cannot prove the BODIES of thousands
of natives are re-entrant.

0.2 DESIGN (Jarred's proposal). A defense-in-depth RATCHET:

1. Every `NativeExecutable` carries a **concurrent-ok bit**. Bit set
   => the native is audited safe to run on N threads simultaneously
   (its only shared-state touches are §K/§N/audit-ruled). Bit clear
   => calls to it on SPAWNED threads serialize on one per-VM
   **native serial lock** ("NL", §3).
2. Default policy: a small audited hot core (property/array/string/
   Math/JSON/Atomics paths) ships concurrent-ok; the long tail and
   ALL Intl/ICU ship locked (§2).
3. Bits move Locked->ConcurrentOk ONLY with TSAN + fuzzer evidence
   recorded in an audit table (§5); the reverse flip needs no
   evidence (the ratchet loosens only one way).

0.3 The bit is NOT a security boundary against malicious natives; it
is an engineering ratchet that converts "all natives must be proven
re-entrant before GIL-off ships" into "un-audited natives are
correct-but-serial, audited natives are parallel". "Serial" means
serial across ALL JS-executing threads of the gilOff VM — carriers
included (NA-I9: under SPEC-ungil, GIL-off carriers run JS in
PARALLEL with spawned threads — SPEC-ungil §A.3.6/ANNEX A36 "GIL-off
EVERY thread uses a real carrier lite", SPEC-ungil.md:272 — so a
carrier-exempt lock would not serialize anything). Serial cost is
paid ONLY on threads of the gilOff VM calling locked natives;
flag-off and GIL-on configurations emit today's code byte-for-byte
(NA-I1).

0.4 The bit COMPLEMENTS, does not replace, the U-T8e hook
dispositions (SPEC-ungil:461-463) — see §6 — and complements the
§K/§N rulings: a native whose shared touches are ruled by N7 rows
still defaults to locked until ITS BODY is audited (§2.5 Intl
example).

## 1. The concurrent-ok bit

### 1.1 Carrier: NativeExecutable (function identity)

The bit lives on `NativeExecutable`
(runtime/NativeExecutable.h:33-105), NOT on a property descriptor and
NOT as a `PropertyAttribute`. Rationale (normative): concurrency
safety is a property of the FUNCTION IDENTITY — the same callable is
reachable through `.call`/`.apply`, `Function.prototype.bind`
products, stored references, cross-thread-published `JSFunction`s
(`JSFunction::getCallData`, runtime/JSFunction.cpp:313, returns the
executable regardless of how the reference arrived), and the
constructor path (`getConstructData`, JSFunction.cpp:483). A
property-level attribute would be bypassed by every one of those.
`NativeExecutable` is the unique identity object for a host function
pair: `m_function`/`m_constructor` (NativeExecutable.h:98-99), shared
by all `JSFunction`s minted over it (JSFunction.cpp:100 ->
VM::getHostFunction, runtime/VM.cpp:1376/:1429).

### 1.2 Storage and JIT addressability

NA-I2 (storage). A dedicated byte `uint8_t m_concurrentOk` is
appended to `NativeExecutable` adjacent to the existing 2-bit
`m_implementationVisibility` field (NativeExecutable.h:101;
bitWidthOfImplementationVisibility = 2,
runtime/ImplementationVisibility.h:38). It MUST be a whole,
independently addressable byte — emitted code does `load8 +
branch` (§4) and a bitfield share with `m_implementationVisibility`
would force a wider load and couple two unrelated writers. A
`static constexpr ptrdiff_t offsetOfConcurrentOk()` is added beside
`offsetOfNativeFunctionFor` (NativeExecutable.h:63-69) and
`offsetOfAsString` (:90); the LLIntOffsetsExtractor friend (:35)
already exposes the class to the asm layer.

NA-I3 (immutability). `m_concurrentOk` is written EXACTLY ONCE, in
the `NativeExecutable` constructor (NativeExecutable.cpp:76), before
`finishCreation` (:61) publishes the cell. It is never mutated
afterward; "ungating" (§5) is a SOURCE change to the policy input at
the creation site, not a runtime flip. Consequences:
- Readers need no fences beyond cell publication: a spawned thread
  can reach the executable only via a published `JSFunction`/cell
  reference, and the SPEC-objectmodel publication rules (OM safe-
  publication of fully-initialized cells) order the ctor store before
  any cross-thread load. Same argument the engine already relies on
  for `m_function` itself.
- DFG/FTL MAY constant-fold the bit when the callee
  `NativeExecutable` is a compile-time constant (known CallVariant):
  no watchpoint needed, the value cannot change (§4.4).

NA-I4 (one bit covers both kinds). The single bit governs BOTH
`m_function` and `m_constructor` (both specialization kinds,
NativeExecutable.h:55-61). Rationale: most constructors are
`callHostFunctionAsConstructor` (JSFunction.cpp:52, installed by the
JITThunks::hostFunctionStub overload at jit/JITThunks.cpp:311-313),
whose body only throws — trivially concurrent-ok — so the bit is
effectively the call-path bit; an executable whose REAL constructor
is not audited MUST stay locked even if its call path is audited.
Splitting into two bits is NA-X2 (extension, not v1).

### 1.3 Cache-key consistency

`JITThunks::hostFunctionStub` interns `NativeExecutable`s in a weak
map keyed by `HostFunctionKey` (function, constructor, visibility,
name — jit/JITThunks.cpp:102, :214-215, :248-313). NA-I5: the
concurrent-ok bit MUST be a deterministic function of the
registration-site policy input for a given key; it is NOT added to
the key. If two registration sites pass conflicting policy for the
same key, creation RELEASE_ASSERTs (debug + release): a silent
first-wins would make the effective bit depend on registration
order, i.e. on which thread/feature initialized first —
unacceptable for a security ratchet.

Enforcement structure (REVISED rev 2, round-1 finding R1.4): the
JITThunks weak map is NOT the enforcement structure. It is weak
(`Weak<NativeExecutable>` entries, jit/JITThunks.cpp:262-268 dead-
entry handling): a Locked registration whose executable is collected
leaves no live entry, so a later conflicting ConcurrentOk
registration of the same key would mint a fresh executable with no
conflict observed — the effective bit would depend on GC timing, and
stale references could even hold Locked and ConcurrentOk twins of
the same body live simultaneously, voiding NL. And the no-JIT path
(`NativeExecutable::create` direct, runtime/VM.cpp:1440-1442) has no
map at all. Therefore: a per-VM STRONG, append-only side table
`HashMap<HostFunctionKey, NativeConcurrency>` (leaf-rank lock, same
rank discipline as VMLite::scratchBufferLock; entries never removed
— one entry per distinct host function, size is trivially bounded)
is written on first registration and consulted by EVERY subsequent
creation, on BOTH funnels (JITThunks::hostFunctionStub and the
no-JIT VM.cpp:1440 arm). The RELEASE_ASSERT compares against this
table. NA-T8 tests the table, including the collected-then-
re-registered sequence.

### 1.4 What "concurrent-ok" asserts (audit obligation)

A bit set to 1 asserts, with §5 evidence: for every reachable path of
the native's body (call AND construct kinds, NA-I4),
1. every touched VM-/global-/process-resident mutable datum is ruled
   by a K4/N7/NA row (per-lite, lock, lazy-publish,
   immutable-after-init, already-safe — SPEC-ungil-audit-K4.md
   classification key);
2. every touched cell-internal datum follows SPEC-ungil §N /
   SPEC-objectmodel rules (cell lock, lock-free arm, rope rules);
3. it performs no thread-affine side effect (no TLS assumption beyond
   the lite, no main-thread-only OS API);
4. any JS re-entry it performs (valueOf/toString/getters/callbacks)
   is itself safe under N mutators — JS re-entry is governed by the
   core SPEC-ungil machinery, so this leg is usually free.

## 2. Default policy

### 2.1 Policy input and funnel

Every `NativeExecutable` creation funnels through exactly two
constructors of policy:

- C++ registration: `VM::getHostFunction` (VM.cpp:1376, :1429) /
  `JITThunks::hostFunctionStub` (JITThunks.cpp:248, :253, :311).
  These gain a `NativeConcurrency { Locked, ConcurrentOk }` parameter
  **defaulting to `Locked`**. NA-I6: the default at every funnel is
  Locked; concurrent-ok is always an explicit opt-in at the
  registration site (greppable, diffable, audit-citable).
- Static property tables (.lut.h via create_hash_table /
  runtime/Lookup.h lazy reification): the LUT attribute grammar gains
  a `ConcurrentOk` marker; absent marker => Locked. The reification
  path plumbs it into the funnel above. (Mechanical; the marker is
  the unit the §5 audit flips.)

### 2.2 Seed allowlist (index; FULL table = history ANNEX PT1, BINDING)

Ships concurrent-ok at v1, each group with an audit row (§5) before
the flag flips on:

- PT1.A property/object hot core: `hasOwnProperty`,
  `propertyIsEnumerable`, `Object.keys`-family fast paths,
  `Object.getPrototypeOf` (bodies already audited as part of OM/K4
  property-path work).
- PT1.B array hot core: index/length-centric Array.prototype natives
  whose shared touches are butterfly/Structure paths ruled by
  SPEC-objectmodel.
- PT1.C string hot core: rope-safe String.prototype natives
  (SPEC-ungil §N.2 ropes take no lock).
- PT1.D Math.* (runtime/MathObject.cpp): pure value math; the
  `m_weakRandom` exception is already per-lite (K4 AUD1.K4 /
  VIII.10).
- PT1.E JSON.parse/stringify (runtime/JSONObject.cpp): allocation +
  property puts on fresh objects; shared touches are heap/OM-ruled.
- PT1.F Atomics.* (runtime/AtomicsObject.cpp): designed for
  concurrency; waiter-list state already process-locked.

Everything else — INCLUDING every Intl/ICU native — ships Locked.

### 2.3 Intl/ICU: ALL locked, normatively

NA-I7: every native reachable from `Intl.*` (runtime/IntlObject.cpp
and the Intl* class files), plus every non-Intl native that calls
into ICU (Date locale paths, `String.prototype.localeCompare`,
`toLocaleString` family, case conversion with locale), is Locked in
v1 and is NOT eligible for PT1 seeding. ICU carries library-global
state (locale data caches, error-prone UEnumeration/UCollator misuse)
whose audit is out of scope for this engine's audits. This is
deliberately REDUNDANT with SPEC-ungil audit row N7-U6 ("Intl:
mutation cell-locked; ICU verified-const or clone-per-use") — N7-U6
rules the ENGINE-side touches; NA-I7 serializes the BODIES until an
ICU-level audit exists. Belt and suspenders is the point (§0.4).
Ungating any Intl native additionally requires the §5 row to cite an
ICU-thread-safety argument per touched ICU API.

### 2.4 Embedder API (Bun)

- Bun links JSC internally and registers natives through
  `VM::getHostFunction`/`JSFunction::create`: the §2.1 parameter IS
  the embedder API. Bun marks its own audited natives ConcurrentOk at
  its call sites; Bun-side evidence lands in the same §5 table
  (rows NA1.E.*).
- The public C API (`JSObjectMakeFunctionWithCallback`,
  API/JSObjectRef.h family) mints Locked executables UNCONDITIONALLY
  in v1; no C-API surface to opt in (C-API clients have made no
  thread-safety promise; SPEC-api keeps C-API entry carrier-bound
  anyway). NA-X3: a `...WithCallbackAndConcurrency` C entry point,
  post-v1.

### 2.5 InternalFunction (no NativeExecutable)

`InternalFunction` constructors carry raw native pointers
(runtime/InternalFunction.h:68 `offsetOfNativeFunctionFor`), bypassing
`NativeExecutable` entirely (separate LLInt trampoline,
llint/LowLevelInterpreter64.asm:3221). v1 rule (NA-I8): on
NL-eligible threads (§3.1) EVERY InternalFunction native call takes
NL — conservatively locked, no per-instance bit. The set is small
(builtin constructors) and constructor-heavy workloads on spawned
threads are not the v1 hot path. CAVEAT (rev 2, round-1 finding
R1.5): "EVERY InternalFunction native call takes NL" is scoped to
calls that actually reach the InternalFunction trampoline/C++ entry;
the DFG lowers constant-callee InternalFunction constructions to
plain graph nodes WITHOUT any call (dfg/DFGByteCodeParser.cpp:
6080-6140 — SymbolConstructor -> NewSymbol, ObjectConstructor ->
NewObject/CallObjectConstructor, typed-array constructors). That
inline set is governed by the extended NA-I16/NA-T6 lint (§4.4):
each member's inlined semantics must be OM/heap-ruled (they are
allocation + structure operations today) and the lint pins the list.
NA-X1: mirror the byte onto `InternalFunction` with the same policy
funnel.

### 2.6 Custom accessors (CustomGetterSetter / static-table CustomValue)

NEW rev 2 (round-1 finding R1.6 — §0.1 names static-table getter
thunks in scope, but rev 1 gave them no carrier, no bracket, no
lint). `JSC_DEFINE_CUSTOM_GETTER`/`SETTER` bodies (105 sites in
runtime/*.cpp alone) are RAW function pointers carried by
`CustomGetterSetter` cells and static-table CustomValue entries.
They never mint a `NativeExecutable`: they are invoked from
IC-generated code and slow-path operations via
`CustomAccessorPtrTag` (bytecode/InlineCacheCompiler.cpp,
GetterSetterAccessCase.cpp, Repatch.cpp, GetByVariant.cpp,
PutByVariant.cpp), bypassing every §4 emitter.

NA-I20 (v1 disposition — conservative, NA-I8-style): on
NL-eligible lites (NA-I9), EVERY custom-accessor invocation routes
through an NL-bracketed slow-path operation. Mechanically: in
gilOff-mode IC compilation (the SPEC-jit gilOff codegen split,
adoption gate §9.2), custom-accessor access cases are not emitted as
inline IC code — they take the existing slow-path operation, which
gains the §4.1 bracket (lite byte test; no executable byte exists,
so the bit test is skipped: custom accessors are unconditionally
Locked in v1). The LLInt/C++ GetValueFunc/PutValueFunc dispatch
sites take the same bracket. No per-accessor bit in v1; NA-X4
(extension): a concurrency marker in the static-table grammar
mirroring §2.1, with the same NA1 evidence bar. NA-T7's grep set
includes `CustomAccessorPtrTag` call sites (§4.6).

## 3. The native serial lock (NL)

### 3.1 Definition and eligibility

One `NativeSerialLock` instance per VM (member of the single
`m_gilOff` VM — exactly one GIL-off VM exists per process,
UNGIL-HANDOUT U0b/U0c). It serializes the BODIES of locked natives
across ALL JS-executing threads of that VM — spawned and carrier
(rev 2, NA-I9).

NA-I9 (eligibility — who ever touches NL; REVISED rev 2, round-1
finding R1.1 — supersedes the rev-1 spawned-only rule, both texts in
history PART B). NL is acquired ONLY on threads whose lite has the
new L2 byte `VMLite::nativeLockEligible` set. The byte is computed
ONCE at lite registration, in the same append region and by the same
recipe as `VMLite::gilOff` (runtime/VMLite.h:234-242):

    nativeLockEligible = vm.m_gilOff   // EVERY lite of the gilOff
                                       // VM: spawned AND carrier

Rationale (normative): under SPEC-ungil, GIL-off carriers execute JS
in PARALLEL with spawned threads ("GIL-off EVERY thread uses a real
carrier lite", SPEC-ungil.md:272; JSLockHolder degrades per §B.1 —
it is not mutual exclusion). A carrier-exempt NL would let an
un-audited Locked body (e.g. any ICU-touching native, NA-I7) run
concurrently on the main thread and a spawned thread — falsifying
§0.3's correct-but-serial guarantee in the normal Bun topology. The
rev-1 justification ("carrier semantics are the GIL-on semantics
already shipping") was unsound: those semantics were safe only
because the GIL also excluded every other JS thread. Owner-word
note: GIL-off carrier lites carry TM-allocated NONZERO TIDs
(runtime/JSLock.cpp:350 `carrier->tid = allocateCarrierTID()`,
:522 `RELEASE_ASSERT(lite->tid)` — tid 0 is never installed
GIL-off), so the ANNEX NL1 owner encoding (0 = free) needs no
carrier-special case. Carrier parks on NL follow the same ANNEX NL1
§A.3-compliant loop as spawned lites.

Flag-off and GIL-on processes never reach the byte at all (level-0
discriminator, §4.1); a GIL-on VM's lites coexisting in a gilOff
process emit 0 (vm.m_gilOff is per-VM). Today's code is emitted
byte-for-byte in those modes (NA-I1).

### 3.2 Acquisition protocol (index; FULL pseudocode = history ANNEX NL1, BINDING)

NL is park-capable and safepoint-polling. Cited protocol: SPEC-ungil
§A.3.2/2b — threads park at poll sites on their own NVS ticket;
"every park site polls post-wake BEFORE re-acquiring access or
running JS/JIT" (§A.3.2b(ii)); the conductor proceeds when every
entered thread is parked / not-entered / access-released (§A.3.1-2,
per EXIT1).

1. Fast path: CAS acquire (owner = lite/tid + depth word). Reentrant
   per-thread via a per-lite `m_nativeLockDepth` (depth survives the
   §3.3 drops as a saved value, never counts across them).
2. Contended path: the wait loop is a §A.3-compliant PARK SITE
   (REVISED rev 2, round-1 finding R1.2 — the rev-1 body and annex
   disagreed on iteration order and the annex order had a
   wake-without-poll hole). Normative ordering, restated to match
   ANNEX NL1 exactly: each iteration (a) polls the lite's stop bit
   (§A.2.3 trap word); if set, parks on its own NVS ticket per the
   standard §J.3 protocol, then runs the §A.3.2b post-wake poll;
   (b) attempts the CAS; (c) on failure, parks on the NL ParkingLot
   bucket with a bounded deadline (ANNEX NL1 step (c)); EVERY wake
   from (c) — unpark or deadline — loops back to (a)'s stop poll
   BEFORE any further CAS attempt. acquire() NEVER returns without
   having run a stop poll after its last park of either kind
   (SPEC-ungil §A.3.2b(ii), SPEC-ungil.md:216-218). If the stop bit
   is observed set AFTER a winning CAS, the thread parks on its NVS
   ticket WHILE HOLDING NL — explicitly legal per NA-I10 (no
   conductor ever wants NL) — completes the post-wake poll, and only
   then returns. A thread blocked on NL therefore never blocks an
   §A.3 conductor, and never resumes a host body inside a stop
   window.
3. NA-I10 (conductor exclusion). §A.3 stop conductors, GC conductors
   (heap §10), and any heap rank 2-10b or api rank 1-3 holder NEVER
   acquire NL. This mirrors the SPEC-ungil §LK negative-edge style
   ("GC/§A.3 conductors acquire NO api lock"). Consequence: a holder
   may be safepoint-stopped WHILE holding NL without deadlock — the
   conductor does not want NL, and all NL waiters are parked
   compliantly per (2).
4. Tokens/heap access are KEPT while parked on NL (same rule as §A.3
   NVS parks).

### 3.3 Mandatory release around JS re-entry

THE load-bearing rule. If a locked native re-enters JS
(valueOf/toString coercion, getters/setters, direct callbacks) while
holding NL, the entire JS callback graph — including further locked
natives ON OTHER THREADS waiting behind it — serializes on NL: the
GIL regrows through the callback graph. Therefore:

NA-I11 (drop rule). NL is NEVER held across JS execution. Every JS
re-entry from native code releases NL fully (depth saved) before
entering JS and re-acquires to the saved depth (via the §3.2
park-capable protocol) before control returns to the native frame.
Funnel (REVISED rev 2, round-1 finding R1.3 — the rev-1
four-function list was falsified by the tree): the funnel is defined
by CALLEE, not by a fixed caller list — EVERY caller of
`vmEntryToJavaScript` / `vmEntryToJavaScriptWith0Arguments`
instantiates the drop scope (lint NA-T7 greps the caller set).
Known callers at this rev: the `Interpreter::executeCall`/
`executeConstruct`/`executeEval`/`executeProgram` JS arms (native
arms at interpreter/Interpreter.cpp:1319 and :1409 route the OTHER
direction), `Interpreter::executeCachedCall`
(interpreter/InterpreterInlines.h:100, direct `vmEntryToJavaScript`
at :127 — this is the path used by EXACTLY the natives that ship
Locked: Array.prototype.sort comparators
runtime/ArrayPrototype.cpp:950, String.prototype.replace functional
replacers runtime/StringPrototype.cpp:399-400, Map/Set/Iterator
helpers), `Interpreter::executeWith0Arguments`-style wrappers
(InterpreterInlines.h:158), `Interpreter::executeModuleProgram`
(interpreter/Interpreter.cpp:1662, entry at :1728 — reached from
moduleLoaderEvaluate and embedder module evaluation), plus the
microtask runner. The drop hook is a
RAII `NativeLockDropScope` keyed on the per-lite depth word
(`m_nativeLockDepth != 0` => save+release; destructor reacquires):
zero work when depth is 0, which is every carrier and every
flag-off/GIL-on configuration (NA-I1 preserved — the C++ check is
one TLS-adjacent load, and it is additionally gated on the same
mode test as §4.1, making it dead code GIL-on).

NA-I12 (exception safety of the drop). JSC propagates host
exceptions by RETURN + pending-exception state (per-lite GIL-off:
the `VMLitePrimitives::m_exception` word — the LLInt trampoline's
own post-call check, LowLevelInterpreter64.asm:3199-3217, and the
JIT thunk's mode-keyed `loadException`,
jit/ThunkGenerators.cpp:524-536), NOT by C++ unwinding through host
frames. Therefore the RAII destructor runs on every exit path of the
re-entry funnel, INCLUDING the exception-pending path, and MUST
re-acquire even when an exception is pending: the native frame above
it still executes (cleanup, scope checks) under its expected lock
state. Re-acquire on the exception path uses the same park-capable
§3.2 loop (a termination trap observed while re-acquiring parks
compliantly; it must not bypass re-acquisition — the frame above
will release to depth 0 on its own exit through the §4 brackets).

### 3.4 Blocking and heap-access transitions while holding NL

NA-I13. NL MUST NOT be held across a heap-access release/re-acquire
transition or an indefinite OS block. Heap-access transitions are
already required to be lock-free of api rank 1-3 (SPEC-ungil §E.2
lock/access rule); NL joins that exclusion. Mechanism: the access
release path debug-asserts `m_nativeLockDepth == 0` (release builds:
allowed but the §A.3-liveness story degrades to "waiters wait", never
deadlock — by NA-I10 nobody the conductor needs is behind NL).
Embedder natives that block (Bun I/O) wrap the blocking region in the
public `NativeLockDropScope` (same RAII as §3.3) — one mechanism,
two drop points.

### 3.5 Rank — proposed §LK row (ADOPTION GATE, §9)

Proposed insertion in the SPEC-ungil §LK merged process lock table
(SPEC-ungil.md:867-925), as row **LK.1c "NativeSerialLock"**:

- Position: inner to heap rank 1 (entry token / heap access — NL is
  acquired only while entered, NA-I9, and kept across §A.3 parks like
  the token); OUTER to api ranks 1-3, heap ranks 2-10b, and all
  leaves — because the locked native's BODY is arbitrary host code
  that may legitimately allocate (heap locks), use API surfaces
  (api locks), and take §K leaf locks.
- It is a LONG-HOLD lock in exactly the sense of the existing
  `NLS::m_lock` long-hold row (SPEC-ungil §LK "Long-hold" note):
  ordered outside heap 2-10 + api 1-3; acyclicity by negative edge —
  NO conductor, NO heap 2-9b holder, NO api 1-3 holder ever ACQUIRES
  NL (NA-I10). Difference from NLS: NL is NEVER held across parks
  that run user JS (NA-I11 forbids it), and never spans access
  transitions (NA-I13), so it needs no §E.2 rank-4 exemption.
- BOTH-SIDES RULE: if ANY edge of this row moves during review (e.g.
  NL made inner to an api rank, or a conductor is permitted to
  acquire it), the change MUST be recorded as a supersession in BOTH
  this spec and SPEC-ungil §LK, per the family convention. Until the
  SPEC-ungil owner lands the cross-cite, this row is
  SUPERSESSION-PENDING and implementation of §3 MUST NOT begin (§9).
- U20 (the SPEC-ungil lock-order lint) extends to NL edges.

## 4. Host-call check shape per tier

The check follows the established two-level gilOff discriminator
(UNGIL-HANDOUT §A.1.3: process-level `JSCConfig::gilOffProcess` byte,
then per-lite byte) and the in-tree mode-split pattern
(`vm.gilOff()` C++ arms, e.g. ThunkGenerators.cpp:482-491;
`group3Primitives()` accessors, interpreter/FrameTracers.h:43-50;
LLInt `branchIfGilOffGroup3To*` macros,
llint/LowLevelInterpreter.asm:497-636).

### 4.1 Common shape (normative)

On the gilOff-mode-split path only:

    if lite->nativeLockEligible        // L2 byte, §3.1; set on EVERY
                                       // lite of the gilOff VM (rev 2)
        if !executable->m_concurrentOk // §1.2 byte
            operationAcquireNativeSerialLock(lite)   // §3.2
    call host function
    if <acquired>                      // same two tests / saved flag
        operationReleaseNativeSerialLock(lite)
    <existing exception check>         // release BEFORE the
                                       // exception branch (NA-I12)

NA-I14 (placement). Release is emitted BEFORE the exception-check
branch on every emitter, so the exception arm (which jumps to
`_llint_throw_from_slow_path_trampoline` /
`operationVMHandleException` and never returns to the trampoline)
cannot leak a held NL. The acquire/release operations are C++ slow
calls; no inline CAS in emitted code in v1 (the locked path is by
definition not the hot path — that is the ratchet's bargain).

NA-I15 (cost; AMENDED rev 2 with NA-I9). GIL-on and flag-off
configurations emit today's code byte-for-byte; the gilOff()==false
/ gilOffProcess==0 arms are unchanged. ALL threads of a gilOff VM —
carriers included, per revised NA-I9 — pay the `load8+branch` of the
lite byte plus the executable-byte load on the already-mode-split
gilOff path, and take NL when the executable is Locked. Lites of a
GIL-on VM coexisting in a gilOff process pay only the lite-byte test
(byte 0, executable byte never loaded). Same cost discipline as the
existing mode splits (jit R1.e EXTENSION pattern,
UNGIL-HANDOUT:172-179: new branches test gilOff-family bytes, never
useJSThreads). The rev-1 "carriers pay one dead-byte test" claim is
superseded — it was the cost side of the unsound carrier exemption.

### 4.2 LLInt

`nativeCallTrampoline` (llint/LowLevelInterpreter64.asm:3161-3219):
the gilOff arm already exists (`branchIfGilOffGroup3ToT3` at the
pre-call topCallFrame store, :3175-3182, lite base in t3). Extension:
on the `.liteStoreTopCallFrame` arm, additionally `loadb
VMLite::nativeLockEligible[t3]`; if set, `loadb
NativeExecutable::m_concurrentOk[a2]` (a2 = executable, loaded at
:3165-3168); if clear, cCall the acquire operation around the host
call. Register notes are implementation detail BUT the constraint is
normative: the pre-call slow call must preserve a0-a2 (the host
call's argument set, see the :3171-3176 liveness comment) — spill
per the trampoline's existing conventions. The post-call
release+exception sequencing follows NA-I14: release happens before
the `.checkLiteException` test (:3211). `internalFunctionCallTrampoline`
(:3220) takes the NA-I8 unconditional arm (lite byte test only, no
executable byte).

### 4.3 Baseline/JIT thunk

`nativeForGenerator` (jit/ThunkGenerators.cpp:455-576): the thunk is
generated per-VM and cached, so the level-0 split is the C++
`vm.gilOff()` branch already used twice in this function (:481-491
topCallFrame, :551 exception arm) — GIL-on VMs get an unchanged
thunk. In the gilOff thunk: after the executable lands in
`argumentGPR2` (:502-512 JSFunction arm), emit `load8` of the lite
byte (lite via `loadVMLite`, the :481-491 pattern) and of
`offsetOfConcurrentOk()`; locked path does the
acquire `callOperation` (operation calls are already in this thunk's
vocabulary: `operationDebuggerWillCallNativeExecutable` :495,
`vmEntryHostFunction` under JITCage :510/:518). Release before the
`loadException` check (:535-536) per NA-I14, on BOTH the JSFunction
and InternalFunction arms (:514-521, per NA-I8). The same shape
applies to the construct-kind thunk (same generator, :460
`executableOffsetToFunction` switch).

### 4.4 DFG/FTL

No new emitter: DFG/FTL calls to host functions land on the
NativeExecutable's call thunk (§4.3) — covered. TWO obligations:

1. NA-I16 (inline-bypass closure; REVISED rev 2, round-1 finding
   R1.7 — rev 1 closed only the handleIntrinsicCall surface). THREE
   surfaces execute a native's semantics without reaching any §4
   bracket; each member of their UNION must be ConcurrentOk or have
   its bypass disabled in gilOff mode:
   a. `handleIntrinsicCall` (dfg/DFGByteCodeParser.cpp:2097, :2558)
      inlines selected natives without calling them.
   b. Specialized intrinsic call thunks: `VM::getHostFunction`
      passes `thunkGeneratorForIntrinsic` (runtime/VM.cpp:1283
      switch, consumed at :1435) and `hostFunctionStub` installs the
      specialized thunk as the executable's CALL CODE
      (jit/JITThunks.cpp:273-275 DirectJITCode) — charCodeAt, sqrt,
      bound-function call, etc. EVERY tier's calls land on the
      specialized fast path, which executes the native's semantics
      and reaches the §4.3-bracketed `nativeForGenerator` thunk only
      via the failure fallback (jit/SpecializedThunkJIT.h:173
      finalize). Rule: in gilOff configs, `getHostFunction` passes
      nullptr for the generator unless the executable's policy is
      ConcurrentOk — Locked executables get the bracketed generic
      thunk for ALL calls. `BoundFunctionCallIntrinsic`
      (VM::getBoundFunction, VM.cpp:1444-1458,
      boundThisNoArgsFunctionCall) is NOT in any PT1 seed and is
      explicitly flagged: either seed it via an NA1 row or its thunk
      is suppressed.
   c. DFG constant-InternalFunction lowering
      (dfg/DFGByteCodeParser.cpp:6080-6140; §2.5 caveat).
   Enforced by lint NA-T6 over the union of (a)'s Intrinsic set,
   (b)'s VM.cpp:1283 switch cases, and (c)'s classInfo list: a
   locked-but-bypassed member is a build error in gilOff test
   configs.
2. Constant-folding: when the callee is a known CallVariant, the
   compiler MAY fold the §4.1 byte tests (bit immutable, NA-I3) and
   either omit the bracket (bit=1) or emit unconditional
   acquire/release calls around the direct native call (bit=0).

### 4.5 C++ direct entry

`Interpreter::executeCall`'s and `executeConstruct`'s native arms
call hosts via `vmEntryToNative`
(interpreter/Interpreter.cpp:1319, :1409) without passing through
either trampoline. The same §4.1 bracket is added as an inline C++
helper at both sites (mode-keyed on the same lite byte; executable
reachable from the CallData/callee). Charter NA-T2 covers this path
explicitly because it is the easiest to forget (API-driven `JSC::call`
from a spawned thread).

### 4.6 Coverage closure

NA-I17 (REVISED rev 2, round-1 finding R1.7). The COMPLETE set of
surfaces where a native's semantics execute is: the four
emitters/sites above (LLInt trampolines, JIT thunk, DFG/FTL
constant-path, C++ entry) PLUS the §4.4.1 inline-bypass union
(specialized thunks, DFG intrinsic inlining, DFG constant-
InternalFunction lowering — governed by the NA-T6 lint, not by
brackets) PLUS the §2.6 custom-accessor dispatch (NA-I20 bracket).
Any new host-call emitter or inline-implementation surface added
later MUST add the bracket, join the NA-T6 lint set, or refuse
gilOff mode. Audit hook: `grep` charter in NA-T7 over FOUR token
families — `HostFunctionPtrTag` call sites, `cloopCallNative`,
`vmEntryToNative` callers, `CustomAccessorPtrTag` call sites — each
bracketed or exempt-cited (`vmEntryHostFunction` JITCage wrapper is
INSIDE the §4.3 bracket; `callHostFunctionAsConstructor` body runs
UNDER the construct-kind bracket of its caller; the §4.4.1 bypass
surfaces are exempt WITH the NA-T6 cross-reference — their fast
paths make no host call, which is exactly why a call-token grep
alone cannot see them). NA-T7 additionally greps
`vmEntryToJavaScript`/`vmEntryToJavaScriptWith0Arguments` CALLERS
for the NA-I11 drop-scope funnel (§3.3) so new JS entries cannot
silently escape it.

## 5. Ungating process (the ratchet)

Extends the K4/N7 audit style (SPEC-ungil-audit-{K4,N7}.md: executed
audit files, rows addressed `<file>.<table>.<row>`, classification
key up front, implementation consumes rows verbatim).

5.1 Audit artifact: `docs/threads/SPEC-nativeaffinity-audit-NA1.md`
(created at first flip; row template = history ANNEX AT1, BINDING).
Row `NA1.<group>.<n>` fields:

    native(s) [symbol + registration file:line] | kind(s) covered
    (call/construct) | shared state touched [each datum -> ruling:
    K4/N7/NA row id or OM/heap § cite] | JS re-entry sites in body |
    ICU APIs touched (Intl gate, NA-I7) | TSAN evidence [run id +
    config + corpus, zero races attributable to the native] | fuzzer
    evidence [campaign id, wall hours, the native demonstrably in
    corpus coverage, zero crashes attributable] | disposition
    {ConcurrentOk | Locked-keep | Locked-blocked-on <row>}

5.2 NA-I18 (flip discipline). A Locked->ConcurrentOk source change
MUST (a) cite its NA1 row in the change description, (b) land the
row in the same change or earlier, (c) carry BOTH evidence columns —
TSAN alone is insufficient (TSAN sees only exercised interleavings;
the fuzzer column exists to force exercise). ConcurrentOk->Locked
(regression response, incident response) requires NO evidence and
MAY land immediately; the row gains a revocation note. The PT1 seed
groups are NOT exempt: each PT1 group needs its NA1 rows executed
before `useJSThreads` ships default-on (they may land bit-set before
that, gated behind the flag).

5.3 TSAN/fuzzer substrate: the existing TSAN no-JIT target
(docs/threads/TSAN.md) + race amplifier (docs/threads/AMPLIFIER.md)
+ the thread-fuzz rig; NA-T5 defines the per-flip campaign shape.

## 6. Interaction with U-T8e hook dispositions

SPEC-ungil §E.1b.4 / U-T8e (SPEC-ungil:461-463): every
`globalObjectMethodTable`/host hook JS-reachable on a spawned thread
gets a disposition in {inline, carrier-queued, refused, unreachable}.
The concurrency bit COMPLEMENTS this; it does not replace or
reclassify it:

- The bit applies ONLY to hooks/natives with disposition **inline**:
  they execute on the spawned thread, so the §4 bracket governs them.
  An inline-disposition hook that is Locked is legal — it runs on
  the spawned thread under NL (still inline: same thread, same
  ordering, just serialized against other locked natives).
- **carrier-queued** hooks (e.g. promiseRejectionTracker spawned
  events, SPEC-ungil §E.1b.4/SD15) execute on a CARRIER at §F.1
  drains; under revised NA-I9 the carrier IS NL-eligible, so a
  Locked carrier-queued hook simply runs under NL like any other
  Locked native — correct, ordered, and on the thread U-T8e
  dispositions demand. (rev 1 said the carrier byte is 0 and NL is
  never touched on carriers; superseded with NA-I9.)
- **refused / unreachable** hooks never execute on spawned threads;
  the bit is moot.
- NA-I19 (no laundering). A disposition MUST NOT be downgraded from
  carrier-queued/refused to inline ON THE STRENGTH OF the native
  lock ("it's serialized now, so inline is fine"): U-T8e
  dispositions encode ORDERING and IDENTITY requirements
  (which thread observes the call), not just data races. Changing a
  disposition remains a SPEC-ungil-side supersession, both sides.

## 7. INV — numbered invariants (normative index)

- NA-I1  Zero serial cost GIL-on/flag-off: today's code
         byte-for-byte; gilOff-VM threads pay the §4.1 checks
         (§3.1, §4.1, NA-I15 as amended).
- NA-I2  Bit storage: whole `uint8_t` on NativeExecutable, JIT-
         addressable via `offsetOfConcurrentOk()` (§1.2).
- NA-I3  Bit immutable after ctor; flips are source changes (§1.2).
- NA-I4  One bit covers call + construct kinds (§1.2).
- NA-I5  Policy deterministic per HostFunctionKey; conflicting
         registration RELEASE_ASSERTs (§1.3).
- NA-I6  Default Locked at every creation funnel (§2.1).
- NA-I7  ALL Intl/ICU natives Locked in v1; ungating needs the ICU
         column (§2.3).
- NA-I8  InternalFunction natives unconditionally locked on eligible
         lites in v1 (§2.5).
- NA-I9  NL touched only by lites with `nativeLockEligible` =
         vm.m_gilOff — EVERY lite of the gilOff VM, carriers
         included (rev 2; §3.1).
- NA-I10 Conductors and heap-2..10b/api-1..3 holders never acquire
         NL (§3.2.3).
- NA-I11 NL fully released around every JS re-entry; reacquired to
         saved depth before native frame resumes (§3.3).
- NA-I12 Drop scope is exception-safe; reacquire happens on the
         exception-pending path too (§3.3).
- NA-I13 NL never held across heap-access transitions / indefinite
         blocks; embedder drop scope for blocking regions (§3.4).
- NA-I14 Emitters release NL before the post-call exception branch
         (§4.1).
- NA-I15 The check shape is load8+branch on the gilOff-mode-split
         path only, per the two-level discriminator (rev 2; §4.1).
- NA-I16 Every member of the inline-bypass UNION (DFG intrinsic
         inlining + specialized intrinsic thunks + DFG constant-
         InternalFunction lowering) is concurrent-ok or its bypass
         is gilOff-disabled (rev 2; §4.4).
- NA-I17 The surface set (§4 emitters + §4.4.1 bypass union + §2.6
         custom accessors) is closed; new surfaces must bracket,
         join NA-T6, or refuse (rev 2; §4.6).
- NA-I18 Locked->ConcurrentOk only with NA1 row + TSAN + fuzzer
         evidence; reverse flips free (§5.2).
- NA-I19 The bit never launders a U-T8e disposition (§6).
- NA-I20 Custom accessors are unconditionally Locked in v1; every
         CustomAccessorPtrTag invocation on eligible lites routes
         through an NL-bracketed slow path (rev 2; §2.6).

## 8. T — test charter (numbered)

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
  entry: LLInt-only (`--useJIT=false`), thunk
  (baseline), DFG/FTL hot loop (forced tier-up), C++ entry
  (API call from a spawned thread, §4.5), InternalFunction
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
  missed both.
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
  deadline expiring.
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
  BoundFunctionCallIntrinsic disposition.
- NA-T7 Surface-closure lint (rev 2). Source scan: every
  `HostFunctionPtrTag`-tagged call site, `cloopCallNative`,
  `vmEntryToNative` caller, and `CustomAccessorPtrTag` call site is
  bracketed/exempt-cited (NA-I17, NA-I20); every
  `vmEntryToJavaScript`/`vmEntryToJavaScriptWith0Arguments` caller
  instantiates the NA-I11 drop scope.
- NA-T8 Policy-conflict assert (rev 2). Registering one function
  pointer twice with conflicting NativeConcurrency RELEASE_ASSERTs
  (NA-I5) via the strong side table; covers the JITThunks path, the
  no-JIT path, AND the collected-then-re-registered sequence (force
  GC of the first executable between registrations — the assert
  must still fire).
- NA-T9 Mode-cost oracle. Byte-compare emitted LLInt/thunk code
  GIL-on and flag-off against a pre-change build (the SPEC-ungil
  §J-style oracle discipline) — NA-I1.
- NA-T10 U-T8e non-interference (rev 2). The carrier-queued
  promiseRejectionTracker corpus (SPEC-ungil §E.1b.4) runs unchanged
  with the bit machinery active; carrier NL acquisitions occur ONLY
  for Locked native bodies (counter instrumentation), and ordering/
  identity of the queued hooks is unchanged. (rev 1 asserted ZERO
  carrier NL acquisitions; superseded with NA-I9.)

## 9. Adoption gates (this spec is NOT in force until all close)

1. SPEC-ungil §LK row LK.1c landed by the SPEC-ungil owner with the
   both-sides cross-cite (§3.5). BLOCKS §3 implementation.
2. SPEC-jit gilOff-mode codegen note for the §4.3/§4.4 arms (an
   EXTENSION of the jit R1.e family, recorded both sides like the
   UNGIL-HANDOUT:172-179 pattern). BLOCKS §4 implementation.
3. SPEC-api note that the C API mints Locked executables (§2.4) —
   one-line cross-cite.
4. VMLite L2 append for `nativeLockEligible` ratified against the
   vmstate L1/L2 append-only layout rule (VMLite.h:234 "Append-only
   per L1/L2; nothing above moves").
5. Adversarial review loop to a clean pass per the family convention;
   review log -> history PART B.

## 10. History / annex index

`docs/threads/SPEC-nativeaffinity-history.md`:
- PART A (BINDING annexes): NL1 (NL acquisition/park pseudocode, full
  text), PT1 (seed policy table, full list), EX1 (re-entry drop scope
  + exception-safety full text), AT1 (NA1 audit row template).
- PART B (non-normative audit trail): revision log, review rounds.
