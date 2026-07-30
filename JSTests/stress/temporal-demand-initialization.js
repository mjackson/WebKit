//@ requireOptions("--useDollarVM=1", "--useTemporal=1")

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(`${label}: expected ${String(expected)}, got ${String(actual)}`);
}

shouldBe($vm.temporalInitializationState(), 0, "global initialization is Temporal-free");

void Date.prototype.toISOString;
void Intl.DateTimeFormat;
Object.keys(globalThis);
shouldBe($vm.temporalInitializationState(), 0, "unrelated built-ins do not initialize Temporal");

const toTemporalInstant = Date.prototype.toTemporalInstant;
shouldBe(typeof toTemporalInstant, "function", "Date.prototype.toTemporalInstant is available");
shouldBe($vm.temporalInitializationState(), 1, "the Date integration initializes structures but not the namespace");

const temporal = Temporal;
shouldBe(typeof temporal, "object", "Temporal is available");
shouldBe($vm.temporalInitializationState(), 3, "the namespace initializes on first access");
shouldBe(Temporal, temporal, "the namespace is initialized once");
shouldBe(Date.prototype.toTemporalInstant, toTemporalInstant, "the Date method is initialized once");

const otherGlobal = $vm.createGlobalObject();
shouldBe($vm.temporalInitializationState(otherGlobal), 0, "each realm starts uninitialized");
shouldBe(Reflect.ownKeys(otherGlobal).includes("Temporal"), true, "the namespace property is visible before materialization");
shouldBe(Reflect.ownKeys(otherGlobal.Date.prototype).includes("toTemporalInstant"), true, "the Date property is visible before materialization");
shouldBe($vm.temporalInitializationState(otherGlobal), 0, "property enumeration does not materialize Temporal");
shouldBe(typeof otherGlobal.Temporal, "object", "another realm exposes Temporal");
shouldBe($vm.temporalInitializationState(otherGlobal), 3, "namespace-first access initializes the namespace and structures");
shouldBe(typeof otherGlobal.Date.prototype.toTemporalInstant, "function", "another realm exposes the Date integration");
shouldBe($vm.temporalInitializationState(otherGlobal), 3, "each realm initializes both entry points at most once");

const deletedGlobal = $vm.createGlobalObject();
shouldBe(delete deletedGlobal.Temporal, true, "the namespace property is configurable");
shouldBe(deletedGlobal.Temporal, undefined, "a deleted namespace is not recreated");
shouldBe($vm.temporalInitializationState(deletedGlobal), 0, "deleting the namespace does not initialize Temporal");

const replacedGlobal = $vm.createGlobalObject();
const replacement = {};
replacedGlobal.Temporal = replacement;
shouldBe(replacedGlobal.Temporal, replacement, "the namespace property can be replaced before materialization");
shouldBe($vm.temporalInitializationState(replacedGlobal), 0, "replacing the namespace does not initialize Temporal");

const nonExtensibleGlobal = $vm.createGlobalObject();
Object.preventExtensions(nonExtensibleGlobal);
shouldBe(typeof nonExtensibleGlobal.Temporal, "object", "the namespace materializes on a non-extensible global");
shouldBe($vm.temporalInitializationState(nonExtensibleGlobal), 3, "preventExtensions preserves pending initialization");
