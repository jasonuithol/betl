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
# Requires the build to have been configured with -DBETL_FUZZ=ON; the
# harness binaries live in build/fuzz/<name>_fuzz. Findings are written
# to fuzz/findings/<name>/ (created on demand). Seeds come from
# fuzz/seeds/<name>/.

set -uo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
target="${1:?usage: run-fuzz.sh <yaml|csv|json> [seconds]}"
secs="${2:-60}"

case "$target" in
    yaml|csv|json) ;;
    *) echo "unknown target '$target' (want yaml|csv|json)" >&2; exit 2 ;;
esac

bin="$repo_root/build/fuzz/${target}_fuzz"
if [[ ! -x "$bin" ]]; then
    echo "missing $bin -- did you build with -DBETL_FUZZ=ON?" >&2
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
