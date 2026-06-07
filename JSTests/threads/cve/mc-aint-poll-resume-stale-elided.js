//@ requireOptions("--useJSThreads=1")
// MC-AINT S4 (docs/threads/cve/map-MC-AINT.md): parked-at-poll resume across
// a Class-A fire — SPEC-jit I21(b) is specified but UNIMPLEMENTED in the
// tree (DFGByteCodeParser.cpp:7039 emits CheckTraps WITHOUT an invalidation
// point when usePollingTraps is forced; compileCheckTraps in DFG/FTL lowers
// a plain poll; CheckTraps clobbers only InternalState so the
// InvalidationPointInjectionPhase places nothing after it; and
// operationHandleTraps returns straight to the post-poll PC).
//
// Mechanism (the async-interruption-at-an-unsafe-point shape, with the
// cooperative stop itself as the interruption): GIL-off, a Class-A
// watchpoint fire runs as an STWR. Reader threads hot in DFG/FTL code park
// at their CheckTraps polls; the fire falsifies the watched fact
// (transitionThreadLocal/writeThreadLocal) and jettisons their elided code;
// on resume each reader continues at the instruction AFTER the poll and may
// execute E1/E2-elided butterfly accesses against the now-false fact —
// e.g. an E1-elided flat read on a butterfly that became shared/segmented
// during the stop (the always-emitted mask does NOT detect a regime
// change). I21(b) exists precisely to forbid this window.
//
// Shape: each round builds a fresh object family owned by a dedicated owner
// thread; reader threads tier up on elided property reads carrying disjoint
// sentinel sets; then a FOREIGN thread performs the first foreign
// write/transition on the hot objects (synchronous fires + jettison under
// STWR — NOT the deferred-fire path, which mc-code-deferred-fire-stale-
// window.js covers; here publication ordering is correct by construction,
// so any oracle violation implicates the RESUME side) and keeps mutating
// (adds that grow/segment the out-of-line storage) while readers run on.
//
// Oracle: o.alpha only ever yields ALPHA-set values, o.beta only BETA-set
// values (or post-takeover sentinels, also disjoint). A cross-sentinel,
// undefined, hole, or torn value = stale elided code executed past a poll
// after its watchpoint fired.
//
// The window is poll-park-to-next-invalidation-boundary — scheduler-
// dependent — so this is an AMPLIFIER-READY race test, not deterministic:
// bounded rounds here, the amplifier widens the window (arm64 weak ordering
// helps the attacker). EXECUTED POST-UNGIL ONLY: under the phase-1 GIL the
// sole mutator runs fires inline and is never parked at a poll across one,
// closing the window by construction.
load("../harness.js", "caller relative");

const READERS = 3;
const ROUNDS = 200;
const READS_PER_ROUND = 50000;

const ALPHA_BASE = 1000000; // owner-phase o.alpha values
const BETA_BASE = 2000000;  // owner-phase o.beta values
const FOREIGN_ALPHA = 3000000; // post-foreign-takeover o.alpha values
const FOREIGN_BETA = 4000000;  // post-foreign-takeover o.beta values
const SPAN = ROUNDS + 8;

function inSet(v, base) { return typeof v === "number" && v >= base && v < base + SPAN; }

const box = { o: null, round: -1 };
const gate = { ready: 0, go: 0, done: 0, stop: 0 };

function freshTarget(round) {
    // Out-of-line properties (inline capacity exhausted by filler) so reads
    // go through the tagged butterfly — the surface E1/E3 elision guards.
    const o = {};
    for (let i = 0; i < 8; ++i)
        o["filler" + i] = i;
    o.alpha = ALPHA_BASE + round;
    o.beta = BETA_BASE + round;
    return o;
}

// Hot read kernel; tier-up happens against owner-thread-local structures
// whose TTL sets are valid+watched => E1/E2/E3 elision in DFG/FTL.
function readPair(o) {
    return [o.alpha, o.beta];
}

const readers = [];
for (let r = 0; r < READERS; ++r) {
    readers.push(new Thread(() => {
        let checks = 0;
        let lastRound = -1;
        while (Atomics.load(gate, "stop") === 0) {
            const o = box.o;
            if (o === null) continue;
            for (let i = 0; i < READS_PER_ROUND; ++i) {
                const [a, b] = readPair(o);
                // a must come from an alpha set, b from a beta set —
                // and from the SAME epoch family (owner or foreign).
                const aOwner = inSet(a, ALPHA_BASE), aForeign = inSet(a, FOREIGN_ALPHA);
                const bOwner = inSet(b, BETA_BASE), bForeign = inSet(b, FOREIGN_BETA);
                if (!(aOwner || aForeign) || !(bOwner || bForeign)) {
                    print("FAILURE: cross-sentinel/torn read after poll-resume: alpha=" + a + " beta=" + b);
                    Atomics.store(gate, "stop", 1);
                    throw new Error("MC-AINT S4 / SPEC-jit I21(b) violated: alpha=" + a + " beta=" + b);
                }
                ++checks;
            }
            if (lastRound !== Atomics.load(gate, "done")) {
                lastRound = Atomics.load(gate, "done");
                Atomics.add(gate, "ready", 1); // round heartbeat
            }
        }
        return checks;
    }));
}

// Foreign mutator: performs the FIRST foreign write (synchronous
// writeThreadLocal fire => SW set => readers' E2-elided code jettisoned
// under the fire's stop) and then grows the object (foreign adds =>
// transition fires + out-of-line growth/segmentation) while readers run.
const foreign = new Thread(() => {
    let rounds = 0;
    let seen = -1;
    while (Atomics.load(gate, "stop") === 0) {
        const round = Atomics.load(gate, "go");
        if (round === seen || box.o === null) continue;
        seen = round;
        const o = box.o;
        // First foreign write: fires writeThreadLocal (Class-A, synchronous
        // STWR) while readers are mid-loop => they park at CheckTraps polls.
        o.alpha = FOREIGN_ALPHA + round;
        o.beta = FOREIGN_BETA + round;
        // Foreign growth: transition fires + butterfly reallocation /
        // segmentation right behind the resume.
        for (let i = 0; i < 24; ++i)
            o["grown" + round + "_" + i] = FOREIGN_ALPHA + round;
        // Re-assert sentinels after growth (offsets may have moved; elided
        // stale readers at old offsets now face grown storage).
        o.alpha = FOREIGN_ALPHA + round;
        o.beta = FOREIGN_BETA + round;
        Atomics.add(gate, "done", 1);
        ++rounds;
    }
    return rounds;
});

// Driver (owner of each round's structure family): build hot, signal, churn.
for (let round = 0; round < ROUNDS && Atomics.load(gate, "stop") === 0; ++round) {
    const o = freshTarget(round);
    // Warm the readers' compiled code shape on the owner thread's structure
    // family (TTL sets valid+watched at compile time).
    for (let i = 0; i < 1000; ++i)
        readPair(o);
    box.o = o;
    Atomics.store(gate, "go", round + 1);
    // Let readers + foreign mutator collide on this round.
    for (let i = 0; i < 20000; ++i) {
        // Owner-side benign rewrites within the owner sentinel set keep the
        // read loop's values moving without leaving the ALPHA/BETA sets.
        o.alpha = ALPHA_BASE + round;
        o.beta = BETA_BASE + round;
    }
}

Atomics.store(gate, "stop", 1);
const counts = joinAll(readers);
const foreignRounds = foreign.join();
for (const c of counts)
    shouldBeTrue(c > 0);
shouldBeTrue(foreignRounds > 0);
print("mc-aint-poll-resume-stale-elided: PASS (" + counts.join(",") + " checks; " + foreignRounds + " foreign rounds)");
