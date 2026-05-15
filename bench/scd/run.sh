#!/usr/bin/env bash
# bench/scd/run.sh — drive the SCD type-2 recipe at a given row count
# and report timings.
#
# Usage:
#   bench/scd/run.sh <n_staging> <n_seeded_dim> [runs=3]
#
# Env:
#   BETL_TEST_PG_BENCH_DSN — required, libpq URI to the bench database
#   BETL_BIN               — defaults to ./build/betl

set -euo pipefail

n="${1:?usage: $0 <n_staging> <n_seeded_dim> [runs]}"
dimn="${2:?usage: $0 <n_staging> <n_seeded_dim> [runs]}"
runs="${3:-3}"

bin="${BETL_BIN:-./build/betl}"
[[ -x "$bin" ]] || { echo "betl binary not found at $bin" >&2; exit 1; }
[[ -n "${BETL_TEST_PG_BENCH_DSN:-}" ]] || {
    echo "BETL_TEST_PG_BENCH_DSN unset" >&2; exit 1;
}

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
export LD_LIBRARY_PATH="$repo_root/deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "==> setup schema"
psql "$BETL_TEST_PG_BENCH_DSN" -q -v ON_ERROR_STOP=1 -f "$script_dir/setup.sql"

times_ms=()
for ((i = 1; i <= runs; i++)); do
    # Re-seed each iteration so every run is the same shape.
    psql "$BETL_TEST_PG_BENCH_DSN" -q -v ON_ERROR_STOP=1 \
         -v n="$n" -v dimn="$dimn" -f "$script_dir/seed.sql"

    t0=$(date +%s%N)
    "$bin" run "$script_dir/yaml/scd.yml" \
           --param "batch_ts=2026-05-15T00:00:00+00" >/dev/null
    t1=$(date +%s%N)
    elapsed_ms=$(( (t1 - t0) / 1000000 ))

    # Inspect the result to make sure it actually did the work.
    counts=$(psql -t -A -F'|' "$BETL_TEST_PG_BENCH_DSN" -c "
        SELECT
          (SELECT count(*) FROM scdbench_dim.customer WHERE is_current),
          (SELECT count(*) FROM scdbench_dim.customer WHERE NOT is_current)
    ")
    printf "iter=%d elapsed_ms=%d  current=%s closed=%s\n" \
           "$i" "$elapsed_ms" "${counts%|*}" "${counts#*|}"
    times_ms+=("$elapsed_ms")
done

sorted=($(printf '%s\n' "${times_ms[@]}" | sort -n))
nrun="${#sorted[@]}"
p50_idx=$((nrun / 2))
min="${sorted[0]}"
max="${sorted[$((nrun-1))]}"
p50="${sorted[$p50_idx]}"
throughput=$(awk -v n="$n" -v p50="$p50" 'BEGIN{printf "%.0f", n / (p50 / 1000.0)}')

echo
printf "SUMMARY: n=%d dim_seed=%d runs=%d  min=%dms p50=%dms max=%dms  throughput≈%s rows/sec\n" \
       "$n" "$dimn" "$nrun" "$min" "$p50" "$max" "$throughput"
