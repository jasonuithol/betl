#!/usr/bin/env bash
# fuzz/run-fuzz.sh — run a libFuzzer harness for a fixed wall-clock
# budget. Defaults to 60 seconds; pass a different value as the second
# argument.
#
# Usage:
#   fuzz/run-fuzz.sh yaml [seconds]
#   fuzz/run-fuzz.sh csv  [seconds]
#   fuzz/run-fuzz.sh json [seconds]
#
# The first invocation lazily configures and builds build-fuzz/ with
# clang + libFuzzer + ASan + UBSan (whole-tree instrumentation, separate
# from the main gcc build/ tree). Subsequent runs reuse it. Findings
# land in fuzz/findings/<name>/ (gitignored).

set -uo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
target="${1:?usage: run-fuzz.sh <yaml|csv|json> [seconds]}"
secs="${2:-60}"

case "$target" in
    yaml|csv|json) ;;
    *) echo "unknown target '$target' (want yaml|csv|json)" >&2; exit 2 ;;
esac

build_dir="$repo_root/build-fuzz"
bin="$build_dir/fuzz/${target}_fuzz"

if [[ ! -x "$bin" ]]; then
    echo "[run-fuzz] configuring build-fuzz/ with clang + sanitizers..." >&2
    cmake -S "$repo_root" -B "$build_dir" \
        -G Ninja -DBETL_FUZZ=ON >&2 || exit $?
    echo "[run-fuzz] building ${target}_fuzz..." >&2
    cmake --build "$build_dir" --target "${target}_fuzz" >&2 || exit $?
fi

if [[ ! -x "$bin" ]]; then
    echo "[run-fuzz] $bin still missing after build" >&2
    exit 2
fi

seeds_dir="$repo_root/fuzz/seeds/${target}"
findings_dir="$repo_root/fuzz/findings/${target}"
mkdir -p "$findings_dir"

# -max_len caps the input size at 64 KB to keep per-iteration cost
# bounded. -timeout=10 makes any single input that takes longer than
# 10s a finding ("hang"). -rss_limit_mb=512 caps memory so a runaway
# alloc doesn't OOM the host.
exec "$bin" \
    -max_total_time="$secs" \
    -max_len=65536 \
    -timeout=10 \
    -rss_limit_mb=512 \
    -artifact_prefix="$findings_dir/" \
    "$seeds_dir"
