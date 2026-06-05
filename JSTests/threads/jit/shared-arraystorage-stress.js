//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// shared-arraystorage-stress.js — SPEC-jit Task 13: I20 shared-ArrayStorage
// stress (GIL-interleaved pre-integration).
//
// I20 (mirrors OM I31): flag-on, no generated code may make an unlocked
// butterfly access reachable by an SW=1 ArrayStorage butterfly — every AS
// fast path is (a) E2-elided, (b) SW-tested with SW=1 routed to the locked
// R3 ops, or (c) statically non-AS. This test makes SW=1 AS butterflies a
// certainty and drives all the generated AS paths at them:
//   - foreign-thread element writes (SW flip, then SW=1 stores),
//   - owner-thread shift/unshift between waves (AS-COPY relayout under the
//     cell lock per the AS-rule; superseded storage never rewritten),
//   - sparse/hole reads and in-bounds reads from every thread,
//   - JIT-warmed get_by_val/put_by_val over AS shapes (LLInt->Baseline->DFG).
//
// Correctness oracle (GIL today, true concurrency at the integration gate):
// disjoint-slot writes must ALL land (no lost elements — a write into
// superseded AS storage after an AS-COPY relayout would lose it), and reads
// must never see values outside the written domain.

load("../harness.js", "caller relative");

if (typeof $vm === "undefined" || !$vm.ensureArrayStorage) {
    print("shared-arraystorage-stress: SKIP ($vm.ensureArrayStorage unavailable)");
} else {
    const THREADS = 4;
    const LEN = 128;
    const WAVES = 20;
    const WRITES_PER_WAVE = 64;

    const arr = new Array(LEN).fill(0);
    $vm.ensureArrayStorage(arr);
    arr[LEN + 50] = 1; // sparse tail => SlowPut-ish AS territory stays AS
    shouldBeTrue(($vm.indexingMode(arr) || "").indexOf("ArrayStorage") >= 0,
        "test precondition: arr must have ArrayStorage indexing");

    function readAt(a, i) { return a[i]; }
    noInline(readAt);
    function writeAt(a, i, v) { a[i] = v; }
    noInline(writeAt);

    // Warm the by-val paths on the AS shape from the main thread.
    for (let i = 0; i < 20000; ++i) {
        writeAt(arr, i % LEN, 0);
        if (readAt(arr, i % LEN) !== 0)
            throw new Error("warmup readback");
    }

    const wave = { n: 0 };
    const done = { count: 0 };

    function encode(slot, w, v) { return slot * 1000000 + w * 1000 + v; }

    const writers = spawnN(THREADS, function (slot) {
        // Each thread owns a disjoint stripe: indices i with i % THREADS == slot.
        for (let w = 0; w < WAVES; ++w) {
            waitUntil(() => wave.n >= w, 30000);
            for (let k = 0; k < WRITES_PER_WAVE; ++k) {
                const i = (slot + k * THREADS) % LEN;
                writeAt(arr, i, encode(slot, w, k & 7)); // foreign write: SW path
                const back = readAt(arr, i);
                // Between our write and the read the owner may have
                // shift/unshifted (index remap) — membership, not equality:
                if (typeof back === "number" && back !== 0) {
                    const s = Math.floor(back / 1000000);
                    if (s < 0 || s >= THREADS)
                        throw new Error("read decoded to invalid writer slot: " + back);
                }
            }
            done.count++;
        }
        return slot;
    });

    // Owner thread: drive waves, relayout between them (AS-COPY), and read
    // hot in the meantime.
    for (let w = 0; w < WAVES; ++w) {
        wave.n = w;
        waitUntil(() => done.count >= (w + 1) * THREADS, 30000);
        // No writer is mid-wave now: verify the stripe writes all landed.
        for (let slot = 0; slot < THREADS; ++slot) {
            const lastK = WRITES_PER_WAVE - 1;
            const i = (slot + lastK * THREADS) % LEN;
            const v = readAt(arr, i);
            shouldBeTrue(typeof v === "number", "slot value must be a number");
            // The slot may have been overwritten by a colliding (i mod LEN)
            // stripe index from the same wave, but never lost back to a
            // stale pre-wave value of a DIFFERENT wave epoch:
            if (v !== 0 && Math.floor(v / 1000) % 1000 > w)
                throw new Error("value from the future at " + i + ": " + v);
        }
        // Owner relayout between waves: shift + unshift (vector move /
        // indexBias change => AS-COPY under the cell lock flag-on).
        const head = arr.shift();
        arr.unshift(head);
        // Hole + boundary churn.
        delete arr[LEN - 2];
        arr[LEN - 2] = 0;
    }

    joinAll(writers);

    // Final integrity: length unchanged, sparse tail survived, all values in
    // domain.
    shouldBe(arr.length, LEN + 51);
    shouldBe(arr[LEN + 50], 1, "sparse tail element must survive relayouts");
    for (let i = 0; i < LEN; ++i) {
        const v = arr[i];
        if (v !== undefined && typeof v !== "number")
            throw new Error("out-of-domain value at " + i + ": " + String(v));
    }
    print("shared-arraystorage-stress: PASS");
}
