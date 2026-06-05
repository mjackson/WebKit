#!/usr/bin/env bash
# run-tests.sh — SPEC-api §8 runner for the shared-memory threads corpus.
#
# Runs, with their //@ header directives honored:
#   JSTests/threads/{api,atomics,races}/*.js          (api-owned, SPEC-api §8)
# plus, when present (not owned here):
#   JSTests/threads/heap-*.js                          (heap)
#   JSTests/threads/objectmodel/*.js                   (objectmodel)
#   JSTests/threads/vmstate/*.js                       (vmstate N6)
#   JSTests/threads/jit/**/*.js                        (jit)
#
# Header directives understood (subset of run-jsc-stress-tests):
#   //@ skip                       skip the file
#   //@ requireOptions("a", ...)   append args to every run of the file
#   //@ runDefault("a", ...)       one run per directive with exactly those
#                                  args (ta-path-unchanged.js runs both ways)
# blocking-gate.js additionally gets --can-block-is-false appended (annex
# T2: "runner appends it"; the threads.yaml stanza does the same at INT).
#
# Runs whose option set is rejected by the jsc build (unknown option — e.g.
# a foreign workstream's corpus naming options whose OptionsList.h hunk has
# not landed yet) are SKIPped, not FAILed: the option set is probed against
# an empty program first. The API-I1..I24 coverage grep ignores //@ skip'ped
# files (API-I14's SPEC-mandated deferral to INT 9.2-6 is reported
# explicitly).
#
# Usage:
#   Tools/threads/run-tests.sh [--filter=SUBSTR] [--amplify] [--list]
#   Tools/threads/run-tests.sh --gates
#
# Options:
#   --filter=SUBSTR   Only run tests whose repo-relative path contains SUBSTR.
#   --amplify         Wrap every run in Tools/threads/amplify.sh (race
#                     amplification); if amplify.sh is missing, warn once and
#                     run plain.
#   --list            Print the resolved test list and exit.
#   --gates           SPEC-api §10 task-13 gate mode (sole option; degrades
#                     gracefully per G15). Runs, in order:
#                       1. races/ via this runner, --amplify (runner falls
#                          back to plain with a warning if amplify.sh is
#                          absent — frozen §8 fallback);
#                       2. races/ under the TSAN no-JIT target
#                          (WebKitBuild/TSan/bin/jsc, override TSAN_JSC=) —
#                          only if that target exists, else SKIP;
#                       3. bench self-gate (API-I19, implement half):
#                          bench-gate.sh --record then gate on the SAME build
#                          must exit 0; uses a throwaway baseline file so the
#                          integrator-recorded Tools/threads/baseline.json is
#                          never touched (the authoritative I19 comparison
#                          against the pre-workstream baseline runs at INT).
#                     Exit 0 iff every gate that ran passed (SKIPs are not
#                     failures).
#
# Environment:
#   JSC=/path/to/jsc  jsc binary (default: WebKitBuild/{Debug,Release}/bin/jsc).
#   AMPLIFY_RUNS=M    Forwarded to amplify.sh --runs (default: amplify's own).
#   TSAN_JSC=PATH     TSAN no-JIT jsc for gate 2 (default:
#                     WebKitBuild/TSan/bin/jsc; missing => gate SKIPped).
#   TSAN_OPTIONS=...  Honored if set; otherwise gate 2 uses the TSAN.md
#                     defaults (suppressions file, halt_on_error=1,
#                     exitcode=66, history_size=7, second_deadlock_stack=1).
#   BENCH_GATE_RUNS=K Runs per benchmark for gate 3 (default: bench-gate's 9).
#
# Exit codes: 0 all pass (and API-I1..I24 coverage grep is complete),
#             1 failures or missing invariant coverage, 2 usage/env error.
#             In --gates mode: 0 all executed gates pass, 1 a gate failed,
#             2 usage/env error.

set -u -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JT="$ROOT/JSTests/threads"
AMPLIFY_SH="$ROOT/Tools/threads/amplify.sh"

FILTER=""
AMPLIFY=0
LIST=0
GATES=0

die() { echo "run-tests: error: $*" >&2; exit 2; }

print_help() {
    # Print the header comment block (everything between the shebang and the
    # first non-comment line), stripped of the leading "# ".
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' \
        "${BASH_SOURCE[0]}"
}

for arg in "$@"; do
    case "$arg" in
        --filter=*) FILTER="${arg#--filter=}" ;;
        --amplify)  AMPLIFY=1 ;;
        --list)     LIST=1 ;;
        --gates)    GATES=1 ;;
        -h|--help)  print_help; exit 0 ;;
        *)          die "unknown argument: $arg" ;;
    esac
done

if [[ "$GATES" -eq 1 && ( -n "$FILTER" || "$AMPLIFY" -eq 1 || "$LIST" -eq 1 ) ]]; then
    die "--gates must be the sole option"
fi

# ---- resolve jsc ----
JSC="${JSC:-}"
if [[ -z "$JSC" ]]; then
    for candidate in "$ROOT/WebKitBuild/Debug/bin/jsc" "$ROOT/WebKitBuild/Release/bin/jsc"; do
        [[ -x "$candidate" ]] && JSC="$candidate" && break
    done
fi
if [[ "$LIST" -eq 0 ]]; then
    [[ -n "$JSC" && -x "$JSC" ]] || die "no jsc binary (set JSC=/path/to/jsc)"
fi

# ============================ --gates mode ============================
# SPEC-api §10 task 13: degrade gracefully (G15). Each gate that can run is
# a hard gate; substrate that is absent (TSAN target) is SKIPped, never
# failed. Semantic questions are settled by SPEC-api §4/§6, not here.
if [[ "$GATES" -eq 1 ]]; then
    SELF="${BASH_SOURCE[0]}"
    GATE_FAILURES=()
    GATE_SKIPS=()

    gate_banner() {
        echo
        echo "==== gate: $* ===="
    }

    # ---- gate 1: race corpus via this runner, amplified (annex §T:
    # "races/ (GI; amplifier+TSAN when present, G15)"). --amplify itself
    # degrades to plain runs with one warning when amplify.sh is absent
    # (frozen §8 fallback), so this gate always executes. ----
    gate_banner "races (run-tests.sh --amplify; jsc = $JSC)"
    if JSC="$JSC" bash "$SELF" --filter=/races/ --amplify; then
        echo "gate[races]: PASS"
    else
        echo "gate[races]: FAIL"
        GATE_FAILURES+=("races")
    fi

    # ---- gate 2: race corpus under the TSAN no-JIT target, only if one
    # exists in this tree (docs/threads/TSAN.md; built by `bash tsan.sh`
    # into WebKitBuild/TSan/bin/jsc). Run plain, not amplified: TSAN's
    # 5-15x slowdown makes amplified campaigns impractical, and TSAN itself
    # is the race oracle here. exitcode=66 in a FAIL line distinguishes a
    # TSAN report from an ordinary test failure. ----
    TSAN_JSC="${TSAN_JSC:-$ROOT/WebKitBuild/TSan/bin/jsc}"
    if [[ -x "$TSAN_JSC" ]]; then
        TSAN_SUPP="$ROOT/Tools/tsan/suppressions.txt"
        TSAN_DEFAULT_OPTS="suppressions=$TSAN_SUPP history_size=7 second_deadlock_stack=1 halt_on_error=1 exitcode=66"
        gate_banner "tsan no-JIT races (jsc = $TSAN_JSC)"
        if TSAN_OPTIONS="${TSAN_OPTIONS:-$TSAN_DEFAULT_OPTS}" JSC="$TSAN_JSC" \
            bash "$SELF" --filter=/races/; then
            echo "gate[tsan]: PASS"
        else
            echo "gate[tsan]: FAIL (exit 66 above = TSAN race report;" \
                 "see docs/threads/TSAN.md, including the CLoop" \
                 "shared-stack known limitation for intermittent" \
                 "CLoop::execute SEGVs under the phase-1 GIL stub)"
            GATE_FAILURES+=("tsan")
        fi
    else
        echo
        echo "gate[tsan]: SKIP — no TSAN no-JIT target at $TSAN_JSC" \
             "(build with: bash tsan.sh; docs/threads/TSAN.md)"
        GATE_SKIPS+=("tsan")
    fi

    # ---- gate 3: bench self-gate (API-I19, implement half). The
    # authoritative I19 comparison uses the integrator-recorded
    # pre-workstream baseline at INT (self-recorded = vacuous, SPEC-api §6
    # I19); the implement-phase bar is mechanical: --record then gate on
    # the SAME build must exit 0. A throwaway baseline file keeps the
    # integrator's Tools/threads/baseline.json untouched. ----
    BENCH_GATE="$ROOT/Tools/threads/bench-gate.sh"
    if [[ -x "$BENCH_GATE" && -d "$ROOT/JSTests/threads/bench" ]]; then
        gate_banner "bench self-gate (bench-gate.sh --record + gate, same build)"
        TMP_BASELINE="$(mktemp "${TMPDIR:-/tmp}/threads-bench-baseline.XXXXXX")" \
            || die "mktemp failed"
        BENCH_RUNS="${BENCH_GATE_RUNS:-9}"
        if bash "$BENCH_GATE" --record --baseline "$TMP_BASELINE" \
                --runs "$BENCH_RUNS" "$JSC" \
           && bash "$BENCH_GATE" --baseline "$TMP_BASELINE" \
                --runs "$BENCH_RUNS" "$JSC"; then
            echo "gate[bench]: PASS"
        else
            echo "gate[bench]: FAIL (record+gate on the same build must" \
                 "exit 0; I19)"
            GATE_FAILURES+=("bench")
        fi
        rm -f "$TMP_BASELINE"
    else
        echo
        echo "gate[bench]: SKIP — bench substrate missing" \
             "($BENCH_GATE / JSTests/threads/bench)"
        GATE_SKIPS+=("bench")
    fi

    # ---- summary ----
    echo
    if [[ ${#GATE_SKIPS[@]} -gt 0 ]]; then
        echo "run-tests: gates skipped (substrate absent, G15): ${GATE_SKIPS[*]}"
    fi
    if [[ ${#GATE_FAILURES[@]} -gt 0 ]]; then
        echo "run-tests: GATES FAIL: ${GATE_FAILURES[*]}"
        exit 1
    fi
    echo "run-tests: GATES PASS (all executed gates green)"
    exit 0
fi
# ========================== end --gates mode ==========================

# ---- collect the corpus (SPEC-api §8 globs; foreign dirs only when present) ----
FILES=()
for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js \
         "$JT"/heap-*.js "$JT"/objectmodel/*.js "$JT"/vmstate/*.js; do
    [[ -e "$f" ]] && FILES+=("$f")
done
if [[ -d "$JT/jit" ]]; then
    while IFS= read -r f; do
        FILES+=("$f")
    done < <(find "$JT/jit" -name '*.js' -type f 2>/dev/null | sort)
fi
[[ ${#FILES[@]} -gt 0 ]] || die "no tests found under $JT"

# ---- header parsing ----
# Echoes one line per run: the jsc args for that run, space-separated
# (threads options never contain spaces). Echoes nothing for a plain run
# marker "<plain>", and "<skip>" for skipped files.
plan_runs() {
    local file="$1"
    local req_opts=()
    local rundefault_lines=()
    local line
    while IFS= read -r line; do
        [[ "$line" == "//@"* ]] || break
        case "$line" in
            "//@ skip"*)
                echo "<skip>"
                return
                ;;
            "//@ requireOptions("*)
                while IFS= read -r opt; do
                    req_opts+=("$opt")
                done < <(grep -o '"[^"]*"' <<<"$line" | tr -d '"')
                ;;
            "//@ runDefault"*)
                local opts
                opts="$(grep -o '"[^"]*"' <<<"$line" | tr -d '"' | tr '\n' ' ')"
                rundefault_lines+=("${opts% }")
                ;;
            *)
                : # other directives (e.g. comments) ignored
                ;;
        esac
    done < "$file"

    local req="${req_opts[*]:-}"
    if [[ ${#rundefault_lines[@]} -eq 0 ]]; then
        if [[ -n "$req" ]]; then echo "$req"; else echo "<plain>"; fi
        return
    fi
    local rd
    for rd in "${rundefault_lines[@]}"; do
        local combined="$req${req:+ }$rd"
        combined="${combined# }"
        if [[ -n "$combined" ]]; then echo "$combined"; else echo "<plain>"; fi
    done
}

WARNED_NO_AMPLIFY=0
PASS=0
FAIL=0
SKIPPED=0
FAILED_TESTS=()
TMP_OUT="$(mktemp "${TMPDIR:-/tmp}/threads-run-tests.XXXXXX")" || die "mktemp failed"
trap 'rm -f "$TMP_OUT"' EXIT

# Option-availability probe: jsc exits nonzero on an unknown option, so a
# requireOptions header naming an option this build does not have would turn
# every run of that file into a FAIL that is really a missing cross-workstream
# OptionsList.h hunk (e.g. threads/vmstate files needing --useVMLite /
# --useStructureAllocationLock before vmstate M_opts lands). Probe the run's
# option set against an empty program first and SKIP (not FAIL) when the
# build rejects it. Cached per option set (plain indexed arrays: macOS bash
# 3.2 has no associative arrays).
PROBED_OPTSETS=()
PROBED_RESULTS=()
options_supported() { # $1 = space-joined args ("" => trivially supported)
    local key="$1"
    [[ -z "$key" ]] && return 0
    local i
    for i in "${!PROBED_OPTSETS[@]}"; do
        if [[ "${PROBED_OPTSETS[$i]}" == "$key" ]]; then
            [[ "${PROBED_RESULTS[$i]}" == "yes" ]]
            return
        fi
    done
    local probe_args=()
    read -r -a probe_args <<<"$key"
    local verdict=no
    if "$JSC" "${probe_args[@]}" -e '' >/dev/null 2>&1; then
        verdict=yes
    fi
    PROBED_OPTSETS+=("$key")
    PROBED_RESULTS+=("$verdict")
    [[ "$verdict" == "yes" ]]
}

# Per-test timeout: threaded tests hang by failure mode (lost wakeup, GIL
# hand-off livelock, deadlocked safepoint), so an unbounded run blocks the
# whole harness. timeout(1) kills the process group; a timeout is a FAIL.
# Override with THREADS_TEST_TIMEOUT_SECS (0 disables).
TEST_TIMEOUT_SECS="${THREADS_TEST_TIMEOUT_SECS:-120}"
TIMEOUT_WRAP=()
if [[ "$TEST_TIMEOUT_SECS" != "0" ]] && command -v timeout >/dev/null 2>&1; then
    TIMEOUT_WRAP=(timeout -k 10 "$TEST_TIMEOUT_SECS")
fi

run_one() { # file, args...
    local file="$1"; shift
    if [[ "$AMPLIFY" -eq 1 && -x "$AMPLIFY_SH" ]]; then
        local aargs=()
        [[ -n "${AMPLIFY_RUNS:-}" ]] && aargs+=(--runs "$AMPLIFY_RUNS")
        local opt
        for opt in "$@"; do
            aargs+=(--jsc-arg "$opt")
        done
        "$AMPLIFY_SH" ${aargs[@]+"${aargs[@]}"} "$JSC" "$file" >"$TMP_OUT" 2>&1
        return $?
    fi
    if [[ "$AMPLIFY" -eq 1 && "$WARNED_NO_AMPLIFY" -eq 0 ]]; then
        echo "run-tests: warning: $AMPLIFY_SH not found/executable; running plain" >&2
        WARNED_NO_AMPLIFY=1
    fi
    ${TIMEOUT_WRAP[@]+"${TIMEOUT_WRAP[@]}"} "$JSC" "$@" "$file" >"$TMP_OUT" 2>&1
    local status=$?
    if [[ ${#TIMEOUT_WRAP[@]} -gt 0 && ( $status -eq 124 || $status -eq 137 ) ]]; then
        echo "run-tests: TIMEOUT after ${TEST_TIMEOUT_SECS}s (hang): $file" >>"$TMP_OUT"
    fi
    return $status
}

for file in "${FILES[@]}"; do
    rel="${file#$ROOT/}"
    if [[ -n "$FILTER" && "$rel" != *"$FILTER"* ]]; then
        continue
    fi
    if [[ "$LIST" -eq 1 ]]; then
        echo "$rel"
        continue
    fi

    runindex=0
    while IFS= read -r runspec; do
        if [[ "$runspec" == "<skip>" ]]; then
            SKIPPED=$((SKIPPED + 1))
            echo "SKIP $rel"
            continue
        fi
        args=()
        if [[ "$runspec" != "<plain>" ]]; then
            read -r -a args <<<"$runspec"
        fi
        # annex T2 / I18: the runner appends --can-block-is-false for the
        # blocking-gate test (G34).
        if [[ "$(basename "$file")" == "blocking-gate.js" ]]; then
            args+=("--can-block-is-false")
        fi
        runindex=$((runindex + 1))
        label="$rel"
        [[ "$runindex" -gt 1 || "$runspec" != "<plain>" ]] && label="$rel [${args[*]:-default}]"
        if ! options_supported "${args[*]:-}"; then
            SKIPPED=$((SKIPPED + 1))
            echo "SKIP $label (option(s) not supported by this jsc build — likely a not-yet-landed OptionsList.h hunk from another workstream, not an api failure)"
            continue
        fi
        if run_one "$file" ${args[@]+"${args[@]}"}; then
            PASS=$((PASS + 1))
            echo "PASS $label"
        else
            status=$?
            FAIL=$((FAIL + 1))
            FAILED_TESTS+=("$label")
            echo "FAIL $label (exit $status)"
            sed 's/^/     | /' "$TMP_OUT"
        fi
    done < <(plan_runs "$file")
done

if [[ "$LIST" -eq 1 ]]; then
    exit 0
fi

# ---- invariant coverage grep (SPEC-api §6: every API-I1..I24 cited by >=1
# §8 test; CI greps this) ----
# A //@ skip'ped file never executes, so its citations are NOT coverage:
# count only non-skipped files. API-I14 is special-cased — its sole citation
# (api/thread-restrict.js) is SPEC-mandatedly skipped until the integrator
# applies the 9.2-6 hooks ("INT gate via 9.2-6; //@ skipped until then"), so
# the deferral is reported visibly on every run instead of counting as
# silently-green coverage.
COVERAGE_FILES=()
SKIPPED_COVERAGE_FILES=()
for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js; do
    [[ -e "$f" ]] || continue
    if grep -qs '^//@ skip' "$f"; then
        SKIPPED_COVERAGE_FILES+=("$f")
    else
        COVERAGE_FILES+=("$f")
    fi
done
MISSING=()
for i in $(seq 1 24); do
    if grep -qsE "API-I$i([^0-9]|\$)" ${COVERAGE_FILES[@]+"${COVERAGE_FILES[@]}"} /dev/null; then
        continue
    fi
    if [[ ${#SKIPPED_COVERAGE_FILES[@]} -gt 0 ]] \
        && grep -qsE "API-I$i([^0-9]|\$)" "${SKIPPED_COVERAGE_FILES[@]}" /dev/null; then
        if [[ "$i" -eq 14 ]]; then
            echo "run-tests: note: API-I14 coverage deferred to INT 9.2-6 (sole citation, api/thread-restrict.js, is //@ skip'ped per SPEC-api I14)"
            continue
        fi
        MISSING+=("API-I$i(only-in-skipped-files)")
    else
        MISSING+=("API-I$i")
    fi
done

echo
echo "run-tests: $PASS passed, $FAIL failed, $SKIPPED skipped"
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "run-tests: COVERAGE FAIL — uncited invariants: ${MISSING[*]}"
fi
if [[ "$FAIL" -gt 0 ]]; then
    echo "run-tests: failed tests:"
    printf '  %s\n' "${FAILED_TESTS[@]}"
fi

[[ "$FAIL" -eq 0 && ${#MISSING[@]} -eq 0 ]] || exit 1
exit 0
