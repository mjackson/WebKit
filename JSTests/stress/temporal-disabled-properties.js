//@ requireOptions("--useDollarVM=1", "--useTemporal=0")

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(`${label}: expected ${String(expected)}, got ${String(actual)}`);
}

shouldBe("Temporal" in globalThis, false, "the namespace is absent when Temporal is disabled");
shouldBe("toTemporalInstant" in Date.prototype, false, "the Date integration is absent when Temporal is disabled");
shouldBe(Reflect.ownKeys(globalThis).includes("Temporal"), false, "the namespace is not enumerated when Temporal is disabled");
shouldBe(Reflect.ownKeys(Date.prototype).includes("toTemporalInstant"), false, "the Date integration is not enumerated when Temporal is disabled");
shouldBe($vm.temporalInitializationState(), 0, "Temporal remains uninitialized when disabled");
