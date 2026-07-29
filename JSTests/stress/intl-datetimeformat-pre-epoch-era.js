//@ requireOptions("--useIntlEraMonthcode=1")

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

function dateInYear(year) {
    const date = new Date(0);
    date.setUTCFullYear(year, 5, 15);
    date.setUTCHours(0, 0, 0, 0);
    return date;
}

for (const [locale, expectedYear] of [["en", "23"], ["bn", "২৩"]]) {
    const formatter = new Intl.DateTimeFormat(locale, {
        calendar: "islamic-civil",
        era: "long",
        year: "numeric",
        timeZone: "UTC",
    });
    const parts = formatter.formatToParts(dateInYear(600));
    const year = parts.find((part) => part.type === "year").value;
    const era = parts.find((part) => part.type === "era").value;

    shouldBe(year, expectedYear, `${locale} pre-epoch Islamic year`);
    shouldBe(era, locale === "en" ? "Before Hijrah" : "BH", `${locale} pre-epoch Islamic era`);
    shouldBe(formatter.format(dateInYear(600)).includes(year), true, `${locale} formatted year`);
    shouldBe(formatter.format(dateInYear(600)).includes(era), true, `${locale} formatted era`);
}

{
    const formatter = new Intl.DateTimeFormat("en", {
        calendar: "coptic",
        era: "long",
        year: "numeric",
        timeZone: "UTC",
    });
    const parts = formatter.formatToParts(dateInYear(250));

    shouldBe(parts.find((part) => part.type === "year").value, "35", "pre-epoch Coptic year");
    shouldBe(parts.find((part) => part.type === "era").value, "Anno Martyrum", "pre-epoch Coptic era");
}
