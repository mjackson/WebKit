# FLAT-GAP-EVIDENCE — why JS-flat W=16 is 4.7×–10.8× behind Java/Go

Evidence pack for the flat-arm wall-clock gap. All runs on `9791feb9b3d9`,
Release `WebKitBuild/Release/bin/jsc`, pinned GIL-off env
(`JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1
JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
JSC_useThreadGILOffUnsafe=1`), host quiet (1-min loadavg 0.7–2.1, 64
HW threads). Raw artifacts: `/tmp/flatgap/{perf{1,16}.data,
perf{1,16}-self.txt, eustack2.raw, gclog16.txt, osr1.txt, holdtime.js}`.

**Baseline (this session):**

| | W=1 flat | W=16 flat | Go W=1 | Go W=16 | Java W=1 | Java W=16 |
|---|---|---|---|---|---|---|
| total_ms | 5874 | 4785 | 1836 | 422 | 1974 | 976 |
| phaseA_ms | 3988 | 2222 | 1315 | 272 | 1319 | 496 |
| phaseB_ms | 603 | 289 | 408 | 50 | 507 | 264 |
| phaseC_ms | 87 | 283 | 17 | 12 | 30 | 106 |
| pc1+pc2+df+csC | 1196 | 1991 | — | — | — | — |

JS-flat / Go @ W=16 = **11.3×**. JS-flat / Java @ W=16 = **4.9×**. JS-flat
W=1 / Go W=1 = **3.2×** — i.e. ~⅔ of the W=16 gap is **single-thread
throughput**, not scaling. phaseA alone (the dominant phase) is 3.03× Go
at W=1 and **8.2× Go at W=16** (Go scales phaseA 4.83×, JS-flat 1.79×).

---

## (1) `perf record -g -F 997` — on-CPU top-20, W=16 vs W=1

W=16 (44 009 samples, ~44.1 cpu-s):

| self% | symbol | reading |
|---|---|---|
| 10.73 | `Interpreter::executeCallImpl` | C++→JS re-entry trampoline (`lock.hold` callback dispatch) |
| 5.78 | `Heap::writeBarrierSlowPath` | write barrier on every closure/env/array store |
| 5.76 | `lockProtoFuncHold` | host fn body: tryLock + getCallData + JSC::call |
| 5.53 | `operationValueSubProfiledOptimize` | **Baseline-JIT** arith slow path (DFG never stable) |
| 5.12 | `operationIteratorNextTryFast` | `for…of tf` Map iterator, **Baseline** slow path |
| 4.94 | `operationGetByValOptimize` | **Baseline** indexed-get slow path |
| 4.00 | `operationGetByIdGaveUp` | IC **gave up** → generic get (megamorphic disabled, §24 caveat) |
| 2.68 | `allocateCell<JSFunction>` | one **arrow-closure alloc per `lock.hold` call** |
| 2.57 | `operationCreateLexicalEnvironmentTDZ` | one env per closure |
| 2.49 | `operationArrayPush` | `pl.docIds.push` / `pl.tfs.push` (segmented butterfly grow) |
| 1.48 | `VM::trapsMaybeNeedHandlingForCurrentThread` | CheckTraps poll |
| 1.03 | `CompleteSubspace::allocateForClient` | shared-heap alloc slow path |
| 0.99 | `Heap::allocationClientForCurrentThread` | per-alloc TLS-ish lookup |
| 0.97 | `arithmeticBinaryOp<jsSub>` | generic sub (Baseline) |
| 0.93 | `JSCellButterfly::visitChildrenImpl` (HeapHelper) | GC mark |
| 0.89 | `iteratorOpenTryFastImpl` | `for…of` open |
| 0.89 | `allocateCell<JSArrayIterator>` | iterator alloc per `for…of` |
| 0.87 | `operationNewArrowFunction` | closure alloc |
| 0.83 | `operationWriteBarrierSlowPath` | barrier (JIT path) |
| 0.65 | `ButterflySpine::publicLength` | segmented array length read |

W=1 (6 363 samples, 6.4 cpu-s) — different shape, **more JIT, less Baseline**:

| self% | symbol |
|---|---|
| 7.99 | `[JIT]` (FTL postingsChecksumFlat hot loop) |
| 6.06 | `Interpreter::executeCallImpl` |
| 3.95 | `PropertyTable::findConcurrently` |
| 3.76 | `JSObject::getOwnNonIndexPropertySlot` |
| 3.71/3.59/3.55 | `[JIT]` (≈14% in JIT bodies) |
| 3.10 | `lockProtoFuncHold` |
| 2.89 | `operationGetByIdGaveUp` |
| 2.11 | `Structure::getConcurrently` |
| 1.88 | `JSObject::getPropertySlot<false>` |
| 1.53 | `Heap::writeBarrierSlowPath` |
| 1.37 | `operationIteratorNextTryFast` |

**Reading.** At W=16 the top-7 self symbols (40.9%) are all
**lock.hold-dispatch + Baseline-JIT operation slow paths**. The flat hot
loop is *not* running compiled code — it's bouncing between JIT stubs and
C++ operations. The W=1 profile shows the same disease (lockProtoFuncHold +
executeCallImpl + getByIdGaveUp + generic property gets ≈ 22%), but a
larger JIT-resident fraction because there are no concurrent jettisons.
Children: `executeCallImpl` is **66.0%** at W=16 — i.e. two-thirds of all
on-CPU samples have the `lock.hold` C++→JS re-entry on their stack; the
closure body is *never* inlined into the surrounding loop because a native
host function sits between them.

---

## (2) `eu-stack` off-CPU, W=16 — where do the 16 threads park?

35 process snapshots → 417 thread-stacks captured. Excluding idle helper
threads (AutomaticThread worklist, bmalloc scavenger):

| site | thread-samples | % of JS-relevant (n≈246) |
|---|---|---|
| ON-CPU (running) | 136 | 55.3% |
| `Condition.wait` (the JS barrier) | 92 | **37.4%** |
| `VMManager::notifyVMStop` (STW stop, via VMTraps) | 8 | 3.3% |
| `Lock.hold` contended park | 5 | 2.0% |
| GC collector running | 4 | 1.6% |
| allocator slow-path park | 1 | 0.4% |

**Reading.** The 16 JS threads spend ~37% of wall **parked at the
barrier** waiting for thread-0's serial sections (pc1+pc2+df+csC =
1991 ms / 4785 ms = 41.6% — matches). **JS `Lock` contention is ~2%** of
off-CPU time — K=128 shard locks vs W=16 means almost no waiting; the
adaptive-spin path absorbs the rest. STW/safepoint is ~5% combined. The
gap is *not* off-CPU; it's the on-CPU work being slow.

The 8 STW-stop stacks all enter through
`operationIteratorNextTryFast`/`operationHandleTraps`/`operationGetByIdGaveUp`
→ `VM::hasExceptionsAfterHandlingTraps` → `VMTraps::handleTraps` →
`VMManager::notifyVMStop` — i.e. the trap poll embedded in the **Baseline
operation slow paths** is the safepoint reach point.

---

## (3) `JSC_logGC=1` at W=16 flat

10 EdenCollection + 8 FullCollection = **18 cycles**. Sum of `p=` pauses =
**164.0 ms**, max 24.2 ms, mean 9.1 ms. Against total_ms 4539 = **3.6% of
wall**. Perturbation: total under logGC = 4539 ms vs 4677–4785 ms
unperturbed (i.e. *faster* under logGC — within run-to-run noise; logGC=1
does not flip the §34 fast-mode here because the flat arm doesn't allocate
the 51 Eden / 35 Full storm the spec arm does).

The "Requesting GC" lines all read `~33 MB allocated this cycle` against a
`33554432` budget; 18 cycles × 33 MB ≈ **600 MB total bytes allocated** —
overwhelmingly transient (closures, lexical envs, Map iterators,
iterator-result arrays, posting-slice copies). **GC pause is not the
ceiling** for the flat arm.

---

## (4) `Lock.hold` calls + per-call cost

`JSC_logJSLockContention=1` (existing instrumentation,
`LockObject.cpp:77-160`):

| | W=1 flat | W=16 flat |
|---|---|---|
| total `hold` acquires | **4 682 780** | 4 682 870 |
| spin-loop iterations | 0 | 1 645 027 |
| ParkingLot parks | 0 | 115 553 (2.5% of acquires) |
| hottest lock | — | lock#90, 987 parks |

Per-call cost micro (`/tmp/flatgap/holdtime.js`, W=1 GIL-off, 2 M iters
each, FTL-warmed):

| pattern | ns/call |
|---|---|
| `lock.hold(() => { x++ })` | **117.0** |
| `callFn(() => { x++ })` (pure-JS, FTL-inlinable) | 12.6 |
| `lock.hold(ingestBody)` (Map.get + 2× push) | 192.4 |
| bare `ingestBody` | 136.6 |

**Reading.** Uncontended `lock.hold` overhead ≈ **104 ns/call** above the
work — composed of (a) host-function call (no DFG inlining of native), (b)
fresh `JSFunction` + `JSLexicalEnvironment` alloc for the arrow closure,
(c) `getCallData` + `JSC::call` → `executeCallImpl` → `vmEntryToJavaScript`
re-entry, (d) `tryLock`. 4.68 M calls × 104 ns = **487 ms of pure
hold-dispatch overhead at W=1** (≈ 8% of W=1 wall). At W=16 the per-call
cost is *higher* (perf attributes 16.5% self ≈ 7.3 cpu-s = 1 559 ns/call to
`lockProtoFuncHold + executeCallImpl` self alone — cache contention on the
shared CodeBlock / VM state in `executeCallImpl`).

Code site: `Source/JavaScriptCore/runtime/LockObject.cpp:800`
(`lockProtoFuncHold`) → `JSC::call` at the epilogue. The closure body
*itself* tier-ups to FTL (the anonymous `#Cue8Xu`/`#Bw8AIg`/`#CTlXvn`
codeblocks all install FTL — see (5)), but the surrounding hot loop cannot
inline through the native trampoline, and every call allocates a fresh
closure (`operationNewArrowFunction` + `operationCreateLexicalEnvironmentTDZ`
= 6.2% self at W=16).

---

## (5) `JSC_verboseOSR=1` — do the flat hot loops reach FTL?

**No.** One W=1 rep (`/tmp/flatgap/osr1.txt`, 15 516 lines):

| function | highest tier installed | reopt count | note |
|---|---|---|---|
| `ingestDocFlat#Dar0Bb` | **DFG only** (8 distinct DFG installs) | 8 jettisons | never FTL; `optimizationDelayCounter` climbs 0→8 (backoff to 655 941) |
| `phaseAFlat#Aii9rM` | DFG | 0 | requests FTL once, never installs |
| `queryPointFlat#DGEVYE` | DFG | 1 | requests FTL, never installs |
| `phaseBFlat#A9sgFT` | DFG | — | 4 OSR-entries to DFG, never FTL |
| `postingsChecksumFlat#D3vRtn` | **FTL** | — | (serial section — does reach FTL) |
| `mix32#CDZV6E` | FTL | 1 | jettisoned once (`HadFTLReplacement`), 201 BadType exits at bc#4 |
| hold-closure `#Cue8Xu` (ingest body) | FTL | 0 | stable |
| hold-closure `#Bw8AIg` (queryPoint body) | FTL | — | exits bc#49 BadType |
| hold-closure `#CTlXvn` (copyPostings body) | FTL | — | exits bc#49 BadType |

`JSC_printEachOSRExit=1` exit-reason histogram (full W=1 run):

```
26 234  exit #39 (bc#368, BadType)   ← ingestDocFlat for-of destructuring
   201  exit #38 (bc#368, BadType)
   201  exit #3  (bc#4,  BadType)    ← mix32
   201  exit #133(bc#851, Overflow)  ← queryScoredFlat
   130  exit #92 (bc#851, Overflow)
   101  exit #25 (bc#293, Overflow)
   101  exit #13 (bc#138, Overflow)
   101  exit #123(bc#716, Overflow)
   100  exit #8  (bc#24,  InadequateCoverage) ← docSeed32 inlined
    …
```

**Reading.** `ingestDocFlat` OSR-exits **26 234 times** at `bc#368
BadType` — the `for (const [t, count] of tf)` Map-iterator destructuring
(bench.js:812). The DFG speculates on the iterator-result array's
IndexingType and misses (segmented/contiguous mismatch under
`useSharedGCHeap`, or the `[t,count]` entry array structure differing from
profile). Each DFG install survives ~100 calls then jettisons; after 8
rounds the exponential backoff pins it in **Baseline for the rest of
phaseA** — which is exactly the `operationValueSubProfiledOptimize` /
`operationIteratorNextTryFast` / `operationGetByValOptimize` /
`operationGetByIdGaveUp` storm in (1). This is **the** dominant
single-thread throughput sink; phaseA is 68% of W=1 wall.

---

## (6) Int32Array allocation under sharedGCHeap, W=16 flat

The flat arm's Int32Array surface is **static**: per-worker scratch
allocated once in `phaseAFlat` (`makeFlatScratch`, bench.js:777) + one
`Int32Array(V)` in `buildDfSnapFlat`. No grow path (the linked-list-in-
Int32Arrays design was withdrawn — bench.js:753-772). Count:

| | per W=16 run |
|---|---|
| `new Int32Array(N_BASE)` | 16 × 3 = 48 (28 000 elem each = 5.38 MB) |
| `new Float64Array(N_BASE)` | 16 × 2 = 32 (28 000 elem each = 7.17 MB) |
| `new Int32Array(V)` | 1 (65 536 elem = 0.26 MB) |
| **total typed-array allocations** | **81** |
| **total typed-array bytes** | **≈ 12.8 MB** |

vs ~600 MB total allocated (logGC). **Typed-array allocation is <2.2% of
bytes and a non-factor.** The allocation pressure is closures + lexical
environments + `JSArrayIterator` + iterator-result 2-tuples + `new Map()`
per doc + `slice()` copies in `copyPostingsFlat`.

---

## Synthesis (the 1-paragraph)

The flat arm is **not running in FTL**. `ingestDocFlat` (68% of W=1 wall)
OSR-exits 26 k times at `bc#368 BadType` on the Map-iterator `[t,count]`
destructure, jettisons its DFG codeblock 8×, and spends the bulk of phaseA
in **Baseline JIT** — perf at W=16 shows 26% self in Baseline `operation*`
slow paths (`ValueSub`, `IteratorNextTryFast`, `GetByValOptimize`,
`GetByIdGaveUp`, `ArrayPush`). On top of that, every one of the **4.68 M
`lock.hold` calls** allocates a fresh arrow `JSFunction` +
`JSLexicalEnvironment` and re-enters JS via the C++ `executeCallImpl`
trampoline (66% children, 11% self at W=16; ≈104 ns/call uncontended,
≈1.5 µs/call observed at W=16) — the closure body cannot inline into the
hot loop because a native host function sits between them. Off-CPU is
**not** the gap: JS-Lock contention is ~2%, STW GC pause is 164 ms (3.6%),
allocator parks <1%; 37% of JS-thread samples are parked at the barrier
waiting for the 1.99 s of thread-0 serial work — but that serial work is
itself the same Baseline-tier disease (pc1/pc2 do reach FTL, so the serial
1.99 s is mostly genuine 4.6 M-posting walk cost). Int32Array allocation is
81 objects / 12.8 MB total — negligible. The two named engine targets the
finders should attack: **(A)** the bc#368 BadType Map-iterator-destructure
speculation miss under `useSharedGCHeap` that pins `ingestDocFlat` in
Baseline (worth ≈ the entire 3× W=1 gap to Go), and **(B)** the
`Lock.prototype.hold` closure-dispatch path — either a DFG-intrinsic /
`lock`/`unlock` pair that lets the body inline, or sinking the arrow
allocation (worth ≈ 0.5 s W=1, more at W=16 from `executeCallImpl` cache
contention).
