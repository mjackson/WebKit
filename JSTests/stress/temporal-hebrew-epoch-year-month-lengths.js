//@ requireOptions("--useTemporal=1")

const expectedCheshvan = [29, 30, 30, 29, 29, 30, 30, 29, 29, 30, 29];
const expectedKislev = [30, 30, 30, 29, 30, 30, 30, 30, 29, 30, 30];

for (let year = 0; year < expectedCheshvan.length; ++year) {
    const cheshvan = Temporal.PlainDate.from({ calendar: "hebrew", year, monthCode: "M02", day: 30 });
    if (cheshvan.day !== expectedCheshvan[year])
        throw new Error(`Hebrew ${year} Cheshvan: expected ${expectedCheshvan[year]} but got ${cheshvan.day}`);

    const kislev = Temporal.PlainDate.from({ calendar: "hebrew", year, monthCode: "M03", day: 30 });
    if (kislev.day !== expectedKislev[year])
        throw new Error(`Hebrew ${year} Kislev: expected ${expectedKislev[year]} but got ${kislev.day}`);
}
