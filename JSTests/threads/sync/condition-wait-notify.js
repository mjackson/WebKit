//@ requireOptions("--useJSThreads=1")
// Condition.prototype.wait/notify/notifyAll: blocking handshakes where the
// MAIN thread is the waiter.
//
// Stub-scheduling notes (phase-1 GIL stub):
// - There is no preemption: a worker thread only starts running once the
//   main thread blocks. This makes "main parks in cond.wait, then a freshly
//   spawned worker wakes it" fully deterministic: when the worker's code
//   runs, main is already parked and the lock is already released.
// - The worker wakers below never block (uncontended holds, lockless
//   notify), which is the one wake-up shape the stub supports soundly.
//   Worker-side cond.wait parks are covered by the skipped
//   condition-worker-waiter.js (see its FIXME).
load("../resources/assert.js", "caller relative");

const lock = new Lock();
const cond = new Condition();

// ---- Error surface ----
shouldThrow(TypeError, () => cond.wait.call({}, lock));
shouldThrow(TypeError, () => cond.wait());            // not a Lock
shouldThrow(TypeError, () => cond.wait({}));          // not a Lock
shouldThrow(TypeError, () => cond.wait(lock));        // lock not held
lock.hold(() => {
    // Held by us, but the receiver is wrong.
    shouldThrow(TypeError, () => cond.wait.call({}, lock));
});
shouldThrow(TypeError, () => cond.notify.call({}));
shouldThrow(TypeError, () => cond.notifyAll.call({}));

// notify with no waiters wakes nobody (and notifications are not buffered
// for future waiters — phase 2 would hang if they were... they are consumed
// only by parked waiters).
shouldBe(cond.notify(), 0);
shouldBe(cond.notifyAll(), 0);

// ---- Phase 1: main waits, worker notifies; wait releases and reacquires ----
{
    const lk = new Lock();
    const cv = new Condition();
    const box = { ready: 0, observations: null };

    const notifier = new Thread(() => {
        // Running at all proves main is parked (no preemption); these
        // observations prove cond.wait released the lock while parked.
        const lockReleasedDuringWait = lk.locked === false;
        let heldInsideNotifierHold;
        lk.hold(() => {  // uncontended: main is parked
            heldInsideNotifierHold = lk.locked;
            box.ready = 1;
        });
        const woken = cv.notify(); // main is parked on cv: must wake exactly it
        box.observations = { lockReleasedDuringWait, heldInsideNotifierHold, woken };
        return "notifier-done";
    });

    let waits = 0;
    lk.hold(() => {
        shouldBeTrue(lk.locked);
        while (!box.ready) {
            waits++;
            cv.wait(lk);
        }
        shouldBeTrue(lk.locked, "wait reacquired the lock before returning");
    });
    shouldBe(notifier.join(), "notifier-done");
    shouldBe(waits, 1, "main parked exactly once");
    shouldBe(box.ready, 1);
    shouldBeTrue(box.observations.lockReleasedDuringWait, "wait released the lock while parked");
    shouldBeTrue(box.observations.heldInsideNotifierHold);
    shouldBe(box.observations.woken, 1, "notify woke exactly the parked main thread");
    shouldBeFalse(lk.locked);
}

// ---- Phase 2: notifyAll with a single parked waiter wakes exactly it ----
{
    const lk = new Lock();
    const cv = new Condition();
    const box = { ready: 0, woken: -1 };

    const notifier = new Thread(() => {
        lk.hold(() => { box.ready = 1; });
        box.woken = cv.notifyAll();
        return cv.notifyAll(); // queue is drained now
    });

    lk.hold(() => {
        while (!box.ready)
            cv.wait(lk);
    });
    shouldBe(notifier.join(), 0, "second notifyAll found an empty queue");
    shouldBe(box.woken, 1, "notifyAll reported the one parked waiter");
}

// ---- Phase 3: a notify on a different condition must not wake the waiter ----
{
    const lk = new Lock();
    const cvA = new Condition();
    const cvB = new Condition();
    const box = { stage: 0, wrongCond: -1, rightCond: -1 };

    const driver = new Thread(() => {
        // Main is parked on cvA (no preemption). cvB notifications must not
        // touch it.
        box.wrongCond = cvB.notify() + cvB.notifyAll();
        lk.hold(() => { box.stage = 1; });
        box.rightCond = cvA.notify();
        return "driver-done";
    });

    lk.hold(() => {
        while (box.stage < 1)
            cvA.wait(lk);
        box.stage = 2;
    });
    shouldBe(driver.join(), "driver-done");
    shouldBe(box.wrongCond, 0, "notifying an unrelated condition wakes nobody");
    shouldBe(box.rightCond, 1);
    shouldBe(box.stage, 2);
}

// ---- Phase 4: same condition reused across sequential waits ----
{
    const lk = new Lock();
    const cv = new Condition();
    const box = { round: 0 };

    for (let round = 1; round <= 3; ++round) {
        const w = new Thread(expected => {
            lk.hold(() => { box.round = expected; });
            return cv.notify();
        }, round);
        lk.hold(() => {
            while (box.round !== round)
                cv.wait(lk);
        });
        shouldBe(w.join(), 1, "round " + round + " woke the parked main thread");
    }
    shouldBe(box.round, 3);
    shouldBe(cv.notify(), 0);
}

// ---- Phase 5: an exception thrown after wait propagates and releases ----
{
    const lk = new Lock();
    const cv = new Condition();
    const box = { ready: 0 };
    const boom = new Error("after-wait");

    const notifier = new Thread(() => {
        lk.hold(() => { box.ready = 1; });
        return cv.notify();
    });

    shouldBe(shouldThrow(() => {
        lk.hold(() => {
            while (!box.ready)
                cv.wait(lk);
            throw boom;
        });
    }), boom);
    shouldBe(notifier.join(), 1);
    shouldBeFalse(lk.locked, "lock released despite the post-wait throw");
}

// Conditions are shareable objects like any other.
shouldBe(Object.prototype.toString.call(cond), "[object Condition]");
shouldThrow(TypeError, () => Condition());
