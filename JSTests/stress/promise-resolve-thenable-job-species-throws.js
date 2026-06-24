// PromiseResolveThenableJob: an abrupt completion of the inlined `then` call
// (here: SpeciesConstructor throwing on the resolution promise) must reject
// the outer promise per ECMA-262 27.2.2.2 step c — not leave it pending with
// the exception leaked to the host. The fast path's slow fallback
// (promiseResolveThenableJobFastSlow) bare-returned on this throw.
//
// Instance-level constructor poison so the global Promise[@@species] and
// promiseSpeciesWatchpoint are untouched: routes to the FastSlow path, not
// the generic thenable job.

function shouldBe(a, b) { if (a !== b) throw new Error(`FAIL: ${a} !== ${b}`); }

const speciesError = new Error("species-boom");
const inner = Promise.resolve();
inner.constructor = {
    get [Symbol.species]() { throw speciesError; }
};

let result = "pending";
new Promise(resolve => resolve(inner)).then(
    () => { result = "fulfilled"; },
    e => { result = e; }
);

drainMicrotasks();
shouldBe(result, speciesError);

// Twin: the await/async-generator path (PromiseResolveThenableJobWithInternalMicrotaskFastSlow).
let awaitResult = "pending";
(async () => {
    try {
        const inner2 = Promise.resolve();
        inner2.constructor = {
            get [Symbol.species]() { throw speciesError; }
        };
        await inner2;
        awaitResult = "fulfilled";
    } catch (e) {
        awaitResult = e;
    }
})();

drainMicrotasks();
shouldBe(awaitResult, speciesError);
