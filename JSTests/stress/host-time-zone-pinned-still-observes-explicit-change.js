//@ runDefault("--useDollarVM=1", "--hostTimeZoneCanChange=0")
//@ skip if $hostOS == "playstation"

// --hostTimeZoneCanChange=0 tells JavaScriptCore that the host time zone is fixed
// for the lifetime of the process, so ports without a change notifier stop
// re-checking it on every VM entry. An explicit change still goes through
// WTF::timeZoneDidChange(), so it must still be observed with the option off.

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected "${want}", got "${got}"`);
}

function cacheableTZ()
{
    return new Intl.DateTimeFormat().resolvedOptions().timeZone;
}

function slowTZ()
{
    // Passing an options object bypasses the fast cache path.
    return new Intl.DateTimeFormat(undefined, {}).resolvedOptions().timeZone;
}

if (!$vm.setHostTimeZone("America/Los_Angeles"))
    throw new Error("Failed to set host time zone to America/Los_Angeles");

// Reach a new VMEntryScope so the time zone change takes effect.
setTimeout(() => {
    expect("first change: cacheable", cacheableTZ(), "America/Los_Angeles");
    expect("first change: slow", slowTZ(), "America/Los_Angeles");
    expect("first change: Date", new Date(Date.UTC(2024, 0, 15, 20)).getTimezoneOffset(), 480);

    // A second change must be observed too: the cached epoch has to track the
    // last observed change rather than latch after the first one.
    if (!$vm.setHostTimeZone("Pacific/Kiritimati"))
        throw new Error("Failed to set host time zone to Pacific/Kiritimati");

    setTimeout(() => {
        expect("second change: cacheable", cacheableTZ(), "Pacific/Kiritimati");
        expect("second change: slow", slowTZ(), "Pacific/Kiritimati");
        expect("second change: Date", new Date(Date.UTC(2024, 0, 15, 20)).getTimezoneOffset(), -840);
    }, 0);
}, 0);
