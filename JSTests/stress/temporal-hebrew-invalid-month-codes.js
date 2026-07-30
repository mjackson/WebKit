//@ requireOptions("--useTemporal=1")

function shouldThrowRangeError(callback, message) {
    try {
        callback();
    } catch (error) {
        if (error instanceof RangeError)
            return;
        throw new Error(`${message}: expected RangeError but got ${error}`);
    }
    throw new Error(`${message}: expected RangeError`);
}

for (const overflow of ["constrain", "reject"]) {
    shouldThrowRangeError(() => {
        Temporal.PlainDate.from({ calendar: "hebrew", year: 5779, monthCode: "M13", day: 1 }, { overflow });
    }, `M13 with ${overflow}`);

    for (let month = 1; month <= 12; ++month) {
        if (month === 5)
            continue;
        const monthCode = `M${String(month).padStart(2, "0")}L`;
        shouldThrowRangeError(() => {
            Temporal.PlainDate.from({ calendar: "hebrew", year: 5779, monthCode, day: 1 }, { overflow });
        }, `${monthCode} with ${overflow}`);
    }
}

const adarI = Temporal.PlainDate.from({ calendar: "hebrew", year: 5779, monthCode: "M05L", day: 1 }, { overflow: "reject" });
if (adarI.monthCode !== "M05L")
    throw new Error(`expected M05L but got ${adarI.monthCode}`);
