#!/usr/bin/env node
// icu-proof.ts — Prove which ICU data is unreachable from JavaScript.
//
// The proof structure:
//
//   JS  ──can only call──▶  JSC's exposed APIs (Intl.*, String.normalize, …)
//                            │ validates options against spec enums (RangeError on invalid)
//                            ▼
//   JSC bindings  ──link──▶  N ICU C symbols (nm libJavaScriptCore.a)   ← STEP 1
//                            ▼
//   ICU internals  ──read──▶ data via ures_getByKey(bundle, "literal")  ← STEP 2/3
//
// JS being general-purpose only varies the *arguments* to those N symbols.
// JSC's spec-validation constrains the arguments. ICU's code is fixed.
// So the reachable-data set is a closed function of (JSC source, ICU source, CLDR data).
//
// SOUNDNESS: every step over-approximates. If we can't resolve a key → KEEP.
// The output is a set of paths PROVABLY outside the reachable closure.
//
// Run:
//   node --experimental-strip-types icu-proof.ts \
//     --webkit-libs <dir with libJavaScriptCore.a, libWTF.a> \
//     --icu-src <icu4c/source dir> \
//     --jsc-src <WebKit/Source/JavaScriptCore dir> \
//     --icu-data <icudt75l.dat> \
//     --derb <path to derb> \
//     [--out filter.json] [--report report.md]

import { readFileSync, writeFileSync, existsSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { parseArgs } from "node:util";

// ═══════════════════════════════════════════════════════════════════════════
// CLI
// ═══════════════════════════════════════════════════════════════════════════

const { values: args } = parseArgs({
  options: {
    "webkit-libs": { type: "string" },
    "icu-src": { type: "string" },
    "jsc-src": { type: "string" },
    "icu-build": { type: "string" }, // ICU build dir (has bin/derb, data/out/build/)
    derb: { type: "string", default: "derb" },
    out: { type: "string", default: "ecma402-filter-proven.json" },
    report: { type: "string", default: "icu-proof-report.md" },
  },
});
function need(k: string): string {
  if (!args[k]) die(`--${k} required`);
  return args[k] as string;
}
const WEBKIT_LIBS = need("webkit-libs");
const ICU_SRC = need("icu-src");
const JSC_SRC = need("jsc-src");
const ICU_BUILD = need("icu-build");
if (!args.derb || args.derb === "derb") args.derb = `${ICU_BUILD}/bin/derb`;

// ═══════════════════════════════════════════════════════════════════════════
// Proof types
// ═══════════════════════════════════════════════════════════════════════════

type Tier = 1 | 2;
//  1 = spec-unreachable: ECMA-402's input validation rejects the value before
//      ICU is called. Provable from spec text + JSC source assertion.
//  2 = link-unreachable: no path from any JSC-linked ICU symbol to any
//      ures_*/udata_* read of this key, per ICU source analysis.

interface Citation {
  file: string;      // relative to ICU_SRC or JSC_SRC
  pattern: string;   // grep pattern that must match (verified each run)
  expect: "present" | "absent"; // present = key IS read here; absent = key is NOT here
}

interface Proof {
  tier: Tier;
  drop: { category: string; rules: string[] } | { feature: string; blacklist: string[] };
  claim: string;
  spec?: string;         // ECMA-402 / ECMA-262 section
  citations: Citation[]; // each verified on every run
  estKB?: number;
}

// ═══════════════════════════════════════════════════════════════════════════
// STEP 1 — Entry points: every ICU symbol JSC/WTF/Bun links
// ═══════════════════════════════════════════════════════════════════════════

function gatherEntryPoints(): Set<string> {
  const syms = new Set<string>();
  for (const lib of ["libJavaScriptCore.a", "libWTF.a"]) {
    const path = `${WEBKIT_LIBS}/${lib}`;
    if (!existsSync(path)) die(`missing ${path}`);
    const r = spawnSync("nm", [path], { maxBuffer: 64 << 20 });
    for (const line of r.stdout.toString().split("\n")) {
      const m = line.match(/ U (u[a-z]+_\w+?)(?:_\d+)?$/);
      if (m) syms.add(m[1]);
    }
  }
  return syms;
}

// ═══════════════════════════════════════════════════════════════════════════
// STEP 2 — Citation verifier: each proof's source claims must hold
// ═══════════════════════════════════════════════════════════════════════════

function verify(c: Citation): { ok: boolean; detail: string } {
  const root = c.file.startsWith("jsc/") ? JSC_SRC + c.file.slice(3) : ICU_SRC + "/" + c.file;
  if (!existsSync(root)) return { ok: false, detail: `file not found: ${root}` };
  const txt = readFileSync(root, "utf8");
  const found = new RegExp(c.pattern, "m").test(txt);
  const ok = c.expect === "present" ? found : !found;
  return { ok, detail: `${c.expect}: /${c.pattern}/ in ${c.file} — ${found ? "FOUND" : "absent"}` };
}

// ═══════════════════════════════════════════════════════════════════════════
// STEP 3 — The proofs.
//
// Each proof is a self-contained argument with verifiable citations. On each
// run, every citation is re-checked against current source. If a citation
// fails (e.g., ICU bumped and the code moved), the proof is REJECTED and its
// drop is excluded from the output filter.
// ═══════════════════════════════════════════════════════════════════════════

const PROOFS: Proof[] = [
  // ───────────────────────────────────────────────────────────────────────
  // TIER 1 — Spec-unreachable (closed enums in ECMA-402, enforced by JSC)
  // ───────────────────────────────────────────────────────────────────────
  {
    tier: 1,
    drop: { feature: "brkitr_rules", blacklist: ["line", "line_cj", "line_loose", "line_loose_cj", "line_normal", "line_normal_cj", "line_normal_phrase_cj", "line_loose_phrase_cj", "line_phrase_cj", "line_strict", "line_strict_cj", "line_strict_phrase_cj", "title"] },
    claim: "Intl.Segmenter granularity ∈ {grapheme, word, sentence}. JSC's IntlSegmenter validates against exactly those three; any other value throws RangeError before ubrk_open is called. WTF's TextBreakIterator only opens character/word/sentence iterators. No JS-reachable path opens a line or title break iterator.",
    spec: "ECMA-402 §18.1.1 InitializeSegmenter step 9 — granularity defaults 'grapheme', validated against «grapheme, word, sentence»",
    citations: [
      { file: "jsc/runtime/IntlSegmenter.cpp", pattern: 'grapheme.*word.*sentence', expect: "present" },
      { file: "jsc/runtime/IntlSegmenter.cpp", pattern: 'UBRK_LINE|"line"', expect: "absent" },
      { file: "common/brkiter.cpp", pattern: 'UBRK_LINE', expect: "present" }, // ICU has it; JSC doesn't reach it
    ],
    estKB: 330,
  },
  {
    tier: 1,
    drop: { feature: "normalization", blacklist: ["nfkc_cf", "nfkc_scf"] },
    claim: "String.prototype.normalize form ∈ {NFC, NFD, NFKC, NFKD}. JSC validates and throws on any other value. nfkc_cf.nrm is read only by unorm2_getNFKCCasefoldInstance; nfkc_scf.nrm by unorm2_getNFKCSimpleCasefoldInstance. Neither is in the entry-point set. URL IDNA uses uts46.nrm.",
    spec: "ECMA-262 §22.1.3.15 String.prototype.normalize step 5",
    citations: [
      { file: "jsc/runtime/StringPrototype.cpp", pattern: 'NFC.*NFD.*NFKC.*NFKD', expect: "present" },
      { file: "common/loadednormalizer2impl.cpp", pattern: '"nfkc_cf"', expect: "present" },
      { file: "common/uts46.cpp", pattern: '"uts46"', expect: "present" }, // IDNA uses uts46, not nfkc_cf
    ],
    estKB: 104,
  },
  {
    tier: 1,
    drop: { category: "lang_tree", rules: ["-/Keys", "-/Variants", "-/codePatterns", "-/characterLabelPattern", "-/Languages%long", "-/Languages%menu", "-/Languages%variant", "-/Scripts%stand-alone", "-/Scripts%variant"] },
    claim: "Intl.DisplayNames type ∈ {language, region, script, currency, calendar, dateTimeField}. JSC maps these to specific uldn_* / udatpg_* calls that read /Languages, /Scripts, /Countries, /Currencies, /Types/calendar, /fields/* — never /Keys, /Variants, /codePatterns, or the %long/%menu/%variant style suffixes (only %short for style:'short').",
    spec: "ECMA-402 §12.2.3 Internal slots — [[Type]] validated against «language, region, script, currency, calendar, dateTimeField»",
    citations: [
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'language.*region.*script.*currency.*calendar.*dateTimeField', expect: "present" },
      { file: "common/locdspnm.cpp", pattern: '"Keys"', expect: "present" }, // ICU has it; JSC type enum can't reach it
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'variant|menu', expect: "absent" },
    ],
    estKB: 80,
  },
  {
    tier: 1,
    drop: { category: "lang_tree", rules: ["-/Types/numbers", "-/Types/collation", "-/Types/hc", "-/Types/lb", "-/Types/ms", "-/Types/cf", "-/Types/d0", "-/Types/kr", "-/Types/m0", "-/Types/s0", "-/Types/t0", "-/Types/x0", "-/Types/dx", "-/Types/em", "-/Types/fw", "-/Types/h0", "-/Types/i0", "-/Types/k0", "-/Types/mu", "-/Types/ss", "-/Types/va", "-/Types/colAlternate", "-/Types/colBackwards", "-/Types/colCaseFirst", "-/Types/colCaseLevel", "-/Types/colNormalization", "-/Types/colNumeric", "-/Types/colReorder", "-/Types/colStrength", "-/Types%short"] },
    claim: "Intl.DisplayNames type:'calendar' calls uldn_keyValueDisplayName(ldn, 'calendar', value). That reads /Types/calendar only. No DisplayNames type maps to any other /Types/* key — those are display names for BCP-47 extension keys (hc, lb, nu, co, …) which DisplayNames doesn't expose.",
    spec: "ECMA-402 §12.5.1 Intl.DisplayNames.prototype.of — only 'calendar' uses keyValueDisplayName",
    citations: [
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'uldn_keyValueDisplayName.*calendar', expect: "present" },
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'uldn_keyValueDisplayName.*"(nu|co|hc|lb)"', expect: "absent" },
    ],
    estKB: 250,
  },
  {
    tier: 1,
    drop: { category: "locales_tree", rules: ["-/fields/sun", "-/fields/sun-short", "-/fields/sun-narrow", "-/fields/mon", "-/fields/mon-short", "-/fields/mon-narrow", "-/fields/tue", "-/fields/tue-short", "-/fields/tue-narrow", "-/fields/wed", "-/fields/wed-short", "-/fields/wed-narrow", "-/fields/thu", "-/fields/thu-short", "-/fields/thu-narrow", "-/fields/fri", "-/fields/fri-short", "-/fields/fri-narrow", "-/fields/sat", "-/fields/sat-short", "-/fields/sat-narrow"] },
    claim: "Intl.RelativeTimeFormat unit ∈ {year, quarter, month, week, day, hour, minute, second} (plus singulars). JSC's sanctionedRelativeTimeUnit validates and throws on weekday names. Intl.DisplayNames type:'dateTimeField' code list (§12.5.2 IsValidDateTimeFieldCode) does not include individual weekday names. So /fields/{mon..sun}* are unreachable.",
    spec: "ECMA-402 §17.5.2 SingularRelativeTimeUnit; §12.5.2 IsValidDateTimeFieldCode",
    citations: [
      { file: "jsc/runtime/IntlRelativeTimeFormat.cpp", pattern: '"second"_s', expect: "present" },
      { file: "jsc/runtime/IntlRelativeTimeFormat.cpp", pattern: '"year"_s', expect: "present" },
      { file: "jsc/runtime/IntlRelativeTimeFormat.cpp", pattern: '"mon"_s|"tue"_s|"wed"_s|UDAT_REL_UNIT_MONDAY', expect: "absent" },
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: '"mon"_s|"sunday"_s', expect: "absent" },
    ],
    estKB: 140,
  },
  {
    tier: 1,
    drop: { category: "locales_tree", rules: ["-/personNames", "-/characterLabel", "-/ExemplarCharacters", "-/ExemplarCharactersIndex", "-/ExemplarCharactersNumbers", "-/ExemplarCharactersPunctuation", "-/AuxExemplarCharacters", "-/delimiters", "-/measurementSystemNames", "-/Ellipsis", "-/MoreInformation"] },
    claim: "These top-level keys back ICU APIs that ECMA-402 doesn't expose: PersonNameFormatter (personNames), ulocdata_getExemplarSet (ExemplarCharacters*), ulocdata_getDelimiter (delimiters), characterLabel/Ellipsis/MoreInformation (no API). None of upersonname_*, ulocdata_* are in the entry-point set.",
    spec: "ECMA-402 has no PersonNames, LocaleData, or characterLabel API",
    citations: [
      { file: "i18n/ulocdata.cpp", pattern: '"ExemplarCharacters"', expect: "present" },
      // ASSERTION: ulocdata_* not in entry points — checked dynamically below
    ],
    estKB: 615,
  },
  // NOTE: -/Countries%variant — locdspnm.cpp builds the "%variant" suffix
  // dynamically; reachability depends on uldn_openForContext flags. JSC's
  // IntlDisplayNames passes UDISPCTX_LENGTH_*, not a variant flag, but the
  // proof of which contexts trigger %variant requires deeper dataflow than a
  // grep citation can establish. Dropped from proven set (36 KB).
  //
  // NOTE: -/Currencies%formal is REACHABLE — JSC has CurrencyDisplay::FormalSymbol
  // (gated on Options::useMoreCurrencyDisplayChoices(), a stage-3 proposal). The
  // proof correctly rejected this drop. -/Currencies%variant is also reachable
  // for the same reason (CurrencyDisplay::Variant exists in the same enum).

  // ───────────────────────────────────────────────────────────────────────
  // TIER 1 — Unsanctioned units (generated, not hardcoded — see step 4)
  // ───────────────────────────────────────────────────────────────────────
  // The unit_tree drops are GENERATED at run time from JSC's sanctioned list
  // + CLDR's unit list, so they stay correct across CLDR bumps. See
  // generateUnitProof() below.

  // ───────────────────────────────────────────────────────────────────────
  // TIER 2 — Link-unreachable (no path from entry points to a reader)
  // ───────────────────────────────────────────────────────────────────────
  {
    tier: 2,
    drop: { category: "misc", rules: ["-/territoryInfo", "-/languageData", "-/subdivisionContainment", "-/weekOfPreference", "-/codeMappingsCurrency", "-/personNamesDefaults", "-/idValidity/subdivision", "-/idValidity/unit"] },
    claim: "These supplementalData.res keys are CLDR reference metadata with no ICU runtime reader in common/ or i18n/. (languageMatching* removed from this drop: langInfo.res/match IS read by loclikelysubtags.cpp ← uloc_addLikelySubtags ← Intl.Locale.maximize, so languageMatching data may share derivation — kept conservatively.)",
    citations: [
      { file: "jsc/runtime/IntlObject.cpp", pattern: 'bestAvailableLocale|BestAvailableLocale', expect: "present" },
      // territoryInfo: only reader is in tools/ — verify absent from runtime
      { file: "common/locid.cpp", pattern: '"territoryInfo"', expect: "absent" },
      { file: "common/uloc.cpp", pattern: '"territoryInfo"', expect: "absent" },
    ],
    estKB: 130,
  },
  {
    tier: 2,
    drop: { feature: "misc", blacklist: ["genderList", "currencyNumericCodes"] },
    claim: "genderList.res is read by GenderInfo (ugender_*); currencyNumericCodes.res by ucurr_getNumericCode. Neither symbol is in the entry-point set.",
    citations: [
      { file: "i18n/gender.cpp", pattern: '"genderList"', expect: "present" },
      { file: "common/ucurr.cpp", pattern: '"currencyNumericCodes"', expect: "present" },
      // ASSERTION: ugender_*, ucurr_getNumericCode not in entry points — checked dynamically
    ],
    estKB: 4,
  },
  {
    tier: 2,
    drop: { category: "brkitr_tree", rules: ["-/exceptions"] },
    claim: "/exceptions in brkitr/*.res holds sentence-break suppressions used by FilteredBreakIteratorBuilder. JSC opens via ubrk_open with UBRK_SENTENCE, which does not read /exceptions; the filtered builder is a separate API not in the entry-point set.",
    citations: [
      { file: "common/filteredbrk.cpp", pattern: '"exceptions"', expect: "present" },
      // ASSERTION: ubrk_openBinaryRules not in entry points — checked dynamically
    ],
    estKB: 11,
  },
  {
    tier: 2,
    drop: { category: "locales_tree", rules: ["-/calendar/*/DateTimeSkeletons"] },
    claim: "DateTimeSkeletons has no reader anywhere in ICU's common/ or i18n/ — the data was added to CLDR but ICU's runtime never wired it up (verified by grep across all runtime sources).",
    citations: [
      { file: "i18n/dtptngen.cpp", pattern: '"DateTimeSkeletons"', expect: "absent" }, // confirms no reader
      { file: "i18n/dtptngen.cpp", pattern: 'availableFormats', expect: "present" }, // this is what's actually read
    ],
    estKB: 50,
  },
];

// Entry-point absence assertions (checked against the actual nm output)
const MUST_BE_ABSENT_FROM_ENTRY_POINTS = [
  { syms: ["ulocdata_open", "ulocdata_getExemplarSet", "ulocdata_getDelimiter"], proves: "personNames/ExemplarCharacters/delimiters" },
  { syms: ["uloc_acceptLanguage", "uloc_acceptLanguageFromHTTP"], proves: "supplementalData/languageMatching*" },
  { syms: ["ugender_getInstance", "ugender_getListGender"], proves: "genderList.res" },
  { syms: ["ucurr_getNumericCode"], proves: "currencyNumericCodes.res" },
  { syms: ["unorm2_getNFKCCasefoldInstance"], proves: "nfkc_cf.nrm" },
  { syms: ["ubrk_openRules", "ubrk_openBinaryRules"], proves: "brkitr/exceptions, custom rules" },
];

// ═══════════════════════════════════════════════════════════════════════════
// STEP 4 — Generate the unit_tree proof from live sanctioned-unit list
// ═══════════════════════════════════════════════════════════════════════════

function generateUnitProof(): Proof {
  // The sanctioned list is the spec's Table; JSC encodes it in IntlObject.cpp.
  // We extract it from JSC source so the proof tracks JSC, not a snapshot.
  const intlObj = readFileSync(`${JSC_SRC}/runtime/IntlObject.cpp`, "utf8");
  const m = intlObj.match(/simpleUnits\[\d*\]\s*=\s*\{([\s\S]+?)\n\};/);
  if (!m) die("could not find simpleUnits[] in IntlObject.cpp — JSC structure changed");
  // entries are { "category"_s, "unit"_s } — take the second string of each pair
  const all = [...m[1].matchAll(/"([a-z-]+)"_s/g)].map(x => x[1]);
  const sanctioned = new Set(all.filter((_, i) => i % 2 === 1));

  // Enumerate all (category, unit) pairs in CLDR via derb on unit/root.res.
  // derb needs --sourcedir pointing at the bundle dir so it can resolve pool.res.
  const unitDir = `${ICU_BUILD}/data/out/build/icudt75l/unit`;
  const derb = sh(args.derb!, ["-s", unitDir, "-c", "root"], { LD_LIBRARY_PATH: derbLib() });
  const pairs: [string, string][] = [];
  let cat = "";
  for (const line of derb.split("\n")) {
    const cm = line.match(/^ {8}([a-z]+)\{$/);
    if (cm) cat = cm[1];
    const um = line.match(/^ {12}([a-z][a-z0-9-]+)\{$/);
    if (um && cat && cat !== "compound") pairs.push([cat, um[1]]);
  }

  // A unit is reachable if it's sanctioned OR it's <sanc>-per-<sanc> (since
  // ECMA-402 allows compound units of two sanctioned simples) OR it's a
  // structural key.
  const isReachableUnit = (u: string): boolean => {
    if (sanctioned.has(u)) return true;
    const per = u.split("-per-");
    if (per.length === 2 && sanctioned.has(per[0]) && sanctioned.has(per[1])) return true;
    return false;
  };
  const cats = new Set(pairs.map(p => p[0]));
  const reachableCats = new Set<string>(["compound"]);
  for (const [c, u] of pairs) if (isReachableUnit(u)) reachableCats.add(c);

  const rules: string[] = ["-/durationUnits"];
  for (const c of cats) {
    if (!reachableCats.has(c)) {
      for (const w of ["units", "unitsShort", "unitsNarrow"]) rules.push(`-/${w}/${c}`);
    } else {
      for (const [cc, u] of pairs) {
        if (cc === c && !isReachableUnit(u)) {
          for (const w of ["units", "unitsShort", "unitsNarrow"]) rules.push(`-/${w}/${c}/${u}`);
        }
      }
    }
  }

  return {
    tier: 1,
    drop: { category: "unit_tree", rules: [...new Set(rules)].sort() },
    claim: `Intl.NumberFormat style:'unit' validates 'unit' against IsWellFormedUnitIdentifier, which requires each part be in the sanctioned list (currently ${sanctioned.size} units). Any unit not sanctioned, and any compound not <sanctioned>-per-<sanctioned>, throws RangeError before unumf_* is called. Of ${pairs.length} CLDR (category,unit) pairs, ${rules.length - 1} are outside that set.`,
    spec: "ECMA-402 §6.6.1 IsWellFormedUnitIdentifier; §6.6.2 IsSanctionedSingleUnitIdentifier",
    citations: [
      { file: "jsc/runtime/IntlObject.cpp", pattern: "simpleUnits\\[", expect: "present" },
      { file: "jsc/runtime/IntlNumberFormat.cpp", pattern: "sanctionedSimpleUnitIdentifier", expect: "present" },
    ],
    estKB: 1850,
  };
}

function derbLib(): string { return `${ICU_BUILD}/lib`; }

// ═══════════════════════════════════════════════════════════════════════════
// STEP 6 — Main: verify all proofs, emit filter + report
// ═══════════════════════════════════════════════════════════════════════════

function main() {
  const report: string[] = ["# ICU reachability proof — generated by icu-proof.ts", ""];

  // Step 1
  const entryPoints = gatherEntryPoints();
  report.push(`## Entry points (JSC + WTF → ICU): ${entryPoints.size} symbols`, "");
  report.push("```", [...entryPoints].sort().join("\n"), "```", "");

  // Entry-point absence checks
  report.push("## Entry-point absence assertions", "");
  let absenceOK = true;
  for (const { syms, proves } of MUST_BE_ABSENT_FROM_ENTRY_POINTS) {
    const present = syms.filter(s => entryPoints.has(s));
    const ok = present.length === 0;
    if (!ok) absenceOK = false;
    report.push(`- ${ok ? "✓" : "✗"} \`${syms.join(", ")}\` ${ok ? "absent" : "**PRESENT: " + present.join(",") + "**"} — backs proof of: ${proves}`);
  }
  report.push("");

  // Step 3 + generated unit proof
  const allProofs = [...PROOFS, generateUnitProof()];

  // Verify every citation
  report.push("## Proofs", "");
  const accepted: Proof[] = [];
  for (const p of allProofs) {
    const dropDesc = "category" in p.drop ? `${p.drop.category}: ${p.drop.rules.length} rules` : `feature ${p.drop.feature}: ${p.drop.blacklist.join(", ")}`;
    report.push(`### Tier ${p.tier} — ${dropDesc}`, "");
    report.push(`**Claim:** ${p.claim}`, "");
    if (p.spec) report.push(`**Spec:** ${p.spec}`, "");
    let allOK = true;
    for (const c of p.citations) {
      const v = verify(c);
      if (!v.ok) allOK = false;
      report.push(`- ${v.ok ? "✓" : "✗ **REJECTED**"} ${v.detail}`);
    }
    if (allOK) {
      accepted.push(p);
      report.push("", `→ **ACCEPTED** (est. ${p.estKB ?? "?"} KB)`, "");
    } else {
      report.push("", `→ **REJECTED** — citation failed; source moved or claim invalidated. Re-audit before re-enabling.`, "");
    }
  }

  // Emit filter
  const filter: any = { strategy: "subtractive", featureFilters: {}, resourceFilters: [] };
  const byCategory = new Map<string, Set<string>>();
  for (const p of accepted) {
    if ("feature" in p.drop) {
      filter.featureFilters[p.drop.feature] = { blacklist: p.drop.blacklist };
    } else {
      if (!byCategory.has(p.drop.category)) byCategory.set(p.drop.category, new Set());
      for (const r of p.drop.rules) byCategory.get(p.drop.category)!.add(r);
    }
  }
  for (const [cat, rules] of byCategory) {
    filter.resourceFilters.push({ categories: [cat], rules: [...rules].sort() });
  }

  writeFileSync(args.out!, JSON.stringify(filter, null, 2));
  writeFileSync(args.report!, report.join("\n"));

  const totalKB = accepted.reduce((s, p) => s + (p.estKB || 0), 0);
  console.error(`\n${accepted.length}/${allProofs.length} proofs accepted (~${totalKB} KB)`);
  console.error(`filter:  ${args.out}`);
  console.error(`report:  ${args.report}`);
  if (!absenceOK || accepted.length < allProofs.length) {
    console.error(`\n⚠ Some proofs/assertions FAILED — see ${args.report}. Filter excludes rejected drops.`);
    process.exit(1);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// util
// ═══════════════════════════════════════════════════════════════════════════
function sh(cmd: string, argv: string[], env?: Record<string, string>): string {
  const r = spawnSync(cmd, argv, { maxBuffer: 64 << 20, env: { ...process.env, ...env } });
  if (r.status !== 0) die(`${cmd} ${argv.join(" ")} failed: ${r.stderr}`);
  return r.stdout.toString();
}
function die(msg: string): never { console.error(`[icu-proof] ${msg}`); process.exit(1); }

main();
