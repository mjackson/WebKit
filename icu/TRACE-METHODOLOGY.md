# How icu/ecma402-filter.json was derived

The filter drops ICU resource-bundle paths that no ECMA-402 API can reach. It
was derived by **runtime instrumentation**, not static analysis:

## Method

1. Build ICU with `CPPFLAGS=-DU_ENABLE_TRACING=1` (or `--enable-tracing`).
   This compiles in `icu::ResourceTracer` (restrace.cpp), which fires on every
   successful resource read — both the C `ures_*` path and the C++
   `ResourceSink` path, post-fallback, post-alias.

2. Register a `utrace_setFunctions` callback in Bun startup that writes
   `(type, file, /full/key/path)` to `BUN_ICU_TRACE_FD`.

3. Run an exhaustive ECMA-402 workload: every `Intl.*` constructor × every
   option value × 600 locales × every method (~158K operations). See
   `ecma402-exhaustive.ts`.

4. The set of paths NEVER traced is provably unreachable from JavaScript.
   Generate `resourceFilters` rules for those.

5. Rebuild ICU data with `ICU_DATA_FILTER_FILE` pointing at the filter; link
   into Bun; verify the Intl test suite + the exhaustive workload produce
   byte-identical output.

## What's dropped (−2.90 MB)

| Tree | Paths | Why unreachable |
|---|---|---|
| `locales_tree` | `personNames`, `characterLabel`, `parse`, `ExemplarCharacters*`, `AuxExemplarCharacters`, `delimiters`, `measurementSystemNames`, `Ellipsis`, `MoreInformation`, `calendar/*/DateTimeSkeletons` | APIs not in ECMA-402 (`ulocdata_*`, `PersonNameFormatter`, parsing) |
| `unit_tree` | 11 categories + 87 individual units | `Intl.NumberFormat` rejects unsanctioned units at the spec level |
| `lang_tree` | `Keys`, `Variants`, `codePatterns`, `characterLabelPattern`, `Languages%{long,menu,variant}`, `Scripts%{stand-alone,variant}` | `Intl.DisplayNames` only reads `Languages`/`Languages%short`, `Scripts`/`Scripts%short`, `Types`/`Types%short`, `localeDisplayPattern` |
| `region_tree` | `Countries%variant`, `Countries%chagos` | `Intl.DisplayNames` only reads `Countries`/`Countries%short` |
| `curr_tree` | `Currencies%variant`, `Currencies%formal` | `currencyDisplay` only supports `symbol`/`narrowSymbol`/`code`/`name` |

## Regenerating after an ICU/CLDR bump

Re-run the trace workload against the new data; diff the accessed-set;
update the filter. The trace artifacts and generator live in Bun's
`tmp/icu-b2/{icu_trace_hook.cpp, ecma402-exhaustive.ts, gen-filter.ts}`.
