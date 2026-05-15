#!/usr/bin/env bash
# tests/run-and-report.sh — run ctest and emit an explicit summary that
# distinguishes ran/passed from silently-skipped. The MCP test runner
# (and most ctest renderers) collapse `exit 77` skips into "Passed
# 0.02s" lines that look identical to a real-fast-pass; if a test
# silently skips because a sibling DB is unreachable, every claim of
# "N tests passed" overstates coverage by N_skipped.
#
# Usage:
#   tests/run-and-report.sh [<ctest -R filter>]
#
# Drives ctest from the build directory with --output-on-failure plus
# verbose-on-skip, then walks the output to count skipped tests. Exit
# code mirrors ctest's (0 = all passed/skipped clean; non-zero = at
# least one real failure).

set -uo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$repo_root/build"
[[ -d "$build_dir" ]] || { echo "$build_dir not present — run mcp build first" >&2; exit 2; }

filter_args=()
if [[ $# -gt 0 ]]; then
    filter_args=(-R "$1")
fi

# ctest 3.16+ surfaces skip status with `--output-on-failure
# --rerun-failed` etc., but the simplest cross-version path is
# `--no-tests=error` (catches a typo'd filter) plus parsing the
# summary footer ourselves. We invoke ctest from inside the build
# tree and tee its output so we can both display and analyse it.
out="$(mktemp)"
trap 'rm -f "$out"' EXIT

(
    cd "$build_dir" || exit 3
    ctest --no-tests=error --output-on-failure "${filter_args[@]}"
) 2>&1 | tee "$out"
rc=${PIPESTATUS[0]}

total=$(grep -cE 'Test #[0-9]+:' "$out" || true)
passed=$(grep -cE '   Passed ' "$out" || true)
failed=$(grep -cE '\*\*\*Failed' "$out" || true)
skipped=$(grep -cE '\*\*\*Skipped' "$out" || true)
# Some ctest output formats render skipped as "Not Run" — capture both.
not_run=$(grep -cE '\*\*\*Not Run' "$out" || true)
skipped=$((skipped + not_run))

# Older ctest collapses skips into Passed and only marks them in the
# detailed footer ("Tests that were skipped:" / "The following tests
# did not run:"). Grab the footer block and count names from there.
if grep -qE 'tests did not run' "$out"; then
    footer_skip=$(awk '/tests did not run/{f=1;next} f && /^[[:space:]]*[0-9]+ - /{c++} END{print c+0}' "$out")
    if (( footer_skip > skipped )); then skipped=$footer_skip; fi
fi

echo
echo "==== SUMMARY ===="
printf "  total=%d  passed=%d  skipped=%d  failed=%d\n" \
       "$total" "$passed" "$skipped" "$failed"
if (( skipped > 0 )); then
    echo
    echo "  Skipped tests (these did NOT exercise the code path — don't"
    echo "  claim full coverage from a run that includes them):"
    awk '/tests did not run/{f=1;next}
         /^Errors/{f=0}
         f && /^[[:space:]]*[0-9]+ - /{print "    " $0}' "$out"
fi
exit "$rc"
