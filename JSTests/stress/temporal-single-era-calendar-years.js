//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

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

const calendars = {
    buddhist: "be",
    coptic: "am",
    ethioaa: "aa",
    hebrew: "am",
    indian: "shaka",
    persian: "ap",
};

for (const [calendar, era] of Object.entries(calendars)) {
    for (const eraYear of [-1, 0, 1]) {
        const date = Temporal.PlainDate.from({ calendar, era, eraYear, monthCode: "M01", day: 1 }, { overflow: "reject" });
        shouldBe(date.year, eraYear, `${calendar} arithmetic year`);
        shouldBe(date.era, era, `${calendar} era`);
        shouldBe(date.eraYear, eraYear, `${calendar} era year`);
    }

    shouldThrowRangeError(() => {
        Temporal.PlainDate.from({ calendar, year: 2, era, eraYear: 1, monthCode: "M01", day: 1 });
    }, `${calendar} inconsistent year`);
}

for (const calendar of ["gregory", "japanese"]) {
    const ce = Temporal.PlainDate.from({ calendar, era: "ad", eraYear: 1, month: 1, day: 1 });
    shouldBe(ce.era, "ce", `${calendar} canonical AD era`);
    const bce = Temporal.PlainDate.from({ calendar, era: "bc", eraYear: 1, month: 1, day: 1 });
    shouldBe(bce.era, "bce", `${calendar} canonical BC era`);
}
