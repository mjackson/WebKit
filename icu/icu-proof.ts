#!/usr/bin/env node
// icu-proof.ts — Prove which ICU data is unreachable from JavaScript.
//
// Two independent proof layers, three output configs:
//
//   LAYER 1 — read-reachability closure (mechanical, over-approximating).
//     Phase 1: which ICU .cpp files survived the linker's dead-strip?
//     Phase 2: in each, find every ures_*/udata_* call, resolve the key arg.
//              Unresolved key → KEEP-ALL for that tree (sound).
//     KEEP_read = union of all resolved paths/prefixes.
//     DROP_L1   = (all paths in data) − KEEP_read.
//
//     Layer 1 has one soft spot: SITE_ANNOTATIONS — manual key resolutions
//     for sites the auto-resolver can't handle. Each is a human claim.
//
//   LAYER 2 — output-unreachability proofs (cited, per-path).
//     PROOFS[]: each names a drop, states a claim, cites spec + source lines.
//     Citations are grep-verified on every run; a failed citation REJECTS
//     the proof. generateUnitProof() reads JSC's simpleUnits[] live.
//
//   ┌─────────┬──────────────────────────────┬──────────────────────────────┐
//   │ Config  │ Derivation                   │ Soundness budget             │
//   ├─────────┼──────────────────────────────┼──────────────────────────────┤
//   │ A       │ Layer 1, annotations OFF     │ 0 human claims               │
//   │ B       │ Layer 1, annotations ON      │ |SITE_ANNOTATIONS| claims    │
//   │ C       │ B ∪ accepted Layer-2 proofs  │ |annotations| + |proofs|     │
//   └─────────┴──────────────────────────────┴──────────────────────────────┘
//
// Run:
//   node --experimental-strip-types icu/icu-proof.ts \
//     --bun-binary <release bun> --webkit-libs <dir with libicu*.a> \
//     --icu-src <icu4c/source> --jsc-src <WebKit/Source/JavaScriptCore> \
//     --icu-build <icu build dir> --out-dir <dir>

import { readFileSync, writeFileSync, existsSync, readdirSync, mkdirSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { join, basename } from "node:path";
import { parseArgs } from "node:util";

const { values: args } = parseArgs({
  options: {
    "bun-binary": { type: "string" },
    "webkit-libs": { type: "string" },
    "icu-src": { type: "string" },
    "jsc-src": { type: "string" },
    "icu-build": { type: "string" },
    "out-dir": { type: "string", default: "icu-proof-out" },
  },
});
const need = (k: string): string => { if (!args[k]) die(`--${k} required`); return args[k] as string; };
const BUN_BIN = need("bun-binary");
const WEBKIT_LIBS = need("webkit-libs");
const ICU_SRC = need("icu-src");
const JSC_SRC = need("jsc-src");
const ICU_BUILD = need("icu-build");
const OUT_DIR = args["out-dir"]!;
mkdirSync(OUT_DIR, { recursive: true });

// ═══════════════════════════════════════════════════════════════════════════
// KEEP-set accumulator
// ═══════════════════════════════════════════════════════════════════════════

interface KeepReason { path: string; site: string; why: string }
class Keep {
  paths = new Map<string, Set<string>>();      // tree → exact paths
  prefixes = new Map<string, Set<string>>();   // tree → prefixes (keep all under)
  keepAllTrees = new Set<string>();            // trees where everything is kept
  dataFiles = new Set<string>();               // .nrm/.icu/.brk/.dict files
  reasons: KeepReason[] = [];
  unresolved: { site: string; expr: string; tree: string }[] = [];
  // Which annotation lemmas were applied, per tree (for soundness-budget tracking)
  annUsed = new Map<string, Set<string>>();    // tree → set of lemma ids

  path(tree: string, p: string, site: string, why: string) {
    (this.paths.get(tree) ?? this.paths.set(tree, new Set()).get(tree)!).add(p);
    this.reasons.push({ path: `${tree}:${p}`, site, why });
  }
  prefix(tree: string, p: string, site: string, why: string) {
    (this.prefixes.get(tree) ?? this.prefixes.set(tree, new Set()).get(tree)!).add(p);
    this.reasons.push({ path: `${tree}:${p}/*`, site, why });
  }
  tree(tree: string, site: string, why: string) {
    this.keepAllTrees.add(tree);
    this.reasons.push({ path: `${tree}:*`, site, why });
  }
  file(name: string, site: string, why: string) {
    this.dataFiles.add(name);
    this.reasons.push({ path: name, site, why });
  }
  unresolvable(tree: string, site: string, expr: string) {
    this.unresolved.push({ site, expr, tree });
    this.tree(tree, site, `UNRESOLVED key expr: ${expr}`);
  }
  annotation(tree: string, lemmaId: string) {
    (this.annUsed.get(tree) ?? this.annUsed.set(tree, new Set()).get(tree)!).add(lemmaId);
  }
  coversTop(tree: string, top: string): boolean {
    // Is top-level path /top reachable per this KEEP set?
    if (this.keepAllTrees.has(tree)) return true;
    const p = "/" + top;
    for (const kp of this.paths.get(tree) ?? []) {
      if (kp === p || kp.startsWith(p + "/")) return true;
      if (kp === "/*" || kp.startsWith("/*/")) return true;
    }
    for (const kp of this.prefixes.get(tree) ?? []) {
      if (kp === p || kp.startsWith(p + "/") || p.startsWith(kp + "/") || kp === "/" || kp === "/*") return true;
    }
    return false;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// LAYER 1 / PHASE 1 — Which ICU source files have code in the linked binary?
// ═══════════════════════════════════════════════════════════════════════════

interface Phase1 { reachableSrcFiles: Set<string>; entrySymbols: Set<string> }

function phase1(): Phase1 {
  // Baseline: every object in libicuuc.a + libicui18n.a.
  const reachableSrcFiles = new Set<string>();
  const symToFile = new Map<string, string>();
  for (const lib of ["libicuuc.a", "libicui18n.a"]) {
    const path = `${WEBKIT_LIBS}/${lib}`;
    if (!existsSync(path)) continue;
    for (const obj of sh("ar", ["t", path]).split("\n")) {
      const m = obj.match(/^(\w+)\.ao?$/);
      if (m) reachableSrcFiles.add(m[1] + ".cpp");
    }
  }
  // Tighten: narrow to files whose symbols survived dead-strip in BUN_BIN.
  const binSyms = new Set<string>();
  for (const line of sh("nm", ["--defined-only", BUN_BIN]).split("\n")) {
    const m = line.match(/ [TtWw] (\S+)$/);
    if (m && (/^_ZN6icu_\d+/.test(m[1]) || (/^u[a-z]{2,}_/.test(m[1]) && !m[1].startsWith("uv_")))) binSyms.add(m[1]);
  }
  if (binSyms.size > 100) {
    for (const lib of ["libicuuc.a", "libicui18n.a"]) {
      for (const line of sh("nm", ["-A", "--defined-only", `${WEBKIT_LIBS}/${lib}`]).split("\n")) {
        const m = line.match(/:(\w+)\.ao?:\w+ [TtWw] (\S+)$/);
        if (m) symToFile.set(m[2], m[1] + ".cpp");
      }
    }
    const narrowed = new Set<string>();
    for (const s of binSyms) { const f = symToFile.get(s); if (f) narrowed.add(f); }
    if (narrowed.size > 0) { reachableSrcFiles.clear(); for (const f of narrowed) reachableSrcFiles.add(f); }
  }
  // Entry symbols: ICU C-API symbols JSC/WTF reference (for Layer-2 absence checks).
  const entrySymbols = new Set<string>();
  for (const lib of ["libJavaScriptCore.a", "libWTF.a"]) {
    for (const line of sh("nm", [`${WEBKIT_LIBS}/${lib}`]).split("\n")) {
      const m = line.match(/ U (u[a-z]+_\w+?)(?:_\d+)?$/);
      if (m) entrySymbols.add(m[1]);
    }
  }
  return { reachableSrcFiles, entrySymbols };
}

// ═══════════════════════════════════════════════════════════════════════════
// LAYER 1 / PHASE 2 — Extract data-read sites from each reachable source file
// ═══════════════════════════════════════════════════════════════════════════

const READ_FNS = [
  "ures_getByKey", "ures_getByKeyWithFallback", "ures_getStringByKey",
  "ures_getStringByKeyWithFallback", "ures_getAllChildrenWithFallback",
  "ures_getAllItemsWithFallback", "ures_openDirect", "ures_open", "ures_openU",
  "udata_open", "udata_openChoice",
  "getAllItemsWithFallback", "getAllChildrenWithFallback",
];

function findIcuSrc(base: string): string | null {
  for (const dir of ["common", "i18n"]) {
    const p = `${ICU_SRC}/${dir}/${base}`;
    if (existsSync(p)) return p;
    const c = p.replace(/\.cpp$/, ".c");
    if (existsSync(c)) return c;
  }
  return null;
}

function phase2(reachable: Set<string>, useAnnotations: boolean): Keep {
  const keep = new Keep();
  for (const base of reachable) {
    const path = findIcuSrc(base);
    if (path) analyzeFile(path, keep, useAnnotations);
  }
  for (const f of grepFiles(JSC_SRC + "/runtime", /ures_|udata_/)) analyzeFile(f, keep, useAnnotations);
  return keep;
}

function analyzeFile(path: string, keep: Keep, useAnnotations: boolean) {
  const src = readFileSync(path, "utf8");
  const rel = path.includes(ICU_SRC) ? path.slice(ICU_SRC.length + 1) : "jsc/" + path.slice(JSC_SRC.length + 1);

  const tags = new Map<string, string>();
  for (const m of src.matchAll(/const\s+char\s*\*?\s*(?:const\s+)?(\w+)\s*(?:\[\s*\])?\s*=\s*u?"([^"]*)"/g)) tags.set(m[1], m[2]);
  for (const m of src.matchAll(/^#define\s+(\w+)\s+u?"([^"]*)"/gm)) tags.set(m[1], m[2]);
  const arrays = new Map<string, string[]>();
  for (const m of src.matchAll(/const\s+char\s*\*\s*(?:const\s+)?(\w+)\s*\[\s*\w*\s*\]\s*=\s*\{([^}]+)\}/g)) {
    arrays.set(m[1], [...m[2].matchAll(/"([^"]+)"/g)].map(x => x[1]));
  }
  const charstrs = new Map<string, string[]>();
  for (const m of src.matchAll(/CharString\s+(\w+)\s*;/g)) charstrs.set(m[1], []);
  for (const m of src.matchAll(/(\w+)\.append\(\s*([^,)]+)/g)) {
    if (charstrs.has(m[1])) charstrs.get(m[1])!.push(m[2].trim());
  }
  // Bounded-set patterns: sound because each resolves to "*" (= keep all subkeys).
  const BOUNDED: { pat: RegExp; wild: string }[] = [
    { pat: /calendarType|calType|calendar.*getType\(\)|cType\b|calendarTypeCArray/, wild: "*" },
    { pat: /numberingSystem.*getName\(\)|nsName|numberingSystem\.utf8/, wild: "*" },
    { pat: /locale\.get(Language|Country|Script|Name|BaseName)\(\)|curLocaleName|parentLocaleName|localeID/, wild: "<locale>" },
    { pat: /^tzid\b|mzID|tzCanonicalID/, wild: "*" },
    { pat: /^iso(Code)?$|currency.*data\(\)|isoCode/, wild: "*" },
  ];
  let lastAnn: string | null = null;
  const resolveExpr = (e: string): string | null => {
    e = e.trim();
    let m = e.match(/^u?"([^"]*)"(?:_s)?$/);
    if (m) return m[1];
    if (tags.has(e)) return tags.get(e)!;
    if (e === "nullptr" || e === "NULL" || e === "0") return "";
    if (/CharString|buildResourcePath|\.append\(|StringPiece/.test(e)) {
      const parts = [...e.matchAll(/"([^"]+)"|(\bg\w+\b)/g)].map(m => m[1] ?? tags.get(m[2]!) ?? null);
      if (parts.length && parts.every(p => p != null)) return parts.join("/").replace(/\/+/g, "/");
    }
    const dm = e.match(/^(\w+)\.data\(\)$/);
    if (dm && charstrs.has(dm[1])) {
      const parts = charstrs.get(dm[1])!.map(p => resolveExpr(p));
      if (parts.length && parts.every(p => p != null)) return parts.join("/").replace(/\/+/g, "/");
      if (parts.some(p => p != null)) return parts.map(p => p ?? "*").join("/").replace(/\/+/g, "/");
    }
    const am = e.match(/^(\w+)\[/);
    if (am && arrays.has(am[1])) return "{" + arrays.get(am[1])!.join(",") + "}";
    for (const b of BOUNDED) if (b.pat.test(e)) return b.wild;
    if (useAnnotations) {
      const ann = SITE_ANNOTATIONS.find(a => a.file === rel && (a.expr === e || a.exprPat?.test(e)));
      if (ann) { lastAnn = ann.id; return ann.resolvesTo; }
    }
    return null;
  };

  const FILE_TREE: Record<string, string[]> = {
    "number_longnames.cpp": ["unit_tree", "locales_tree"],
    "number_compact.cpp": ["locales_tree"],
    "number_symbolswrapper.cpp": ["locales_tree"],
    "number_skeletons.cpp": [],
    "dcfmtsym.cpp": ["locales_tree", "curr_tree"],
    "dtfmtsym.cpp": ["locales_tree"],
    "dtptngen.cpp": ["locales_tree"],
    "dtitvinf.cpp": ["locales_tree"],
    "smpdtfmt.cpp": ["locales_tree"],
    "reldtfmt.cpp": ["locales_tree"],
    "reldatefmt.cpp": ["locales_tree"],
    "listformatter.cpp": ["locales_tree"],
    "measfmt.cpp": ["unit_tree", "locales_tree"],
    "measunit_extra.cpp": ["misc"],
    "plurrule.cpp": ["misc"],
    "tznames_impl.cpp": ["zone_tree", "misc"],
    "tzfmt.cpp": ["zone_tree", "locales_tree"],
    "tzgnames.cpp": ["zone_tree", "locales_tree"],
    "timezone.cpp": ["misc"],
    "olsontz.cpp": ["misc"],
    "zonemeta.cpp": ["misc"],
    "windtfmt.cpp": ["misc"],
    "dayperiodrules.cpp": ["misc"],
    "numsys.cpp": ["misc"],
    "collationroot.cpp": ["coll_tree"],
    "collationtailoring.cpp": ["coll_tree"],
    "ucol_res.cpp": ["coll_tree"],
    "rulebasedcollator.cpp": ["coll_tree"],
    "ucurr.cpp": ["curr_tree", "misc"],
    "currpinf.cpp": ["curr_tree", "locales_tree"],
    "region.cpp": ["misc"],
    "gender.cpp": ["misc"],
    "ulocdata.cpp": ["locales_tree", "misc"],
    "ucln_in.cpp": [],
    "brkiter.cpp": ["brkitr_tree"],
    "filteredbrk.cpp": ["brkitr_tree"],
    "rbbi.cpp": ["brkitr_tree"],
    "locdspnm.cpp": ["lang_tree", "region_tree", "locales_tree"],
    "locresdata.cpp": ["locales_tree"],
    "loclikelysubtags.cpp": ["misc"],
    "loclikely.cpp": ["misc"],
    "localematcher.cpp": ["misc"],
    "locid.cpp": ["misc"],
    "locavailable.cpp": ["misc"],
    "uloc_keytype.cpp": ["misc"],
    "uloc_tag.cpp": ["misc"],
    "uresbund.cpp": ["*"],
    "uresdata.cpp": ["*"],
    "loadednormalizer2impl.cpp": ["nrm"],
    "normalizer2impl.cpp": ["nrm"],
    "uts46.cpp": ["nrm"],
    "ucase.cpp": ["icu"],
    "uchar.cpp": ["icu"],
    "uprops.cpp": ["icu"],
    "characterproperties.cpp": ["icu"],
  };
  const base = basename(path);
  let cats: string[];
  if (FILE_TREE[base]) {
    cats = FILE_TREE[base];
    if (cats.length === 0) return;
    if (cats[0] === "*") return;
  } else {
    const treesOpened = new Set<string>();
    for (const m of src.matchAll(/"ICUDATA[-_](\w+)"|U_ICUDATA_(\w+)/g)) treesOpened.add((m[1] || m[2] || "").toLowerCase());
    if (/ures_open\w*\s*\(\s*(nullptr|NULL|0)\s*,/.test(src)) treesOpened.add("locales");
    const TREE_CAT: Record<string, string> = {
      locales: "locales_tree", curr: "curr_tree", lang: "lang_tree", region: "region_tree",
      unit: "unit_tree", zone: "zone_tree", coll: "coll_tree", brkitr: "brkitr_tree", "": "locales_tree",
    };
    cats = [...treesOpened].map(t => TREE_CAT[t] || "misc");
    if (cats.length === 0) cats = ["misc"];
  }

  const fnPat = new RegExp(`\\b(${READ_FNS.join("|")})\\s*\\(`, "g");
  for (const m of src.matchAll(fnPat)) {
    const fn = m[1];
    const argList = balancedArgs(src, m.index! + m[0].length);
    if (!argList) continue;
    const argv = splitArgs(argList);
    const site = `${rel}:${lineOf(src, m.index!)}`;

    if (fn === "udata_open" || fn === "udata_openChoice") {
      const name = resolveExpr(argv[2] || ""), type = resolveExpr(argv[1] || "");
      if (name && type) keep.file(`${name}.${type}`, site, `udata_open("${name}","${type}")`);
      else keep.file("*", site, `udata_open with unresolved name`);
      continue;
    }
    let keyArg = argv[1] || "";
    if (fn.startsWith("getAll")) keyArg = argv[0];
    lastAnn = null;
    const key = resolveExpr(keyArg);

    for (const cat of cats) {
      if (lastAnn) keep.annotation(cat, lastAnn);
      if (fn.includes("AllChildren") || fn.includes("AllItems")) {
        if (key !== null) keep.prefix(cat, "/" + key, site, `${fn} iterates`);
        else keep.unresolvable(cat, site, keyArg);
      } else if (fn === "ures_open" || fn === "ures_openDirect" || fn === "ures_openU") {
        // bundle open — provenance only
      } else {
        if (key !== null) keep.path(cat, "/" + key, site, fn);
        else keep.unresolvable(cat, site, keyArg);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// LAYER 1 — SITE ANNOTATIONS (lemmas A##)
//
// Each is a human claim that an unresolvable key expression takes the given
// value(s). Audit field says how to verify. SOUNDNESS DEPENDS on these for
// configs B and C; config A ignores them.
// ═══════════════════════════════════════════════════════════════════════════

interface Ann { id: string; file: string; expr?: string; exprPat?: RegExp; resolvesTo: string; audit: string }
const SITE_ANNOTATIONS: Ann[] = [
  { file: "i18n/number_longnames.cpp", exprPat: /^(key|genderKey|caseKey|aliasKey)\.data\(\)$/, resolvesTo: "*",
    audit: "key built from unit category/subtype; bounded by sanctioned units" },
  { file: "i18n/number_longnames.cpp", exprPat: /^(feature|structure)$/, resolvesTo: "grammaticalData/*",
    audit: "grammar keys under /grammaticalData" },
  { file: "common/uloc_keytype.cpp", exprPat: /^(legacy|bcp)KeyId$/, resolvesTo: "*",
    audit: "iterates all keys in keyTypeData.res keyMap/typeMap" },
  { file: "i18n/measunit_extra.cpp", expr: "CATEGORY_TABLE_NAME", resolvesTo: "unitQuantities",
    audit: "constexpr at top of file" },
  { file: "i18n/dtptngen.cpp", exprPat: /^path\.data\(\)$/, resolvesTo: "calendar/*/*",
    audit: "path = calendar/<type>/{availableFormats,appendItems,DateTimePatterns,intervalFormats}" },
  { file: "i18n/smpdtfmt.cpp", exprPat: /^resourcePath\.data\(\)$/, resolvesTo: "calendar/*/DateTimePatterns*",
    audit: "resourcePath = calendar/<type>/DateTimePatterns[%atTime]" },
  { file: "i18n/plurrule.cpp", expr: "typeKey", resolvesTo: "{locales,locales_ordinals}",
    audit: "typeKey = type==ORDINAL ? 'locales_ordinals' : 'locales'" },
  { file: "i18n/plurrule.cpp", expr: "setKey", resolvesTo: "rules/*",
    audit: "setKey = rule-set name from locales table" },
  { file: "i18n/tznames_impl.cpp", expr: "key", resolvesTo: "*",
    audit: "key is mzID or tzID — bounded by zone list" },
  { file: "i18n/calendar.cpp", exprPat: /^region\.data\(\)$/, resolvesTo: "*",
    audit: "region code under calendarPreferenceData" },
  { file: "i18n/dcfmtsym.cpp", exprPat: /nsName|ns\.data/, resolvesTo: "*",
    audit: "numbering system name under NumberElements" },
  { file: "common/ucurr.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "currency code lookups — KEEP all curr_tree" },
  { file: "common/locdspnm.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "display-name lookups by language/region/script code" },
  { file: "common/locresdata.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "generic locale resource lookup" },
  { file: "common/locdispnames.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "display-name lookups — KEEP all lang/region trees" },
  { file: "common/resbund.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "ResourceBundle wrapper — keys come from caller" },
  { file: "i18n/zonemeta.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "tz/mz ID lookups in metaZones/timezoneTypes" },
  { file: "i18n/rbnf.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "RBNF — removed via icupkg filter; reachability moot" },
  { file: "i18n/tmutfmt.cpp", exprPat: /.*/, resolvesTo: "units/duration/*",
    audit: "TimeUnitFormat reads units/duration only" },
  { file: "i18n/currpinf.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "currency plural patterns" },
  { file: "i18n/number_compact.cpp", exprPat: /.*/, resolvesTo: "NumberElements/*/patterns*/*",
    audit: "compact patterns under NumberElements/<ns>/patternsLong|Short" },
  { file: "i18n/reldatefmt.cpp", exprPat: /.*/, resolvesTo: "fields/*",
    audit: "reads /fields/<unit>[-style]" },
  { file: "i18n/timezone.cpp", exprPat: /.*/, resolvesTo: "*",
    audit: "zoneinfo64.res lookups by tz ID" },
  { file: "jsc/runtime/IntlDurationFormat.cpp", exprPat: /numberingSystem/, resolvesTo: "NumberElements/*",
    audit: "reads NumberElements/<ns>/patterns" },
  { file: "i18n/calendar.cpp", expr: "type", resolvesTo: "calendarData/*",
    audit: "type = calendar type ∈ supportedValuesOf('calendar')" },
  { file: "i18n/ucol_res.cpp", expr: "type", resolvesTo: "collations/*",
    audit: "type = collation type from -u-co-" },
  { file: "i18n/listformatter.cpp", expr: "currentStyle", resolvesTo: "listPattern/*",
    audit: "style ∈ {standard, or, unit}[-short|-narrow]" },
  { file: "i18n/dcfmtsym.cpp", expr: "cc", resolvesTo: "Currencies/*",
    audit: "cc = ISO 4217 currency code" },
  { file: "common/brkiter.cpp", expr: "type", resolvesTo: "boundaries/*",
    audit: "type ∈ {grapheme,word,sentence,line,title}" },
  { file: "i18n/numsys.cpp", exprPat: /^(buffer|name)$/, resolvesTo: "*",
    audit: "numbering system name" },
  { file: "common/brkeng.cpp", exprPat: /uscript_getShortName/, resolvesTo: "dictionaries/*",
    audit: "script short name picks dictionary" },
  { file: "i18n/ucal.cpp", exprPat: /prefRegion/, resolvesTo: "calendarPreferenceData/*",
    audit: "region code under calendarPreferenceData" },
].map((a, i) => ({ id: `A${String(i + 1).padStart(2, "0")}`, ...a }));

// ═══════════════════════════════════════════════════════════════════════════
// LAYER 2 — Output-unreachability proofs (lemmas P##)
// ═══════════════════════════════════════════════════════════════════════════

type Tier = 1 | 2;
interface Citation { file: string; pattern: string; expect: "present" | "absent" }
interface Proof {
  id: string;
  tier: Tier;
  drop: { category: string; rules: string[] } | { feature: string; blacklist: string[] };
  claim: string;
  spec?: string;
  citations: Citation[];
  absentSyms?: string[];   // verified against entry-point set
  estKB?: number;
}

function verify(c: Citation): { ok: boolean; detail: string } {
  const root = c.file.startsWith("jsc/") ? JSC_SRC + c.file.slice(3) : ICU_SRC + "/" + c.file;
  if (!existsSync(root)) return { ok: false, detail: `file not found: ${root}` };
  const found = new RegExp(c.pattern, "m").test(readFileSync(root, "utf8"));
  const ok = c.expect === "present" ? found : !found;
  return { ok, detail: `${c.expect} /${c.pattern}/ in ${c.file} — ${found ? "found" : "absent"}` };
}

const PROOFS: Proof[] = [
  {
    id: "P01", tier: 1,
    drop: { feature: "brkitr_rules", blacklist: ["line", "line_cj", "line_loose", "line_loose_cj", "line_normal", "line_normal_cj", "line_normal_phrase_cj", "line_loose_phrase_cj", "line_phrase_cj", "line_strict", "line_strict_cj", "line_strict_phrase_cj", "title"] },
    claim: "Intl.Segmenter granularity ∈ {grapheme, word, sentence}; JSC validates and throws on others. WTF only opens char/word/sentence iterators. No JS-reachable path opens a line or title break iterator.",
    spec: "ECMA-402 §18.1.1 step 9",
    citations: [
      { file: "jsc/runtime/IntlSegmenter.cpp", pattern: 'grapheme.*word.*sentence', expect: "present" },
      { file: "jsc/runtime/IntlSegmenter.cpp", pattern: 'UBRK_LINE|"line"', expect: "absent" },
      { file: "common/brkiter.cpp", pattern: 'UBRK_LINE', expect: "present" },
    ],
    estKB: 330,
  },
  {
    id: "P02", tier: 1,
    drop: { feature: "normalization", blacklist: ["nfkc_cf", "nfkc_scf"] },
    claim: "String.prototype.normalize form ∈ {NFC, NFD, NFKC, NFKD}. nfkc_cf/nfkc_scf are read only by unorm2_getNFKC*CasefoldInstance, neither in entry-point set. URL IDNA uses uts46.nrm.",
    spec: "ECMA-262 §22.1.3.15 step 5",
    citations: [
      { file: "jsc/runtime/StringPrototype.cpp", pattern: 'NFC.*NFD.*NFKC.*NFKD', expect: "present" },
      { file: "common/loadednormalizer2impl.cpp", pattern: '"nfkc_cf"', expect: "present" },
      { file: "common/uts46.cpp", pattern: '"uts46"', expect: "present" },
    ],
    absentSyms: ["unorm2_getNFKCCasefoldInstance"],
    estKB: 104,
  },
  {
    id: "P03", tier: 1,
    drop: { category: "lang_tree", rules: ["-/Keys", "-/Variants", "-/codePatterns", "-/characterLabelPattern", "-/Languages%long", "-/Languages%menu", "-/Languages%variant", "-/Scripts%stand-alone", "-/Scripts%variant"] },
    claim: "Intl.DisplayNames type ∈ {language, region, script, currency, calendar, dateTimeField}. JSC's uldn_* calls read /Languages, /Scripts, /Types/calendar — never /Keys, /Variants, /codePatterns, or %long/%menu/%variant suffixes.",
    spec: "ECMA-402 §12.2.3",
    citations: [
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'language.*region.*script.*currency.*calendar.*dateTimeField', expect: "present" },
      { file: "common/locdspnm.cpp", pattern: '"Keys"', expect: "present" },
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'variant|menu', expect: "absent" },
    ],
    estKB: 80,
  },
  {
    id: "P04", tier: 1,
    drop: { category: "lang_tree", rules: ["-/Types/numbers", "-/Types/collation", "-/Types/hc", "-/Types/lb", "-/Types/ms", "-/Types/cf", "-/Types/d0", "-/Types/kr", "-/Types/m0", "-/Types/s0", "-/Types/t0", "-/Types/x0", "-/Types/dx", "-/Types/em", "-/Types/fw", "-/Types/h0", "-/Types/i0", "-/Types/k0", "-/Types/mu", "-/Types/ss", "-/Types/va", "-/Types/colAlternate", "-/Types/colBackwards", "-/Types/colCaseFirst", "-/Types/colCaseLevel", "-/Types/colNormalization", "-/Types/colNumeric", "-/Types/colReorder", "-/Types/colStrength", "-/Types%short"] },
    claim: "DisplayNames type:'calendar' calls uldn_keyValueDisplayName(ldn,'calendar',v), reading /Types/calendar only. No DisplayNames type maps to any other /Types/* key.",
    spec: "ECMA-402 §12.5.1",
    citations: [
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'uldn_keyValueDisplayName.*calendar', expect: "present" },
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: 'uldn_keyValueDisplayName.*"(nu|co|hc|lb)"', expect: "absent" },
    ],
    estKB: 250,
  },
  {
    id: "P05", tier: 1,
    drop: { category: "locales_tree", rules: ["-/fields/sun", "-/fields/sun-short", "-/fields/sun-narrow", "-/fields/mon", "-/fields/mon-short", "-/fields/mon-narrow", "-/fields/tue", "-/fields/tue-short", "-/fields/tue-narrow", "-/fields/wed", "-/fields/wed-short", "-/fields/wed-narrow", "-/fields/thu", "-/fields/thu-short", "-/fields/thu-narrow", "-/fields/fri", "-/fields/fri-short", "-/fields/fri-narrow", "-/fields/sat", "-/fields/sat-short", "-/fields/sat-narrow"] },
    claim: "Intl.RelativeTimeFormat unit and DisplayNames dateTimeField code lists exclude weekday names. /fields/{mon..sun}* unreachable.",
    spec: "ECMA-402 §17.5.2, §12.5.2",
    citations: [
      { file: "jsc/runtime/IntlRelativeTimeFormat.cpp", pattern: '"second"_s', expect: "present" },
      { file: "jsc/runtime/IntlRelativeTimeFormat.cpp", pattern: '"year"_s', expect: "present" },
      { file: "jsc/runtime/IntlRelativeTimeFormat.cpp", pattern: '"mon"_s|"tue"_s|"wed"_s|UDAT_REL_UNIT_MONDAY', expect: "absent" },
      { file: "jsc/runtime/IntlDisplayNames.cpp", pattern: '"mon"_s|"sunday"_s', expect: "absent" },
    ],
    estKB: 140,
  },
  {
    id: "P06", tier: 1,
    drop: { category: "locales_tree", rules: ["-/personNames", "-/characterLabel", "-/ExemplarCharacters", "-/ExemplarCharactersIndex", "-/ExemplarCharactersNumbers", "-/ExemplarCharactersPunctuation", "-/AuxExemplarCharacters", "-/delimiters", "-/measurementSystemNames", "-/Ellipsis", "-/MoreInformation"] },
    claim: "These keys back ICU APIs ECMA-402 doesn't expose: PersonNameFormatter, ulocdata_getExemplarSet, ulocdata_getDelimiter. None of upersonname_*/ulocdata_* are in the entry-point set.",
    spec: "ECMA-402 has no PersonNames/LocaleData API",
    citations: [
      { file: "i18n/ulocdata.cpp", pattern: '"ExemplarCharacters"', expect: "present" },
    ],
    absentSyms: ["ulocdata_open", "ulocdata_getExemplarSet", "ulocdata_getDelimiter"],
    estKB: 615,
  },
  {
    id: "P07", tier: 2,
    drop: { category: "misc", rules: ["-/territoryInfo", "-/languageData", "-/subdivisionContainment", "-/weekOfPreference", "-/codeMappingsCurrency", "-/personNamesDefaults", "-/idValidity/subdivision", "-/idValidity/unit"] },
    claim: "These supplementalData.res keys are CLDR reference metadata with no ICU runtime reader in common/ or i18n/.",
    citations: [
      { file: "jsc/runtime/IntlObject.cpp", pattern: 'bestAvailableLocale|BestAvailableLocale', expect: "present" },
      { file: "common/locid.cpp", pattern: '"territoryInfo"', expect: "absent" },
      { file: "common/uloc.cpp", pattern: '"territoryInfo"', expect: "absent" },
    ],
    estKB: 130,
  },
  {
    id: "P08", tier: 2,
    drop: { feature: "misc", blacklist: ["genderList", "currencyNumericCodes"] },
    claim: "genderList.res read by ugender_*; currencyNumericCodes.res by ucurr_getNumericCode. Neither symbol in entry-point set.",
    citations: [
      { file: "i18n/gender.cpp", pattern: '"genderList"', expect: "present" },
      { file: "common/ucurr.cpp", pattern: '"currencyNumericCodes"', expect: "present" },
    ],
    absentSyms: ["ugender_getInstance", "ugender_getListGender", "ucurr_getNumericCode"],
    estKB: 4,
  },
  {
    id: "P09", tier: 2,
    drop: { category: "brkitr_tree", rules: ["-/exceptions"] },
    claim: "/exceptions holds sentence-break suppressions used by FilteredBreakIteratorBuilder; ubrk_open(UBRK_SENTENCE) does not read it.",
    citations: [
      { file: "common/filteredbrk.cpp", pattern: '"exceptions"', expect: "present" },
    ],
    absentSyms: ["ubrk_openRules", "ubrk_openBinaryRules"],
    estKB: 11,
  },
  {
    id: "P10", tier: 2,
    drop: { category: "locales_tree", rules: ["-/calendar/*/DateTimeSkeletons"] },
    claim: "DateTimeSkeletons has no reader in ICU's common/ or i18n/ — CLDR added the data but ICU never wired it up.",
    citations: [
      { file: "i18n/dtptngen.cpp", pattern: '"DateTimeSkeletons"', expect: "absent" },
      { file: "i18n/dtptngen.cpp", pattern: 'availableFormats', expect: "present" },
    ],
    estKB: 50,
  },
];

function generateUnitProof(): Proof {
  const intlObj = readFileSync(`${JSC_SRC}/runtime/IntlObject.cpp`, "utf8");
  const m = intlObj.match(/simpleUnits\[\d*\]\s*=\s*\{([\s\S]+?)\n\};/);
  if (!m) die("could not find simpleUnits[] in IntlObject.cpp");
  const all = [...m[1].matchAll(/"([a-z-]+)"_s/g)].map(x => x[1]);
  const sanctioned = new Set(all.filter((_, i) => i % 2 === 1));

  const unitDir = `${ICU_BUILD}/data/out/build/icudt75l/unit`;
  const derb = sh(`${ICU_BUILD}/bin/derb`, ["-s", unitDir, "-c", "root"], { LD_LIBRARY_PATH: `${ICU_BUILD}/lib` });
  const pairs: [string, string][] = [];
  let cat = "";
  for (const line of derb.split("\n")) {
    const cm = line.match(/^ {8}([a-z]+)\{$/);
    if (cm) cat = cm[1];
    const um = line.match(/^ {12}([a-z][a-z0-9-]+)\{$/);
    if (um && cat && cat !== "compound") pairs.push([cat, um[1]]);
  }
  const isReachable = (u: string): boolean => {
    if (sanctioned.has(u)) return true;
    const per = u.split("-per-");
    return per.length === 2 && sanctioned.has(per[0]) && sanctioned.has(per[1]);
  };
  const cats = new Set(pairs.map(p => p[0]));
  const reachableCats = new Set<string>(["compound"]);
  for (const [c, u] of pairs) if (isReachable(u)) reachableCats.add(c);

  const rules: string[] = ["-/durationUnits"];
  for (const c of cats) {
    if (!reachableCats.has(c)) {
      for (const w of ["units", "unitsShort", "unitsNarrow"]) rules.push(`-/${w}/${c}`);
    } else {
      for (const [cc, u] of pairs) if (cc === c && !isReachable(u))
        for (const w of ["units", "unitsShort", "unitsNarrow"]) rules.push(`-/${w}/${c}/${u}`);
    }
  }
  return {
    id: "P11", tier: 1,
    drop: { category: "unit_tree", rules: [...new Set(rules)].sort() },
    claim: `Intl.NumberFormat style:'unit' validates unit against IsWellFormedUnitIdentifier (sanctioned list, ${sanctioned.size} units). Of ${pairs.length} CLDR (category,unit) pairs, ${rules.length - 1} are outside that set.`,
    spec: "ECMA-402 §6.6.1, §6.6.2",
    citations: [
      { file: "jsc/runtime/IntlObject.cpp", pattern: "simpleUnits\\[", expect: "present" },
      { file: "jsc/runtime/IntlNumberFormat.cpp", pattern: "sanctionedSimpleUnitIdentifier", expect: "present" },
    ],
    estKB: 1850,
  };
}

// ═══════════════════════════════════════════════════════════════════════════
// PHASE 4 — Enumerate top-level paths in each tree
// ═══════════════════════════════════════════════════════════════════════════

function enumerateTopLevel(): Map<string, Set<string>> {
  const all = new Map<string, Set<string>>();
  const derb = (dir: string, name: string) =>
    sh(`${ICU_BUILD}/bin/derb`, ["-s", dir, "-c", name], { LD_LIBRARY_PATH: `${ICU_BUILD}/lib` });
  const trees: Record<string, string> = {
    locales_tree: "", curr_tree: "curr", lang_tree: "lang", region_tree: "region",
    unit_tree: "unit", zone_tree: "zone", coll_tree: "coll", brkitr_tree: "brkitr",
  };
  for (const [cat, sub] of Object.entries(trees)) {
    const dir = `${ICU_BUILD}/data/out/build/icudt75l${sub ? "/" + sub : ""}`;
    if (!existsSync(dir)) continue;
    const tops = new Set<string>();
    for (const name of ["root", "en", "de", "ja", "zh", "ar"]) {
      if (!existsSync(`${dir}/${name}.res`)) continue;
      const stack: string[] = [];
      for (const line of derb(dir, name).split("\n")) {
        const open = line.match(/^(\s*)([\w%\-]+)\{$/);
        if (open) { stack.push(open[2]); if (stack.length === 2) tops.add(open[2]); continue; }
        const inline = line.match(/^(\s*)([\w%\-]+)(?::\w+)?\{.*\}$/);
        if (inline && stack.length === 1) tops.add(inline[2]);
        if (/^\s*\}$/.test(line)) stack.pop();
      }
    }
    tops.delete("Version"); tops.delete("%%ALIAS"); tops.delete("%%Parent");
    all.set(cat, tops);
  }
  return all;
}

// ═══════════════════════════════════════════════════════════════════════════
// MAIN — compute three configs, emit filters + report
// ═══════════════════════════════════════════════════════════════════════════

interface Drop { rule: string; lemmas: string[] }
interface Config {
  name: string; desc: string;
  resourceFilters: Map<string, Drop[]>;
  featureFilters: Map<string, { blacklist: string[]; lemma: string }>;
  lemmaSet: Set<string>;
  estKB: number;
}

function computeLayer1Drops(keep: Keep, allTops: Map<string, Set<string>>): Map<string, string[]> {
  const out = new Map<string, string[]>();
  for (const [cat, tops] of allTops) {
    if (keep.keepAllTrees.has(cat)) continue;
    const drops: string[] = [];
    for (const t of tops) if (!keep.coversTop(cat, t)) drops.push(`-/${t}`);
    if (drops.length) out.set(cat, drops.sort());
  }
  return out;
}

function main() {
  const rpt: string[] = [
    "# ICU data reachability — layered proof",
    "",
    "Three filter configs at increasing aggressiveness, each tagged with the",
    "exact set of human claims (lemmas) its soundness depends on.",
    "",
  ];

  // ── Layer 1 ────────────────────────────────────────────────────────────
  const p1 = phase1();
  rpt.push(`## Layer 1 — read-reachability closure`, "",
    `- Entry symbols (JSC+WTF → ICU): **${p1.entrySymbols.size}**`,
    `- ICU source files with linked code: **${p1.reachableSrcFiles.size}**`, "");

  const keepA = phase2(p1.reachableSrcFiles, /*useAnnotations*/ false);
  const keepB = phase2(p1.reachableSrcFiles, /*useAnnotations*/ true);
  const allTops = enumerateTopLevel();
  const dropsA = computeLayer1Drops(keepA, allTops);
  const dropsB = computeLayer1Drops(keepB, allTops);

  rpt.push("| | annotations OFF (A) | annotations ON (B) |",
    "|---|---|---|",
    `| unresolved sites | ${keepA.unresolved.length} | ${keepB.unresolved.length} |`,
    `| KEEP-ALL trees | ${[...keepA.keepAllTrees].sort().join(", ") || "—"} | ${[...keepB.keepAllTrees].sort().join(", ") || "—"} |`,
    `| trees with drops | ${dropsA.size} | ${dropsB.size} |`,
    "");

  // ── Layer 2 ────────────────────────────────────────────────────────────
  const allProofs = [...PROOFS, generateUnitProof()];
  const accepted: Proof[] = [], rejected: { p: Proof; why: string[] }[] = [];
  rpt.push(`## Layer 2 — output-unreachability proofs (${allProofs.length})`, "");
  for (const p of allProofs) {
    const fails: string[] = [];
    for (const c of p.citations) { const v = verify(c); if (!v.ok) fails.push(v.detail); }
    for (const s of p.absentSyms ?? []) if (p1.entrySymbols.has(s)) fails.push(`entry symbol PRESENT: ${s}`);
    if (fails.length === 0) accepted.push(p);
    else rejected.push({ p, why: fails });
  }
  rpt.push(`- Accepted: **${accepted.length}** / ${allProofs.length}`,
    rejected.length ? `- Rejected: ${rejected.map(r => r.p.id).join(", ")}` : "- Rejected: —", "");

  // ── Build configs ──────────────────────────────────────────────────────
  const cfgA: Config = { name: "A", desc: "Layer 1, annotations OFF — zero human claims",
    resourceFilters: new Map(), featureFilters: new Map(), lemmaSet: new Set(), estKB: 0 };
  for (const [cat, rules] of dropsA)
    cfgA.resourceFilters.set(cat, rules.map(r => ({ rule: r, lemmas: [] })));

  const cfgB: Config = { name: "B", desc: "Layer 1, annotations ON",
    resourceFilters: new Map(), featureFilters: new Map(), lemmaSet: new Set(), estKB: 0 };
  for (const [cat, rules] of dropsB) {
    const dropsInA = new Set(dropsA.get(cat) ?? []);
    const treeLemmas = [...(keepB.annUsed.get(cat) ?? [])].sort();
    cfgB.resourceFilters.set(cat, rules.map(r => ({
      rule: r, lemmas: dropsInA.has(r) ? [] : treeLemmas,
    })));
    for (const l of treeLemmas) cfgB.lemmaSet.add(l);
  }

  const cfgC: Config = { name: "C", desc: "Layer 1 (annotations ON) ∪ Layer 2 proofs",
    resourceFilters: new Map(), featureFilters: new Map(), lemmaSet: new Set(cfgB.lemmaSet), estKB: 0 };
  // Start from B, then add Layer-2 drops.
  for (const [cat, drops] of cfgB.resourceFilters) cfgC.resourceFilters.set(cat, drops.map(d => ({ ...d })));
  for (const p of accepted) {
    cfgC.lemmaSet.add(p.id);
    cfgC.estKB += p.estKB ?? 0;
    if ("feature" in p.drop) {
      cfgC.featureFilters.set(p.drop.feature, { blacklist: p.drop.blacklist, lemma: p.id });
    } else {
      const cat = p.drop.category;
      const list = cfgC.resourceFilters.get(cat) ?? [];
      const have = new Set(list.map(d => d.rule));
      for (const r of p.drop.rules) {
        if (have.has(r)) { list.find(d => d.rule === r)!.lemmas.push(p.id); }
        else list.push({ rule: r, lemmas: [p.id] });
      }
      cfgC.resourceFilters.set(cat, list);
    }
  }

  // ── Emit filters ───────────────────────────────────────────────────────
  const writeFilter = (cfg: Config) => {
    const filter: any = { strategy: "subtractive", featureFilters: {}, resourceFilters: [] };
    for (const [feat, { blacklist }] of cfg.featureFilters) filter.featureFilters[feat] = { blacklist };
    for (const [cat, drops] of cfg.resourceFilters)
      filter.resourceFilters.push({ categories: [cat], rules: drops.map(d => d.rule).sort() });
    writeFileSync(`${OUT_DIR}/config-${cfg.name}.json`, JSON.stringify(filter, null, 2));
    return filter;
  };
  const fA = writeFilter(cfgA), fB = writeFilter(cfgB), fC = writeFilter(cfgC);
  const ruleCount = (f: any) =>
    f.resourceFilters.reduce((s: number, r: any) => s + r.rules.length, 0) +
    Object.values<any>(f.featureFilters).reduce((s, ff) => s + ff.blacklist.length, 0);

  // ── Report: per-config sections with lemma dependencies ────────────────
  rpt.push("## Soundness budget", "",
    "| Config | Human claims | Rules | est. KB (L2 only) |",
    "|---|---|---|---|",
    `| A | **0** | ${ruleCount(fA)} | — |`,
    `| B | **${cfgB.lemmaSet.size}** annotations | ${ruleCount(fB)} | — |`,
    `| C | **${cfgB.lemmaSet.size}** annotations + **${accepted.length}** proofs = **${cfgC.lemmaSet.size}** | ${ruleCount(fC)} | ~${cfgC.estKB} |`,
    "");

  for (const cfg of [cfgA, cfgB, cfgC]) {
    rpt.push(`## Config ${cfg.name} — ${cfg.desc}`, "");
    rpt.push(`**Lemmas this config depends on:** ${cfg.lemmaSet.size === 0 ? "none" : [...cfg.lemmaSet].sort().join(", ")}`, "");
    for (const [feat, { blacklist, lemma }] of cfg.featureFilters) {
      rpt.push(`### feature \`${feat}\``, "");
      for (const b of blacklist) rpt.push(`- \`${b}\` ← ${lemma}`);
      rpt.push("");
    }
    for (const [cat, drops] of cfg.resourceFilters) {
      rpt.push(`### \`${cat}\` (${drops.length} rules)`, "");
      for (const d of drops.sort((a, b) => a.rule.localeCompare(b.rule)))
        rpt.push(`- \`${d.rule}\` ← ${d.lemmas.length ? d.lemmas.join(", ") : "(mechanical)"}`);
      rpt.push("");
    }
  }

  // ── Report: lemma catalog ──────────────────────────────────────────────
  rpt.push("## Lemma catalog", "", "### Layer-1 annotations", "");
  for (const a of SITE_ANNOTATIONS) {
    const used = [...keepB.annUsed].filter(([, s]) => s.has(a.id)).map(([t]) => t);
    rpt.push(`- **${a.id}** \`${a.file}\` ${a.expr ?? a.exprPat} → \`${a.resolvesTo}\` — ${a.audit}` +
      (used.length ? ` _(applied in: ${used.join(", ")})_` : " _(unused)_"));
  }
  rpt.push("", "### Layer-2 proofs", "");
  for (const p of allProofs) {
    const ok = accepted.includes(p);
    const dropDesc = "category" in p.drop ? `${p.drop.category}: ${p.drop.rules.length} rules` : `feature ${p.drop.feature}`;
    rpt.push(`#### ${p.id} (Tier ${p.tier}) — ${dropDesc} ${ok ? "✓" : "✗ REJECTED"}`, "",
      `**Claim:** ${p.claim}`, p.spec ? `**Spec:** ${p.spec}` : "", "");
    for (const c of p.citations) { const v = verify(c); rpt.push(`- ${v.ok ? "✓" : "✗"} ${v.detail}`); }
    for (const s of p.absentSyms ?? []) rpt.push(`- ${p1.entrySymbols.has(s) ? "✗" : "✓"} entry symbol absent: \`${s}\``);
    if (!ok) rpt.push("", `→ **REJECTED** — excluded from config C.`);
    rpt.push("");
  }
  if (keepB.unresolved.length) {
    rpt.push("## Residual unresolved sites (config B)", "",
      "Each forces KEEP-ALL on its tree. Adding an annotation would tighten config B/C.", "");
    for (const u of keepB.unresolved) rpt.push(`- \`${u.tree}\` ← \`${u.site}\`: \`${u.expr}\``);
    rpt.push("");
  }

  writeFileSync(`${OUT_DIR}/report.md`, rpt.join("\n"));
  writeFileSync(`${OUT_DIR}/keep-B.json`, JSON.stringify({
    keepAllTrees: [...keepB.keepAllTrees],
    paths: Object.fromEntries([...keepB.paths].map(([k, v]) => [k, [...v].sort()])),
    prefixes: Object.fromEntries([...keepB.prefixes].map(([k, v]) => [k, [...v].sort()])),
    annUsed: Object.fromEntries([...keepB.annUsed].map(([k, v]) => [k, [...v].sort()])),
  }, null, 2));

  console.error(`\nentry symbols:       ${p1.entrySymbols.size}`);
  console.error(`reachable src files: ${p1.reachableSrcFiles.size}`);
  console.error(`L2 proofs accepted:  ${accepted.length}/${allProofs.length}` +
    (rejected.length ? `  (rejected: ${rejected.map(r => r.p.id).join(", ")})` : ""));
  console.error(`\nconfig A: ${ruleCount(fA)} rules, 0 lemmas`);
  console.error(`config B: ${ruleCount(fB)} rules, ${cfgB.lemmaSet.size} lemmas`);
  console.error(`config C: ${ruleCount(fC)} rules, ${cfgC.lemmaSet.size} lemmas, ~${cfgC.estKB} KB (L2 est)`);
  console.error(`\n→ ${OUT_DIR}/{config-A,config-B,config-C}.json, report.md, keep-B.json`);
  if (rejected.length) {
    console.error(`\n⚠ ${rejected.length} proof(s) REJECTED:`);
    for (const r of rejected) console.error(`  ${r.p.id}: ${r.why[0]}`);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// util
// ═══════════════════════════════════════════════════════════════════════════
function sh(cmd: string, argv: string[], env?: Record<string, string>): string {
  const r = spawnSync(cmd, argv, { maxBuffer: 256 << 20, env: { ...process.env, ...env } });
  if (r.error) die(`${cmd}: ${r.error}`);
  return r.stdout.toString();
}
function die(msg: string): never { console.error(`[icu-proof] ${msg}`); process.exit(1); }
function lineOf(s: string, i: number): number { return s.slice(0, i).split("\n").length; }
function balancedArgs(s: string, start: number): string | null {
  let d = 1, i = start;
  while (i < s.length && d > 0) { if (s[i] === "(") d++; else if (s[i] === ")") d--; i++; }
  return d === 0 ? s.slice(start, i - 1) : null;
}
function splitArgs(s: string): string[] {
  const out: string[] = []; let d = 0, cur = "";
  for (const c of s) {
    if (c === "(" || c === "{" || c === "[") d++;
    else if (c === ")" || c === "}" || c === "]") d--;
    if (c === "," && d === 0) { out.push(cur); cur = ""; } else cur += c;
  }
  out.push(cur);
  return out.map(x => x.trim());
}
function grepFiles(dir: string, pat: RegExp): string[] {
  const out: string[] = [];
  const walk = (d: string) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (/\.(cpp|c|h)$/.test(e.name) && pat.test(readFileSync(p, "utf8"))) out.push(p);
    }
  };
  walk(dir);
  return out;
}

main();
