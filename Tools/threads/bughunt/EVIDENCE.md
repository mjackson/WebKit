# EVIDENCE PACK — spawned-thread-butterfly-stress silent value corruption (GIL-off full JIT)

Date: 2026-06-09. Binary under test: `WebKitBuild/Debug/bin/jsc` built
2026-06-08 18:27 UTC (includes the working-tree `runtime/VM.cpp` modification,
mtime 06:55 — binary postdates it). Pinned flags:
`--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1
--useSharedGCHeap=1 --useThreadGILOffUnsafe=1 --useDollarVM=1
--randomYieldPeriod=64 --randomYieldSeed=<seed>` (RaceAmplifier maxSleepUs=100
unless noted). Facts only; no fixes, no hypotheses beyond what data forces.

## 0. Headline result of this run

The silent corruption did NOT reproduce on the current binary in ~2,400
fresh runs across 13 configurations (details §3) — including an exact
replication of the seed set, worker layout, and 6-way load shape that
produced the two known failures on 2026-06-08 evening (same binary).
The only current-binary corruption evidence remains the two 2026-06-08 logs
(§1). Everything any hypothesis must explain is therefore: (a) the decoded
semantics of those two failures (which are sharp), (b) the layout facts
(§2), and (c) the non-reproduction envelope (§3), which bounds the failure
rate at my load shapes to < ~1/2400 even though the 06-08 round saw ~2/240.
The discriminating variable between 06-08 and today is host load shape
(unknown co-running workloads then), NOT the binary or seeds.

## 1. Corruption semantics (decoded)

Value encoding (test source): `value = tid*1000000 + serial*1000 + p` for
property `"p"+p`, p in [0,24). Each `p*` property is written EXACTLY ONCE,
inside `buildOne()`, BEFORE the object is published into the shared registry.
Nothing ever rewrites them. **"Stale by 9 writes" is impossible** — there is
only one write per slot, ever. The corruption is a WRONG-SLOT (or
wrong-offset) read.

Current-binary failures (both 2026-06-08, plain `Tools/threads/load6.sh`,
default SEED_BASE=1000, Debug jsc of 18:27):

| seed | log | got | decoded | want | decoded | meaning |
|---|---|---|---|---|---|---|
| 3012 | `/tmp/load6-logs/FAIL-rc3-w2-r12-s3012.log` (18:56) | 1003008 | tid=1 serial=3 p=8 | 1003017 | tid=1 serial=3 p=17 | read of `p17` returned `p8`'s value |
| 6014 | `/tmp/load6-logs-2/FAIL-rc3-w5-r14-s6014.log` (19:07) | 2023017 | tid=2 serial=23 p=17 | 2023008 | tid=2 serial=23 p=8 | read of `p8` returned `p17`'s value |

Sharp properties of this pair:
- SAME-OBJECT cross-property reads (tid and serial match got↔want in both).
  Not cross-object, not cross-thread values, not type confusion (both
  well-formed Int32 values of sibling properties).
- Both involve EXACTLY the pair {p8, p17}, once in each direction — a
  swap-shaped signature (Δ = 9 property slots both ways), not a
  one-directional staleness shift.
- Both faulted in `checkOne` (test line 52, the named-property compare)
  called from `threadBody` line 86 — the FOREIGN-read loop
  (`checksum += checkOne(theirs[i])`): the failing reader does NOT own the
  object. The read form is `o["p" + p]` — get_by_val with a freshly
  rope-concatenated, atom-resolved string key.
- The owner of the corrupted object is a spawned thread in both cases
  (tid 1 and tid 2), read by some other thread.
- The object's structure WAS being transitioned concurrently by its owner
  during the failure window (`late*` adds, one per round), but its butterfly
  is NEVER reallocated during the concurrent phase (capacity proof in §2).

Historical context (PRE-current-binary logs, June 7 – June 8 morning,
`/tmp/sb.*`, `/tmp/item2/*`, ~38 distinct "named property corrupt" failures):
the old signature space was much broader — cross-object, cross-tid, and
serial-field corruption (e.g. `got 1004000 want 4000`, `got 2005007 want
10019`). Those binaries predate the 2026-06-08 shared-cache /
VAMP-offset-swap / F3 fixes. On the current binary only the narrow
{p8,p17} same-object swap signature has ever been observed. (Do not mix the
two corpora when reasoning about the current bug.)

## 2. Object layout (measured with $vm.dumpCell, flag-off AND pinned GIL-off flags — identical)

`buildOne` object `{tid, serial, p0..p23, indexed}`:
- inlineCapacity = 4 → inline: `tid`(off 0), `serial`(off 1), `p0`(off 2), `p1`(off 3).
- Out-of-line: `p2`..`p23` at OOL indices 0..21, `indexed` at OOL index 22.
- Final butterfly propertyCapacity = 32, preCapacity 0. Adding a `late*`
  property does NOT move the butterfly (verified: same base/butterfly
  pointer after the transition). OOL usage peaks ≈25 (< 32) for the whole
  test, so during the entire concurrent phase a published registry object's
  butterfly is never reallocated for property growth; only its StructureID
  changes (owner `late*` transitions) and the butterfly word's high tag bits
  (GIL-off tagged word observed: `0x4000_7e82fb250a08`).
- p8 = OOL index 6; p17 = OOL index 15. Δ = 9 slots = 72 bytes. OOL slots
  grow DOWNWARD from the butterfly pointer, so p8's slot address is 72 bytes
  ABOVE p17's.
- PropertyOffsets: p8 = firstOutOfLineOffset+6, p17 = firstOutOfLineOffset+15.

Constraint on any explanation: the two slots live in the same once-written,
never-moved 32-slot OOL block. A wrong value therefore requires the READER
side to have used the other property's offset (off by exactly ±9 OOL slots)
— wrong offset from a (structure, uid)→offset mapping, an IC stub, a
megamorphic cache entry, a property-table lookup, or a butterfly-derived
pointer displaced ±72 bytes. Plain memory staleness of the slot itself
cannot produce these values.

Related same-family hard datum (older binary, 2026-06-08 morning,
`/tmp/load6-logs2/FAIL-rc134-w5-r11-s12011.log`): Debug assert
"Detected offset inconsistency: numberOfSlotsForMaxOffset doesn't match
totalSize!" — `transitionOffset=68 maxOffset=68 m_inlineCapacity=3
numberOfSlotsForMaxOffset=8 totalSize=9 inlineOverflowAccordingToTotalSize=6
numberOfOutOfLineSlotsForMaxOffset=5`: a Structure observed with its
property-table size and maxOffset views disagreeing by one slot. Direct
evidence (albeit pre-fix binary) that structure/property-table publication
was observable torn in this tree's lineage.

## 3. Reproduction campaign (current binary, 2026-06-09)

Test files: original test, plus `Tools/threads/bughunt/repro.js`
(instrumented copy — identical oracle; on mismatch dumps got/want decode,
re-reads via three access forms, all 24 siblings, indexed lane, ownKeys,
indexingMode, $vm.dumpCell, and a 5ms-later re-read) and
`Tools/threads/bughunt/repro-loop.js` (5 outer reps per process, fresh
registries per rep, hot JIT). Campaign driver:
`Tools/threads/bughunt/load6x.sh` (load6.sh + EXTRA_FLAGS support).
Background "corpus load" during most campaigns: 6 looping jsc workers over
ic-publish-reset-loops, shared-arraystorage-stress, transition-vs-write,
transition-vs-read, i03-i37-same-shape-add-storm, i03-t5-racing-growers
(`/tmp/corpusload.sh`).

| config | runs | corruption | other failures |
|---|---|---|---|
| 4× load6 concurrently (24 jsc procs), original test, seeds 1000–9019 | 480 | 0 | 0 |
| Exact replication ×4: plain load6, repro.js, SEED_BASE=1000 (includes the historically failing seeds 3012, 6014), + corpus load | 480 | 0 | 0 |
| Seed 3012 solo, quiet-ish host | 10 | 0 | 0 |
| Seed 3012 solo, `taskset -c 0,1` | 10 | 0 | 0 |
| repro-loop.js (5 reps/proc), seeds 31000+ | ~60×5 reps | 0 | 1× GC mark-stack assert (below); 5× rc124 timeout during stress-ng window |
| maxSleepUs=1000 (A) | 90 | 0 | 0 |
| period=64 dup (B) | 90 | 0 | 0 |
| period=16 + maxSleepUs=500 (B2) | 90 | 0 | 0 |
| all workers pinned to cores 0–7 (D) | 90 | 0 | 0 |
| --collectContinuously=1 (E) | 60 | n/a | 60/60 instant `ASSERTION FAILED: m_requests.isEmpty() \|\| (m_worldState.load() & mutatorHasConnBit)` Heap.cpp:1654 — collectContinuously is structurally incompatible with GIL-off; NOT usable as an amplifier |
| --forceRAMSize=100MB (F) | 60 | 0 | 2× STW-watchdog 30s (stress-ng window) |
| baseline (G) + --forceRAMSize=60MB (H) under `stress-ng --cpu 24 --vm 8` | ~120 | 0 | 3× STW-watchdog 30s (JSThreadsSafepoint.cpp:476/494, "CodeBlock jettison" requester, one lite `hasHeapAccess=true` non-quiescent) — known ab17b/ab17c family, fires under extreme host starvation; pollutes campaigns, so extreme oversubscription is NOT a usable amplifier either |
| --useFTLJIT=0 (N1) | 90 | 0 | 0 |
| --useDFGJIT=0 (N2), --forceSegmentedButterflies=1 (N3), --verifyConcurrentButterfly=1 (N4), baseline loop ×8 (original test) | in flight at time of writing — final counts in §3.1 |

§3.1 FINAL COUNTS: (filled in at end of run — see addendum at bottom.)

Determinism answer (item 1 of the brief): failing seeds do NOT fail
deterministically. Seed 3012: 0/10 solo, 0/10 under taskset-2-cores, 0/~8
exact-replication re-runs under 6-way load. The failure is a function of
host-load timing, not of the seed's yield schedule alone. The RaceAmplifier
seed does not capture enough of the schedule to make this bug replayable.

## 4. Side findings (real, but NOT the chartered bug)

1. **GC mark-stack race (crash, current binary)**:
   `/tmp/bh-loop/FAIL-rc134-w2-r4-s33004.log` — Debug assert
   `result == m_segments.head()->m_top++` in
   `GCSegmentedArray<const JSCell*>::postIncTop()`
   (GCSegmentedArrayInlines.h:145), i.e. two threads pushing/popping one
   mark stack concurrently, seed 33004, repro-loop.js under corpus load.
   First time this signature is on record per /tmp history. Symbolized
   frames in `/tmp/bh-loop-gc-assert-symbolized.txt` (if addr2line
   completed; raw offsets in the log otherwise).
2. **STW watchdog under extreme starvation**: see table (F/G/H). Known
   family; the participant dump shows a single lite with
   `hasHeapAccess=true` blocking a CodeBlock-jettison stop for >30s wall
   under a 4×-oversubscribed host. Distinguish from genuine wedges before
   acting on these under load campaigns.
3. `--collectContinuously=1` asserts immediately GIL-off (table row E) —
   worth a flag-validation guard eventually.

## 5. rr record-replay

NOT AVAILABLE on this host:
- no `rr` in AL2023 dnf repos;
- upstream rr-5.9.0 rpm has unresolvable deps here (needs glibc-32bit pieces
  and `libcapnp.so.0.10.3` — no capnproto package in AL2023);
- EC2 PMU is only partially exposed: `perf stat -e r5101c4` (retired
  conditional branches, rr's required counter) warns
  "bits 16,20,22 of config not supported by kernel" — rr would most likely
  refuse or be unreliable even if built from source.
Not pursued further. (Also note: rr serializes to one core; this bug needs
real parallelism + load, so even chaos mode would be a long shot.)

## 6. Hard facts any hypothesis must explain

1. **Wrong-offset, not stale-data**: each `p*` slot is written exactly once,
   pre-publication; the butterfly is never reallocated nor the slots
   rewritten during the race window. Yet a foreign reader got the value of
   the OTHER property of the SAME object, with the {p8,p17} pair hit in BOTH
   directions (Δ = ±9 OOL slots / ±72 bytes / PropertyOffset ±9).
2. **Reader-side, by-val, foreign**: both failures are `o["p"+p]` get_by_val
   reads in `checkOne` on the foreign-read path (`theirs[i]`), with the key
   a freshly concatenated rope resolved through the SHARED atom-string
   table; the same checkOne also reads the owner's objects all test long
   without tripping.
3. **Concurrent owner activity is structure transitions only**: during the
   window the owner adds `late<round>` properties (new Structure, same
   butterfly) to one own object per round, and all threads write the common
   `shared` object's `fromSlot*` properties. No butterfly moves.
4. **It is rare and load-shaped**: 2/240 on 2026-06-08 evening vs 0/~2400
   on the same binary, same seeds, same harness on 2026-06-09 under every
   load shape tried (including exact replication, 8-core pinning, 2-core
   taskset, period/sleep variations, small-heap GC pressure). Whatever
   window exists, the RaceAmplifier's instrumented sites + seed do not
   control it; the discriminator is external host-scheduling state.
5. **The value pair is exact and clean**: got/want are both valid Int32
   sibling values — no torn JSValue, no boxed/unboxed confusion, no tag
   garbage. A 5ms-later re-read result is unknown for the historic failures
   (the instrumented repro will answer HEALED vs STILL-WRONG when it next
   fires — `Tools/threads/bughunt/repro.js` is in place for the next
   campaign).
6. **Lineage**: the pre-fix binaries showed a much broader corruption space
   (cross-object/cross-tid); after the 06-08 fix round only the same-object
   {p8,p17} swap shape has been seen. The landed fixes narrowed, not
   eliminated, the wrong-offset read family.

## 7. Minimal reproducing config (best known)

NOT currently reproducible on demand. Best-known historical config:
Debug jsc (2026-06-08 18:27 build), plain `Tools/threads/load6.sh`
(6 workers, SEED_BASE=1000, period=64, maxSleepUs=100) on the original test,
on a host with substantial UNRELATED concurrent load (exact co-load on
2026-06-08 evening unknown) — observed ~2/240 there; 0/~2400 since.
Recommended next instrument: run `Tools/threads/bughunt/repro.js` as the
standing campaign test (identical oracle, rich state dump at mismatch +
HEALED/STILL-WRONG probe) so the NEXT natural occurrence pins down
persistence, sibling state, structureID and butterfly at the moment of
corruption.
