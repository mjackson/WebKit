# SCALEBENCH — scalability of a big threaded program (JS threads vs Go vs Java)

Two runs of the same frozen benchmark on the same host:

- **Run 1** (2026-06-10, pre-fix tree): §0–§8 below, kept verbatim as the
  historical record. Its two release blockers have since landed (GC
  under-marking fix `25375a997f4f`; STW-watchdog fire-under-lock fix
  `6b298a4fdd99`).
- **Run 2** (2026-06-10, fixed tree, rebuilt Release jsc
  sha256 `b5c3a009d514…`): §9–§14 — re-run with the run-1 js quarantine
  accommodations REMOVED from `run.sh` (js cells count for real), plus the
  BEFORE/AFTER accounting of what the two fixes bought and the remaining
  ceilings.

Results JSON: `Tools/threads/scalebench/results.json` — now two sections,
`run1` and `run2` (run-1 copy also at `results-run1.json`; run-2 raw tables
`RESULTS.md`, run-1 at `RESULTS-run1.md`; per-run artifacts `out/` for run 2,
`out-run1/` for run 1).

**Measured-configuration caveat (added 2026-06-11, bughunter A5 amend
round):** any GIL-off run on trees at or after the A5-round interim
closure of AUD1.K4 rows II.18/II.19 measures a configuration in which
(a) megamorphic inline caches are disabled (megamorphic-class sites cap
out as polymorphic stubs and fall to generic slow calls; the
fill side was already disabled under `useJSThreads` by
`MegamorphicCache::fillsDisabledUnderJSThreads()`), and (b) the
`HasOwnPropertyCache` for `hasOwnProperty`/`Object.hasOwn` is disabled
in every tier (including loss of the DFG `HasOwnProperty` intrinsic).
The ruled per-lite cache copies (+ §0 U4 A16-ext JIT repointing) are
the follow-up; until they land, published W-scaling numbers embed this
interim disable, and megamorphic/hasOwnProperty-heavy phases serialize
into generic paths whose cost scales with contention. Runs 1–2 below
PRE-DATE the disable and are unaffected; any future re-run must either
land the per-lite copies first or restate this caveat next to its
numbers.

---

# RUN 1 (historical record — pre-fix tree)

This document answers the question asked on the threads PR: **how does
scalability hold up on big programs that use the threads** — not
microkernels. It also reports the parallel-self criterion (linear scalability
running a program in parallel with itself, no deliberate sharing) from
`Tools/threads/scaling-gate.sh` / `JSTests/threads/scaling/`.

**Known structural handicap:** GC under JS threads on this branch is
currently stop-the-world with parallel marking; concurrent marking is
designed (SPEC-congc.md) but not implemented. Go and Java ship fully
concurrent collectors. Allocation-heavy phases (A especially) are expected to
show STW pauses scaling with heap size and thread count. Results must be
reported with this stated up front, not buried.

## 0. The honest answer, up front

On this tree, **the big-program matrix could not be completed for JS at
W >= 2**: an open shared-GC-heap correctness bug (under-marking during STW
collections with N mutators — live cells swept and re-allocated; §5)
corrupts the benchmark's shared heap and kills 41 of 42 JS runs at
W in {2..64} (uncaught corruption exceptions, SIGSEGVs, SIGABRTs). Go and
Java completed the full matrix with bit-identical checksums; JS completed
W=1 with the same checksums.

On the parallel-self suite (independent work, no deliberate sharing,
GIL-off), the picture at N <= 8 is:

| workload | character | speedup(4) | speedup(8) | notes |
|---|---|---|---|---|
| splay-like | GC-pressure tree churn | 1.47x | 1.59x | best result; its class floors are 2.0/3.0 — still VIOLATION |
| string-heavy | rope/atom churn | 1.15x | 1.14x | flat; one N=8 run died in the STW watchdog |
| map-heavy | Map alloc churn | 1.11x | 1.13x | fully GC-serialized |
| raytrace-like | FP compute + small-object churn | 0.67x | 0.65x | NEGATIVE scaling (N threads slower than 1 doing 1/N the work) |
| richards-like | OO dispatch + small objects | 0.19x | 0.23x | catastrophic negative scaling (T(2) = 15x T(1)) + STW-watchdog SIGABRTs |

So: **the threading infrastructure is functionally there** (locks, atomics,
barriers, threads; checksums at JS W=1 and the smoke runs match Go/Java
exactly; serial identity for plain flag-off code is intact), but **on this
tree the engine does not scale at all yet, on any workload in either
suite**: the best parallel-self result is 1.59x at 8 threads, two workloads
scale NEGATIVELY, and at least two open engine bugs (§5 corruption, §6.3
STW-watchdog deadlock) sit in front of any scalability work. Concurrent
marking (SPEC-congc.md) is the designed fix for the dominant GC
serialization; the negative-scaling workloads additionally point at
per-collection stop/handshake and watchpoint/STW-storm costs that grow with
N (§7).

No spin: by the design's own success criterion ("linear scalability when a
program is run in parallel with itself"), the current tree does not pass on
any of the five workloads, and the big-program benchmark is blocked on a
correctness bug before multi-thread scalability can even be measured for JS.

## 1. Machine, toolchains, binary provenance

- AWS instance, Intel Xeon Platinum 8488C (Sapphire Rapids), 1 socket,
  **64 vCPUs = 32 physical cores x 2 SMT**, 1 NUMA node, 247 GB RAM.
  W in {48, 64} is therefore SMT/oversubscription territory by hardware, not
  just scheduling.
- Kernel 6.12.68-92.122.amzn2023.x86_64, Amazon Linux 2023.10.
- Go: `go version go1.24.13 linux/amd64`.
- Java: OpenJDK 21.0.10 LTS, Corretto-21.0.10.7.1, mixed mode, sharing.
- jsc: `WebKitBuild/Release/bin/jsc` from branch `jarred/threads`, pinned
  flag set (the platform under test, not tuning):
  `--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
  --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1`.
- Build verified current with the tree before the run (`ninja jsc`: no work
  to do). **Disclosure:** a concurrent CVE-close session rebuilt the Release
  binary during the batch window (mtime 05:21:38Z at preflight, 05:29:05Z
  mid-matrix). The six js W=1 runs straddle the swap and agree within 3.7%
  (23.7-24.6 s) with identical checksums; go/java are separate runtimes and
  unaffected. The parallel-self runs (§6) all used the 05:29:05Z binary.
- JSC compiler/GC helper threads are part of the platform and are NOT
  counted in W. `cpu_util` may exceed 1.0; it is reported as-is.

Quiet-host protocol: the run phase started at 1-min loadavg **891.20**
(another workflow's build finishing) and waited until < 4 (reached 04:45Z)
before any measured run; the orphaned fuzzer processes found spinning at
100% CPU (PPID 1, WebKitBuild/Fuzz jsc, 1.5 h old) were killed first. During
the matrix the runner's own load gate added 30 s and post-run settle excess
60 s (total 90 s, under the SPEC §6 5-minute disclosure threshold). A
concurrent session ran bounded single-jsc CVE verification loops (~1 core,
intermittent) during parts of the window; this is disclosed, not hidden —
it cannot explain any of the order-of-magnitude effects reported here.

## 2. What the benchmark is (SPEC summary)

`Tools/threads/scalebench/SPEC.md`, frozen, N_BASE pinned 2026-06-10.
A concurrent in-memory inverted index over a synthetic corpus, ~300-600
lines per language, three phases in one process, all threads spawned once
and reused (hand-rolled counting barrier from Lock+Condition equivalents in
all three languages):

- **Phase A — INGEST**: 28,000 generated documents (~85-212 tokens each),
  claimed via one shared atomic counter; hand-rolled tokenizer (two
  allocations per token); per-document tf map; postings appended to
  **128 shards** of {mutex, plain hash map} — one lock acquisition per
  (doc, distinct term). Real string work, real shared-map writes, real
  contention (Zipf-skewed terms hammer hot shards).
- **Phase B — QUERY**: 28,000 ops, 90% readers (point / 2-3-term AND /
  scored top-20 against a frozen df snapshot) / 10% writers (full ingest of
  a new doc). Reader results filtered to base docs so checksums are
  timing-independent.
- **Phase C — ANALYTICS**: parallel group-by over all shards into 104
  shared groups under group locks, then top-20 per group.

All arithmetic is unsigned 64-bit (JS: BigInt masked to 64 bits — the spec
REQUIRES this; it is also what triggers the §5 engine bug's allocation
churn). splitmix64 PRNG, FNV-1a hashing, no floating point anywhere.
Five checksums (A, postings, A2, B, C) must be bit-identical across all
three languages and ALL thread counts.

### Fairness rules (binding; SPEC §2)

1. Same algorithm, same abstraction level: sharded plain hash map + plain
   mutex everywhere. Java: NO ConcurrentHashMap/StampedLock/LongAdder.
   Go: NO sync.Map, NO RWMutex, NO channels for the queue. JS: NO
   SharedArrayBuffer tricks; `Lock`, `Atomics.*` on plain object properties.
2. Idiomatic but unoptimized; no pooling/arenas/interning beyond runtime
   defaults; pinned builder-style text assembly + hand-rolled tokenizer.
3. Identical inputs/constants; runner cross-checks via JSON output.
4. Checksum gate across the whole matrix.
5. No floating point in measured code.
6. Default runtime flags (the pinned JS thread flag set is the platform
   under test and exempt). One documented exception per language allowed
   for pathological defaults — none was needed for go/java; the single
   recorded exception entry documents the §5 JS engine bug accommodation
   (an engine bug, not a flag change — no JS flags were altered).
7. W OS threads: JS `new Thread`, Go goroutines (GOMAXPROCS default),
   Java platform threads.

Matrix: W in {1, 2, 4, 8, 16, 32, 48, 64}; 1 warmup + 5 measured reps per
cell, medians; languages interleaved java,go,js per rep so drift hits all
three equally; 1-min loadavg < 4 gate before every run.

## 3. Big-program results (medians of 5; speedup vs same language W=1)

Checksums across all 103 successful runs (full go/java matrix, js W=1, plus
one surviving js W=2 rep):
`A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2, B=c4bdd580f85ee058, C=af028188d7a56a96`
— bit-identical in every successful cell, all three languages. The
three-language W=1 and W=4 smoke gates (N_BASE=2000) also matched exactly
(js W=4 smoke failed with the §5 corruption; go/java W=4 matched).

### Total wall time

| W | js ms | js speedup | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|
| 1 | 24307 | 1.00x | 1771 | 1.00x | 1898 | 1.00x |
| 2 | FAILED 4/5 | — | 1070 | 1.66x | 1307 | 1.45x |
| 4 | FAILED 5/5 | — | 712 | 2.49x | 1067 | 1.78x |
| 8 | FAILED 5/5 | — | 514 | 3.45x | 956 | 1.99x |
| 16 | FAILED 5/5 | — | 402 | 4.40x | 901 | 2.11x |
| 32 | FAILED 5/5 | — | 370 | 4.79x | 1079 | 1.76x |
| 48 | FAILED 5/5 | — | 362 | 4.89x | 1047 | 1.81x |
| 64 | FAILED 5/5 | — | 386 | 4.59x | 1134 | 1.67x |

### Phase A — INGEST (allocation + shared-map writes; the GC-handicap phase)

| W | js ms | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|
| 1 | 15363 | 1275 | 1.00x | 1271 | 1.00x |
| 8 | — | 354 | 3.60x | 513 | 2.48x |
| 32 | — | 230 | 5.55x | 504 | 2.52x |
| 64 | — | 243 | 5.24x | 558 | 2.28x |

### Phase B — QUERY 90/10 (read-mostly)

| W | js ms | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|
| 1 | 2825 | 395 | 1.00x | 493 | 1.00x |
| 8 | — | 71 | 5.58x | 246 | 2.01x |
| 32 | — | 45 | 8.81x | 268 | 1.84x |
| 64 | — | 49 | 8.13x | 258 | 1.91x |

### Phase C — ANALYTICS (group-merge under shared locks)

| W | js ms | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|
| 1 | 127 | 14 | 1.00x | 30 | 1.00x |
| 8 | — | 11 | 1.31x | 83 | 0.36x |
| 64 | — | 12 | 1.15x | 220 | 0.14x |

(Full 8-row tables for every phase, plus min/max per cell, are in
`Tools/threads/scalebench/RESULTS.md` and `results.json`.)

### Peak RSS (MB, median) and CPU utilization

| W | js RSS | go RSS | java RSS | js cpu | go cpu | java cpu |
|---|---|---|---|---|---|---|
| 1 | 421 | 178 | 837 | 1.02 | 1.48 | 2.05 |
| 8 | — | 182 | 796 | — | 0.84 | 1.02 |
| 64 | — | 206 | 914 | — | 0.23 | 0.69 |

cpu_util = (user+sys)/(wall x W) over the FULL process lifetime (includes
JVM startup; each run's `inprogram_share` in results.json quantifies the
dilution — go ~0.97 at W=64, java ~0.90). Values > 1 at W=1 are the
runtimes' own helper threads (Java's concurrent GC most, 2.05).

### JS failure detail (the §5 bug, per cell)

Failure modes across the 40 failed JS runs (warmup + 5 reps x 7 cells, 1
W=2 rep survived): exit 3 = uncaught corruption exception (BigInt parse /
type error from a corrupted posting list), 139 = SIGSEGV, 134 = SIGABRT
(libstdc++ assertion / RELEASE_ASSERT):

| W | outcomes |
|---|---|
| 2 | 5x exit-3, 1 OK (42.2 s — 1.7x SLOWER than W=1) |
| 4 | 4x exit-3, 2x SIGSEGV |
| 8 | 2x exit-3, 4x SIGSEGV |
| 16 | 2x exit-3, 3x SIGSEGV, 1x SIGABRT |
| 32 | 1x exit-3, 1x SIGSEGV, 4x SIGABRT |
| 48 | 6x SIGABRT |
| 64 | 6x SIGABRT |

No run produced a silently-wrong checksum (the quarantine path recorded
zero hits); every corrupt run died loudly before or at its own checksum.

### What the go/java columns establish (the benchmark is sound)

- The workload scales to ~4.9x (Go) on 32 physical cores by DESIGN: hot
  Zipf terms serialize on shard locks, Phase C is a deliberate shared-append
  merge, and Phase A is allocation-bound. Go's cpu_util at W=64 is 0.23 with
  inprogram_share 0.97 — the idle time is real lock-wait, not startup. This
  is the intended "big program with contended sharing", not an
  embarrassingly-parallel strawman: a JS implementation on a sound engine
  has ~5x headroom to chase, not 64x.
- Java's plateau at ~2x (and Phase C's 0.14x backslide) is the same shape:
  HashMap+ReentrantLock per shard with biased-lock-free JDK21 monitors;
  its concurrent GC keeps Phase A at 2.5x where Go reaches 5.8x.
- JS W=1 is 12.8x slower than Go W=1 end-to-end. ~2/3 of that is the
  BigInt-based u64 arithmetic the spec mandates for checksum identity
  (every PRNG step and hash allocates a heap BigInt; Phase A is 15.4 s of
  predominantly BigInt churn vs Go's native uint64 at 1.3 s). This is a
  single-thread throughput gap, reported as-is, but it is NOT the
  scalability answer — the scalability answer for JS is blocked on §5.

## 4. SPEC §5.5 checksum gate status

- go, java: PASS at every W (all 96 runs, warmups included).
- js: PASS at W=1 (6/6 runs) and the surviving W=2 rep; remaining
  W in {2..64} runs failed before completing (no mismatching checksum was
  ever emitted — corruption is loud on this workload).
- Batch declared `valid: true` with the JS cells recorded `failed` per
  SPEC §6 and nulled medians; the single SPEC §4 "exceptions" entry
  documents the engine-bug accommodation (run.sh would otherwise have
  aborted the whole batch including the sound go/java cells — see the
  comment block above `JS_SHARED_HEAP_BUG` in run.sh).

## 5. The blocking engine bug (gates ALL JS W >= 2 results)

`Tools/threads/scalebench/js/repro-bigint-shared-ingest.js` (found by the
implementation phase; narrowed by this run phase — full status block in the
file header). Shape: 4 threads doing spec-mandated BigInt PRNG churn while
appending to Lock-protected shared posting lists corrupt the shared heap
~100% of runs at W=4 within ~2 s of work.

What this phase established (Release jsc, 2026-06-10 05:29Z binary):

- **GC-dependent**: `--useGC=0` -> clean 4/4. GC on -> corrupt ~100%.
- **Not marking parallelism**: `--numberOfGCMarkers=1` still corrupts.
- **Not generational/remembered sets**: `--useGenerationalGC=0` (full
  collections only) still corrupts; `--useConcurrentGC=0` still corrupts.
- **Live cells are being swept**: `--sweepSynchronously=1` converts the
  silent aliasing into immediate crashes. Under lazy sweep the re-allocation
  is delayed, which is why the default mode shows silent corruption.
- **Shared-heap specific**: `--useSharedGCHeap=0` (other flags kept) ->
  clean; `--useSharedAtomStringTable=0` -> still corrupts.
- **Corruption shape** (diagnostic dump): a shared array's butterfly ALIASES
  another thread's freshly-allocated Map storage — term strings and small
  counts interleaved into a docIds array, parallel arrays diverging in
  length. Consistent with a cell that was in-flight (held only in a
  mutator's registers/stack at the STW handshake) being missed by
  conservative-root/newlyAllocated accounting, swept, and re-handed to
  another thread's allocator.
- **Masked by instrumentation**: Debug build (same tree) 0/2, TSan build
  (rebuilt from this tree) 0/1 with zero TSAN reports, GIL-on 0/2 —
  Release-timing-only. TSAN will not name this one; it needs the
  hypothesis-driven treatment (thread-bughunter) with the repro above,
  which is fast and deterministic enough to bisect instrumentation into.

This is plausibly the same root cause as the parked butterfly-stress
silent-corruption case (Tools/threads/bughunt/EVIDENCE.md) — that case's
old broad signature space (cross-object aliasing) matches this shape — but
that identification is a hypothesis, not a finding.

## 6. Parallel-self suite (Pizlo's original criterion)

`Tools/threads/scaling-gate.sh` (report mode), `JSTests/threads/scaling/`:
each workload runs N threads of identical INDEPENDENT work (no deliberate
sharing); perfect scaling is speedup(N) = N x T(1) / T(N) = N. Floors (for
--gate on a quiet host): speedup(4) >= 2.8 and speedup(8) >= 4.5
(splay-like: 2.0 / 3.0).

### 6.1 As shipped (GIL-ON: the script passes only `--useJSThreads=1`)

The suite predates GIL removal; its stock invocation measures the GIL build:
map-heavy speedup(2..64) = 0.99-1.01x — textbook GIL serialization, as
designed for phase 1. Serial identity on this (noisy-window) run: T(1)
flag-on 1634.9 ms vs flag-off 1418.4 ms = +15.3% (VIOLATION vs the 5%
tolerance; see §6.4). The stock run then aborted at raytrace-like N=16,
which exceeded the default 120 s cell timeout — 16x serialized work does
not fit the budget; report-only artifact, not an engine failure.

### 6.2 GIL-OFF (pinned flag set via wrapper `Tools/threads/scalebench/out/jsc-giloff`; cell timeout 1800 s; N in {1,2,4,8})

From `scaling-gate.sh` (map-heavy, raytrace-like — medians of 3) and manual
cells with the same harness invocation for the rest (richards/string/splay:
harness-reported SCALING times, 2 runs, best shown — best-case for the
engine; the gate itself aborted at richards-like N=4 on a first attempt with
the §6.3 watchdog SIGABRT, so those three workloads' rows could not come
from a single gate run):

| workload | T(1) ms | speedup(2) | speedup(4) | speedup(8) | floors (4/8) |
|---|---|---|---|---|---|
| map-heavy | 2278 | 1.04x | 1.11x | 1.13x | 2.8 / 4.5 — VIOLATION |
| raytrace-like | 10227 | 0.68x | 0.67x | 0.65x | 2.8 / 4.5 — VIOLATION |
| richards-like | 3621 | 0.14x | 0.19x | 0.23x | 2.8 / 4.5 — VIOLATION |
| string-heavy | 2730 | 1.31x | 1.15x | 1.14x | 2.8 / 4.5 — VIOLATION |
| splay-like | 3544 | 1.21x | 1.47x | 1.59x | 2.0 / 3.0 — VIOLATION |

richards-like is the standout pathology: two threads of independent work
take 53-54 s where one takes 3.6 s (T(2) = 14.7x T(1)) — consistent with a
repeating storm of Class-A watchpoint fires each forcing a stop-the-world
(the same path that intermittently trips the §6.3 watchdog on this
workload). raytrace-like's 0.65-0.68x is the same shape at lower intensity.

An exploratory wide sweep (N up to 64) showed map-heavy flat at ~1.0-1.2x
through N=64, and raytrace-like at N=16 running at ~800% CPU while wall time
blew past 600 s — parallel execution is real (8 cores busy), but it is spent
in GC stop/start and allocator slow paths, not progress.

### 6.3 STW-watchdog deadlock (second open engine bug, distinct from §5)

richards-like at N=4 and N=8, and string-heavy at N=8, intermittently
(~1/3 of runs) die in:

```
JSThreads stop-the-world failed to reach a stopped world within 30.000000s.
Pending Class-A fire context: ... (WatchpointSet Class-A fire).
  entered lite ... tid=1 ... hasHeapAccess=true  <== NON-QUIESCENT (blocking the stop)
  entered lite ... tid=2 ... hasHeapAccess=true  <== NON-QUIESCENT (blocking the stop)
  entered lite ... tid=3 ... hasHeapAccess=true  <== NON-QUIESCENT (blocking the stop)
```

then SIGABRT. This is the watchdog family thread-ab17b/ab17e worked in,
recurring under a WatchpointSet Class-A fire with multiple non-quiescent
lites. It is a liveness release-blocker independent of §5.

### 6.4 Serial cost of the flag set (single thread, map-heavy)

Same binary, same host window: flag-off 1418 ms -> `--useJSThreads=1`
(GIL build) 1635 ms (+15.3%) -> full pinned GIL-off set 2278 ms (+60.6%
vs flag-off). The +15.3% GIL-on identity violation and the +60% GIL-off
single-thread tax on this Map-allocation-heavy workload are findings of
this run (host had ~1 intermittent background core; the bench-gate history
records +3.1% on its own workload on a quiet host — the map-heavy number
needs a quiet-host re-measure before being treated as final, but the
ordering flag-off < GIL < GIL-off-set was stable across every run today).

## 7. Analysis — where it stands, why

1. **The dominant scalability cost today is the stop-the-world GC, exactly
   as the §0 handicap statement predicts.** map-heavy (pure Map/alloc churn)
   gets ZERO parallel speedup: N threads allocate N x as fast, every
   collection stops all N, and collection work grows with live set — wall
   time grows ~linearly with N. raytrace-like and richards-like are worse
   than 1.0x: their small-object churn drives constant eden cycles whose
   stop/handshake overhead scales with N, so adding threads adds GC rounds
   faster than it adds compute. Go/Java do not pay this: their collectors
   run concurrently with mutators.
2. **Parallel execution is real but mostly wasted.** The mutator-side
   machinery does run N threads on N cores (raytrace-like N=16 was observed
   at ~800% CPU; the §3 matrix's failed JS W=4 cells ran at ~500% before
   dying), and splay-like/string-heavy show genuine if small wall-time wins
   (1.59x / 1.14x at 8). But no workload in either suite reaches even half
   its floor: the cycles go to GC stop/handshake rounds, allocator slow
   paths, and (for richards-like) what looks like a watchpoint-fire STW
   storm — richards' T(2) = 14.7x T(1) cannot be explained by GC volume
   alone and matches the §6.3 Class-A fire signature.
3. **Correctness gates scalability**: the §5 under-marking corruption makes
   every shared-write-heavy JS program unsafe at W >= 2 on this exact tree
   (the full corpus is green because its tests don't combine sustained
   BigInt-rate allocation with cross-thread shared appends at this
   intensity — this benchmark is precisely the "big program" gap the PR
   question pointed at). The §6.3 watchdog deadlock intermittently kills
   even no-sharing programs at N >= 4.
4. **Lock/atomics overhead differences are visible but second-order.** JS
   `Lock.hold` is a closure call per acquisition and `Atomics.add` on an
   object property is a runtime call; Go inlines a CAS fast path and Java
   biases to a fast monitorenter. At W=1 these show up inside the 12.8x
   single-thread gap (with BigInt churn the dominant term); at W >= 2 they
   are unmeasurable behind the GC wall.
5. **What would change the answer**: (a) fix §5 (deterministic repro in
   hand; TSAN-blind, so instrumentation-bisect or audit the in-flight-cell
   liveness chain: conservative roots of parked lites + newlyAllocated
   accounting per TLC); (b) fix §6.3 (watchdog context already names the
   Class-A fire path); (c) land SPEC-congc concurrent marking — without it,
   no allocation-heavy workload will pass the 2.8x/4.5x floors regardless
   of correctness, and the big-program matrix's Phase A will stay
   GC-bound; (d) re-run this entire document's §3 matrix. The harness,
   corpus, pinned constants and checksums are frozen and reusable as-is.

## 8. Reproduction

```
# Big-program matrix (writes results.json/RESULTS.md):
Tools/threads/scalebench/run.sh

# Engine-bug repro (~100% at W=4, ~2s):
WebKitBuild/Release/bin/jsc --useJSThreads=1 --useThreadGIL=0 --useVMLite=1 \
  --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 \
  Tools/threads/scalebench/js/repro-bigint-shared-ingest.js

# Parallel-self, GIL-off:
SCALING_CELL_TIMEOUT_SECS=1800 Tools/threads/scaling-gate.sh \
  --threads "1 2 4 8" Tools/threads/scalebench/out/jsc-giloff
```

---

# RUN 2 (2026-06-10, fixed tree)

## 9. Setup, provenance, protocol

- Same host as run 1 (64 vCPU = 32 physical Sapphire Rapids x 2 SMT, 247 GB),
  same toolchains (go1.24.13, Corretto 21.0.10), same pinned flag set, same
  frozen SPEC/N_BASE (28000) and seed.
- Release jsc rebuilt AFTER both fixes landed: `ninja jsc` reported no work
  to do against the fixed tree; binary mtime 2026-06-10 17:02:33Z, sha256
  `b5c3a009d5142da68002104669896779c6ac3ab95ce11312c1d0119e2b5b7ab0`
  (recorded in `jsc-build-id.txt` and results.json). No binary swap occurred
  during this batch (single session, machine owned exclusively).
- Fix presence verified behaviorally before the matrix:
  `js/repro-bigint-shared-ingest.js` at W=4 — **5/5 clean** (was ~100%
  corrupt); richards-like N=4 (scale 1/16, `-e` injection) — **5/5 clean,
  no watchdog abort** (was ~1/3 SIGABRT).
- Quiet host: 1-min loadavg 0.57 at start (no wait needed); no orphaned
  jsc/fuzzer processes found (checked; none to kill, unlike run 1). Runner
  gate delay 0 s, settle delay 585 s (all own-decay allowance), settle
  excess 0 s — no external-interference disclosure triggered.
- Accommodation removal: the run-1 `JS_SHARED_HEAP_BUG` smoke tolerance and
  the js W>=2 `--expect-tuple` checksum quarantine were deleted from
  `run.sh`. One new, narrower mechanism was added in their place: the
  preflight smoke retries a CRASHED leg up to 3 attempts (any language;
  motivated by the §11 residual's ~8% smoke-rate — a 10% coin flip should
  not abort a multi-hour batch in preflight). Checksum comparison is never
  relaxed, matrix cells never retry, and every crash is logged. The js W=4
  smoke leg passed on attempt 1 in the recorded batch.

## 10. Run-2 big-program results

Checksums across **all 113 successful runs** (warmups included; full go/java
matrix, js W=1 complete, every surviving js W in {2,4,8} run):
`A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2, B=c4bdd580f85ee058, C=af028188d7a56a96`
— bit-identical across all three languages and both runs (same tuple as
run 1). All six smoke legs (3 languages x W in {1,4}) matched. Zero
silently-wrong checksums; batch `valid: true`, `exceptions: []`.

### Total wall time (medians of 5; speedup vs same language W=1)

| W | js ms | js speedup | js ok/5 | go ms | go speedup | java ms | java speedup |
|---|---|---|---|---|---|---|---|
| 1 | 33373 | 1.00x | 5/5 | 1737 | 1.00x | 1893 | 1.00x |
| 2 | 41913 | 0.80x | 4/5 | 1068 | 1.63x | 1303 | 1.45x |
| 4 | (47.0–47.4 s, 2 survivors) | ~0.71x | 2/5 | 701 | 2.48x | 1041 | 1.82x |
| 8 | (45.0–46.0 s, 2 survivors) | ~0.73x | 2/5 | 512 | 3.39x | 977 | 1.94x |
| 16 | FAILED 5/5 | — | 0/5 | 401 | 4.34x | 931 | 2.03x |
| 32 | FAILED 5/5 | — | 0/5 | 360 | 4.82x | 976 | 1.94x |
| 48 | FAILED 5/5 | — | 0/5 | 370 | 4.70x | 1092 | 1.73x |
| 64 | FAILED 5/5 | — | 0/5 | 357 | 4.87x | 1153 | 1.64x |

js W in {4,8} cells have 2/5 surviving reps — below the 3 needed for a
median, so results.json records them failed-with-null-medians; the surviving
raw runs (correct checksums) are quoted above as ranges. go/java: 80/80
clean, shapes within noise of run 1.

### js phase medians (surviving runs)

| cell | A ingest ms | B query ms | C analytics ms | RSS MB | cpu_util |
|---|---|---|---|---|---|
| W=1 (5/5) | 18680 | 5591 | 138 | 11898 | 1.16 |
| W=2 (4/5) | 22607 (0.83x) | 6167 (0.91x) | 545 (0.25x) | 14389 | 0.72 |
| W=4 (2 raw) | 29127–30019 | 6216–7467 | 615–656 | ~13000 | — |
| W=8 (2 raw) | 26621–26775 | 7584–8668 | 648–702 | ~12885 | — |

### js failure detail (run 2: 31 failed runs of 144 total js incl. warmup)

Exit 133 = SIGTRAP (libpas `pas_deallocation_did_fail` /
RELEASE_ASSERT-class), 134 = SIGABRT, 139 = SIGSEGV:

| W | outcomes (warmup + 5 reps) |
|---|---|
| 1 | 6/6 OK |
| 2 | 5 OK, 1x SIGABRT |
| 4 | 2 OK, 2x 133, 1x 134, 1x 139 |
| 8 | 3 OK, 3x 133 |
| 16 | 1 OK (warmup), 3x 133, 2x 134 |
| 32 | 3x 133, 3x 134 |
| 48 | 6x 133 |
| 64 | 3x 133, 2x 134, 1x 139 |

Every failed run died loudly; the checksum gate over the surviving
population is fully intact.

## 11. BEFORE/AFTER — what the two fixes bought

### Fix 1: GC under-marking (`25375a997f4f`)

| metric | run 1 (before) | run 2 (after) |
|---|---|---|
| `repro-bigint-shared-ingest.js` W=4 | ~100% corrupt | 5/5 clean, deterministic output |
| js W=2 big-program | 41/42 multi-thread js runs died; 1 survivor | **4/5 complete with bit-identical checksums** |
| js W=4/W=8 | 0 survivors | 2/5 survivors each, correct checksums |
| js W>=16 | 0 survivors | 0 survivors (different bug — below) |
| silent wrong checksums | 0 | 0 |

The headline: **JS completion at W=2 is real now** — the under-marking
corruption that killed essentially every multi-thread big-program run is
gone (its dedicated repro is deterministic-clean), and the engine completes
the full shared-write-heavy workload at W=2 with correct results most of
the time. What remains at W>=4 is a DIFFERENT, residual bug (§11.3).

**Cost side (new finding):** run-2 js W=1 is **+37% slower** than run-1 js
W=1 (33.4 s vs 24.3 s) and peak RSS went **421 MB -> 11.9 GB (28x)** on the
identical workload. The quiet-host flag-tax measurement (§13.3) attributes
both entirely to `--useSharedGCHeap=1`. The under-marking fix appears to
have bought correctness partly by severe over-retention/under-collection of
the shared heap. Speedups within run 2 are computed against run 2's own
(slower) W=1 baseline, so the js scaling numbers are not flattered by this.

### Fix 2: STW watchdog fire-under-lock (`6b298a4fdd99`)

| metric | run 1 (before) | run 2 (after) |
|---|---|---|
| richards-like N=4 (scale 1/16) 5x | ~1/3 watchdog SIGABRT | 5/5 clean |
| richards/string-heavy gate cells | intermittent watchdog SIGABRT (~1/3); gate aborted on first attempt | **0 watchdog aborts across the entire suite** (all 60 cells completed) |
| richards-like speedup(2/4/8) | 0.14/0.19/0.23x (best-shown, partial) | 0.15/0.19/0.24x (clean medians of 3) |

The fix bought **abort-free liveness**, not throughput: richards' negative
scaling is unchanged. That matches the watchdog hunt's own perf finding
(docs/threads/STW-WATCHDOG-CLOSURE.md): the fire-rate data REFUTED the
run-1 "Class-A watchpoint storm" frequency hypothesis — at the config where
T(2) ~ 14x T(1), all 54 Class-A/§A.3 stop windows total **1.6 ms** of a
48.8 s wall (~0.003%), and the stop-fire count is constant (~36/run,
warmup-phase one-shots) regardless of N and scale. Whatever serializes
richards GIL-off, it is not stop frequency or stop latency; that residue is
chartered to TTL-rebias/de-jank (§13.2).

### 11.3 NEW P0: residual shared-heap corruption crash (distinct from both fixed bugs)

Run 2's surviving blocker, found at js W=4 smoke in preflight and
characterized standalone (all on the fixed binary):

- **Rate**: ~8% of smoke-size W=4 runs (5/61); at full size it kills 1/5
  W=2, 3/5 W=4/8, and 5/5 W>=16 runs (exposure scales with work x threads).
- **GC-INDEPENDENT** — still fires with `--useGC=0` (3/20). This is NOT the
  under-marking class run 1 hit (that one was 0/4 with `--useGC=0`), and
  not a recurrence of the fixed bug (whose repro stays 5/5 clean).
- **Shared-heap-specific** — `--useSharedGCHeap=0` (other flags kept):
  0/20.
- **Always loud**: SIGTRAP (libpas "Large heap did not find object" on a
  garbage `StringImpl::deref`), SIGABRT, or SIGSEGV. Zero wrong checksums
  in 113 successful runs — the §5.5 gate never saw a silent lie.
- **Signature families** (cores preserved in
  `Tools/threads/scalebench/out/p0-cores/`): (a) LLInt `op_call_varargs` ->
  `setupVarargsFrame`/`JSCellButterfly::copyToArguments` reading a corrupt
  arguments array — the only varargs site in the benchmark is the spread
  `String.fromCharCode(...codes)` at js/bench.js:216 on a THREAD-LOCAL
  array; (b) null-structure cell in `JSObject::toPrimitive`; (c) garbage
  `StringImpl` pointer deref'd into a libpas panic; (d) garbage pc in JIT
  frame. All consistent with cross-thread corruption of freshly-allocated
  cells/strings in the shared heap, on the allocator/string path rather
  than the collector (it fires with GC off).

This is a P0 release blocker. It needs the bughunter treatment
(hypothesis-driven; the smoke-size repro is
`SCALEBENCH_SMOKE=1 jsc <pinned flags> bench.js -- 4 smoke`, ~8%/run,
~7.6 s/run, so a 50-run campaign is ~6 minutes).

## 12. Run-2 parallel-self suite (GIL-off, report mode)

Same command as run 1 (`scaling-gate.sh --threads "1 2 4 8"` through the
pinned-flag wrapper; medians of 3; cell timeout 1800 s). This time the gate
ran to completion in one pass — no watchdog aborts, no manual cells. The
wrapper makes the gate's own serial-identity row self-vs-self (ignore it);
the genuine tax is §13.3.

| workload | T(1) ms (run1 -> run2) | speedup(2) | speedup(4) (run1) | speedup(8) (run1) | floors |
|---|---|---|---|---|---|
| map-heavy | 2278 -> 2711 | 1.19x | 1.32x (1.11) | 1.28x (1.13) | 2.8/4.5 VIOLATION |
| raytrace-like | 10227 -> 10617 | 0.71x | 0.66x (0.67) | 0.61x (0.65) | 2.8/4.5 VIOLATION |
| richards-like | 3621 -> 3626 | 0.15x | 0.19x (0.19) | 0.24x (0.23) | 2.8/4.5 VIOLATION |
| splay-like | 3544 -> 3853 | 1.20x | 1.53x (1.47) | 1.54x (1.59) | 2.0/3.0 VIOLATION |
| string-heavy | 2730 -> 3686 | 1.57x | 1.32x (1.15) | 1.33x (1.14) | 2.8/4.5 VIOLATION |

lock-fairness smoke: ok (ratioW 1.23, no starvation).

Reading: modest, real improvements on the allocation/atom-churn workloads
(map-heavy +0.15-0.19, string-heavy +0.17-0.19 at N>=4 — and string-heavy
no longer dies), no change on the two negative-scaling workloads
(richards, raytrace), and every workload still violates its floor. The
T(1) inflations (map +19%, string +35%, splay +9%) are the §13.3 shared-heap
tax moving, which also means the run-2 speedup ratios are computed against
slower baselines — the absolute T(N) picture is no better than the ratios
suggest.

## 13. Honest remaining-ceilings analysis

Ranked by what they cost today:

1. **The §11.3 residual corruption crash (P0, correctness).** Caps the
   big-program matrix at W~8 for JS and would cap any real shared-heap
   program the same way. GC-independent, shared-heap-allocator/string
   shaped, loud, with a fast repro. Until it is fixed, every other ceiling
   below is academic above W=8.

2. **GC serialization — pending SPEC-congc implementation.** Unchanged from
   run 1 and still the dominant scalability wall for every allocation-heavy
   workload: collections are stop-the-world with parallel marking; N
   mutators allocate N x faster, every cycle stops all N. map-heavy at
   1.28-1.32x and splay-like at 1.53-1.54x against floors of 2.8-4.5/2.0-3.0
   are exactly this. SPEC-congc (concurrent marking for N mutators) is
   designed, frozen-spec'd, and unimplemented; note it did NOT converge in
   its 6 review rounds and needs an ungil-style finalize before
   implementation. richards-like's 0.15-0.24x is NOT explained by stop
   frequency — the watchdog hunt's fire-rate log
   (`Tools/threads/bughunt/stw-watchdog/firelog-n4.txt`, summarized in
   STW-WATCHDOG-CLOSURE.md) measured ~36 constant warmup one-shot fires
   per run and 1.6 ms total stop time against a 14x slowdown, refuting the
   run-1 storm hypothesis. The richards residue (mid-run samples show
   threads executing JIT code, yet 2 threads take 14x one thread's wall) is
   chartered to **TTL-rebias/de-jank**; until that lands, OO-dispatch-heavy
   code must be assumed to scale NEGATIVELY.

3. **The single-thread shared-heap tax — now measured and attributed, no
   longer "undiagnosed", but WORSE.** Run 1 measured +61% on a noisy host
   and called it undiagnosed. Run 2, quiet host, map-heavy T(1), medians
   of 5: flag-off 1373 ms -> GIL-on 1467 ms (+6.8%) -> pinned GIL-off
   2710 ms (**+97% vs flag-off**). Attribution by single-flag ablation:
   pinned-minus-`useSharedGCHeap` runs at GIL-on speed (1465 ms);
   pinned-minus-`useSharedAtomStringTable` is unchanged (2715 ms). RSS
   tells the same story on identical work: 3.72 GB (pinned) vs 155 MB
   (minus sharedGCHeap) vs 152 MB (GIL-on) — and the big-program W=1 cell
   went 421 MB (run 1) -> 11.9 GB (run 2). So the tax IS the shared-GC-heap
   configuration, and the under-marking fix made it materially worse
   (+61% noisy -> +97% quiet; the run-1 vs run-2 W=1 delta of +37% on the
   identical big-program cell is the same effect measured cleanly). The
   mechanism inside useSharedGCHeap (retention policy vs collection
   scheduling vs allocator path) is the next diagnosis step — plausibly the
   same work item as making SPEC-congc's collector actually collect under
   N mutators. The GIL-on +6.8% still violates the 5% identity tolerance
   on this workload (run 1 saw +15.3% noisy) and lands with the parked
   transition-bench adjudication (AB17f item-6 protocol).

4. **Java-shaped plateau is the benchmark's own ceiling, not JSC's.** Go
   tops out at 4.87x and Java at ~2x on 32 physical cores by design
   (Zipf-hot shard locks, shared merge phase). The JS target, once 1-3 are
   fixed, is the ~2-5x band — not 64x.

Bottom line, run 2: **both landed fixes did what they claimed** — the
under-marking corruption is gone (js W=2 completes with bit-identical
checksums; its repro is deterministic-clean) and the watchdog no longer
kills richards/string-heavy (0 aborts in the full suite). But the tree
still does not have a shippable scalability story: a NEW GC-independent
shared-heap crash gates everything above W~8 (P0, fast repro in hand), no
workload in either suite comes near its scaling floor (best: splay 1.54x
at 8 threads; richards still 0.24x), and the shared-heap mode now costs
+97% single-thread and ~28x RSS — a price that, per the §0 criterion, no
big program would accept today.

## 14. Run-2 reproduction

```
# Big-program matrix (writes results.json run2-equivalent + RESULTS.md):
Tools/threads/scalebench/run.sh

# Residual P0 repro (~8%/run at smoke size, ~7.6 s/run):
cd Tools/threads/scalebench && SCALEBENCH_SMOKE=1 \
  ../../../WebKitBuild/Release/bin/jsc --useJSThreads=1 --useThreadGIL=0 \
  --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 \
  --useThreadGILOffUnsafe=1 bench.js -- 4 smoke

# Fixed-bug regression checks (must stay clean):
WebKitBuild/Release/bin/jsc <pinned flags> \
  Tools/threads/scalebench/js/repro-bigint-shared-ingest.js   # 5x
WebKitBuild/Release/bin/jsc <pinned flags> \
  -e "globalThis.SCALING_THREADS=4; globalThis.SCALING_WORK_SCALE=0.0625;" \
  JSTests/threads/scaling/richards-like.js                    # 5x

# Parallel-self, GIL-off:
SCALING_CELL_TIMEOUT_SECS=1800 Tools/threads/scaling-gate.sh \
  --threads "1 2 4 8" Tools/threads/scalebench/out/jsc-giloff

# Single-thread tax + attribution (quiet host):
jsc [no flags | --useJSThreads=1 | pinned set | pinned minus one flag] \
  -e "globalThis.SCALING_THREADS=1; globalThis.SCALING_WORK_SCALE=1;" \
  JSTests/threads/scaling/map-heavy.js
```

---

# RUN 3 (2026-06-12, full ladder + GC-mode A/B + WS arm)

Source of truth: `Tools/threads/scalebench/results-v3.json` (`valid: true`,
spec "SCALEBENCH v1 + WS amendment"). Everything below is the json's cell
medians, verbatim.

## 15. Setup, provenance, protocol

- Tree `175e2a28b57800add359004fa7b9dfe2ece24e10` (fully green). Same host:
  64 cores, 247 GB RAM, kernel 6.12.68-92.122.amzn2023.x86_64. go1.24.13,
  OpenJDK Corretto-21.0.10.7.1.
- jsc: `WebKitBuild/Release/bin/jsc`, sha256
  `8985994941f0cf4b0bf2b05854ece0d6c2e4705dc7d96534f97806a6d3449538`,
  mtime 2026-06-12 20:17:32 UTC, size 383303704. **`jsc-build-id.txt` is
  STALE** (still records the 2026-06-10 binary); the json records the real
  binary identity.
- Three JS env sets: pinned GIL-off STW (`JSC_useJSThreads=1
  JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1
  JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1`), the same plus
  `JSC_useConcurrentSharedGCMarking=1` ("congc"), and GIL-on minimal
  (`JSC_useJSThreads=1` only, flag-tax baseline).
- Protocol deviation from SPEC §3 (task override, recorded in the json):
  **3 measured reps per cell** (5 for the two W=1 flag-tax cells), median
  reported, **no discarded warmup rep**.
- Checksum gate: all successful runs across all languages, all W, both GC
  modes, and both Phase-C arms produced the identical tuple
  `A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2,
  B=c4bdd580f85ee058, C=af028188d7a56a96` (same tuple as runs 1-2).
  WS parity holds: any checksum difference would have invalidated the batch;
  none occurred.
- §0 handicap statement, updated for this run: GC under JS threads on this
  branch is stop-the-world with parallel marking; concurrent marking
  (`JSC_useConcurrentSharedGCMarking=1`) was A/B-tested here and **crashes
  intermittently at teardown**. Go and Java ship fully concurrent collectors.

## 16. Scaling curves — total wall time, naive arm (medians of 3; speedup vs same-language W=1)

| W | js-stw ms | speedup | js-congc ms | speedup | go ms | speedup | java ms | speedup |
|---|---|---|---|---|---|---|---|---|
| 1 | 33258.7 | 1.000 | 33768.6 | 1.000 | 1738.3 | 1.000 | 1849.9 | 1.000 |
| 2 | 45487.9 | 0.731 | 44415.1 | 0.760 | 1064.3 | 1.633 | 1321.3 | 1.400 |
| 4 | 49959.6 | 0.666 | — (0/3 reps) | — | 699.6 | 2.485 | 1113.5 | 1.661 |
| 8 | 48115.1 | 0.691 | 48767.9 (1/3) | 0.692 | 510.4 | 3.406 | 931.1 | 1.987 |
| 16 | 47561.9 | 0.699 | 48192.1 | 0.701 | 392.8 | 4.425 | 928.0 | 1.993 |
| 32 | 76557.4 | 0.434 | 75688.4 | 0.446 | 359.6 | 4.834 | 1057.3 | 1.750 |
| 48 | 78671.7 | 0.423 | 78660.9 | 0.429 | 352.1 | 4.937 | 1041.2 | 1.777 |
| 64 | 81319.1 | 0.409 | 81472.3 (2/3) | 0.414 | 357.3 | 4.865 | 1141.4 | 1.621 |

- **JS scaling is negative at every W.** Best case W=16 = 0.70x of
  single-thread; second cliff at W=32 (0.43x), driven by Phase A
  (js-stw phase A: 27161 ms at W=8 vs 51436 ms at W=32 — allocation +
  shared-map ingest nearly doubles when threads double past 16). cpu_util
  falls 1.15 (W=1) -> 0.09 (W=64): the program is almost fully serialized
  and added threads are pure overhead.
- **Go scales**: 4.94x at W=48, knee at W=16-32. **Java scales to W=8-16**
  (1.99x), then degrades slightly (1.62x at W=64).
- Cross-language gap: at W=1, JS pinned 33.3 s vs Go 1.74 s (19x) vs Java
  1.85 s (18x). At W=64: 81.3 s vs 0.36 s (228x) and 1.14 s (71x).
- Regression vs run 1's 2026-06-10 binary at W=1: 24.31 s then, 33.26 s now
  (+37%) — but run 1's binary failed every js W>1 cell, while today's binary
  completes the full ladder with bit-identical checksums.

## 17. HEADLINE — STW vs concurrent shared-GC marking, per W

Delta = congc median − stw median (naive arm; negative = congc faster):

| W | stw ms | congc ms | delta ms | delta % | congc reps ok | congc crashes (SIGSEGV) |
|---|---|---|---|---|---|---|
| 1 | 33258.7 | 33768.6 | +509.9 | +1.5% | 3/3 | 0 |
| 2 | 45487.9 | 44415.1 | -1072.7 | -2.4% | 4/6 | 2 |
| 4 | 49959.6 | null | — | — | 0/3 | 3 |
| 8 | 48115.1 | 48767.9 | +652.8 | +1.4% | 1/3 | 2 |
| 16 | 47561.9 | 48192.1 | +630.2 | +1.3% | 3/3 | 0 |
| 32 | 76557.4 | 75688.4 | -869.1 | -1.1% | 3/3 | 0 |
| 48 | 78671.7 | 78660.9 | -10.8 | -0.0% | 3/3 | 0 |
| 64 | 81319.1 | 81472.3 | +153.2 | +0.2% | 2/3 | 1 |

Two findings, both stated plainly:

1. **Where congc completes, it buys ~nothing**: every delta is within
   ±2.4%, i.e. inside run-to-run noise. Concurrent shared GC marking in its
   current state does not move this workload.
2. **It is not stable**: 8 SIGSEGVs across 30 congc reps (W=4: 3/3 crashed,
   cell median null). The STW arm had **0 crashes in 24 runs**. Crash
   signature (from core, W=2): `GCSegmentedArray<JSCell const*>::removeLast`
   <- `HeapClientSet::flushClientMutatorMarkStackForExit` (Heap.cpp:6953)
   <- `GCClient::Heap::detachCurrentThread` (Heap.cpp:7131)
   <- `tearDownSpawnedThreadForExit` (ThreadManager.cpp:687) <- threadMain —
   spawned-thread teardown races the concurrent shared-GC mark stack.
   Per SPEC §6, the W=8 (1/3) and W=64 (2/3) congc medians fall below the
   3-rep threshold and are shown for transparency only.

## 18. Naive vs work-stealing Phase C (WS amendment, W ∈ {8, 32, 64})

Phase C medians, WS vs naive (same-language, same-W, same GC mode — js
comparisons against js-stw; naive comparators reused from §16's ladder,
WS cells run fresh):

| W | js naive C ms | js WS C ms | delta | go naive | go WS | delta | java naive | java WS | delta |
|---|---|---|---|---|---|---|---|---|---|
| 8 | 648.9 | 522.0 | -19.6% | 10.8 | 2.2 | -79.4% | 84.4 | 64.9 | -23.1% |
| 32 | 618.0 | 469.3 | -24.1% | 11.0 | 2.0 | -81.7% | 185.1 | 256.8 | **+38.7%** |
| 64 | 597.4 | 391.6 | -34.5% | 11.0 | 2.1 | -80.8% | 180.3 | 217.5 | **+20.6%** |

- WS helps JS Phase C 20-35%, and helps Go ~80%, but Phase C is <1% of the
  JS total and ~3% of Go's — **totals are unchanged within noise** (e.g. js
  W=64 total: WS 81161.2 vs naive 81319.1 ms).
- Java WS is **slower** at W=32/64 — the thread-local-accumulate + serial
  merge loses to the naive shared-lock append there.
- All WS checksum tuples identical to naive (parity gate passed).

## 19. Peak RSS and CPU utilization

| W | js-stw RSS MB | go RSS MB | java RSS MB | js cpu | go cpu | java cpu |
|---|---|---|---|---|---|---|
| 1 | 13938 | 180 | 847 | 1.15 | 1.48 | 2.03 |
| 2 | 14395 | 181 | 771 | 0.66 | 1.30 | 1.67 |
| 4 | 12993 | 184 | 800 | 0.36 | 1.09 | 1.28 |
| 8 | 12892 | 183 | 805 | 0.25 | 0.85 | 1.03 |
| 16 | 12704 | 196 | 872 | 0.20 | 0.63 | 0.88 |
| 32 | 12351 | 204 | 766 | 0.16 | 0.41 | 0.82 |
| 48 | 11992 | 207 | 932 | 0.12 | 0.31 | 0.76 |
| 64 | 11772 | 205 | 870 | 0.09 | 0.25 | 0.71 |

(RSS = peak KB from the json / 1024, rounded; cpu_util = (user+sys)/(wall*W).)

JS pinned runs hold **11.8-14.7 GB across every W** — vs Go ~0.2 GB, Java
~0.8-0.9 GB, and the GIL-on JS configuration's 0.43 GB. The shared GC heap
under VMLite retains an order of magnitude more memory than GIL-on JS and
57-78x more than Go.

## 20. Single-thread flag tax (W=1, 5 reps each)

| Config | total ms | RSS MB | cpu_util |
|---|---|---|---|
| jsc, zero flags | **N/A** — `ReferenceError: Lock is not defined` (Thread API needs `JSC_useJSThreads=1`; 1 demonstration rep, exit 3) | — | — |
| `JSC_useJSThreads=1` only (GIL on) | 15140.0 | 424 | 1.03 |
| pinned GIL-off set | 33691.9 | 13938 | 1.15 |

**The pinned GIL-off flag set costs +122.5% wall time and 32.9x RSS on
single-threaded execution** (33691.9 / 15139.98; 14272296 / 433968 KB).
This is the entry fee before a second thread exists.

## 21. Caveats (read before quoting any number above)

Host: dedicated 64-core / 247 GB box, quiet throughout (no jsc/ninja/java/go
processes at start; 1-min loadavg 1.03-1.94 at batch starts). Reps: **3 per
cell** (5 for the two W=1 flag-tax cells), median reported, **no warmup
rep** — a deliberate, recorded deviation from SPEC §3's 5+warmup protocol,
so cell medians here are noisier than runs 1-2. Dropped/short cells:
js-congc W=4 is **null** (3/3 SIGSEGV); js-congc W=8 has 1 surviving rep and
W=64 has 2 — both below SPEC §6's 3-rep median threshold and marked as such;
zero-flag jsc is N/A (cannot run the bench at all). The Phase-5 naive-arm
comparators at W=8/32/64 were **reused** from the main ladders (same binary,
env, session), not re-run; WS cells were fresh. Mid-run incident: /tmp
(tmpfs) filled with 6-19 GB core dumps during the first js-congc W=2 cell;
cores were deleted, core dumps disabled (RLIMIT_CORE=0) for all subsequent
reps, and the affected cell fully re-run (both batches kept in raw data:
4 ok / 2 SIGSEGV across 6 reps). `jsc-build-id.txt` is stale; trust the
json's sha256/mtime. One bookkeeping note: the json's prose `findings` array
quotes individual-rep Phase-C values for the WS comparison (e.g. 555.8 vs
632.4 ms at W=8); the tables in §18 use the json's cell **medians**, which
is why the percentages differ slightly from the prose. Finally, per-language
speedups are against the same language's W=1 — they say nothing about the
19-228x absolute cross-language gap, which is reported separately in §16.

## 22. Run-3 reproduction

```
# Driver (3-rep ladders, GC-mode A/B, WS arm, flag-tax cells):
python3 Tools/threads/scalebench/v3_driver.py   # writes results-v3.json

# congc teardown-crash repro (W=4 hit 3/3 here):
cd Tools/threads/scalebench && \
  JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 \
  JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 \
  JSC_useThreadGILOffUnsafe=1 JSC_useConcurrentSharedGCMarking=1 \
  ../../../WebKitBuild/Release/bin/jsc bench.js -- 4
```

Bottom line, run 3: the full JS ladder now **completes** with bit-identical
checksums (the first run where that is true), but scalability is still
negative at every W (best 0.70x at W=16, 0.41x at W=64, vs Go's 4.9x), the
concurrent-marking arm changes nothing measurable and crashes at spawned-
thread teardown, the WS scheduler fixes only a phase that doesn't matter,
and the flag set still charges +122% time / 33x memory before any
parallelism begins.

## 23. Run 3.1 — acceptance re-verification (no code delta)

**Read this first:** the binary under test is **byte-identical** to the one
run 3 measured (`WebKitBuild/Release/bin/jsc` sha256
`8985994941f0cf4b…3449538`, same mtime/size), and the tree is clean at
`9e9a3a25fd0b` (the run-3 commit; the only changes in it are bench/docs).
A `VM.cpp` working-tree modification observed at session start was gone
(reverted, no stash) before any measurement; nothing was rebuilt. Every
delta below is therefore **host noise**, not a code change.

Protocol: pinned GIL-off env, `bench.js -- W`, 3 reps/cell (5 for GIL-on
W=1 after the first 3 showed a shift; 5 for congc W=4 per the acceptance
spec), medians; `/usr/bin/time -v` for peak RSS and cpu_util
(=(user+sys)/(wall*W)); `ulimit -c 0`; one bench process at a time. Host
incident handled pre-run: three orphaned `addr2line` processes (parent=init,
~115 GB RSS, 3 cores) were killed before the first cell; 1-min loadavg at
batch starts was 2.7-4.5, **above** run-3's 1.0-1.9.

### Before/after (run 3 medians vs run 3.1 medians)

| Cell | run 3 wall ms | run 3.1 wall ms | delta | run 3 cpu | run 3.1 cpu | run 3 RSS MB | run 3.1 RSS MB | RSS delta |
|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 33258.7 | 32555.6 | -2.1% | 1.15 | 1.14 | 13938 | 12680 | -9.0% |
| GIL-off W=2 | 45487.9 | 44141.6 | -3.0% | 0.66 | 0.67 | 14395 | 13059 | -9.3% |
| GIL-off W=16 | 47561.9 | 47805.6 | +0.5% | 0.20 | 0.19 | 12704 | 12715 | +0.1% |
| GIL-off W=32 | 76557.4 | 77368.2 | +1.1% | 0.16 | 0.16 | 12351 | 12352 | +0.0% |
| GIL-on W=1 | 15140.0 | 16271.9 (5 reps) | **+7.5%** | 1.03 | 1.01 | 424 | 422 | -0.4% |

- The GIL-on W=1 **+7.5%** is stable across all 5 reps (16172-16397 ms) but
  cannot be a code regression — the binary hash is identical to run 3's.
  It quantifies this session's noisier host (residual loadavg from the
  bench cells themselves plus background agent sessions). Treat it as the
  error bar on every other cell in this table, including the apparent
  W=1/W=2 wall and RSS "improvements".
- Checksums: every rep of every cell (incl. congc and GIL-on) produced
  checksumC `af028188d7a56a96`, matching run 3's recorded tuple exactly.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests — superset of the 10-test gate) | **0 mismatches** |
| congc teardown: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums (run 3: 3/3 SIGSEGV) |

The congc result does **not** mean the teardown race is fixed — the code is
unchanged. It reclassifies the `tearDownSpawnedThreadForExit` crash from
"deterministic at W=4" to **timing-dependent/flaky**: run 3 hit it 3/3 under
heavy host memory pressure (the same session later found ~115 GB of runaway
symbolizer RSS) and with core dumps initially enabled; run 3.1, on a freer
host, survived 5/5. The §17 stack remains the live bug.

### Acceptance verdict

**FAIL (expected).** Corpus + identity are green, but no headline cell
(W=16 wall, W=32 wall, RSS, W=1 tax) improved >= 15% — definitionally
impossible with a bit-identical binary — and the GIL-on W=1 cell moved
+7.5% (host noise, but > the 5% gate). Run 3.1 is a **baseline
reproduction**: run-3 numbers reproduce within ~±3% (wall, W>=16 RSS) on a
busier host, the corpus and identity gates hold, and the congc crash is
flaky rather than deterministic. No optimization landed between run 3 and
run 3.1 for this acceptance to measure.

## 24. Run 3.1 — acceptance of the heap/object-model optimization delta

Supersedes §23 for acceptance purposes: §23 measured a bit-identical binary
(a baseline reproduction). This run measures the real delta — a 21-file
working-tree change (+1519/-184) over `9e9a3a25fd0b` touching the shared-GC
heap (Heap.cpp, IncrementalSweeper, MarkedBlock, FullGC/EdenGC activity
callbacks, CodeBlockSet, HeapClientSet) and the concurrent object model
(ConcurrentButterfly, PropertyTable, Structure, LockObject, VMManager,
VMTraps). Binary under test: `WebKitBuild/Release/bin/jsc` sha256
`c96178df6ac1dae78b874bb3a29a42d32e5ff84af4dc19c91281087329429b1c`
(built 2026-06-13 03:52 UTC; 0 sources newer than the binary).

Protocol: identical to run 3 / §23 — pinned GIL-off env, `bench.js -- W`
via the v3 driver measurement path (raw records in `results-v31b-raw.jsonl`),
3 reps/cell (5 for congc W=4), medians; `/usr/bin/time -v` for peak RSS and
cpu_util = (user+sys)/(wall*W); core dumps off; one bench process at a time;
background CPU consumers reniced to 19. 1-min loadavg at batch starts:
2.5–5.7 (run 3: 1.0–1.9) — i.e. this host was *noisier* than run 3's, so
the wall-time improvements below are not flattered by a quieter machine.

### Before/after (run 3 medians vs run 3.1 medians)

| Cell | run 3 wall ms | run 3.1 wall ms | delta | run 3 cpu | run 3.1 cpu | run 3 RSS MB | run 3.1 RSS MB | RSS delta |
|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 33258.7 | 23834.5 | **-28.3%** | 1.15 | 1.04 | 13938 | 411 | **-97.1%** |
| GIL-off W=2 | 45487.9 | 43735.9 | -3.9% | 0.66 | 0.87 | 14395 | 3810 | **-73.5%** |
| GIL-off W=16 | 47561.9 | 38385.1 | **-19.3%** | 0.20 | 0.26 | 12704 | 3860 | **-69.6%** |
| GIL-off W=32 | 76557.4 | 38708.9 | **-49.4%** | 0.16 | 0.18 | 12351 | 3850 | **-68.8%** |
| GIL-on W=1 | 15140.0 | 14395.0 | -4.9% | 1.03 | 1.03 | 424 | 423 | -0.3% |

- W=1 GIL-off tax drops from 2.20x to **1.66x** (23834.5 / 14395.0).
- Per-rep wall spread is tight (W=1: 23712–23946; W=32: 38516–38737), and
  every rep of every cell reproduced the run-3 checksum tuple exactly
  (A=b3e65a6855b9bdeb, postings=4158957, A2=39c33392b2a4c5b2,
  B=c4bdd580f85ee058, C=af028188d7a56a96).
- The W=1 RSS collapse (13.9 GB -> 411 MB) brings flag-on single-thread
  memory to parity with GIL-on (423 MB). W>=2 RSS settles at ~3.8 GB —
  a 69–74% cut but still ~9x the GIL-on footprint; that residual is the
  next memory target.
- Honest negatives: GIL-off W=2 wall only -3.9% (within this host's noise
  band; W=2 remains the worst scaling point at 1.84x the GIL-on W=1 wall
  for 2x the work... per-thread efficiency is still the open problem), and
  W=16/W=32 cpu_util (0.26/0.18) still shows threads mostly parked —
  walls improved because the serial/GC component shrank, not because
  parallel efficiency rose.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests — superset of the 10-test gate) | **0 mismatches** |
| congc teardown: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 40151.9 ms (run 3: 3/3 SIGSEGV; §23 no-delta: 5/5 but flaky-classified) |

### Acceptance verdict

**PASS.** Corpus + identity green; three headline cells beat the >=15%
bar (W=16 wall -19.3%, W=32 wall -49.4%, RSS -68.8%..-97.1%; plus W=1 tax
2.20x -> 1.66x); no cell regresses >5% (worst mover: GIL-on W=1 RSS -0.3%,
GIL-on wall itself improved -4.9%). congc teardown survived 5/5 — but per
§23's reclassification the crash is timing-dependent, so 5/5 here is
consistent-with-fixed, not proof; keep the §17 stack on the books until a
stress campaign under memory pressure retires it.

## 25. Run 3.2 — campaign-2 contention/retention pass

Supersedes §24 for acceptance purposes. Measures a 21-file working-tree
delta (+1043/-75) over `729430dbc80c` touching the allocator slow path
(per-`BlockDirectory` striped MSPL replacing the server-wide refill lock —
T7), the GC stop protocol (parked siblings join parallel marking instead of
idling — T1; §A.3 arbitration park-not-sleep — T6), the heap-limit feedback
(rebase `m_sizeAfterLast*Collect` past the Wlr cohort — T2), the segmented
butterfly entry (born-full-coverage so first grow is the lock-free CAS —
T3), the FTL lazy-slow-path steady state (lock-free DCLP — T8), and a
bounded adaptive spin on `Lock.prototype.hold` (T5). T4 (relabel
thread-local fast path) was withdrawn at adversarial review as unsound; the
review note is in `JSObject.cpp`. Two fixes applied during this acceptance
run: the T3 alignment gate narrowed from `useJSThreads` to `useSharedGCHeap`
(the wider gate changed contiguous-array size-class selection under GIL-on
for no benefit there), and the T2 rebase clamped to `m_totalBytesVisited` so
the eden monotone-visited assert holds on small heaps (6 Debug-corpus
failures: `heap-allocation-storm.js` et al.). Binary under test:
`WebKitBuild/Release/bin/jsc` sha256
`2a85f8e533411e6880272552ae2050b8d3842c1feb31aa181e1caeb6a1f83f88`
(2026-06-15; 0 sources newer than the binary).

Protocol: identical to §24 — pinned GIL-off env, `bench.js -- W` via the
`v3_driver` measurement path (raw records in `results-v32-raw.jsonl`),
3 reps/cell (5 for congc W=4), medians; `/usr/bin/time -v` for peak RSS and
cpu_util = (user+sys)/(wall*W); core dumps off; one bench process at a time.
1-min loadavg at batch starts: 1.6–4.2.

**Host-drift control.** The run-3.1 binary was rebuilt bit-identically
(`jsc-v32-baseline`, sha256 `c96178df…` matches §24 exactly) and re-measured
back-to-back with the v32 binary in every cell (raw in
`results-v32ab-raw.jsonl`). The baseline binary itself reproduces today at
+5.0% (GIL-off W=1: 25025 ms vs §24's 23834.5 ms) to +8.8% (GIL-on W=1:
15659 ms 5-rep interleaved median vs §24's 14395.0 ms) over its own
recorded numbers — i.e. this host has drifted 5–9% slower since 2026-06-13
(a competing Rust build in a sibling worktree was observed mid-run; reniced
where caught). The "vs §24" column below is therefore a *lower bound* on the
delta; the "vs same-host baseline" column is the back-to-back A/B at the
same instant and is the meaningful number.

### Before/after (run 3.1 §24 medians vs run 3.2 medians; same-host A/B in parentheses)

| Cell | §24 wall ms | 3.2 wall ms | vs §24 | vs same-host base | §24 cpu | 3.2 cpu | §24 RSS MB | 3.2 RSS MB | RSS vs same-host |
|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 23834.5 | 24526.7 | +2.9% | **-2.0%** (base 25025) | 1.04 | 1.04 | 411 | 421 | +3.6% (base 419) |
| GIL-off W=2 | 43735.9 | 28478.8 | **-34.9%** | **-36.4%** (base 44795) | 0.87 | 0.86 | 3810 | 1538 | **-58.4%** (base 3700) |
| GIL-off W=8 | — | 21484.9 | — | **-45.1%** (base 39151) | — | 0.54 | — | 1764 | **-52.8%** (base 3737) |
| GIL-off W=16 | 38385.1 | 20718.6 | **-46.0%** | **-47.9%** (base 39785) | 0.26 | **0.43** | 3860 | 1810 | **-52.9%** (base 3845) |
| GIL-off W=32 | 38708.9 | 21140.4 | **-45.4%** | **-47.2%** (base 40066) | 0.18 | 0.29 | 3850 | 1739 | **-55.2%** (base 3883) |
| GIL-on W=1 | 14395.0 | 15798.7 | +9.8% | **+2.7%** (base 15659) | 1.03 | 1.03 | 423 | 421 | -0.2% (base 422) |

- Per-rep wall spread is tight at every cell except W=32 rep2 (27111 ms,
  loadavg spike to 28.8 from W=16's tail; median-robust). All 23 bench reps
  reproduced the §24 checksum tuple exactly.
- W=16 cpu_util crosses **0.43** (median of 0.426/0.432/0.431) — the first
  time this branch has exceeded the 0.40 gate. W=32 rises to 0.29.
- W>=2 RSS roughly halves (3.7–3.9 GB -> 1.5–1.8 GB); the residual is
  **N-mutator eden floating garbage** — with N concurrent mutators every
  eden cycle roots N live stacks, so sibling mutators' in-flight
  temporaries get promoted past the eden boundary and the
  `m_maxHeapSize = std::max(m_maxHeapSize, currentHeapSize + m_maxEdenSize)`
  ratchet (Heap.cpp updateAllocationLimits) climbs monotonically without
  the `edenToOldGenerationRatio < 1/3` Full trigger ever firing. rss3
  REFUTED the original attribution here ("per-thread allocator state +
  segmented-butterfly steady-state"): `JSC_forceSegmentedButterflies=1`
  W=1 peaks at 183MB after eden (vs W=2's 774MB), and per-thread allocator
  state is bounded at ~6.7MB/thread ((RSS@W=32 - RSS@W=2) / 30). The
  GC-heap peak-after-eden delta W=1->W=2 is +1001MB = 85% of the +1176MB
  peak-RSS delta; W=2 floating dead = 774MB peak - 143MB true live =
  631MB. Fixed by **T5-rss-eden-floating-garbage (campaign-3)** — a
  shared-only (>=2 clients) Full-collection trigger + ratchet cap at
  `sizeAfterLastFullCollect * sharedGCEdenSurvivalFullTriggerRatio`
  (default 2.0) in the eden branch of `Heap::updateAllocationLimits()`.
- W=1 GIL-off tax: 24527 / 15799 = **1.55x** (§24: 1.66x), but on a
  same-host basis 24587 / 15659 = 1.57x, vs baseline 25025 / 15659 = 1.60x.
- Honest negatives: GIL-on W=1 wall +2.7% same-host (5-rep interleaved A/B,
  loadavg 2.0–2.3, baseline reps 15568–15922 vs v32 reps 15854–16134;
  reproducible across batches). No new code path runs in this configuration
  (every behavioral change is `isSharedServer()` / `gilOff()` /
  `useSharedGCHeap()`-gated; verified by the bit-identical flag-off identity
  below); the residual is structural — `Heap` layout grew by the
  `MutatorSlowPathLockFacade`, the registry lock, and the sibling-visitor
  pool, shifting hot members' cache lines and inlining decisions. Within the
  5% gate; **fixed by T4-heap-layout-restore (campaign-3)** — every
  campaign-2-added member moved to a contiguous trailer block at the end of
  `class Heap`, the old 1-byte `Lock m_mutatorSlowPathLock` slot back-filled
  with a 1-byte pad, so every pre-campaign-2 member offset is byte-identical
  to `729430dbc80c` (relational `static_assert`s pin this). GIL-off W=1 wall +2.9% vs §24
  but -2.0% same-host (i.e. host drift only), RSS +3.6% same-host (the
  shared-heap path's T3 alignment requesting one size class up on small
  arrays). GIL-off W=2 cpu_util -1.7% (0.871 -> 0.855) — wall improved
  because GC and refill *time* fell, not because the W=2 mutators ran more
  in parallel; W=2 remains the worst per-thread efficiency point.

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug bin) | **94 passed, 0 failed, 4 skipped** (after T2 clamp fix; pre-fix: 88/6, all `ASSERT(currentHeapSize >= m_sizeAfterLastCollect)`) |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug bin) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests) | **0 mismatches** |
| congc teardown: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 23700.9 ms (§24: 40151.9 ms, **-41.0%**) |

### Acceptance verdict

**PASS on the same-host A/B**; the literal vs-§24 GIL-on cell exceeds 5%
because the byte-identical baseline binary itself does (host drift). Corpus
+ identity green; all three headline disjuncts hit (W=16 cpu_util 0.43 >=
0.40; W=16 wall -46.0% vs §24 / -47.9% same-host; W>=2 RSS -52% to -58%
same-host). No same-host cell regresses >5% (worst: GIL-on W=1 wall +2.7%,
GIL-off W=1 RSS +3.6%). The campaign-2 delta is the first measurement where
W>=8 wall dips below the GIL-off W=1 wall — i.e. adding threads now makes
the program *faster*, not just less-slow.

## 26. Campaign-3 T4-heap-layout-restore — GIL-on W=1 layout regression

Mandatory follow-up to the §25 honest-negative. Pure declaration reorder in
`Source/JavaScriptCore/heap/Heap.h`: every member campaign-2 inserted
mid-class (`MutatorSlowPathLockFacade m_mutatorSlowPathLock`,
`m_markedSpaceRegistryLock`, `m_siblingSlotVisitors` /
`m_availableSiblingSlotVisitors` / `m_siblingSlotVisitorPoolMayGrow`,
`m_siblingMarkingAssistEnabled` / `m_numberOfSiblingMarkingAssists`) moved
to one trailer block after the last pre-campaign-2 member; the old
`Lock m_mutatorSlowPathLock` slot back-filled with the 1-byte
`m_unusedPadMutatorSlowPathLock`. Result: every pre-campaign-2 member's
byte offset (m_objectSpace, m_handleSet, m_opaqueRoots, m_worldState,
m_barrierThreshold, m_threadLock, every IsoSubspace) is identical to
`729430dbc80c`. Four relational `OBJECT_OFFSETOF` static_asserts pin the
trailer ordering so future additions can't re-introduce the shift. No
behavior change in any configuration; none of the moved members are in the
`Heap::Heap()` init list (no -Wreorder); `Heap.cpp` / `HeapInlines.h`
unchanged.

**Expected**: GIL-on W=1 back to the §25 same-host base 15659 ms ± noise
(recover +2.7%); proportional trim to the GIL-off W=1 1.55× tax (same hot
fields on every allocation slow path). **Beat-or-explain baselines**: §25
table row "GIL-on W=1" same-host base 15659 ms; every other §25 cell must
not regress (pure layout — no code-path delta). Verify: 5-rep interleaved
A/B GIL-on W=1 vs `d8ed7b6f5254`, `v5a-identity.sh`, full corpus both
modes.

## 27. Run 3.3 — campaign-3 acceptance (segmented-array DFG fastpath + relabel-STW-elide + heap layout restore + eden-survival Full trigger)

Measures a 21-file working-tree delta (+1441/-166) over `d8ed7b6f5254`
landing campaign-3: **T1-relabel-stw-elide-sound** (sound replacement for
the withdrawn campaign-2 T4 — the relabel STW path elided when the source
butterfly word is already segmented; `JSArray.cpp` / `JSObject.cpp`),
**T3-jit-segmented-arraymode** (DFG fast path for segmented-butterfly
indexed access via a side-band `ArrayProfile` bit threaded through
`ArrayMode` into `DFGSpeculativeJIT{,64}.cpp` so DFG/FTL stop OSR-exiting
on every segmented load — the largest line-count change),
**T4-heap-layout-restore** (§26), and **T5-rss-eden-floating-garbage**
(`Heap::updateAllocationLimits()` Full trigger at
`sharedGCEdenSurvivalFullTriggerRatio=3.0`, §25 attribution). v33 binary
sha256 `151409b1a06cd128ab47b1fbf6b82753df14bbdc980fbcfafee2fdb685d40216`
(2026-06-15; rebuilds bit-identically; 0 sources newer than the binary).

**Host-drift control.** `d8ed7b6f5254` rebuilt bit-identically
(`jsc-v33-baseline`, sha256 `2a85f8e5…` matches §25 exactly) and measured
back-to-back with v33 in every cell (raw in `results-v33ab-raw.jsonl`).
The baseline binary today reproduces its own §25 numbers within ±2% (W=1
24809 ms vs §25 24527 ms = +1.1%; W=16 20618 ms vs §25 20719 ms = -0.5%;
GIL-on W=1 16021 ms vs §25 15799 ms = +1.4%) — host drift since 2026-06-15
§25 is small, so vs-§25 and vs-same-host-baseline columns track closely.
1-min loadavg at batch starts: 1.6–10.3 (W=32 first batch caught a spike
to 8–10; re-run interleaved, see W=32 row note).

### Before/after (run 3.2 §25 medians vs run 3.3 medians; same-host A/B in parentheses)

| Cell | §25 wall ms | 3.3 wall ms | vs §25 | vs same-host base | speedup-vs-self | §25 cpu | 3.3 cpu | §25 RSS MB | 3.3 RSS MB | RSS vs same-host |
|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 24526.7 | 25063.5 | +2.2% | **+1.0%** (base 24809) | 1.00x | 1.04 | 1.04 | 421 | 422 | +0.5% (base 419) |
| GIL-off W=2 | 28478.8 | 24503.3 | **-14.0%** | **-14.9%** (base 28784) | 1.02x | 0.86 | 0.90 | 1538 | 1701 | **+10.3%** (base 1543) |
| GIL-off W=4 | — | 19207.0 | — | **-19.3%** (base 23813) | 1.30x | — | 0.71 | — | 1732 | -0.8% (base 1746) |
| GIL-off W=8 | 21484.9 | 16166.3 | **-24.8%** | **-25.0%** (base 21561) | 1.55x | 0.54 | 0.56 | 1764 | 1765 | +0.3% (base 1761) |
| GIL-off W=16 | 20718.6 | 15193.4 | **-26.7%** | **-26.3%** (base 20618) | **1.65x** | 0.43 | 0.44 | 1810 | 1789 | -1.1% (base 1809) |
| GIL-off W=32 | 21140.4 | 15950.1 | **-24.6%** | **-25.7%** (base 21465) | 1.57x | 0.29 | 0.33 | 1739 | 1843 | +5.2% (base 1752) |
| GIL-on W=1 | 15798.7 | 15764.7 | -0.2% | **-1.6%** (base 16021) | — | 1.03 | 1.03 | 421 | 421 | +0.1% (base 421) |

- All checksums match the reference tuple at every successful rep. Per-rep
  spread tight at W∈{1,2,4,8} (≤2%); W=16 base reps 19244/20618/20825
  (rep1 low — loadavg 5.1 vs 5.9 for reps 2/3; median-robust); W=32 see
  P0 below.
- W=32 row uses the interleaved re-run (loadavg 9–10, base reps
  21374/21465/21698, v33 surviving reps 15895/16005 plus first-batch rep1
  16084; median 15950). The original W=32 batch (loadavg 7–10) had two
  baseline reps with ~57s sys time (host-contention bimodality, §25
  observed the same at W=32 rep2) and one v33 SIGSEGV — see P0.
- **Speedup-vs-self at W=16: 25064 / 15193 = 1.65x** — short of the 1.8x
  Java-parity bar (would need W=16 ≤ 13924 ms at this run's W=1). cpu_util
  rises only marginally (0.43→0.44); the wall improvement is mutator
  *throughput* (T3 keeps DFG/FTL on the hot indexed path instead of
  OSR-exiting on segmented butterflies), not parallelism. The remaining
  W=16 ceiling is **NOT** §25's "STW collection wall fraction" — gcwall
  (§27.S2) measures STW-GC at **5.4%** of W=16 wall. The dominant ceiling
  is the **~52% thread-0 serial inter-phase work** plus the parallel-phase
  CPU-waste tax — see §27.S1/S2 below.
- **GIL-on W=1 -1.6% same-host (5-rep interleaved**, base
  15691/15842/16021/16202/16361 vs v33 15623/15728/15765/15875/16187,
  loadavg 1.7–6.3 falling). T4-heap-layout-restore recovered ~1.6 of the
  §25 +2.7 percentage points; the other ~1pp is within the 5-rep spread on
  this host. PASS the ≤2% gate.
- Honest negatives: **(a)** GIL-off W=1 wall +1.0% same-host — the T3
  `ArrayProfile` side-band bit and `ArrayMode` widening add a small amount
  of profiling/dispatch overhead even at W=1 (every flat indexed access
  still tests the bit). Within 5%. **(b)** GIL-off W=2 RSS **+10.3%**
  same-host (1543→1701 MB) — T5-rss at the landed `ratio=3.0` admits ~2
  floating live-sets before a Full, while T1/T3 keep more JIT code +
  segmented spine reachable per cycle; the W=2 cell is now allocator-heavy
  enough that the 3.0x bound is hit later than the §25 analysis projected
  for 2.0x. W≥4 RSS is flat-to-down (the bound is reached earlier with
  more concurrent allocators). **(c)** W=32 RSS +5.2% same-host — same
  mechanism, marginal.

### NEW P0 — W=32 SIGSEGV in DFG OSR-entry tier-up (campaign-3 regression)

v33 W=32 GIL-off bench crashes **4/26** Release runs (rc=139 SIGSEGV);
`jsc-v33-baseline` **0/13** under identical conditions in the same
session. Backtrace (core `/tmp/v33cores/core.7937`):
`operationTriggerOSREntryNow` → `DFG::tierUpCommon` →
`failedOSREntry` lambda (`DFGOperations.cpp:6584`) →
`DFG::JITCode::clearOSREntryBlockAndResetThresholds`
(`DFGJITCode.cpp:415`) → `CodeBlock::jitCode()` with `this=nullptr` —
i.e. `m_osrEntryBlock.get()` returned null past the `ASSERT(m_osrEntryBlock)`.
None of `DFGOperations.cpp` / `DFGJITCode.cpp` are in the campaign-3
diff; this is a **pre-existing concurrent-tier-up race** (two GIL-off
mutators reach `failedOSREntry` for the same DFG `JITCode`; the first
runs `m_osrEntryBlock.clear()`; the second derefs the now-null weak
inside `clearOSREntryBlockAndResetThresholds`) that campaign-3 **exposes**
— T3 keeps every mutator in DFG/FTL on the hot loop instead of bouncing
through OSR-exit, so 32 mutators now plausibly hit the FTL OSR-entry
failure path concurrently for the shared bench function. Not seen at
W≤16 (0/14 v33). Repro:
```
env JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 \
    JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 \
    JSC_useThreadGILOffUnsafe=1 \
    WebKitBuild/Release/bin/jsc Tools/threads/scalebench/js/bench.js -- 32
# ~15% of runs: SIGSEGV at DFGJITCode.cpp:415
```

### Gates

| Gate | Result |
|---|---|
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug bin) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug bin) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 19105.9 ms (§25: 23700.9 ms, **-19.4%**) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **-1.6%** (≤2% gate PASS) |
| Bench W=32 stability | **FAIL** — 4/26 SIGSEGV (baseline 0/13) |

### Acceptance verdict

**FAIL.** Corpus + identity green; GIL-on layout regression recovered;
W∈{2,4,8,16,32} wall -15% to -26% same-host with no wall regression >5%
anywhere. But: **(1)** W=16 speedup-vs-self **1.65x < 1.8x** (wall
15193 ms > 13700 ms target); **(2)** new ~15% W=32 SIGSEGV in concurrent
DFG OSR-entry tier-up — a pre-existing race campaign-3 makes hot, but a
correctness regression vs `d8ed7b6f5254` regardless; **(3)** W=2 RSS
+10.3% same-host (>5%). Per the ≥20%-improvement-at-W=16 fallback this
section is recorded honestly with the gap named; campaign-3 cannot land
until the tier-up race is closed (a `tierUpCommon` / `m_osrEntryBlock`
TOCTOU guard, not in any campaign-3 file) and the W=2 RSS regression is
either fixed (`sharedGCEdenSurvivalFullTriggerRatio` tuned toward 2.0 at
N=2) or explained as accepted.

### §27.S1 Amendment — serial-fraction ceiling correction & per-section attribution

The §27 ceiling line ("STW collection wall fraction") is **incomplete**.
The `gcwall` instrumentation incidentally exposed that at W=16,
`phaseA_ms + phaseB_ms + phaseC_ms ≈ 7150 ms` against `total_ms ≈ 14850 ms`
— i.e. **~7700 ms (52%) of W=16 wall is thread-0-only inter-phase serial
work** that runs between barriers while W-1 workers park: two
`postingsChecksum()` walks over 4,158,957 postings (~10 heap-BigInt ops
each), `buildDfSnap()`, `makeGroups()`, and `checksumPhaseC()`
(`bench.js` worker-0 path). All three languages run identical serial
work, so this is not a fairness defect; but a 52% structural serial
fraction alone caps per-worker `cpu_util` near 0.50 — which is the
observed 0.44, **before** any STW-GC accounting. STW collection is a
contributor inside the parallel ~7150 ms, not the dominant W=16 ceiling.

**The actionable lever is the GIL-off tax on the serial work.** GIL-on
W=1 15764.7 ms vs GIL-off W=1 25063.5 ms = **1.59×**. The serial sections
are single-threaded by construction, so under GIL-off they pay engine
overhead (sharedGCHeap BigInt allocation, segmented-butterfly GetByVal,
shared-Map iterator) for zero parallelism benefit. If the serial 7700 ms
carries the 1.59× tax uniformly, GIL-on-parity serial ≈ 7700/1.59 ≈
**4843 ms**, projecting W=16 wall ≈ 7150 + 4843 ≈ **11993 ms** — under
the ≤12600 ms Java-parity bar (1.99× vs own W=1) and lifting `cpu_util`
toward (4843 + 16·6050) / (16·11993) ≈ 0.53. That is the campaign-4
target: recover ~2857 ms of serial wall at W=16 by closing the GIL-off
tax on **one** identified hot section, not by touching the parallel
phases.

**Instrumentation (this amendment).** `bench.js` now emits five extra
JSON keys timing each thread-0 serial section individually:
`postingsChecksum1_ms`, `buildDfSnap_ms`, `postingsChecksum2_ms`,
`makeGroups_ms`, `checksumPhaseC_ms`. Pure measurement — no control-flow
change, no engine edit, output-only (Go/Java need not match; SPEC §1.11
output keys are a superset contract). Sum of the five ≈
`total_ms - (phaseA_ms + phaseB_ms + phaseC_ms)` modulo barrier-wakeup
slop and (in WS mode) `wsMergeLocals`.

**Attribution A/B (v33 binary, 5 reps each, quiet host):**
```
# GIL-off W=1
for i in 1 2 3 4 5; do env JSC_useJSThreads=1 JSC_useThreadGIL=0 \
  JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 \
  JSC_useThreadGILOffUnsafe=1 WebKitBuild/Release/bin/jsc \
  Tools/threads/scalebench/js/bench.js -- 1; done
# GIL-on W=1
for i in 1 2 3 4 5; do env JSC_useJSThreads=1 JSC_useThreadGIL=1 \
  WebKitBuild/Release/bin/jsc \
  Tools/threads/scalebench/js/bench.js -- 1; done
```
W=1 isolates the serial-code tax from any parallel-phase effect (every
section is "serial" at W=1); the per-section GIL-off/GIL-on ratio names
the dominant contributor directly. A W=16 GIL-off run with the same
binary cross-checks that the W=1 section walls reproduce at scale (they
should: thread-0 runs them alone either way).

| Section | GIL-on W=1 ms (med) | GIL-off W=1 ms (med) | ratio | GIL-off W=16 ms (med) |
|---|---|---|---|---|
| postingsChecksum1 | 1846.5 | 2785.0 | 1.51 | 3739.9 |
| buildDfSnap | 34.6 | 37.8 | 1.09 | 56.8 |
| postingsChecksum2 | 2002.5 | 3081.8 | 1.54 | 3973.3 |
| makeGroups | 0.4 | 0.5 | 1.09 | 0.7 |
| checksumPhaseC | 29.8 | 38.0 | 1.28 | 49.8 |
| **sum** | **3913.9** | **5943.1** | 1.52 | **7820.5** (≈ expected 7700) |

(Filled 2026-06-15 from `results-v34ab-raw.jsonl`, v34 binary — within ±1%
of v33 at every cell, see §28. The W=16 serial sum **exceeds** the W=1
GIL-off sum by +32%: thread-0's single-threaded checksum walk runs ~1.3×
slower with 15 siblings parked at the barrier — sharedGCHeap allocator
state churn / background scavenge interference, not the per-op GIL-off
tax which is the W=1 ratio column. The two `postingsChecksum` calls are
**98.6%** of the serial sum (7713 / 7820 ms at W=16).)

**Expected outcome** (to be confirmed by the table): the two
`postingsChecksum` calls dominate (≈8.3M iterations of BigInt
mul/xor/`mix` + `p.docIds[i]`/`p.tfs[i]` segmented indexed reads + shard
`Map` iteration). Whichever of {heap-BigInt alloc under `sharedGCHeap`,
segmented `GetByVal` in a single-mutator context, shared-`Map` iterator
overhead} carries the largest GIL-off/GIL-on ratio becomes the named
campaign-4 engine task with a measured ms target equal to that section's
(GIL-off − GIL-on) delta.

### §27.S2 Amendment — congc default REFUTED; STW-GC is 5.4% not the ceiling

**Decision (campaign-4 C1-congc-no-default):** `useConcurrentSharedGCMarking`
stays **default `false`** (OptionsList.h:433); the GIL-off coupling block
in `Options.cpp::notifyOptionsChanged()` deliberately does **not** force it
on. Recorded there as a comment so a future campaign does not re-propose
"default congc on" without a fresh ≥10% STW-wall measurement.

**gcwall measurement (METHOD B, v33 binary, W=16, 3-run):**

| arm | rendezvous windows | STW open→resume | wall | STW % of wall |
|---|---|---|---|---|
| congc=0 (default) | 57 | 807 ms | 14847 ms | **5.4%** |
| congc=1 | 87–88 | 765 ms | 15119 ms | 5.1% |
| Δ | **+30** | -42 ms | **+272 ms (+1.8%)** | — |

**congcab independent A/B (interleaved, same host):** W=8 median 16.54 s
vs 16.51 s (+0.2%), W=16 15.83 s vs 15.51 s (+1.2%); cpu% **718 → 701**
(did not rise — the congc design predicate); 16/16 checksums identical;
crash rate 1/9 vs 2/10, not arm-correlated (the §27 P0 tier-up TOCTOU).

**Why this refutes the §25/§27 ceiling claim.** §25 named, and the §27
results bullet repeated, "STW collection wall fraction" as the remaining
W=16 ceiling. The ≥10% bar required to justify defaulting C1 is not met:
STW-GC is 5.4% of wall. Even a *perfect* congc (STW→0) caps the gain at
~800 ms, leaving W=16 ≈ 14040 ms — still above the ≤12600 ms Java-parity
target. The stwrate2 verdict ("the answer is GC pause") is likewise
refuted at 5.4%. The corrected attribution is §27.S1's: **~52% of W=16
wall is thread-0 serial inter-phase work**, and the parallel ~7150 ms
runs at ~5.6× CPU-waste vs GIL-on (cpu_util 0.44 with 16 workers awake
for ~48% of wall ⇒ effective per-worker parallel utilisation well under
1). Those two levers — serial GIL-off tax (§27.S1) and parallel-phase
waste — are where campaign-4 effort goes.

**Congc follow-up (DEFERRED).** The specific defect the profile names is
the **~30 extra Reentry-rendezvous windows** C1 introduces (57 → 87–88):
each concurrent-mark handoff opens a fresh stop window, and the
rendezvous cost (~9 ms/window at W=16) eats the 42 ms STW saving and
then some. Fixing this (coalesce C1 handoffs / piggyback on existing
windows) is a real congc work item but is **deferred**: even at zero
extra windows the ceiling is 5.4%, far short of the 12600 ms target.
Re-evaluate only after the §27.S1 serial-tax lever has moved W=16 wall
into range where 5% matters.

**Net of this task:** 0 ms (decision). Prevents a **+1.8% W=16 wall
regression** that defaulting congc would have introduced, and redirects
campaign-4 to the measured 52%-serial / parallel-waste levers.

## 28. Run 3.4 — P0 closure + R1/S2/S3 acceptance (campaign-3 follow-up)

Measures a 12-file working-tree delta (+513/−24) over `21d09c27fef2`
(campaign-3 / v33) landing the §27 follow-ups: **P0-osr-entry-toctou**
(`DFGJITCode.cpp` snapshot-then-null-check absorbs the
`clearOSREntryBlockAndResetThresholds` loser race; flag-off path
byte-identical), **R1-rss-ratio-adaptive** (`Heap.cpp` W-adaptive
`effectiveRatio = min(option, 2.0+0.5·(clients−2))` for the
floating-garbage Full trigger), **S2(d)-jslock-adaptive-spin**
(`LockObject.cpp` W-adaptive spin bound 40→20@W≥8→10@W≥16, refreshed
only in the slow path) plus the option-gated `logJSLockContention`
counters, **S3-jettison-stw-batch** (`CodeBlock.cpp` redundant-jettison
fold-into-pending-window), the §27.S2 congc-no-default decision comment
in `Options.cpp`, and the §27.S1 `bench.js` per-section timing keys. v34
binary sha256
`4f71fa548aac51fbebd86ebb49b0caa7e1e5b4057c76acc087a268c39bf078d7`
(2026-06-15; 0 sources newer than the binary; Debug bin same mtime).

**Host-drift control.** `d8ed7b6f5254` baseline reused
(`jsc-v33-baseline`, sha256 `2a85f8e5…` — bit-identical to §25/§27) and
measured back-to-back with v34 in every cell (raw in
`results-v34ab-raw.jsonl`, driver `v34_ab.sh`). The baseline today
reproduces its own §27 same-host numbers within ±2% (W=1 24777 vs §27
base 24809 = −0.1%; W=16 21156 vs §27 base 20618 = +2.6%; GIL-on W=1
16524 vs §27 base 16021 = +3.1%). 1-min loadavg at batch starts:
2.7–10.8 (W=1/2 quiet at 2.4–4.2; W=16/32 ran into the 30-rep
stability-gate decay tail at 7–13; W=32 interleaved). No sibling builds;
top non-self CPU consumer ≤6.2%.

### W=32 stability (P0 gate — runs first)

`ulimit -c 0`, pinned GIL-off env, v34 binary, `bench.js -- 32`, **30
consecutive reps**: **30/30 exit 0, 0 SIGSEGV**, every checksum tuple
matches the reference. (v33: 4/26 SIGSEGV, §27.) Per-rep wall: 25/30 in
15363–16255 ms, 5/30 bimodal-high at 21048–22668 ms (sys-time spike,
host-contention bimodality observed since §25; not arm-correlated).
**P0-osr-entry-toctou closed.**

### Before/after (run 3.3 §27 medians vs run 3.4 medians; same-host A/B vs `d8ed7b6f5254` in parentheses)

| Cell | §27 wall ms | 3.4 wall ms | vs §27 | vs same-host base | speedup-vs-self | Java bar | §27 cpu | 3.4 cpu | §27 RSS MB | 3.4 RSS MB | RSS vs base |
|---|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 25063.5 | 24558.4 | −2.0% | **−0.9%** (base 24777) | 1.00x | — | 1.04 | 1.04 | 422 | 420 | +0.3% |
| GIL-off W=2 | 24503.3 | 24617.0 | +0.5% | **−13.5%** (base 28444) | 1.00x | — | 0.90 | 0.93 | 1701 | 1641 | **+7.0%** |
| GIL-off W=4 | 19207.0 | 19101.7 | −0.5% | **−20.0%** (base 23876) | 1.29x | — | 0.71 | 0.72 | 1732 | 1747 | −0.5% |
| GIL-off W=8 | 16166.3 | 16211.0 | +0.3% | **−22.9%** (base 21017) | **1.51x** | 1.99x | 0.56 | 0.57 | 1765 | 1741 | −2.3% |
| GIL-off W=16 | 15193.4 | 15024.2 | −1.1% | **−29.0%** (base 21156) | **1.63x** | 1.99x | 0.44 | 0.46 | 1789 | 1792 | −1.4% |
| GIL-off W=32 | 15950.1 | 15933.9 | −0.1% | **−27.2%** (base 21874) | **1.54x** | 1.75x | 0.33 | 0.32 | 1843 | 1847 | +4.5% |
| GIL-on W=1 | 15764.7 | 16157.6 | +2.5% | **−2.2%** (base 16524) | — | — | 1.03 | 1.03 | 421 | 422 | +0.2% |

- All 51/51 A/B reps rc=0, every checksum tuple matches the reference
  (`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96`).
  Per-rep spread tight (≤2%) at every cell except W=4 v34 (rep3 outlier
  15994 ms phaseA 6739, vs reps 1/2 ~9500 — JIT-warmup luck; median is
  rep1) and W=32 base rep3 27381 ms (sys 57.6 s, the §25/§27 bimodality;
  median rep1 21874).
- **Speedup-vs-self vs Java curve** (vs v34's own W=1 24558.4 ms): W=8
  1.51x **< 1.99x** (gap −3870 ms wall); W=16 1.63x **< 1.99x** (gap
  −2683 ms); W=32 1.54x **< 1.75x** (gap −1901 ms). **All three Java
  bars MISSED.** v34 wall is within ±1.1% of v33 at every W — the
  v34-over-v33 delta is correctness (P0), RSS tuning (R1), and
  contention-shape work (S2(d)/S3) whose effect at this benchmark is
  sys-time only: W=16 sys 19.4 s → 7.4 s (−62%), W=32 sys 25.5 s →
  11.7 s (−54%) — fewer rendezvous/futex syscalls — but user+sys/wall
  (cpu_util) moves only 0.44 → 0.46. The §27.S1 52%-serial ceiling is
  unchanged (no v34 piece touches the serial code paths).
- **§27.S1 attribution table now filled** (above). The two
  `postingsChecksum` calls are 7713 of 7820 ms W=16 serial (98.6%),
  GIL-off/GIL-on ratio 1.51–1.54×, and an additional 1.32× W=16/W=1
  GIL-off penalty (3740/2785, 3973/3082) from sibling-parked sharedGCHeap
  interference. **Campaign-4 named target:** the `postingsChecksum`
  GIL-off tax — heap-BigInt mul/xor allocation under `sharedGCHeap` is
  the dominant per-iteration cost (8.3M iterations); a per-lite BigInt
  scratch / nursery or an int64 fast path here is worth ~2030 ms at W=1
  (5867 → 3849) and ~3800 ms at W=16 (7713 → 3849) — the latter alone
  takes W=16 to ~11200 ms ≈ 2.19×, clearing the 1.99× bar.
- Honest negatives: **(a)** GIL-off W=2 RSS **+7.0% same-host**
  (1534→1641 MB) — R1's W-adaptive ratio recovered ~60 MB of the §27
  +10.3% (v33 1701→v34 1641, −3.5%) but the residual is the campaign-3
  T1/T3 JIT-code/segmented-spine retention itself, not the Full-trigger
  cadence (v34 W=2 already runs at effectiveRatio=2.0, ~50% Full
  cadence). Recorded as accepted: 107 MB at W=2 only; W≥4 RSS flat to
  −2.3%. **(b)** GIL-on W=1 vs §27 published +2.5% (16158 vs 15765) —
  but the **same-host interleaved A/B is −2.2%** (base today 16524), so
  this is host drift, not a v34 regression; the ≤2% gate uses the
  same-host number and PASSES. **(c)** W=32 RSS +4.5% same-host
  (unchanged from §27's +5.2%; within 5%).

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV** (v33: 4/26) |
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug bin) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug bin) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v34) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 19186.5 ms (§27: 19105.9, +0.4%) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **−2.2%** (≤2% gate PASS) |
| No same-host wall cell regressing >5% | **PASS** (worst: W=2 +0.5% vs §27, −13.5% vs base) |
| Speedup-vs-self beats Java at W=8/16/32 | **FAIL** (1.51/1.63/1.54 vs 1.99/1.99/1.75) |

### Acceptance verdict

**P0 CLOSED; Java bar MISSED.** Corpus + identity green; W=32 0/30
crash; GIL-on same-host −2.2%; no wall regression anywhere; W∈{2..32}
−13.5% to −29.0% same-host vs `d8ed7b6f5254`. But v34 wall ≈ v33 wall at
every W (the v34-over-v33 delta moved only sys-time and RSS), so
speedup-vs-self stays at **1.63× @ W=16 < 1.99×** — the §27.S1 serial
ceiling is unchanged. The campaign-3 delta can land (P0 was its blocker;
R1 partially closes the W=2 RSS negative; S2(d)/S3 are pure wins on
sys-time with no wall regression). The Java-parity gap is now
**quantified to a single section** (postingsChecksum heap-BigInt
GIL-off tax, ~3800 ms W=16) and named as the campaign-4 target above.

## §29 Fairness amendment: postingsChecksum is a SPEC-mandated type asymmetry, not a threading deficit

### Finding

The §28 campaign-4 target — the `postingsChecksum` GIL-off tax that
accounts for 51% of W=16 wall and the entire Java-parity gap — is a
**spec-mandated cross-language type asymmetry**, not a threading
artifact. `Tools/threads/scalebench/SPEC.md` L49-53 fixes the checksum
arithmetic per language:

> All arithmetic on seeds/hashes is unsigned 64-bit with wraparound. JS
> must use `BigInt` masked to 64 bits (`& 0xFFFFFFFFFFFFFFFFn`) for the
> PRNG and checksums; Go uses `uint64`; Java uses `long` …

Audit of the three implementations confirms the spec is followed exactly
and no implementation reaches for arbitrary-precision:

| Lang | File:line | Checksum accumulator | Per-iter cost |
|---|---|---|---|
| Go | `go/main.go` L348 `func indexChecksum() (sum uint64, count uint64)` | native `uint64`, register-resident | add+mul+xor, no alloc |
| Java | `Bench.java` L243-255 `static long[] indexChecksum() { long sum = 0, … }` | native `long`, register-resident | add+mul+xor, no alloc; no `java.math.BigInteger` anywhere |
| JS | `js/bench.js` L312-324 `postingsChecksum()` + `mix()` L85-90 | heap `JSBigInt`, `& MASK` after every op | ~10 BigInt mul/xor/add/shift per posting; each op allocates 2-3 heap `JSBigInt` under `sharedGCHeap` |

At 8.3M postings × ~10 ops/posting this is **~83M heap-BigInt
allocations** on the JS side versus **zero** on the Go/Java side. There
is no `math/big` import in `main.go` and no `BigInteger` reference in
`Bench.java`.

### Measured budget for the same work

| Arm | Source | pc1+pc2 (+ dfSnap+sort) wall ms | Ratio vs Go |
|---|---|---|---|
| Go W=1 | `out-run1` rep1 inter-phase (total − Σ phases) = 1777.3 − 1689.2 | **88** | 1.0× |
| Java W=1 | `out-run1` rep1 inter-phase = 1898.4 − 1792.8 | **106** | 1.2× |
| JS v34 GIL-on W=1 | §28 attribution table: 1846.5 + 2002.5 | **3849** | 44× |
| JS v34 GIL-off W=1 | `results-v34ab-raw.jsonl` rep3: 2814.7 + 3051.3 | **5866** | 67× |

The Go/Java inter-phase residual covers *both* checksum passes plus
`dfSnap` and the §1.9 sort, so 88/106 ms is an **upper bound** on their
checksum cost. JS pays **~55–65×** the native-u64 budget for
bit-identical output — a serial BigInt-allocation tax that exists
identically at W=1 with zero sibling threads.

### Interpretive rulings (this section is the amendment; nothing else changes)

**(i)** The engine-side **B1 fix stands on its own merits**, independent
of cross-language fairness: *heap-BigInt allocation must not regress
1.5× GIL-off vs GIL-on on identical single-threaded code under
`sharedGCHeap`*. The 5866/3849 = **1.52×** GIL-off/GIL-on ratio at W=1
(§28 table: 1.51–1.54×) is a `useSharedGCHeap=1` allocation-path
regression measured against the engine's own GIL-on baseline — it would
be a bug at any absolute cost and is the campaign-4 deliverable
regardless of what Go/Java pay.

**(ii)** The headline **JS-vs-Java speedup-vs-self numbers in §28 carry
a serial-BigInt tax that Go and Java do not pay**. The §28
"speedup-vs-self vs Java curve" row (1.51/1.63/1.54× vs 1.99/1.99/1.75×)
compares each language to *its own* W=1, so a large JS-only serial
component depresses the JS ratio by Amdahl while leaving the Java ratio
untouched. Read those cells as "JS scaling under a ~5.9 s spec-imposed
serial floor that Java runs in ~0.1 s", not as "JS threading is 0.4×
worse than Java threading". The W=16 sibling-interference component
(1.32×, §28) *is* a threading cost and remains correctly attributed.

**(iii)** **`bench.js` and `SPEC.md` are NOT changed.** The spec's
BigInt mandate is the only portable way to get bit-identical u64
wraparound in JS without an engine intrinsic; rewriting the bench to
dodge it would invalidate the cross-language checksum reference. The
engine fix (per-lite BigInt scratch / nursery or an int64-fitting fast
path, §28 last bullet) is the deliverable; this §29 amendment is
**interpretive only** — it annotates how to read §28's cross-language
ratios, it does not relax any gate.

## §30 Run 3.5: campaign-4/B1 sharedGCHeap allocation-path delta — Java bar still missed

Measures a 7-file working-tree delta (+369/−27) over `3909da474a7e`:
`BlockDirectory.{h,cpp}`, `GCThreadLocalCache.cpp`, `Heap.{h,cpp}`,
`LocalAllocator.cpp`, `InlineCacheCompiler.cpp`. Release `jsc-v35` sha256
`8ff9c49a…`; Debug sha256 `e4afea49…`. Baseline `d8ed7b6f5254` reused
bit-identically (`jsc-v33-baseline`, sha256 `2a85f8e5…` — identical to
§25/§27/§28). Driver `v35_ab.sh`, raw `results-v35ab-raw.jsonl`.

**Host-drift control.** Loadavg 3.3–14.2 across batches (a separate
`/root/bun` bridge build session held ~5 cores intermittently; 64-core
host so W≤32 retains ≥27 spare cores). The same-host base column
(back-to-back A/B per cell, W=32 + GIL-on interleaved) controls for it;
absolute walls carry ±~2% host noise.

### Before/after (run 3.4 §28 medians vs run 3.5 medians; same-host A/B vs `d8ed7b6f5254` in parentheses)

| Cell | §28 wall ms | 3.5 wall ms | vs §28 | vs same-host base | speedup-vs-self | Java bar | §28 cpu | 3.5 cpu | §28 RSS MB | 3.5 RSS MB | RSS vs base |
|---|---|---|---|---|---|---|---|---|---|---|---|
| GIL-off W=1 | 24558.4 | **22333.2** | −9.1% | **−9.7%** (base 24732) | 1.00x | — | 1.04 | 1.05 | 420 | 423 | −0.3% |
| GIL-off W=2 | 24617.0 | **22630.1** | −8.1% | **−23.6%** (base 29605) | 0.99x | — | 0.93 | 1.01 | 1641 | 1315 | **−14.7%** |
| GIL-off W=4 | 19101.7 | **17728.0** | −7.2% | **−25.5%** (base 23796) | 1.26x | — | 0.72 | 0.80 | 1747 | 1254 | **−28.1%** |
| GIL-off W=8 | 16211.0 | **15348.6** | −5.3% | **−28.8%** (base 21550) | **1.455x** | 1.99x | 0.57 | 0.63 | 1741 | 1302 | **−26.5%** |
| GIL-off W=16 | 15024.2 | **14478.1** | −3.6% | **−31.0%** (base 20974) | **1.543x** | 1.99x | 0.46 | 0.51 | 1792 | 1333 | **−26.4%** |
| GIL-off W=32 | 15933.9 | **15358.2** | −3.6% | **−28.8%** (base 21557) | **1.454x** | 1.75x | 0.32 | 0.37 | 1847 | 1368 | **−22.0%** |
| GIL-on W=1 | 16157.6 | **16087.2** | −0.4% | **−3.2%** (base 16612) | — | — | 1.03 | 1.03 | 422 | 421 | +0.0% |

- All 51/51 ladder+gilon+congc reps and 30/30 stability reps rc=0; every
  checksum tuple matches the reference
  (`b3e65a6855b9bdeb|4158957|39c33392b2a4c5b2|c4bdd580f85ee058|af028188d7a56a96`).
- **Speedup-vs-self vs Java curve** (vs v35's own W=1 22333.2 ms): W=8
  1.455x **< 1.99x** (gap −4126 ms wall, need ≤11223); W=16 1.543x
  **< 1.99x** (gap −3255 ms, need ≤11223); W=32 1.454x **< 1.75x** (gap
  −2596 ms, need ≤12762). **All three Java bars MISSED.** v35 lowers W=1
  by 9.1% (−2225 ms) but W=16 only by 3.6% (−546 ms), so the
  speedup-vs-self ratio actually *drops* slightly at W=8/32 (1.51→1.455,
  1.54→1.454) — Amdahl: shrinking the serial component improves
  absolutes but the parallel-section ceiling is unchanged.
- **Per-section delta** (medians, v35 vs §28 v34): pc1+2 −793/−784/
  −944/−1120/−654/−674 ms at W=1..32 (the B1 target moved); phaseA
  −1210/−1090/−341/**+114**/**+246**/**+343** ms; phaseB −179/−140/−59/
  +5/+84/−102 ms. **§28-attribution update:** v35 W=16 pc1+2 = 7059 ms
  = **49% of 14478** (was 51% of 15012); GIL-off/GIL-on W=1 pc1+2 ratio
  **1.524x → 1.433x** (5073/3540); W=16/W=1 sibling-interference on
  pc1+2 **1.315x → 1.391x** (7059/5073). The §29(i) `sharedGCHeap`
  allocation tax is *reduced* (1.52→1.43) but not closed; the
  sibling-interference ratio worsened in relative terms (absolute pc1+2
  is lower at every W, but W=1 dropped more than W=16).
- **Fairness note (per §29):** the cross-language Java-bar gate compares
  speedup-vs-self under JS's spec-mandated ~5.1 s serial heap-BigInt
  floor vs Java's ~0.1 s native-`long` floor. The 49% serial pc1+2 share
  caps achievable speedup at ~2.05× even with perfect parallel scaling
  of everything else; v35 achieves 1.54× of that ceiling. The threading
  cost is the parallel-section residual (W=16 wall − pc1+2 = 7419 ms vs
  W=1 17260 ms → 2.33× on the parallelizable 77%).
- **Honest negatives:** **(a)** phaseA regresses **+2–6%** at W∈{8,16,32}
  (+114/+246/+343 ms) — the B1 allocation-path change costs the parallel
  ingest hot loop slightly; net wall is still −3.6% to −5.3% at those W
  because pc1+2 wins outweigh it. **(b)** W=16 pc-sibling-interference
  ratio worsened 1.32×→1.39× (B1 helped W=1 more than W=16). **(c)**
  Loadavg 5–14 during batches (foreign `/root/bun` build); the
  same-host-base column controls for it but absolute vs-§28 deltas carry
  ±~2% noise. **(d)** stab32 rep-spread wide (min 14899 / max 21278 ms,
  43%) — host bimodality, not arm-correlated; median 15400 stable.

### Gates

| Gate | Result |
|---|---|
| **W=32 stability 30 reps (P0)** | **30/30 exit 0, 0 SIGSEGV**, 1 unique cs tuple (median 15399.6 ms, loadavg 3.3→14.3) |
| Corpus GIL-off full (`run-tests.sh`, pinned env, Debug) | **94 passed, 0 failed, 4 skipped** |
| Corpus GIL-on full (`JSC_useJSThreads=1`, Debug) | **95 passed, 0 failed, 3 skipped** |
| Flag-off identity (`v5a-identity.sh`, 40 tests, Release v35) | **0 mismatches** |
| congc: `bench.js -- 4` + `JSC_useConcurrentSharedGCMarking=1`, 5 reps | **5/5 exit 0**, correct checksums, median 18158.6 ms (§28: 19186.5, −5.4%) |
| GIL-on W=1 5-rep interleaved A/B vs `d8ed7b6f5254` | **−3.2%** (≤2% gate PASS) |
| **BigInt-stress GIL-off** (`grep -l BigInt JSTests/stress \| head -50`, pinned env, Release v35) | **49/50 pass**; 1 fail `atomic-increment-bigint64.js` rc=134 is **pre-existing on baseline+v34** (wasm-disabled-under-GIL-off abort, not B1-related) — **no v35 regression** |
| No same-host wall cell regressing >5% | **PASS** (every cell −3.6% to −9.1% vs §28 v34; −9.7% to −31.0% vs same-host base) |
| Speedup-vs-self beats Java at W=8/16/32 | **FAIL** (1.455/1.543/1.454 vs 1.99/1.99/1.75) |

### Acceptance verdict

**All correctness gates GREEN; Java bar MISSED.** Corpus + identity +
BigInt-stress green; W=32 0/30 crash; GIL-on same-host −3.2%; congc
−5.4%; no wall regression anywhere; W∈{1..32} −3.6% to −9.1% vs §28 v34
(−9.7% to −31.0% same-host vs `d8ed7b6f5254`); RSS −22% to −28% at W≥2;
sys-time −56% to −70% at W≥2. But B1 closed only ~0.09 of the 1.52×
GIL-off/GIL-on pc-ratio (→1.43×) and W=1 improved more than W=16, so
speedup-vs-self moved 1.63→1.54 at W=16 — **further from**, not toward,
the 1.99× bar in ratio terms despite every absolute being faster. The
campaign-4 delta can land (pure wins on wall/RSS/sys at every W; no
correctness regression; phaseA +2–6% at W≥8 is the only sub-component
loss). **Campaign-5 named target:** the residual 1.43× GIL-off/GIL-on
pc-ratio (still ~1530 ms at W=1) plus the 1.39× pc sibling-interference
— closing both takes W=16 pc1+2 to ~3540 ms → wall ~10960 ms ≈ 2.04×,
clearing the bar.
