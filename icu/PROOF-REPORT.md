# ICU data reachability — layered proof

Three filter configs at increasing aggressiveness, each tagged with the
exact set of human claims (lemmas) its soundness depends on.

## Layer 1 — read-reachability closure

- Entry symbols (JSC+WTF → ICU): **168**
- ICU source files with linked code: **277**

| | annotations OFF (A) | annotations ON (B) |
|---|---|---|
| unresolved sites | 101 | 0 |
| KEEP-ALL trees | brkitr_tree, coll_tree, curr_tree, lang_tree, locales_tree, misc, region_tree, unit_tree, zone_tree | — |
| trees with drops | 0 | 2 |

## Layer 2 — output-unreachability proofs (11)

- Accepted: **11** / 11
- Rejected: —

## Soundness budget

| Config | Human claims | Rules | est. KB (L2 only) |
|---|---|---|---|
| A | **0** | 0 | — |
| B | **3** annotations | 3 | — |
| C | **3** annotations + **11** proofs = **14** | 378 | ~3564 |

## Config A — Layer 1, annotations OFF — zero human claims

**Lemmas this config depends on:** none

## Config B — Layer 1, annotations ON

**Lemmas this config depends on:** A26, A29, A31

### `coll_tree` (1 rules)

- `-/%%DEPENDENCY` ← A26

### `brkitr_tree` (2 rules)

- `-/%%DEPENDENCY` ← A29, A31
- `-/extensions` ← A29, A31

## Config C — Layer 1 (annotations ON) ∪ Layer 2 proofs

**Lemmas this config depends on:** A26, A29, A31, P01, P02, P03, P04, P05, P06, P07, P08, P09, P10, P11

### feature `brkitr_rules`

- `line` ← P01
- `line_cj` ← P01
- `line_loose` ← P01
- `line_loose_cj` ← P01
- `line_normal` ← P01
- `line_normal_cj` ← P01
- `line_normal_phrase_cj` ← P01
- `line_loose_phrase_cj` ← P01
- `line_phrase_cj` ← P01
- `line_strict` ← P01
- `line_strict_cj` ← P01
- `line_strict_phrase_cj` ← P01
- `title` ← P01

### feature `normalization`

- `nfkc_cf` ← P02
- `nfkc_scf` ← P02

### feature `misc`

- `genderList` ← P08
- `currencyNumericCodes` ← P08

### `coll_tree` (1 rules)

- `-/%%DEPENDENCY` ← A26

### `brkitr_tree` (3 rules)

- `-/%%DEPENDENCY` ← A29, A31
- `-/exceptions` ← P09
- `-/extensions` ← A29, A31

### `lang_tree` (39 rules)

- `-/characterLabelPattern` ← P03
- `-/codePatterns` ← P03
- `-/Keys` ← P03
- `-/Languages%long` ← P03
- `-/Languages%menu` ← P03
- `-/Languages%variant` ← P03
- `-/Scripts%stand-alone` ← P03
- `-/Scripts%variant` ← P03
- `-/Types/cf` ← P04
- `-/Types/colAlternate` ← P04
- `-/Types/colBackwards` ← P04
- `-/Types/colCaseFirst` ← P04
- `-/Types/colCaseLevel` ← P04
- `-/Types/collation` ← P04
- `-/Types/colNormalization` ← P04
- `-/Types/colNumeric` ← P04
- `-/Types/colReorder` ← P04
- `-/Types/colStrength` ← P04
- `-/Types/d0` ← P04
- `-/Types/dx` ← P04
- `-/Types/em` ← P04
- `-/Types/fw` ← P04
- `-/Types/h0` ← P04
- `-/Types/hc` ← P04
- `-/Types/i0` ← P04
- `-/Types/k0` ← P04
- `-/Types/kr` ← P04
- `-/Types/lb` ← P04
- `-/Types/m0` ← P04
- `-/Types/ms` ← P04
- `-/Types/mu` ← P04
- `-/Types/numbers` ← P04
- `-/Types/s0` ← P04
- `-/Types/ss` ← P04
- `-/Types/t0` ← P04
- `-/Types/va` ← P04
- `-/Types/x0` ← P04
- `-/Types%short` ← P04
- `-/Variants` ← P03

### `locales_tree` (33 rules)

- `-/AuxExemplarCharacters` ← P06
- `-/calendar/*/DateTimeSkeletons` ← P10
- `-/characterLabel` ← P06
- `-/delimiters` ← P06
- `-/Ellipsis` ← P06
- `-/ExemplarCharacters` ← P06
- `-/ExemplarCharactersIndex` ← P06
- `-/ExemplarCharactersNumbers` ← P06
- `-/ExemplarCharactersPunctuation` ← P06
- `-/fields/fri` ← P05
- `-/fields/fri-narrow` ← P05
- `-/fields/fri-short` ← P05
- `-/fields/mon` ← P05
- `-/fields/mon-narrow` ← P05
- `-/fields/mon-short` ← P05
- `-/fields/sat` ← P05
- `-/fields/sat-narrow` ← P05
- `-/fields/sat-short` ← P05
- `-/fields/sun` ← P05
- `-/fields/sun-narrow` ← P05
- `-/fields/sun-short` ← P05
- `-/fields/thu` ← P05
- `-/fields/thu-narrow` ← P05
- `-/fields/thu-short` ← P05
- `-/fields/tue` ← P05
- `-/fields/tue-narrow` ← P05
- `-/fields/tue-short` ← P05
- `-/fields/wed` ← P05
- `-/fields/wed-narrow` ← P05
- `-/fields/wed-short` ← P05
- `-/measurementSystemNames` ← P06
- `-/MoreInformation` ← P06
- `-/personNames` ← P06

### `misc` (8 rules)

- `-/codeMappingsCurrency` ← P07
- `-/idValidity/subdivision` ← P07
- `-/idValidity/unit` ← P07
- `-/languageData` ← P07
- `-/personNamesDefaults` ← P07
- `-/subdivisionContainment` ← P07
- `-/territoryInfo` ← P07
- `-/weekOfPreference` ← P07

### `unit_tree` (277 rules)

- `-/durationUnits` ← P11
- `-/units/acceleration` ← P11
- `-/units/angle/arc-minute` ← P11
- `-/units/angle/arc-second` ← P11
- `-/units/angle/radian` ← P11
- `-/units/angle/revolution` ← P11
- `-/units/area/dunam` ← P11
- `-/units/area/square-centimeter` ← P11
- `-/units/area/square-foot` ← P11
- `-/units/area/square-inch` ← P11
- `-/units/area/square-kilometer` ← P11
- `-/units/area/square-meter` ← P11
- `-/units/area/square-mile` ← P11
- `-/units/area/square-yard` ← P11
- `-/units/concentr/item` ← P11
- `-/units/concentr/karat` ← P11
- `-/units/concentr/milligram-ofglucose-per-deciliter` ← P11
- `-/units/concentr/millimole-per-liter` ← P11
- `-/units/concentr/mole` ← P11
- `-/units/concentr/permille` ← P11
- `-/units/concentr/permillion` ← P11
- `-/units/concentr/permyriad` ← P11
- `-/units/consumption/liter-per-100-kilometer` ← P11
- `-/units/consumption/mile-per-gallon-imperial` ← P11
- `-/units/duration/century` ← P11
- `-/units/duration/decade` ← P11
- `-/units/duration/quarter` ← P11
- `-/units/electric` ← P11
- `-/units/energy` ← P11
- `-/units/force` ← P11
- `-/units/frequency` ← P11
- `-/units/graphics` ← P11
- `-/units/length/astronomical-unit` ← P11
- `-/units/length/decimeter` ← P11
- `-/units/length/earth-radius` ← P11
- `-/units/length/fathom` ← P11
- `-/units/length/furlong` ← P11
- `-/units/length/light-year` ← P11
- `-/units/length/micrometer` ← P11
- `-/units/length/nanometer` ← P11
- `-/units/length/nautical-mile` ← P11
- `-/units/length/parsec` ← P11
- `-/units/length/picometer` ← P11
- `-/units/length/point` ← P11
- `-/units/length/solar-radius` ← P11
- `-/units/light` ← P11
- `-/units/mass/carat` ← P11
- `-/units/mass/dalton` ← P11
- `-/units/mass/earth-mass` ← P11
- `-/units/mass/grain` ← P11
- `-/units/mass/microgram` ← P11
- `-/units/mass/milligram` ← P11
- `-/units/mass/ounce-troy` ← P11
- `-/units/mass/solar-mass` ← P11
- `-/units/mass/ton` ← P11
- `-/units/mass/tonne` ← P11
- `-/units/power` ← P11
- `-/units/pressure` ← P11
- `-/units/speed/beaufort` ← P11
- `-/units/speed/knot` ← P11
- `-/units/temperature/generic` ← P11
- `-/units/temperature/kelvin` ← P11
- `-/units/torque` ← P11
- `-/units/volume/acre-foot` ← P11
- `-/units/volume/barrel` ← P11
- `-/units/volume/bushel` ← P11
- `-/units/volume/centiliter` ← P11
- `-/units/volume/cubic-centimeter` ← P11
- `-/units/volume/cubic-foot` ← P11
- `-/units/volume/cubic-inch` ← P11
- `-/units/volume/cubic-kilometer` ← P11
- `-/units/volume/cubic-meter` ← P11
- `-/units/volume/cubic-mile` ← P11
- `-/units/volume/cubic-yard` ← P11
- `-/units/volume/cup` ← P11
- `-/units/volume/cup-metric` ← P11
- `-/units/volume/deciliter` ← P11
- `-/units/volume/dessert-spoon` ← P11
- `-/units/volume/dessert-spoon-imperial` ← P11
- `-/units/volume/dram` ← P11
- `-/units/volume/drop` ← P11
- `-/units/volume/fluid-ounce-imperial` ← P11
- `-/units/volume/gallon-imperial` ← P11
- `-/units/volume/hectoliter` ← P11
- `-/units/volume/jigger` ← P11
- `-/units/volume/megaliter` ← P11
- `-/units/volume/pinch` ← P11
- `-/units/volume/pint` ← P11
- `-/units/volume/pint-metric` ← P11
- `-/units/volume/quart` ← P11
- `-/units/volume/quart-imperial` ← P11
- `-/units/volume/tablespoon` ← P11
- `-/units/volume/teaspoon` ← P11
- `-/unitsNarrow/acceleration` ← P11
- `-/unitsNarrow/angle/arc-minute` ← P11
- `-/unitsNarrow/angle/arc-second` ← P11
- `-/unitsNarrow/angle/radian` ← P11
- `-/unitsNarrow/angle/revolution` ← P11
- `-/unitsNarrow/area/dunam` ← P11
- `-/unitsNarrow/area/square-centimeter` ← P11
- `-/unitsNarrow/area/square-foot` ← P11
- `-/unitsNarrow/area/square-inch` ← P11
- `-/unitsNarrow/area/square-kilometer` ← P11
- `-/unitsNarrow/area/square-meter` ← P11
- `-/unitsNarrow/area/square-mile` ← P11
- `-/unitsNarrow/area/square-yard` ← P11
- `-/unitsNarrow/concentr/item` ← P11
- `-/unitsNarrow/concentr/karat` ← P11
- `-/unitsNarrow/concentr/milligram-ofglucose-per-deciliter` ← P11
- `-/unitsNarrow/concentr/millimole-per-liter` ← P11
- `-/unitsNarrow/concentr/mole` ← P11
- `-/unitsNarrow/concentr/permille` ← P11
- `-/unitsNarrow/concentr/permillion` ← P11
- `-/unitsNarrow/concentr/permyriad` ← P11
- `-/unitsNarrow/consumption/liter-per-100-kilometer` ← P11
- `-/unitsNarrow/consumption/mile-per-gallon-imperial` ← P11
- `-/unitsNarrow/duration/century` ← P11
- `-/unitsNarrow/duration/decade` ← P11
- `-/unitsNarrow/duration/quarter` ← P11
- `-/unitsNarrow/electric` ← P11
- `-/unitsNarrow/energy` ← P11
- `-/unitsNarrow/force` ← P11
- `-/unitsNarrow/frequency` ← P11
- `-/unitsNarrow/graphics` ← P11
- `-/unitsNarrow/length/astronomical-unit` ← P11
- `-/unitsNarrow/length/decimeter` ← P11
- `-/unitsNarrow/length/earth-radius` ← P11
- `-/unitsNarrow/length/fathom` ← P11
- `-/unitsNarrow/length/furlong` ← P11
- `-/unitsNarrow/length/light-year` ← P11
- `-/unitsNarrow/length/micrometer` ← P11
- `-/unitsNarrow/length/nanometer` ← P11
- `-/unitsNarrow/length/nautical-mile` ← P11
- `-/unitsNarrow/length/parsec` ← P11
- `-/unitsNarrow/length/picometer` ← P11
- `-/unitsNarrow/length/point` ← P11
- `-/unitsNarrow/length/solar-radius` ← P11
- `-/unitsNarrow/light` ← P11
- `-/unitsNarrow/mass/carat` ← P11
- `-/unitsNarrow/mass/dalton` ← P11
- `-/unitsNarrow/mass/earth-mass` ← P11
- `-/unitsNarrow/mass/grain` ← P11
- `-/unitsNarrow/mass/microgram` ← P11
- `-/unitsNarrow/mass/milligram` ← P11
- `-/unitsNarrow/mass/ounce-troy` ← P11
- `-/unitsNarrow/mass/solar-mass` ← P11
- `-/unitsNarrow/mass/ton` ← P11
- `-/unitsNarrow/mass/tonne` ← P11
- `-/unitsNarrow/power` ← P11
- `-/unitsNarrow/pressure` ← P11
- `-/unitsNarrow/speed/beaufort` ← P11
- `-/unitsNarrow/speed/knot` ← P11
- `-/unitsNarrow/temperature/generic` ← P11
- `-/unitsNarrow/temperature/kelvin` ← P11
- `-/unitsNarrow/torque` ← P11
- `-/unitsNarrow/volume/acre-foot` ← P11
- `-/unitsNarrow/volume/barrel` ← P11
- `-/unitsNarrow/volume/bushel` ← P11
- `-/unitsNarrow/volume/centiliter` ← P11
- `-/unitsNarrow/volume/cubic-centimeter` ← P11
- `-/unitsNarrow/volume/cubic-foot` ← P11
- `-/unitsNarrow/volume/cubic-inch` ← P11
- `-/unitsNarrow/volume/cubic-kilometer` ← P11
- `-/unitsNarrow/volume/cubic-meter` ← P11
- `-/unitsNarrow/volume/cubic-mile` ← P11
- `-/unitsNarrow/volume/cubic-yard` ← P11
- `-/unitsNarrow/volume/cup` ← P11
- `-/unitsNarrow/volume/cup-metric` ← P11
- `-/unitsNarrow/volume/deciliter` ← P11
- `-/unitsNarrow/volume/dessert-spoon` ← P11
- `-/unitsNarrow/volume/dessert-spoon-imperial` ← P11
- `-/unitsNarrow/volume/dram` ← P11
- `-/unitsNarrow/volume/drop` ← P11
- `-/unitsNarrow/volume/fluid-ounce-imperial` ← P11
- `-/unitsNarrow/volume/gallon-imperial` ← P11
- `-/unitsNarrow/volume/hectoliter` ← P11
- `-/unitsNarrow/volume/jigger` ← P11
- `-/unitsNarrow/volume/megaliter` ← P11
- `-/unitsNarrow/volume/pinch` ← P11
- `-/unitsNarrow/volume/pint` ← P11
- `-/unitsNarrow/volume/pint-metric` ← P11
- `-/unitsNarrow/volume/quart` ← P11
- `-/unitsNarrow/volume/quart-imperial` ← P11
- `-/unitsNarrow/volume/tablespoon` ← P11
- `-/unitsNarrow/volume/teaspoon` ← P11
- `-/unitsShort/acceleration` ← P11
- `-/unitsShort/angle/arc-minute` ← P11
- `-/unitsShort/angle/arc-second` ← P11
- `-/unitsShort/angle/radian` ← P11
- `-/unitsShort/angle/revolution` ← P11
- `-/unitsShort/area/dunam` ← P11
- `-/unitsShort/area/square-centimeter` ← P11
- `-/unitsShort/area/square-foot` ← P11
- `-/unitsShort/area/square-inch` ← P11
- `-/unitsShort/area/square-kilometer` ← P11
- `-/unitsShort/area/square-meter` ← P11
- `-/unitsShort/area/square-mile` ← P11
- `-/unitsShort/area/square-yard` ← P11
- `-/unitsShort/concentr/item` ← P11
- `-/unitsShort/concentr/karat` ← P11
- `-/unitsShort/concentr/milligram-ofglucose-per-deciliter` ← P11
- `-/unitsShort/concentr/millimole-per-liter` ← P11
- `-/unitsShort/concentr/mole` ← P11
- `-/unitsShort/concentr/permille` ← P11
- `-/unitsShort/concentr/permillion` ← P11
- `-/unitsShort/concentr/permyriad` ← P11
- `-/unitsShort/consumption/liter-per-100-kilometer` ← P11
- `-/unitsShort/consumption/mile-per-gallon-imperial` ← P11
- `-/unitsShort/duration/century` ← P11
- `-/unitsShort/duration/decade` ← P11
- `-/unitsShort/duration/quarter` ← P11
- `-/unitsShort/electric` ← P11
- `-/unitsShort/energy` ← P11
- `-/unitsShort/force` ← P11
- `-/unitsShort/frequency` ← P11
- `-/unitsShort/graphics` ← P11
- `-/unitsShort/length/astronomical-unit` ← P11
- `-/unitsShort/length/decimeter` ← P11
- `-/unitsShort/length/earth-radius` ← P11
- `-/unitsShort/length/fathom` ← P11
- `-/unitsShort/length/furlong` ← P11
- `-/unitsShort/length/light-year` ← P11
- `-/unitsShort/length/micrometer` ← P11
- `-/unitsShort/length/nanometer` ← P11
- `-/unitsShort/length/nautical-mile` ← P11
- `-/unitsShort/length/parsec` ← P11
- `-/unitsShort/length/picometer` ← P11
- `-/unitsShort/length/point` ← P11
- `-/unitsShort/length/solar-radius` ← P11
- `-/unitsShort/light` ← P11
- `-/unitsShort/mass/carat` ← P11
- `-/unitsShort/mass/dalton` ← P11
- `-/unitsShort/mass/earth-mass` ← P11
- `-/unitsShort/mass/grain` ← P11
- `-/unitsShort/mass/microgram` ← P11
- `-/unitsShort/mass/milligram` ← P11
- `-/unitsShort/mass/ounce-troy` ← P11
- `-/unitsShort/mass/solar-mass` ← P11
- `-/unitsShort/mass/ton` ← P11
- `-/unitsShort/mass/tonne` ← P11
- `-/unitsShort/power` ← P11
- `-/unitsShort/pressure` ← P11
- `-/unitsShort/speed/beaufort` ← P11
- `-/unitsShort/speed/knot` ← P11
- `-/unitsShort/temperature/generic` ← P11
- `-/unitsShort/temperature/kelvin` ← P11
- `-/unitsShort/torque` ← P11
- `-/unitsShort/volume/acre-foot` ← P11
- `-/unitsShort/volume/barrel` ← P11
- `-/unitsShort/volume/bushel` ← P11
- `-/unitsShort/volume/centiliter` ← P11
- `-/unitsShort/volume/cubic-centimeter` ← P11
- `-/unitsShort/volume/cubic-foot` ← P11
- `-/unitsShort/volume/cubic-inch` ← P11
- `-/unitsShort/volume/cubic-kilometer` ← P11
- `-/unitsShort/volume/cubic-meter` ← P11
- `-/unitsShort/volume/cubic-mile` ← P11
- `-/unitsShort/volume/cubic-yard` ← P11
- `-/unitsShort/volume/cup` ← P11
- `-/unitsShort/volume/cup-metric` ← P11
- `-/unitsShort/volume/deciliter` ← P11
- `-/unitsShort/volume/dessert-spoon` ← P11
- `-/unitsShort/volume/dessert-spoon-imperial` ← P11
- `-/unitsShort/volume/dram` ← P11
- `-/unitsShort/volume/drop` ← P11
- `-/unitsShort/volume/fluid-ounce-imperial` ← P11
- `-/unitsShort/volume/gallon-imperial` ← P11
- `-/unitsShort/volume/hectoliter` ← P11
- `-/unitsShort/volume/jigger` ← P11
- `-/unitsShort/volume/megaliter` ← P11
- `-/unitsShort/volume/pinch` ← P11
- `-/unitsShort/volume/pint` ← P11
- `-/unitsShort/volume/pint-metric` ← P11
- `-/unitsShort/volume/quart` ← P11
- `-/unitsShort/volume/quart-imperial` ← P11
- `-/unitsShort/volume/tablespoon` ← P11
- `-/unitsShort/volume/teaspoon` ← P11

## Lemma catalog

### Layer-1 annotations

- **A01** `i18n/number_longnames.cpp` /^(key|genderKey|caseKey|aliasKey)\.data\(\)$/ → `*` — key built from unit category/subtype; bounded by sanctioned units _(unused)_
- **A02** `i18n/number_longnames.cpp` /^(feature|structure)$/ → `grammaticalData/*` — grammar keys under /grammaticalData _(applied in: unit_tree, locales_tree)_
- **A03** `common/uloc_keytype.cpp` /^(legacy|bcp)KeyId$/ → `*` — iterates all keys in keyTypeData.res keyMap/typeMap _(applied in: misc)_
- **A04** `i18n/measunit_extra.cpp` CATEGORY_TABLE_NAME → `unitQuantities` — constexpr at top of file _(unused)_
- **A05** `i18n/dtptngen.cpp` /^path\.data\(\)$/ → `calendar/*/*` — path = calendar/<type>/{availableFormats,appendItems,DateTimePatterns,intervalFormats} _(applied in: locales_tree)_
- **A06** `i18n/smpdtfmt.cpp` /^resourcePath\.data\(\)$/ → `calendar/*/DateTimePatterns*` — resourcePath = calendar/<type>/DateTimePatterns[%atTime] _(applied in: locales_tree)_
- **A07** `i18n/plurrule.cpp` typeKey → `{locales,locales_ordinals}` — typeKey = type==ORDINAL ? 'locales_ordinals' : 'locales' _(applied in: misc)_
- **A08** `i18n/plurrule.cpp` setKey → `rules/*` — setKey = rule-set name from locales table _(applied in: misc)_
- **A09** `i18n/tznames_impl.cpp` key → `*` — key is mzID or tzID — bounded by zone list _(applied in: misc, zone_tree)_
- **A10** `i18n/calendar.cpp` /^region\.data\(\)$/ → `*` — region code under calendarPreferenceData _(applied in: locales_tree)_
- **A11** `i18n/dcfmtsym.cpp` /nsName|ns\.data/ → `*` — numbering system name under NumberElements _(unused)_
- **A12** `common/ucurr.cpp` /.*/ → `*` — currency code lookups — KEEP all curr_tree _(applied in: misc, curr_tree)_
- **A13** `common/locdspnm.cpp` /.*/ → `*` — display-name lookups by language/region/script code _(unused)_
- **A14** `common/locresdata.cpp` /.*/ → `*` — generic locale resource lookup _(applied in: locales_tree)_
- **A15** `common/locdispnames.cpp` /.*/ → `*` — display-name lookups — KEEP all lang/region trees _(applied in: curr_tree, region_tree, lang_tree)_
- **A16** `common/resbund.cpp` /.*/ → `*` — ResourceBundle wrapper — keys come from caller _(applied in: locales_tree)_
- **A17** `i18n/zonemeta.cpp` /.*/ → `*` — tz/mz ID lookups in metaZones/timezoneTypes _(applied in: misc)_
- **A18** `i18n/rbnf.cpp` /.*/ → `*` — RBNF — removed via icupkg filter; reachability moot _(unused)_
- **A19** `i18n/tmutfmt.cpp` /.*/ → `units/duration/*` — TimeUnitFormat reads units/duration only _(applied in: unit_tree)_
- **A20** `i18n/currpinf.cpp` /.*/ → `*` — currency plural patterns _(applied in: locales_tree, curr_tree)_
- **A21** `i18n/number_compact.cpp` /.*/ → `NumberElements/*/patterns*/*` — compact patterns under NumberElements/<ns>/patternsLong|Short _(applied in: locales_tree)_
- **A22** `i18n/reldatefmt.cpp` /.*/ → `fields/*` — reads /fields/<unit>[-style] _(applied in: locales_tree)_
- **A23** `i18n/timezone.cpp` /.*/ → `*` — zoneinfo64.res lookups by tz ID _(applied in: misc)_
- **A24** `jsc/runtime/IntlDurationFormat.cpp` /numberingSystem/ → `NumberElements/*` — reads NumberElements/<ns>/patterns _(unused)_
- **A25** `i18n/calendar.cpp` type → `calendarData/*` — type = calendar type ∈ supportedValuesOf('calendar') _(applied in: locales_tree)_
- **A26** `i18n/ucol_res.cpp` type → `collations/*` — type = collation type from -u-co- _(applied in: coll_tree)_
- **A27** `i18n/listformatter.cpp` currentStyle → `listPattern/*` — style ∈ {standard, or, unit}[-short|-narrow] _(applied in: locales_tree)_
- **A28** `i18n/dcfmtsym.cpp` cc → `Currencies/*` — cc = ISO 4217 currency code _(applied in: locales_tree, curr_tree)_
- **A29** `common/brkiter.cpp` type → `boundaries/*` — type ∈ {grapheme,word,sentence,line,title} _(applied in: brkitr_tree)_
- **A30** `i18n/numsys.cpp` /^(buffer|name)$/ → `*` — numbering system name _(applied in: misc)_
- **A31** `common/brkeng.cpp` /uscript_getShortName/ → `dictionaries/*` — script short name picks dictionary _(applied in: brkitr_tree)_
- **A32** `i18n/ucal.cpp` /prefRegion/ → `calendarPreferenceData/*` — region code under calendarPreferenceData _(applied in: locales_tree)_

### Layer-2 proofs

#### P01 (Tier 1) — feature brkitr_rules ✓

**Claim:** Intl.Segmenter granularity ∈ {grapheme, word, sentence}; JSC validates and throws on others. WTF only opens char/word/sentence iterators. No JS-reachable path opens a line or title break iterator.
**Spec:** ECMA-402 §18.1.1 step 9

- ✓ present /grapheme.*word.*sentence/ in jsc/runtime/IntlSegmenter.cpp — found
- ✓ absent /UBRK_LINE|"line"/ in jsc/runtime/IntlSegmenter.cpp — absent
- ✓ present /UBRK_LINE/ in common/brkiter.cpp — found

#### P02 (Tier 1) — feature normalization ✓

**Claim:** String.prototype.normalize form ∈ {NFC, NFD, NFKC, NFKD}. nfkc_cf/nfkc_scf are read only by unorm2_getNFKC*CasefoldInstance, neither in entry-point set. URL IDNA uses uts46.nrm.
**Spec:** ECMA-262 §22.1.3.15 step 5

- ✓ present /NFC.*NFD.*NFKC.*NFKD/ in jsc/runtime/StringPrototype.cpp — found
- ✓ present /"nfkc_cf"/ in common/loadednormalizer2impl.cpp — found
- ✓ present /"uts46"/ in common/uts46.cpp — found
- ✓ entry symbol absent: `unorm2_getNFKCCasefoldInstance`

#### P03 (Tier 1) — lang_tree: 9 rules ✓

**Claim:** Intl.DisplayNames type ∈ {language, region, script, currency, calendar, dateTimeField}. JSC's uldn_* calls read /Languages, /Scripts, /Types/calendar — never /Keys, /Variants, /codePatterns, or %long/%menu/%variant suffixes.
**Spec:** ECMA-402 §12.2.3

- ✓ present /language.*region.*script.*currency.*calendar.*dateTimeField/ in jsc/runtime/IntlDisplayNames.cpp — found
- ✓ present /"Keys"/ in common/locdspnm.cpp — found
- ✓ absent /variant|menu/ in jsc/runtime/IntlDisplayNames.cpp — absent

#### P04 (Tier 1) — lang_tree: 30 rules ✓

**Claim:** DisplayNames type:'calendar' calls uldn_keyValueDisplayName(ldn,'calendar',v), reading /Types/calendar only. No DisplayNames type maps to any other /Types/* key.
**Spec:** ECMA-402 §12.5.1

- ✓ present /uldn_keyValueDisplayName.*calendar/ in jsc/runtime/IntlDisplayNames.cpp — found
- ✓ absent /uldn_keyValueDisplayName.*"(nu|co|hc|lb)"/ in jsc/runtime/IntlDisplayNames.cpp — absent

#### P05 (Tier 1) — locales_tree: 21 rules ✓

**Claim:** Intl.RelativeTimeFormat unit and DisplayNames dateTimeField code lists exclude weekday names. /fields/{mon..sun}* unreachable.
**Spec:** ECMA-402 §17.5.2, §12.5.2

- ✓ present /"second"_s/ in jsc/runtime/IntlRelativeTimeFormat.cpp — found
- ✓ present /"year"_s/ in jsc/runtime/IntlRelativeTimeFormat.cpp — found
- ✓ absent /"mon"_s|"tue"_s|"wed"_s|UDAT_REL_UNIT_MONDAY/ in jsc/runtime/IntlRelativeTimeFormat.cpp — absent
- ✓ absent /"mon"_s|"sunday"_s/ in jsc/runtime/IntlDisplayNames.cpp — absent

#### P06 (Tier 1) — locales_tree: 11 rules ✓

**Claim:** These keys back ICU APIs ECMA-402 doesn't expose: PersonNameFormatter, ulocdata_getExemplarSet, ulocdata_getDelimiter. None of upersonname_*/ulocdata_* are in the entry-point set.
**Spec:** ECMA-402 has no PersonNames/LocaleData API

- ✓ present /"ExemplarCharacters"/ in i18n/ulocdata.cpp — found
- ✓ entry symbol absent: `ulocdata_open`
- ✓ entry symbol absent: `ulocdata_getExemplarSet`
- ✓ entry symbol absent: `ulocdata_getDelimiter`

#### P07 (Tier 2) — misc: 8 rules ✓

**Claim:** These supplementalData.res keys are CLDR reference metadata with no ICU runtime reader in common/ or i18n/.


- ✓ present /bestAvailableLocale|BestAvailableLocale/ in jsc/runtime/IntlObject.cpp — found
- ✓ absent /"territoryInfo"/ in common/locid.cpp — absent
- ✓ absent /"territoryInfo"/ in common/uloc.cpp — absent

#### P08 (Tier 2) — feature misc ✓

**Claim:** genderList.res read by ugender_*; currencyNumericCodes.res by ucurr_getNumericCode. Neither symbol in entry-point set.


- ✓ present /"genderList"/ in i18n/gender.cpp — found
- ✓ present /"currencyNumericCodes"/ in common/ucurr.cpp — found
- ✓ entry symbol absent: `ugender_getInstance`
- ✓ entry symbol absent: `ugender_getListGender`
- ✓ entry symbol absent: `ucurr_getNumericCode`

#### P09 (Tier 2) — brkitr_tree: 1 rules ✓

**Claim:** /exceptions holds sentence-break suppressions used by FilteredBreakIteratorBuilder; ubrk_open(UBRK_SENTENCE) does not read it.


- ✓ present /"exceptions"/ in common/filteredbrk.cpp — found
- ✓ entry symbol absent: `ubrk_openRules`
- ✓ entry symbol absent: `ubrk_openBinaryRules`

#### P10 (Tier 2) — locales_tree: 1 rules ✓

**Claim:** DateTimeSkeletons has no reader in ICU's common/ or i18n/ — CLDR added the data but ICU never wired it up.


- ✓ absent /"DateTimeSkeletons"/ in i18n/dtptngen.cpp — absent
- ✓ present /availableFormats/ in i18n/dtptngen.cpp — found

#### P11 (Tier 1) — unit_tree: 277 rules ✓

**Claim:** Intl.NumberFormat style:'unit' validates unit against IsWellFormedUnitIdentifier (sanctioned list, 45 units). Of 564 CLDR (category,unit) pairs, 768 are outside that set.
**Spec:** ECMA-402 §6.6.1, §6.6.2

- ✓ present /simpleUnits\[/ in jsc/runtime/IntlObject.cpp — found
- ✓ present /sanctionedSimpleUnitIdentifier/ in jsc/runtime/IntlNumberFormat.cpp — found
