# SPEC-nativeaffinity — per-NativeExecutable concurrency bit + native serial lock

Status: DRAFT rev 5 (2026-06-07; review rounds 1-4 applied — history
PART B). Frozen-spec conventions per the
SPEC-{heap,vmstate,objectmodel,jit,api,ungil}.md family: normative
clauses cite `file:line` (tree at branch jarred/threads as of this
rev) or a `SPEC-* §`; FULL-text overflow lives in
`docs/threads/SPEC-nativeaffinity-history.md` PART A as BINDING
annexes (ANNEX NL1/PT1/EX1/AT1/BL1/SC1/TC1/CF1); supersessions are recorded BOTH
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
correct-but-serial, audited natives are parallel". "Serial" covers NativeExecutable-backed natives ONLY — raw
methodTable hook pointers get NO NL coverage (NA-I24, §6). It means
serial across ALL JS-executing threads of the gilOff VM — carriers
included (NA-I9: under SPEC-ungil, GIL-off carriers run JS in
PARALLEL with spawned threads — SPEC-ungil §A.3.6/ANNEX A36 "GIL-off
EVERY thread uses a real carrier lite", SPEC-ungil.md:272 — so a
carrier-exempt lock would not serialize anything). Serial cost is
paid ONLY on threads of the gilOff VM calling locked natives;
flag-off and GIL-on configurations EXECUTE today's instruction
sequences unchanged (NA-I1 as RESTATED rev 4, R3.10 — see §7).

0.4 The bit COMPLEMENTS, does not replace, the U-T8e hook
dispositions (SPEC-ungil:461-463) — see §6; for raw
globalObjectMethodTable hooks the U-T8e audit is the SOLE N-mutator
safety story (NA-I24) — and complements the
§K/§N rulings: a native whose shared touches are ruled by N7 rows
still defaults to locked until ITS BODY is audited (§2.5 Intl
example).

## 1. The concurrent-ok bit

### 1.1 Carrier: NativeExecutable (function identity)

The bit lives on `NativeExecutable`
(runtime/NativeExecutable.h:33-105), NOT on a property descriptor and
NOT as a `PropertyAttribute`. Rationale (normative): concurrency
safety is a property of the FUNCTION IDENTITY — the same callable is
reachable via `.call`/`.apply`, bind products, stored references,
cross-thread-published `JSFunction`s (`getCallData`,
runtime/JSFunction.cpp:313) and the constructor path
(`getConstructData`, :483); a property-level attribute would be
bypassed by every one of those.
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

### 1.3 Policy key and consistency

`JITThunks::hostFunctionStub` interns `NativeExecutable`s in a weak
map keyed by `HostFunctionKey` — std::tuple of function,
constructor, visibility, NAME (jit/JITThunks.h:224; equal()
JITThunks.cpp:102). NA-I5 (REKEYED rev 5, R4.2/R4.6 — FULL text =
history ANNEX CF1.2, BINDING): the POLICY key is the
`(m_function, m_constructor)` TaggedNativeFunction PAIR, NOT the
full HostFunctionKey. Policy is a claim about the BODY, so it must
be name-independent: the name leg is a dynamic runtime String
(JSNativeStdFunction registers ONE pointer under per-instance
names, runtime/JSNativeStdFunction.cpp:55-58; JSCustomGetterFunction
keys per-property names, runtime/JSCustomGetterFunction.cpp:66), so
rev 4's full-key table (a) grew unboundedly, pinning
embedder-controlled Strings forever — "size bounded by distinct
host functions" was FALSE — and (b) let two ALIASES of one body
carry conflicting policy with no assert, making the Locked alias's
serialization vacuous (the §0.3 guarantee). The bit MUST be a
deterministic function of the pair; conflicting policy for one pair
RELEASE_ASSERTs (debug + release) — registration-order- AND
alias-independent, strictly stronger than rev 4.

Enforcement structure (rev 2, R1.4; REKEYED rev 5): a per-VM STRONG,
append-only `HashMap<PolicyKey, NativeConcurrency>` (leaf-rank lock;
bounded by distinct code-address pairs) written on first
registration, consulted by EVERY creation — all §2.1 funnels AND
exempt-cited direct sites. The JITThunks weak map is NOT the
enforcement structure (weak entries GC-timing-dependent,
JITThunks.cpp:262-268; the no-JIT arm has no map, VM.cpp:1440-1442).
MODE DISPOSITION (rev 5, R4.8): the table is active in EVERY
configuration by design — NA-I5 is a registration-determinism
invariant, not a gilOff-only one; cost = one leaf-lock consult per
host-function REGISTRATION (cold path), declared in NA-I1. NA-T8
tests it: JITThunks + no-JIT + collected-then-re-registered +
same-body-different-name/visibility conflicts.

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

### 1.5 Dispatch trampolines (NEW rev 5, R4.3)

NA-I25. A NativeExecutable whose function is a DISPATCH TRAMPOLINE —
body behavior determined by callee-cell or out-of-band state, not by
the function pointer — is PERMANENTLY Locked; NA1 rows for its pair
may only carry Locked-keep. In-tree instance: JSNativeStdFunction's
`runStdFunction` (runtime/JSNativeStdFunction.cpp:60-65 — downcasts
the callee cell, invokes the std::function stored ON the cell): the
§1.2 byte identifies only the trampoline; the semantic body is an
OPEN SET (every lambda ever installed under it), so the §1.4 audit
obligation is undischargeable and a flip would bless lambdas written
after the audit. The ANNEX AT1 template gains a mandatory "body
closed over key?" field (the auditor must establish the pointer
fully determines the audited body before ANY flip). Per-cell
affinity = NA-X6, post-v1. FULL text = ANNEX CF1.3, BINDING.

## 2. Default policy

### 2.1 Policy input: every creation (funnels + exempt-cited direct sites)

(REVISED rev 5, R4.1 filed 2x — rev 4's "exactly two constructors
of policy" was falsified: the tree has THREE
`NativeExecutable::create` callers. FULL text = ANNEX CF1.1,
BINDING.) `NativeExecutable::create` ITSELF carries the
`NativeConcurrency { Locked, ConcurrentOk }` parameter with a HARD
DEFAULT of Locked — NA-I6 extended from "every funnel" to "every
creation": un-plumbed direct creators are conservatively safe by
construction. Sites:

- Policy funnels (plumb the registration site's input):
  `VM::getHostFunction` (VM.cpp:1376, :1429) /
  `JITThunks::hostFunctionStub` (JITThunks.cpp:248, :253, :311);
  concurrent-ok is always an explicit opt-in at the registration
  site (greppable, diffable, audit-citable).
- Static property tables (.lut.h via create_hash_table /
  runtime/Lookup.h lazy reification): the LUT attribute grammar gains
  a `ConcurrentOk` marker; absent marker => Locked. The reification
  path plumbs it into the funnel above. (Mechanical; the marker is
  the unit the §5 audit flips.)
- Direct site, EXEMPT-CITED: `WebAssemblyFunction::create`
  (wasm/js/WebAssemblyFunction.cpp:101) mints a deliberately
  NON-interned CallIC-identity clone of the :99 funneled base.
  Disposition: Locked UNCONDITIONALLY (callWebAssemblyFunction is
  reachable on NL-eligible lites via .call/.apply/CallIC slow paths
  on wasm exports; the §4.5 wasm exempt-cite covers only the
  vmEntryToWasm CALL arm, not this creation); consults the NA-I5
  table like every creation — policy is per-PAIR (§1.3), so the
  diversified clones are one entry and cannot diverge from the
  funneled base. NA-T7 gains a NINTH token family:
  `NativeExecutable::create(` callers, each a funnel or
  exempt-cited — a future direct creator is a lint failure.

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
into ICU (Date locale paths, `localeCompare`, `toLocaleString`
family, locale case conversion), is Locked in v1 and NOT eligible
for PT1 seeding. ICU carries library-global state whose audit is
out of scope here. Deliberately REDUNDANT with SPEC-ungil row N7-U6
(which rules the ENGINE-side touches); NA-I7 serializes the BODIES
until an ICU-level audit exists (§0.4). Ungating any Intl native
additionally requires the §5 row to cite an ICU-thread-safety
argument per touched ICU API.

### 2.4 Embedder API (Bun)

- Bun links JSC internally and registers natives through
  `VM::getHostFunction`/`JSFunction::create`: the §2.1 parameter IS
  the embedder API. Bun marks its own audited natives ConcurrentOk at
  its call sites; Bun-side evidence lands in the same §5 table
  (rows NA1.E.*). REV 5 (R4.3): the opt-in exists ONLY for natives
  with DEDICATED function pointers — std-function-backed natives
  (JSNativeStdFunction shape) are permanently Locked per NA-I25;
  per-call-site policy is impossible at pair granularity (two sites,
  different lambdas, different policies => RELEASE_ASSERT), and
  name-splitting no longer even appears to help, by design (§1.3).
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
NL — conservatively locked, no per-instance bit; small set, not the
v1 hot path. CAVEAT (rev 2, R1.5): scoped to calls that actually
reach the InternalFunction trampoline/C++ entry — the DFG lowers
constant-callee InternalFunction constructions to plain graph nodes
WITHOUT any call (dfg/DFGByteCodeParser.cpp:6080-6140); that inline
set is NA-I16(c)/NA-T6-governed (§4.4): each member OM/heap-ruled,
list lint-pinned. NA-X1: mirror the byte onto `InternalFunction`
with the same policy funnel.

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

NA-I20 (v1 disposition — conservative, NA-I8-style; REGROUNDED
rev 3 R2.7, rev 4 R3.6; FULL enumeration + cites = history ANNEX
SC1.4, BINDING — body compressed rev 5 under the size cap, content
unchanged): on NL-eligible lites (NA-I9), every custom-accessor
invocation is NL-bracketed AT THE DISPATCH FUNNEL — the bracket
wraps the typed `FunctionPtr` INVOCATION, NOT "the slow-path
operation" (custom-ness is unknown until after lookup). Bracketed
funnels (normative, lint-pinned): `PropertySlot::customGetter`'s
`m_data.custom.getValue(...)` (runtime/PropertySlot.cpp:36-48) and
the put-side `customSetter(...)` invocations
(runtime/JSObject.cpp:1449-1493 + PutPropertySlot-driven sites).
Tag/symbol set = FIVE tags + TWO vmEntry symbols, ALL NA-T7 token
families (SC1.4 enumerates each with file:line). gilOff IC
suppression: the FOUR custom AccessCase kinds are NOT CREATED in
gilOff mode (bytecode/Repatch.cpp:711-715/:1251; slow-path-only
give-up state, no regeneration livelock; the
InlineCacheCompiler.cpp:3462 gilOff publish arm becomes unreachable
gilOff, retained GIL-on — superseded by suppression). The slow path
reaches the bracketed funnels (custom accessors unconditionally
Locked v1 — no executable byte exists). DFG/FTL direct-call nodes
are SEPARATE (§4.4.3, NA-I23). NA-X4: static-table marker, post-v1.
Gate §9.2 names this suppression (rev 4, R3.9).

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

Rationale (normative; FULL text = history round 1, R1.1): GIL-off
carriers execute JS in PARALLEL with spawned threads
(SPEC-ungil.md:272; JSLockHolder degrades per §B.1) — a
carrier-exempt NL would let an un-audited Locked body run
concurrently on main + spawned, falsifying §0.3 in the normal Bun
topology. Owner-word: carrier TIDs are NONZERO GIL-off
(JSLock.cpp:350/:522), so NL1's 0=free encoding needs no carrier
case; carrier parks follow the same NL1 loop.

Flag-off and GIL-on processes never reach the byte at all (level-0
discriminator, §4.1); a GIL-on VM's lites coexisting in a gilOff
process emit 0 (vm.m_gilOff is per-VM). Today's instruction
sequences execute unchanged in those modes (NA-I1, rev 4).

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
2. Contended path: a §A.3-compliant PARK SITE (REVISED rev 2,
   R1.2). Ordering NORMATIVELY IDENTICAL to ANNEX NL1 (BINDING full
   text): poll-first — each iteration (a) polls the lite's stop bit
   (§A.2.3 trap word), parking on its own NVS ticket per §J.3 if
   set; (b) attempts the CAS; (c) on failure parks on the NL
   ParkingLot bucket with a bounded deadline. EVERY wake — unpark
   or deadline — loops back to (a) BEFORE any CAS; acquire() NEVER
   returns without a stop poll after its last park of either kind
   (SPEC-ungil §A.3.2b(ii), SPEC-ungil.md:216-218); a winning CAS
   re-polls and, if the stop bit is set, parks on the NVS ticket
   WHILE HOLDING NL — legal per NA-I10. A thread blocked on NL
   never blocks an §A.3 conductor and never resumes a host body
   inside a stop window.
3. NA-I10 (conductor exclusion). §A.3 stop conductors, GC conductors
   (heap §10), and any heap rank 2-10b or api rank 1-3 holder NEVER
   acquire NL. This mirrors the SPEC-ungil §LK negative-edge style
   ("GC/§A.3 conductors acquire NO api lock"). Consequence: a holder
   may be safepoint-stopped WHILE holding NL without deadlock — the
   conductor does not want NL, and all NL waiters are parked
   compliantly per (2).
4. Tokens KEPT while parked on NL. Heap access: kept at §A.3
   JSThreads stops; at rule-8 GC stops the NVS park performs the F8
   MANDATORY revert (SPEC-ungil.md:289-298) — legal WITH NL held
   (rev 3, R2.6; NA-I13 §3.4).

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
Funnel (REVISED rev 3, R2.2 — rev 2's two-symbol enumeration
recurred the R1.3 bug one level down): defined by CALLEE over the
WHOLE JS-entry symbol FAMILY — every caller of any
`vmEntryToJavaScript*` symbol of the llint/LLIntThunks.h
declaration block (:39 bare + :46-52 `With0..With6Arguments`; that
block NORMATIVELY defines the family — a future With7 cannot
silently escape) instantiates the drop scope. NA-T7's symbol list
is GENERATED from LLIntThunks.h, prefix-matched. Known caller set =
ANNEX EX1 items 1-8 (BINDING; load-bearing:
`Interpreter::tryCallWithArguments`, InterpreterInlines.h:132-171 —
the sort comparator enters via With2Arguments, NOT
executeCachedCall, on ARM64/X86_64 — and the microtask arms, EX1
items 3-4). The drop hook is a
RAII `NativeLockDropScope` keyed on the per-lite depth word
(`m_nativeLockDepth != 0` => save+release; destructor reacquires):
zero work when depth is 0. Mode gating (REGROUNDED rev 4 R3.11;
rev 5 R4.5 — rev 4's `NativeLockDropScope(VMLite*)` signature
forced every call site to materialize the lite BEFORE the gate, a
TLS load per JS entry in flag-off processes contradicting R3.11's
own NA-I1 restatement): the DEFAULT ctor takes NO lite. Its FIRST
test is the level-0 `g_jscConfig.gilOffProcess` byte (the §4.1
discriminator); only on gilOff processes does it then resolve the
current lite INSIDE the ctor via `VMLite::currentIfExists()` (TLS
`t_currentVMLite`, runtime/VMLite.cpp:67; L4 accessor block
VMLite.h:333-345). Flag-off/GIL-on processes pay one predictable
global-byte branch per JS-entry funnel; TLS/lite/depth loads never
execute (NA-I1 as restated). EX1 site 8 alone uses the
EXPLICIT-LITE form — the §F.5 funnel is keyed on the THREAD's
gilOff-VM lite, not the entered VM's (BL1.2), and passes the
pre-swap lite it already holds (the setCurrent return value / LIFO
restore tuple); same body minus the TLS resolve, level-0 gate still
first (ANNEX EX1 amended to match, rev 5).

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

(REVISED rev 3, R2.1/R2.5/R2.6; index — FULL text = history ANNEX
BL1, BINDING.)

NA-I13. NL never held across a VOLUNTARY heap-access transition
(those paths debug-assert `m_nativeLockDepth == 0`) or an
indefinite block another mutator must release. EXEMPT from the
assert: (a) §J.3 park-site MANDATORY reverts — the rule-8 GC-stop
F8 revert (SPEC-ungil.md:289-298) is legal WITH NL held at an NL1
stop poll (BL1.1; rev 2's unqualified assert fired on every such
GC stop); (b) rev 5, R4.7 (blocker): the HBT4 conductor-bracket
transitions — release-access -> arbitration -> GCL, including the
§LK.4b loser park access-released (SPEC-ungil.md:240-247,
:873-886) — when the thread is a §K.5/heap-rule §A.3 conductor or
arbitration loser: a Locked native can legitimately reach
haveABadTime (SPEC-ungil §K.5 rule 5, SPEC-ungil.md:768-780;
conductor = caller) or a synchronous collection mid-body, WITH NL
held (BL1.6 — rev 4's sole exemption (a) made a SPEC-ungil-mandated
path fire the assert, the R2.6 bug class recurring). VOLUNTARY
native-side transitions remain forbidden and asserted.

NA-I21 (cross-VM nesting, R2.1; BL1.2). The §F.5 nested foreign-VM
entry funnel (UNGIL-HANDOUT :2262-2290; carrier-only, §F.6(e))
instantiates `NativeLockDropScope` keyed on the THREAD's gilOff-VM
lite at its F8-revert point — inside VM B no §3.3 caller can
release VM A's NL — reacquiring at LIFO restore; RELEASE_ASSERT
(not debug) depth==0 in the nested window. GATE §9.6
(SUPERSESSION-PENDING: §F.5/§F.6(e)/A36C + §F.6 IU NL-depth row).

NA-I22 (engine blocking natives, R2.5; BL1.3). The threads-API
natives — G11 (SPEC-api.md:15: `join()`, `lock.hold()`,
`cond.wait()`) plus the Lock/Condition/Thread/ThreadLocal family —
are seeded ConcurrentOk as ANNEX PT1.G (bodies = the
api-§5.9-audited lock discipline; NA1 rows still required, §5.2):
they never hold NL. Demotion to Locked requires internal drop
scopes around every blocking region and NLS::m_lock acquisition
(BL1.3 demotion rule).

Embedder natives that block (Bun I/O) wrap the blocking region in
the public `NativeLockDropScope` (§3.3). LIVENESS SCOPE (BL1.5;
supersedes rev 2's unscoped "never deadlock"): NA-I10 = CONDUCTOR
liveness only; MUTATOR liveness = NA-I11 + NA-I21 + NA-I22 + §3.5
negative edge, each with a constructible deadlock if violated.

### 3.5 Rank — proposed §LK row (ADOPTION GATE, §9)

Proposed insertion in the SPEC-ungil §LK merged process lock table
(SPEC-ungil.md:867-925), as row **LK.1c "NativeSerialLock"**:

- Position: inner to heap rank 1 (entry token / heap access — NL is
  acquired only while entered, NA-I9, and kept across §A.3 parks like
  the token); OUTER to api ranks 1-3, heap ranks 2-10b, and all
  leaves — because the locked native's BODY is arbitrary host code
  that may legitimately allocate (heap locks), use API surfaces
  (api locks), and take §K leaf locks.
- It is a LONG-HOLD lock in the sense of the `NLS::m_lock`
  long-hold row (SPEC-ungil §LK, SPEC-ungil.md:902-907): ordered
  outside heap 2-10 + api 1-3; acyclicity by negative edge — NO
  conductor, heap 2-10b holder, or api 1-3 holder ever ACQUIRES NL
  (NA-I10; range PINNED rev 4, R3.7: 2-10b VERBATIM, matching
  NA-I10/§3.2.3 — rev 3 wrote 2-9b here, copied from the narrower
  NLS row; heap L4's allocation back-edge makes 10a/10b sections
  non-lock-terminal, so the exclusion is STATED; §9.1 records the
  range verbatim). §E.2 exemption (rev 4, R3.8 — supersedes rev 3's
  "needs no §E.2 rank-4 exemption", contradicted by BL1.1's NL-held
  F8 revert at rule-8 GC stops, the shape §E.2's park-site clause
  polices and U20 lints): NL carries an NLS-style rank-4 carve-out
  LIMITED to §J.3 park-site MANDATORY F8 reverts/re-acquisitions
  (acyclic via NA-I10); VOLUNTARY transitions remain forbidden
  (NA-I13). NL is NEVER held across parks that run user JS
  (NA-I11). Both clauses join the LK.1c row text and the §9.1
  SUPERSESSION-PENDING scope (U20 gains the matching exemption).
  Conductor-HOLD clause (rev 5, R4.7 — the NLH1.4 analog NL
  lacked; FULL walk = ANNEX BL1.6, BINDING): an §A.3/GC conductor
  MAY hold NL on ENTRY — it never ACQUIRES it (NA-I10 addressed
  only acquisition). The resulting NL > §LK.4b-slot > GCL edges
  are acyclic because slot and GCL holders never acquire NL
  (NA-I10's negative edge); the arbitration-LOSER case (NL held
  across an access-released §LK.4b slot park for the winner's
  whole stop window) is live: NL waiters deadline to NVS parks
  within one quantum (NL1 rev-5 conductor paragraph), so the
  foreign stop's fan-out completes. The §LK.4b held-with
  amendment (long-hold NL excluded, NLS-style wording) joins the
  §9.1 SUPERSESSION-PENDING scope.
- LONG-HOLD vs LONG-HOLD edge (rev 3, R2.4; BL1.4 walks the cycle
  rev 2 left constructible). Pinned: **NLS::m_lock OUTER to NL**
  (the NLS > NL direction exists structurally: lock.hold runs user
  JS holding NLS::m_lock, SPEC-ungil.md:902-903, and the EX1 dtor
  reacquires NL inside fn's epilogue). Negative edge: **no NL
  holder ever blocks on NLS::m_lock or any G11 blocking primitive**
  — discharged by NA-I22/PT1.G; a future Locked native touching NLS
  state must use the internal drop scope. Companion SPEC-api §5.9
  row joins the §9.1 SUPERSESSION-PENDING scope.
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

NA-I15 (cost; AMENDED rev 2 with NA-I9; rev 4 R3.10: "byte-for-byte"
restated per NA-I1 — EXECUTED-path identity). GIL-on and flag-off
configurations execute today's instruction sequences; the
gilOff()==false / gilOffProcess==0 arms are unchanged. ALL threads of a gilOff VM —
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

DFG/FTL calls to host functions land on the NativeExecutable's call
thunk (§4.3) — covered EXCEPT the NA-I16 union below (rev 4, R3.3:
rev 3's "no new emitter" sentence was falsified by the DOMJIT
signature arm, which emits a direct CallDOM). Obligations:

1. NA-I16 (inline-bypass closure; REVISED rev 2 R1.7, EXTENDED
   rev 4 R3.3/R3.4). SIX surfaces execute a native's semantics
   without reaching any §4 bracket; each member of their UNION must
   be ConcurrentOk or have its bypass disabled in gilOff mode:
   a. `handleIntrinsicCall` (dfg/DFGByteCodeParser.cpp:2097, :2558)
      inlines selected natives without calling them.
   b. Specialized intrinsic call thunks (rev 2, R1.7):
      `thunkGeneratorForIntrinsic` (runtime/VM.cpp:1283 switch,
      consumed at :1435) installed as the executable's CALL CODE
      (jit/JITThunks.cpp:273-275 DirectJITCode); the §4.3 bracket
      is reached only via the failure fallback
      (jit/SpecializedThunkJIT.h:173). RULE: gilOff getHostFunction
      passes nullptr for the generator unless ConcurrentOk.
      `BoundFunctionCallIntrinsic` (VM.cpp:1444-1458) is not
      PT1-seeded: NA1 row or suppressed.
   c. DFG constant-InternalFunction lowering
      (dfg/DFGByteCodeParser.cpp:6080-6140; §2.5 caveat).
   d. DOMJIT signature dispatch (rev 4, R3.3 — filed 2x; FULL text
      = history ANNEX SC1.1, BINDING): `signatureFor` ->
      `handleDOMJITCall` (dfg/DFGByteCodeParser.cpp:2106-2114,
      :5244) emits CallDOM — a DIRECT, type-check-skipping call of
      `signature->functionWithoutTypeCheck`
      (dfg/DFGSpeculativeJIT.cpp:11603; ftl/FTLLowerDFGToB3.cpp:
      22055); keyed on `DOMJIT::Signature`, NOT an Intrinsic. RULE
      (mirrors b): gilOff getHostFunction passes nullptr for the
      signature unless ConcurrentOk — kills the NativeDOMJITCode
      arm AND starves `signatureFor`. $vm signatures PT1-LOCKED.
      NA-I23's CallDOMGetter row does NOT cover CallDOM.
   e./f. Intrinsic GETTER inlining, DFG + IC arms (rev 4, R3.4;
      FULL text = ANNEX SC1.2): `handleIntrinsicGetter`
      (dfg/DFGByteCodeParser.cpp:5263, dispatch :6743) and
      `IntrinsicGetterAccessCase`/`emitIntrinsicGetter`
      (bytecode/IntrinsicGetterAccessCase.cpp:37-48;
      InlineCacheCompiler.cpp:3575/:4536; admission
      `canEmitIntrinsicGetter` :4473) execute getter-native
      semantics with no call. No getter intrinsic is PT1-seeded:
      each member OM/heap-ruled and lint-pinned, else
      intrinsic-getter inlining is gilOff-disabled alongside b.
   Lint NA-T6 over the union: (a) Intrinsic set; (b) VM.cpp:1283
   cases; (c) classInfo list; (d) DOMJIT::Signature constructions
   + non-null-signature getHostFunction calls; (e)/(f)
   handleIntrinsicGetter switch + canEmitIntrinsicGetter set — a
   locked-but-bypassed member is a build error in gilOff test
   configs.
2. Constant-folding: when the callee is a known CallVariant, the
   compiler MAY fold the §4.1 byte tests (bit immutable, NA-I3) and
   either omit the bracket (bit=1) or emit unconditional
   acquire/release calls around the direct native call (bit=0).
3. NA-I23 (DFG/FTL direct custom-accessor calls; rev 3, R2.8 —
   reached neither §2.6's IC mechanism nor any C++ funnel).
   `CallCustomAccessorGetter`/`Setter`/`CallDOMGetter` lower to
   DIRECT calls of the retagged accessor pointer
   ("bypassedFunction": dfg/DFGSpeculativeJIT.cpp:11689/:11813/
   :11840; ftl/FTLLowerDFGToB3.cpp:15961-15989, :22171-22187; built
   from statuses at dfg/DFGByteCodeParser.cpp:6647/:7076/:5559).
   v1: in gilOff configs the byte-code parser DOES NOT BUILD these
   nodes (the access compiles as generic get/put reaching the §2.6
   funnels) — mirrors the §4.4.1b thunk suppression; chosen over
   the starvation argument because parser-side suppression is
   locally verifiable and lint-pinned (NA-T6 gains the three node
   kinds). Lowering files NA-T7-exempt WITH the NA-T6 cross-ref.

### 4.5 C++ direct entry

The caller set of `vmEntryToNative` is DEFINED by the NA-T7
vmEntryToNative token family (rev 4, R3.2 — rev 3's "both sites"
was falsified; the body text defines the surface, R2.2 convention).
Known members, THREE sites, each gaining the §4.1 bracket as an
inline C++ helper (mode-keyed on the lite byte; executable from the
CallData/callee): `Interpreter::executeCall` / `executeConstruct`
native arms (interpreter/Interpreter.cpp:1319, :1409) AND the
runJSMicrotask native arm (runtime/JSMicrotask.cpp:206,
CallData::Type::Native — reachable from plain JS on a spawned
thread via queueMicrotask(nativeFn)). The adjacent wasm arm (:204)
is EXEMPT-with-cite: spawned Wasm refused v1 (SPEC-ungil §I);
carrier JS-to-Wasm is not a host-native call. NA-T2 covers this
path + the microtask-native-callee cell (rev 4).

### 4.6 Coverage closure

NA-I17 (REVISED rev 3 R2.7/R2.8; rev 4 R3.1-R3.4). The COMPLETE
set of surfaces where a NativeExecutable-backed native's semantics
execute is: the four emitters/sites above (LLInt trampolines, JIT
thunk, DFG/FTL constant-path, C++ entry — the §4.5 caller set
DEFINED by the NA-T7 vmEntryToNative family) PLUS the §4.4.1
inline-bypass union a-f (governed by the NA-T6 lint, not by
brackets) PLUS the §2.6 custom-accessor dispatch funnels (NA-I20
bracket) PLUS the §4.4.3 DFG/FTL direct custom-accessor node family
(NA-I23, gilOff-suppressed). Raw methodTable hooks are EXCLUDED by
construction — they are not natives in this spec's sense and have
NO coverage (NA-I24, §6). Any new host-call emitter or
inline-implementation surface added later MUST add the bracket,
join the NA-T6 lint set, or refuse gilOff mode. Audit hook: `grep`
charter in NA-T7 over NINE token families (rev 4 R3.6; rev 5 R4.1)
— `HostFunctionPtrTag` call sites, `cloopCallNative`,
`vmEntryToNative` callers, `CustomAccessorPtrTag` +
`GetValueFuncPtrTag` + `PutValueFuncPtrTag` call sites, the
`.custom.getValue`/`customSetter(` call-expression tokens,
`vmEntryCustomGetter`/`vmEntryCustomSetter` symbol callers,
GetValueFuncWithPtrPtrTag/PutValueFuncWithPtrPtrTag call sites, and
`NativeExecutable::create(` CALLERS (rev 5 — the family that would
have caught the §2.1 wasm site; each caller a funnel or
exempt-cited) — each site bracketed or exempt-cited (exempt set +
rationale = ANNEX TC1 NA-T7; e.g. `vmEntryHostFunction` is INSIDE
the §4.3 bracket, the §4.4.1/§4.4.3 bypass surfaces are exempt WITH
the NA-T6 cross-reference). NA-T7 additionally greps callers of the
WHOLE `vmEntryToJavaScript*` family — symbol list GENERATED from
the llint/LLIntThunks.h:39-52 declaration block, prefix-matched
(§3.3, R2.2) — for the NA-I11 drop-scope funnel.

## 5. Ungating process (the ratchet)

Extends the K4/N7 audit style (SPEC-ungil-audit-{K4,N7}.md: executed
audit files, rows addressed `<file>.<table>.<row>`, classification
key up front, implementation consumes rows verbatim).

5.1 Audit artifact: `docs/threads/SPEC-nativeaffinity-audit-NA1.md`
(created at first flip). Row `NA1.<group>.<n>`; the field set =
history ANNEX AT1, BINDING (natives/kinds/shared state/cell state/
JS re-entry/ICU/TSAN/fuzzer/disposition/revocations + the rev-5
"body closed over key?" field, NA-I25).

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

- NA-I24 (rev 4, R3.1 — blocker; FULL text + cites = history ANNEX
  SC1.3, BINDING; supersedes rev 3's §6 NL claims for hooks): NL
  and the §4 brackets cover ONLY NativeExecutable-backed natives.
  Raw `globalObjectMethodTable`/host-hook pointers
  (GlobalObjectMethodTable.h:58-71) mint NO NativeExecutable
  ("Locked" is not REPRESENTABLE) and are direct member-pointer
  calls from VM internals (sites = SC1.3), reaching no §4 emitter,
  §2.6 funnel, or NA-T7 family: NO NL coverage; safety rests SOLELY
  on the U-T8e disposition audit (§0.4). Anti-laundering (inverse
  of NA-I19): a U-T8e INLINE disposition MUST NOT be granted on the
  strength of NL serialization — none exists for this family. A
  hook implemented by REGISTERING a NativeExecutable-backed host
  function is covered normally (Locked legal: runs on the spawned
  thread under NL).
- **carrier-queued** hooks (e.g. promiseRejectionTracker spawned
  events, SPEC-ungil §E.1b.4/SD15) execute on a CARRIER at §F.1
  drains as raw-pointer calls (VM.cpp:2265): NOT NL-serialized
  (rev 3's "runs under NL" superseded, R3.1); their safety argument
  is the SD15 carrier-drain ordering itself, per the U-T8e
  disposition.
- **refused / unreachable** hooks never execute on spawned threads;
  the bit is moot.
- NA-I19 (no laundering). A disposition MUST NOT be downgraded from
  carrier-queued/refused to inline ON THE STRENGTH OF the native
  lock ("it's serialized now, so inline is fine"): U-T8e
  dispositions encode ORDERING and IDENTITY requirements
  (which thread observes the call), not just data races. Changing a
  disposition remains a SPEC-ungil-side supersession, both sides.

## 7. INV — numbered invariants (normative index)

- NA-I1  Per-surface cost contract (RESTATED rev 5, R4.8 — rev 4's
         "non-GILOFF builds byte-identical outright" was false on
         the C++ axis: `gilOffProcess` is an UNCONDITIONAL field
         (JSCConfig.h:177, latch :79-96) and GILOFF_TLS exists only
         in llint (LLIntOfflineAsmConfig.h:187-191), so the
         EX1/§4.5/§2.6/§1.3 C++ surfaces compile and execute in
         every build). LLInt: non-GILOFF_TLS ASM byte-identical;
         new bytes confined to branchIfGilOffGroup3* arms (jit
         R1.e pattern). GIL-on per-VM thunks byte-identical. C++:
         flag-off/GIL-on pay one predictable gilOffProcess branch
         per EX1/§4.5/§2.6 funnel in ALL builds, plus one §1.3
         leaf-lock consult per host-function REGISTRATION (cold
         path; declared, not denied). gilOff-VM threads pay the
         §4.1 checks (NA-I15).
- NA-I2  Bit storage: whole `uint8_t` on NativeExecutable, JIT-
         addressable via `offsetOfConcurrentOk()` (§1.2).
- NA-I3  Bit immutable after ctor; flips are source changes (§1.2).
- NA-I4  One bit covers call + construct kinds (§1.2).
- NA-I5  Policy deterministic per (function, constructor) PAIR —
         not per HostFunctionKey (rev 5); conflicting registration
         RELEASE_ASSERTs, alias- and order-independent; table
         active in every configuration (§1.3).
- NA-I6  Default Locked at EVERY NativeExecutable creation — the
         parameter lives on `create` itself (rev 5); direct sites
         exempt-cited (§2.1).
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
- NA-I13 NL never held across VOLUNTARY heap-access transitions /
         indefinite blocks; exempt: rule-8 GC park-site reverts
         (rev 3; BL1.1) + HBT4 conductor-bracket/loser-park
         transitions when the thread is an §A.3 conductor or
         arbitration loser (rev 5; §3.4/BL1.6).
- NA-I14 Emitters release NL before the post-call exception branch
         (§4.1).
- NA-I15 The check shape is load8+branch on the gilOff-mode-split
         path only, per the two-level discriminator (rev 2; §4.1).
- NA-I16 Every member of the inline-bypass UNION a-f (intrinsic
         call inlining + specialized thunks + constant-
         InternalFunction lowering + DOMJIT/CallDOM + intrinsic
         getter DFG/IC inlining) is concurrent-ok or its bypass is
         gilOff-disabled (rev 4; §4.4).
- NA-I17 The surface set (§4 emitters + §4.4.1 union a-f + §2.6
         custom accessors + §4.4.3 nodes) is closed; methodTable
         hooks excluded by NA-I24; new surfaces must bracket,
         join NA-T6, or refuse (rev 4; §4.6).
- NA-I18 Locked->ConcurrentOk only with NA1 row + TSAN + fuzzer
         evidence; reverse flips free (§5.2).
- NA-I19 The bit never launders a U-T8e disposition (§6).
- NA-I20 Custom accessors unconditionally Locked v1; bracket wraps
         the FunctionPtr invocation at the C++ dispatch funnels;
         gilOff suppression = the four AccessCase kinds not created
         in bytecode/Repatch.cpp; five accessor tags + two vmEntry
         symbols lint-covered (rev 4; §2.6).
- NA-I21 §F.5 nested-VM entry drops the thread's gilOff-VM NL at
         the F8-revert point + RELEASE_ASSERT depth==0; gate §9.6
         (rev 3; §3.4/BL1.2).
- NA-I22 Threads-API/G11 natives seeded ConcurrentOk (PT1.G); never
         Locked without internal drop scopes (rev 3; §3.4/BL1.3).
- NA-I23 DFG/FTL custom-accessor nodes not built in gilOff configs;
         NA-T6-pinned; does NOT cover CallDOM (rev 4; §4.4.3).
- NA-I24 Raw methodTable/host-hook pointers have NO NL coverage;
         safety = U-T8e audit alone; inline dispositions never
         granted on the strength of NL (rev 4; §6/ANNEX SC1.3).
- NA-I25 Dispatch-trampoline executables (runStdFunction family)
         are PERMANENTLY Locked; AT1 "body closed over key?" field
         mandatory before any flip; ConcurrentOk opt-in only for
         dedicated function pointers (rev 5; §1.5/§2.4/CF1.3).

## 8. T — test charter (index; FULL charters = history ANNEX TC1, BINDING)

(MOVED to ANNEX TC1 rev 4 under the size cap; the annex text is
normative and includes every rev 2-4 arm.)

- NA-T1 Serialization witness: Locked body, N spawned threads +
  carrier arm — zero overlap; bit=1 control overlaps under the
  amplifier; TSAN arm clean BECAUSE of NL.
- NA-T2 Tier coverage matrix: LLInt / thunk / DFG-FTL / C++ entry /
  microtask-native-callee (rev 4, R3.2) / InternalFunction `new` /
  construct kind — each cell serializes.
- NA-T3 Re-entry drop: blocking + throwing `toString` proves
  NA-I11/NA-I12; CachedCall, module, small-arity (With2) and
  microtask-runner arms.
- NA-T4 Conductor liveness vs NL holder/waiters within watchdog;
  wake-mid-stop, lost-wakeup, multi-waiter, GC-stop-with-NL arms;
  rev 5: conductor-holds-NL arm (haveABadTime + sync GC from a
  Locked body; no NA-I13 assert; NL held at resume).
- NA-T11 Nesting + G11 liveness: §F.5 nested-window depth 0;
  join-vs-Locked-exit completes; contended hold/wait — no NA-I13
  assert.
- NA-T5 Flip campaign template per NA1 row (amplifier + TSAN no-JIT
  + ASAN JIT + thread-fuzz; evidence ids in the row, NA-I18).
- NA-T6 Inline-bypass lint over the §4.4.1 union a-f sets; rev 4:
  + signature-bearing executables (gilOff builds NO CallDOM /
  Call-with-signature nodes) + intrinsic-getter sets; NA-I23
  node-kind check.
- NA-T7 Surface-closure lint: NINE token families (§4.6; rev 5 +
  `NativeExecutable::create(` callers), each site bracketed/
  exempt-cited; every `vmEntryToJavaScript*` caller instantiates
  the drop scope; symbol list GENERATED from LLIntThunks.h:39-52
  with snapshot self-test (R2.2).
- NA-T8 Policy-conflict RELEASE_ASSERT via the strong side table:
  JITThunks + no-JIT + collected-then-re-registered + rev 5
  same-body-different-name/visibility alias arm.
- NA-T9 Mode-cost oracle (RECHARTERED rev 4 R3.10, rev 5 R4.8):
  GIL-on per-VM thunk byte-compare; arm-level LLInt diff (new bytes
  only behind branchIfGilOffGroup3*); (c) scoped to the ASM
  artifact — offlineasm output identical for non-GILOFF_TLS
  configs — plus a C++ branch-count/TLS-load oracle at the EX1
  funnels (whole-binary byte identity dropped; TC1).
- NA-T10 U-T8e non-interference: queued-hook ordering/identity
  unchanged; carrier NL only for Locked NativeExecutable bodies,
  NEVER for raw-pointer hook invocations (rev 4, NA-I24).

## 9. Adoption gates (this spec is NOT in force until all close)

1. SPEC-ungil §LK row LK.1c landed by the SPEC-ungil owner,
   both-sides (§3.5) — rev 3 scope: long-hold edge NLS::m_lock > NL
   + negative edge + SPEC-api §5.9 companion row (R2.4). rev 4
   scope (R3.7/R3.8/R3.9): negative-edge range heap 2-10b VERBATIM;
   the NL §E.2 rank-4 carve-out (§J.3 park-site MANDATORY F8
   reverts only — U20 gains the matching exemption); the §E.2-close
   depth==0 assert + ~VM m_word==0 assert ordered against EXIT1.9
   (NL1 teardown — SPEC-ungil-owned lifecycle text, previously
   edited with no gate). rev 5 scope (R4.7): the LK.1c
   conductor-HOLD clause + the §LK.4b held-with amendment
   (long-hold NL excluded; BL1.6). BLOCKS §3 implementation.
2. SPEC-jit gilOff-mode codegen note (jit R1.e family extension,
   both sides) covering BY NAME (rev 4, R3.9 — rev 3's wording
   bound none of the suppressions): (a) the §4.3/§4.4 bracket arms;
   (b) the §2.6 IC suppression (four AccessCase kinds,
   bytecode/Repatch.cpp); (c) the NA-I23 parser suppression;
   (d) the §4.4.1b/d generator/signature suppressions + (e)/(f)
   intrinsic-getter disposition. BLOCKS §4 implementation.
3. SPEC-api notes: (a) C API mints Locked executables (§2.4);
   (b) rev 3: threads-API natives are PT1.G ConcurrentOk (NA-I22) —
   api-owner ack that their lock discipline is the §5.9-audited set.
4. VMLite L2 append for BOTH NL1 fields — `nativeLockEligible` AND
   `uint32_t m_nativeLockDepth` (rev 4, R3.9: rev 3's gate silently
   dropped the depth word the rev-1 open-item list named) —
   ratified against the vmstate L1/L2 append-only layout rule
   (VMLite.h:234 "Append-only per L1/L2; nothing above moves").
5. Adversarial review loop to a clean pass per the family convention;
   review log -> history PART B.
6. (rev 3, R2.1) SPEC-ungil/UNGIL-HANDOUT owner lands the §F.5
   nested-entry NL drop obligation (NA-I21) + §F.6 IU NL-depth row,
   both-sides. BLOCKS §3 in any VM-nesting embedder topology.

## 10. History / annex index

`docs/threads/SPEC-nativeaffinity-history.md`:
- PART A (BINDING annexes): NL1 (NL protocol full text; rev-5
  conductor/loser paragraph), PT1 (seed table incl. PT1.G), EX1
  (drop scope + caller list; rev-5 no-lite default ctor +
  explicit-lite site-8 form), BL1 (blocking/nesting/liveness;
  rev-5 BL1.6 conductor-hold), AT1 (NA1 row template + rev-5
  "body closed over key?" field), SC1 (rev-4 surface closures),
  TC1 (full test charters), CF1 (rev-5 creation closure / policy
  key / trampolines).
- PART B (non-normative audit trail): revision log, review rounds.
