//@ requireOptions("--useTemporal=1")

function shouldThrowTypeError(callback, message) {
    try {
        callback();
    } catch (error) {
        if (error instanceof TypeError)
            return;
        throw new Error(`${message}: expected TypeError but got ${error}`);
    }
    throw new Error(`${message}: expected TypeError`);
}

for (const calendar of ["gregory", "japanese"]) {
    shouldThrowTypeError(() => {
        Temporal.PlainDate.from({ calendar, monthCode: "M05", month: 6, day: 1 });
    }, `${calendar} missing year before conflicting month`);

    shouldThrowTypeError(() => {
        Temporal.PlainDate.from({ calendar, year: 2020, monthCode: "M05", month: 6 });
    }, `${calendar} missing day before conflicting month`);

    shouldThrowTypeError(() => {
        Temporal.PlainDate.from({ calendar, year: 400000, day: 1 });
    }, `${calendar} missing month before out-of-range year`);

    shouldThrowTypeError(() => {
        Temporal.PlainYearMonth.from({ calendar, monthCode: "M05", month: 6 });
    }, `${calendar} missing year before conflicting month`);
}
