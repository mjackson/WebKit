//@ requireOptions("--useDollarVM=1")
// SPEC-heap.md T10: I10 — with --useSharedGCHeap left at its default (off),
// fast/slow allocation paths execute today's code: TLC bypassed, server
// allocators populated, legacy collection protocol (incl. concurrent
// marking), MutatorSlowPathLocker a no-op. The sole option-off behavior
// delta is the legacy runEndPhase hook + epoch-reclaim call (§9 note),
// which heap-epoch-reclaim.js covers positively; here we check:
//
//   1. The shared-mode harness scenarios REFUSE to run (manifest-8 guard /
//      requireSharedHeapOption), so nothing shared-mode can leak into the
//      default configuration.
//   2. epochReclaim still passes (the I10-exempt legacy reclamation works
//      with the option off).
//   3. Plain allocation + GC churn is deterministic.
//
// The QUANTITATIVE half of the I10 gate is Tools/threads/bench-gate.sh over
// JSTests/threads/bench/ (option off by default) plus
// JSTests/threads/heap-bench-allocation.js — see INTEGRATE-heap.md (T10).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // 1. Shared-mode scenarios refuse when the option is off (either the
    //    $vm guard throws or the harness returns false; both count).
    for (const scenario of ["allocationStorm", "clientChurnVsGC", "issRevertChurn"]) {
        let refused = false;
        try {
            refused = !$vm.sharedHeapTest(scenario, 2, 4);
        } catch {
            refused = true;
        }
        shouldBeTrue(refused, scenario + " must refuse with --useSharedGCHeap off");
    }

    // 2. The legacy reclamation site works option-off (I10 exemption).
    shouldBeTrue($vm.sharedHeapTest("epochReclaim", 1, 16), "epochReclaim option-off");
}

// 3. Deterministic allocation/GC churn on the legacy path.
let sum = 0;
for (let i = 0; i < 20000; ++i) {
    const o = { a: i, b: [i, i + 1] };
    sum += o.a + o.b[1];
}
if (typeof gc === "function")
    gc();
shouldBe(sum, 400000000); // sum of (2i + 1) for i in [0, 20000)
print("PASS");
