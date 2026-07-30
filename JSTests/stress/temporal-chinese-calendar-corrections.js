//@ requireOptions("--useTemporal=1")

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected}, got ${actual}`);
}

const leapMonth = Temporal.PlainDate.from({
    calendar: "chinese",
    year: 1987,
    month: 7,
    day: 1,
});
shouldBe(leapMonth.toString(), "1987-07-26[u-ca=chinese]", "1987 leap month ISO date");
shouldBe(leapMonth.monthCode, "M06L", "1987 leap month code");

const formatter = new Intl.DateTimeFormat("en", {
    calendar: "chinese",
    timeZone: "UTC",
    year: "numeric",
    month: "numeric",
    day: "numeric",
});
const monthPart = formatter.formatToParts(Date.UTC(1987, 6, 26))
    .find(({ type }) => type === "month");
shouldBe(monthPart.value, "6bis", "1987 leap month formatting");

for (const [year, expectedDays] of [
    [2026, 354],
    [2027, 354],
    [2029, 355],
    [2030, 354],
]) {
    const date = Temporal.PlainDate.from({
        calendar: "chinese",
        year,
        month: 1,
        day: 1,
    });
    shouldBe(date.daysInYear, expectedDays, `${year} days in year`);
}
