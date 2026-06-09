# EVIDENCE PACK — spawned-thread-butterfly-stress silent value corruption (GIL-off full JIT)

Status: IN PROGRESS — campaigns running. Facts only; no fixes, no hypotheses beyond what data forces.

Target: `JSTests/threads/jit/spawned-thread-butterfly-stress.js`, rc=3
"named property corrupt", under 6-way load (`Tools/threads/load6.sh`),
Debug jsc, pinned GIL-off flags + `--randomYieldPeriod=64 --randomYieldSeed=<seed>`.

## 1. Corruption semantics (decoded — from test source + prior failure logs)

Value encoding (test source): `value = tid*1000000 + serial*1000 + p` for
property `"p"+p`, p in [0,24). Each `p*` property is written EXACTLY ONCE,
inside `buildOne()`, BEFORE the object is published into the shared registry.
Nothing ever rewrites them. Therefore "stale by 9 writes" is impossible —
there is only one write per slot, ever. The corruption is a WRONG-SLOT read.

Observed failures (prior round logs kept in /tmp):

| seed | got     | decoded        | want    | decoded         | meaning |
|------|---------|----------------|---------|-----------------|---------|
| 3012 (`/tmp/load6-logs/FAIL-rc3-w2-r12-s3012.log`) | 1003008 | tid=1 serial=3 p=8 | 1003017 | tid=1 serial=3 p=17 | read of `p17` returned `p8`'s value |
| 6014 (`/tmp/load6-logs-2/FAIL-rc3-w5-r14-s6014.log`) | 2023017 | tid=2 serial=23 p=17 | 2023008 | tid=2 serial=23 p=8 | read of `p8` returned `p17`'s value |

Both are SAME-OBJECT cross-property reads (tid and serial match between got
and want). Not cross-object, not cross-thread values, not stale values.
Both involve EXACTLY the pair {p8, p17}, in BOTH directions (a swap-shaped
signature, not a one-directional shift). Both stacks fault at
`checkOne` line 52 (the named-property check) called from `threadBody`
line 86 — the FOREIGN-read loop (`checksum += checkOne(theirs[i])`), i.e.
a reader that does NOT own the object.

Related (same test family, prior campaign, seed 12011,
`/tmp/load6-logs2/FAIL-rc134-w5-r11-s12011.log`): Debug assert
"Detected offset inconsistency: numberOfSlotsForMaxOffset doesn't match
totalSize!" with `transitionOffset=68 maxOffset=68 m_inlineCapacity=3
numberOfSlotsForMaxOffset=8 totalSize=9 inlineOverflowAccordingToTotalSize=6
numberOfOutOfLineSlotsForMaxOffset=5` → a Structure whose property-table
view and maxOffset view disagree by exactly ONE slot was observed in this
tree under the same load harness. (Different inlineCapacity=3 object — not
the registry object — but it is hard evidence that structure/property-table
publication can be observed in a torn state.)

## 2. Object layout (measured with $vm.dumpCell, both flag-off and pinned GIL-off flags)

`buildOne` object `{tid, serial, p0..p23, indexed}`:
- inlineCapacity = 4 → inline slots: `tid`(off 0), `serial`(off 1), `p0`(off 2), `p1`(off 3).
- Out-of-line: `p2`..`p23` at OOL indices 0..21, `indexed` at OOL index 22.
- Final butterfly propertyCapacity = 32, preCapacity 0. Verified: adding a
  `late*` property does NOT reallocate the butterfly (same base pointer
  after transition) — OOL usage peaks ≈25 < 32, so during the whole
  concurrent phase the butterfly of a published registry object is never
  reallocated for property growth; only its Structure transitions (owner
  `late*` adds) and butterfly-word tag bits change.
- p8 = OOL index 6; p17 = OOL index 15. Δ = 9 slots = 72 bytes.
  PropertyOffsets: p8 = firstOutOfLineOffset+6, p17 = firstOutOfLineOffset+15.
  OOL slots grow DOWNWARD from the butterfly pointer, so p8's slot address is
  72 bytes ABOVE p17's slot.
- GIL-off butterfly word carries tag bits in the high byte (observed
  `0x40007e82fb250a08` vs untagged `0x00007e82fb250a08` flag-off).

Constraint this puts on any explanation: the two slots are in the same
once-written, never-reallocated 32-slot OOL block; a "wrong value" read means
either the reader computed the wrong OFFSET for the uid (off by exactly 9
slots, in either direction), or read through a butterfly-derived pointer
displaced by ±72 bytes, or an offset↔uid association was crossed (e.g. a
cache/IC keyed lookup returning the other property's offset).

## 3. Reproduction campaign

(in progress — counts and seeds to be filled in)

## 4. Tier/flag narrowing

(in progress)

## 5. rr record-replay

`rr` is NOT installed on this host (`which rr` → not found). Not attempted.
(Would likely be of limited value anyway: rr serializes to one core, and this
bug needs true parallelism + host load; chaos mode might still work, but the
tool is unavailable.)

## 6. Hard facts any hypothesis must explain

(to be finalized after campaigns)
