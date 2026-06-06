// Serial-perf gate: transition-heavy object construction.
//
// Builds objects whose properties spill past inline capacity, exercising
// the structure-transition chain plus butterfly (re)allocation. Under the
// threads design, owner-thread transitions with valid
// transitionThreadLocal/writeThreadLocal watchpoints must proceed with no
// locking or CAS — this is exactly the path the watchpoint elision is
// supposed to keep at today's speed. The blog post's worst case here is
// ~7x; the gate holds it to <=1%.

(function() {
    function make(seed) {
        var o = {};
        o.a = seed;
        o.b = seed + 1;
        o.c = seed + 2;
        o.d = seed + 3;
        o.e = seed + 4;
        o.f = seed + 5;
        o.g = seed + 6;
        o.h = seed + 7;
        o.i = seed + 8;
        o.j = seed + 9;
        o.k = seed + 10;
        o.l = seed + 11;
        return o;
    }
    noInline(make);

    function run() {
        var sum = 0;
        for (var i = 0; i < 100000; ++i) {
            var o = make(i & 0xff);
            sum += o.a + o.l;
        }
        return sum;
    }
    noInline(run);

    // Per iteration: seed + (seed + 11) where seed = i & 0xff.
    var expected = 0;
    for (var i = 0; i < 100000; ++i)
        expected += 2 * (i & 0xff) + 11;

    // Longer measured region: ~285ms instead of ~57ms, so a 1% gate margin
    // is resolvable and standalone `perf record` of the measured loop gets
    // enough samples for attribution (the 9-run median protocol is
    // unchanged; harness.js already supports explicit iteration counts).
    reportBench("transition-heavy-constructor", run, expected, 20, 250);
})();
