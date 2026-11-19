#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Upstream's C++ unit tests (the Makefile's test-* recipes), built by mayhem/build.sh into
# mayhem/out/tests/ with normal flags. Each binary asserts its results (assert()/explicit checks,
# non-zero exit on failure); the ones that print a PASS line must also print it.
T="$SRC/mayhem/out/tests"
[ -d "$T" ] || { echo "test.sh: $T missing — run mayhem/build.sh first" >&2; emit_ctrf "opensprinkler-unit" 0 1; exit 1; }

# name | needs a data dir (1/0) | required stdout line ('' = exit status only)
TESTS=(
  "sprinkler_log|1|sprinkler log tests passed"
  "storage_files|1|"
  "station_extra|1|Station extra storage PASS"
  "buffer_filler|1|"
  "bundle_codec|0|"
  "cycle_soak|0|Cycle/soak policy PASS"
  "cycle_scheduler|0|Production cycle scheduler PASS"
  "runtime_queue|0|Runtime queue PASS"
  "queue_capacity|0|"
  "output_sequencer|0|"
  "weather_failsafe|0|"
  "weather_sensor_cache|0|"
  "string_buffer|0|"
  "board_profiles|0|"
  "hardware_detection|0|"
  "flow_rate_window|0|"
  "sensor_units|0|"
  "system_internal_sensor|0|"
  "onboard_analog|1|"
  "w5500_frame|0|"
  "ch390_health|0|CH390 HEALTH TEST PASS"
  "ch390_raw|0|CH390 RAW DRIVER TEST PASS"
  "ethernet_lifecycle|0|ETHERNET LIFECYCLE TEST PASS"
  "hostname|0|"
  "update_http_reader|0|Updater reader: deadlines, trickles, limits, EOF, exact length and rollover PASS"
)

passed=0; failed=0
for entry in "${TESTS[@]}"; do
  IFS='|' read -r name needs_dir marker <<<"$entry"
  bin="$T/$name"
  if [ ! -x "$bin" ]; then echo "FAIL $name (missing binary)"; failed=$((failed+1)); continue; fi
  args=()
  data=""
  if [ "$needs_dir" = 1 ]; then data=$(mktemp -d); args=("$data/"); fi
  out=$("$bin" "${args[@]}" 2>&1); rc=$?
  [ -n "$data" ] && rm -rf "$data"
  if [ $rc -ne 0 ]; then
    echo "FAIL $name (exit $rc)"; printf '%s\n' "$out" | tail -5; failed=$((failed+1))
  elif [ -n "$marker" ] && ! grep -qF "$marker" <<<"$out"; then
    echo "FAIL $name (missing '$marker')"; failed=$((failed+1))
  else
    echo "PASS $name"; passed=$((passed+1))
  fi
done

emit_ctrf "opensprinkler-unit" "$passed" "$failed" 0
