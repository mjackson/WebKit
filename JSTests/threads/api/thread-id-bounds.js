//@ requireOptions("--useJSThreads=1", "--maxJSThreads=4")
// API-I17: spawned thread ids are in [1, 0x7ffe] and unique; exceeding
// maxJSThreads live Threads => RangeError at spawn; ids are reissued only by
// the Dev-10 rebias (not yet landed), so fresh spawns get fresh ids.
//
// --maxJSThreads=4 makes the live-cap half testable cheaply. Under the
// phase-1 cooperative GIL the spawned fns CANNOT have run yet (main holds
// the GIL continuously between spawn and the first join), so all 4 threads
// are still live (Running) when the 5th spawn is attempted — no scheduling
// assumption beyond the GIL itself, and the join afterwards bounds the test.
load("../harness.js", "caller relative");

shouldBe(Thread.current.id, 0, "main thread id is 0 (5.1)");

const ids = new Set();
const threads = spawnN(4, i => i);
for (const t of threads) {
    shouldBeTrue(Number.isInteger(t.id), "id must be an integer");
    shouldBeTrue(t.id >= 1 && t.id <= 0x7ffe, "spawned id in [1, 0x7ffe], got " + t.id);
    shouldBeFalse(ids.has(t.id), "ids must be unique");
    ids.add(t.id);
    shouldBe(t.id, t.id, "id is stable");
}

// 5th live thread: RangeError, exact message (5.1 / §3 maxJSThreads).
shouldThrow(RangeError, () => new Thread(() => 0),
    "too many live Threads (or thread-ID space exhausted)");

// The failed spawn must not have consumed a TID or leaked a live entry:
// after joining (threads finish and unregister), spawning works again...
shouldBe(joinAll(threads).join(","), "0,1,2,3");
const t2 = new Thread(() => "again");

// ...and pre-rebias (Dev 10) the new id is FRESH — never one of the retired
// ids, and still in range.
shouldBeFalse(ids.has(t2.id), "TIDs must not be reused before the Dev-10 rebias");
shouldBeTrue(t2.id >= 1 && t2.id <= 0x7ffe);
shouldBe(t2.join(), "again");

// Repeated spawn/join cycles keep allocating monotonically fresh unique ids.
let prev = t2.id;
for (let i = 0; i < 8; ++i) {
    const t = new Thread(() => 0);
    shouldBeTrue(t.id > prev, "ids grow monotonically pre-rebias (got " + t.id + " after " + prev + ")");
    prev = t.id;
    shouldBe(t.join(), 0);
}
