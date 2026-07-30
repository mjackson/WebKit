//@ requireOptions("--useTemporal=1")

function assertSame(actual, expected, message)
{
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected}, got ${actual}`);
}

for (const calendar of ["islamic", "islamic-civil", "islamic-rgsa", "islamic-tbla", "islamic-umalqura"]) {
    const bh1 = Temporal.PlainDate.from({ calendar, era: "bh", eraYear: 1, monthCode: "M06", day: 15 });
    assertSame(bh1.year, 0, `${calendar} 1 BH arithmetic year`);
    assertSame(bh1.era, "bh", `${calendar} 1 BH era`);
    assertSame(bh1.eraYear, 1, `${calendar} 1 BH era year`);

    const ah1 = bh1.add({ years: 1 }, { overflow: "reject" });
    assertSame(ah1.year, 1, `${calendar} year addition across era boundary`);
    assertSame(ah1.monthCode, "M06", `${calendar} year addition preserves month code`);
    assertSame(ah1.day, 15, `${calendar} year addition preserves day`);
    assertSame(ah1.era, "ah", `${calendar} year addition result era`);
    assertSame(ah1.eraYear, 1, `${calendar} year addition result era year`);

    const difference = bh1.until(ah1, { largestUnit: "years" });
    assertSame(difference.years, 1, `${calendar} year difference across era boundary`);
    assertSame(difference.months, 0, `${calendar} year difference remainder months`);

    const remapped = Temporal.PlainDate.from({ calendar, era: "ah", eraYear: 0, monthCode: "M01", day: 1 });
    assertSame(remapped.year, 0, `${calendar} non-positive era year arithmetic year`);
    assertSame(remapped.era, "bh", `${calendar} non-positive era year remapped era`);
    assertSame(remapped.eraYear, 1, `${calendar} non-positive era year remapped era year`);
}
