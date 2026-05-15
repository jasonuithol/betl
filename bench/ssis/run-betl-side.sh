#!/usr/bin/env bash
# Time `betl run <yaml>` over N iterations with W warmups discarded.
# Mirrors mcp-ssis's benchmark_package(runs, warmup) discipline so the
# resulting numbers are directly comparable.
#
# Usage:
#   bench/ssis/run-betl-side.sh <yaml> [runs=6] [warmup=1]
#
# Env:
#   BETL_TEST_MSSQL_DSN — required, ODBC DSN string
#   BETL_BIN            — defaults to ./build/betl
#
# Write-bench YAMLs should put their TRUNCATE as the first pipeline
# step (sql.execute) — TRUNCATE is metadata-only on SQL Server and the
# extra wall-clock cost is below the noise floor of the measured runs.
#
# Output: one CSV line per measured iteration, plus a min/p50/max summary.

set -euo pipefail

yaml="${1:?usage: $0 <yaml> [runs] [warmup]}"
runs="${2:-6}"
warmup="${3:-1}"

bin="${BETL_BIN:-./build/betl}"
[[ -x "$bin" ]] || { echo "betl binary not found at $bin" >&2; exit 1; }
[[ -n "${BETL_TEST_MSSQL_DSN:-}" ]] || { echo "BETL_TEST_MSSQL_DSN unset" >&2; exit 1; }

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
export LD_LIBRARY_PATH="$repo_root/deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

times_ms=()
for ((i=1; i<=runs; i++)); do
    # Use `date +%s%N` for ms resolution; bash's $SECONDS is too coarse.
    t0=$(date +%s%N)
    "$bin" run "$yaml" >/dev/null 2>&1
    rc=$?
    t1=$(date +%s%N)
    elapsed_ms=$(( (t1 - t0) / 1000000 ))
    if (( rc != 0 )); then
        echo "iter $i: betl run failed (rc=$rc)" >&2
        exit $rc
    fi
    label="run"
    if (( i <= warmup )); then
        label="warmup"
    else
        times_ms+=("$elapsed_ms")
    fi
    printf "iter=%d label=%-6s elapsed_ms=%d\n" "$i" "$label" "$elapsed_ms"
done

# Stats over measured runs only.
sorted=($(printf '%s\n' "${times_ms[@]}" | sort -n))
n="${#sorted[@]}"
min="${sorted[0]}"
max="${sorted[$((n-1))]}"
p50_idx=$((n / 2))
p50="${sorted[$p50_idx]}"

sum=0
for t in "${sorted[@]}"; do sum=$((sum + t)); done
mean=$((sum / n))

printf "\nSUMMARY: yaml=%s runs=%d warmups=%d  min=%dms p50=%dms max=%dms mean=%dms\n" \
       "$yaml" "$n" "$warmup" "$min" "$p50" "$max" "$mean"
