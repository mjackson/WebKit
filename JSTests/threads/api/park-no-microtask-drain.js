//@ requireOptions("--useJSThreads=1")
// 5.2 yield-point contract: pending microtasks must NOT run inside a blocking
// host call. Every park site releases the GIL via GILDroppedSection
// (LockObject.h), whose plain JSLock unlock currently falls into
// JSLock::willReleaseLock()'s VM::drainMicrotasks() (the drain is guarded on
// m_lockDropDepth, which GILDroppedSection does not bump) — landed deviation
// D11, docs/threads/INTEGRATE-api.md.
//
// SKIPPED until the 9.2-9 JSLock hunk (JSLock::unlockAllForThreadParking +
// the GILDroppedSection splice) is INTEGRATOR-applied; the integrator deletes
// this `//@ skip` line together with that hunk. Under the current tree the
// shouldBeFalse assertions below fail by design (the queued reactions run at
// the park site).
//
// Conventions (annex T2): self-checking, failure = throw; every spawned
// thread is joined; blocking ops bounded.
load("../harness.js", "caller relative");

asyncTestStart(1);

// ---- join: a reaction queued before the park must not run inside it ----
{
    let ran = false;
    const t = new Thread(() => {
        sleepMs(50); // ensure the joiner genuinely parks
        return 42;
    });
    Promise.resolve().then(() => { ran = true; });
    shouldBe(t.join(), 42);
    shouldBeFalse(ran, "microtask must not run inside join's GIL-dropped park");
}

// ---- cond.wait ----
{
    const lock = new Lock();
    const cond = new Condition();
    const box = { waiting: 0 };
    const t = new Thread(() => {
        waitUntil(() => box.waiting === 1);
        sleepMs(20);
        lock.hold(() => cond.notify()); // parks until the waiter releases the lock
        return "notified";
    });
    let ran = false;
    Promise.resolve().then(() => { ran = true; });
    lock.hold(() => {
        box.waiting = 1;
        cond.wait(lock);
    });
    shouldBeFalse(ran, "microtask must not run inside cond.wait's park");
    shouldBe(t.join(), "notified");
}

// ---- contended lock.hold ----
{
    const lock = new Lock();
    const box = { holderIn: 0 };
    const t = new Thread(() => lock.hold(() => {
        box.holderIn = 1;
        sleepMs(100); // hold long enough for main to park contended
        return "held";
    }));
    waitUntil(() => box.holderIn === 1);
    let ran = false;
    Promise.resolve().then(() => { ran = true; });
    lock.hold(() => {}); // contended: parks until the holder's 100ms hold ends
    shouldBeFalse(ran, "microtask must not run inside lock.hold's contended park");
    shouldBe(t.join(), "held");
}

// ---- notify()'s D2 handoff yield (jsThreadGILHandoffYield routes through
// the same GILDroppedSection) ----
{
    const cond = new Condition();
    let ran = false;
    Promise.resolve().then(() => { ran = true; });
    shouldBe(cond.notify(), 0); // no waiters; still a yield point (D2)
    shouldBeFalse(ran, "microtask must not run inside notify()'s handoff yield");
}

// ---- property Atomics.wait (the harness's own sleepMs lane uses it; assert
// directly on a private lane) ----
{
    const lane = { v: 0 };
    let ran = false;
    Promise.resolve().then(() => { ran = true; });
    shouldBe(Atomics.wait(lane, "v", 0, 30), "timed-out");
    shouldBeFalse(ran, "microtask must not run inside the property Atomics.wait park");
}

// The queued reactions all run at the natural turn boundary instead.
Promise.resolve().then(() => asyncTestPassed());
