#!/usr/bin/env bash
# Postgres-side bench runner. Mirrors run-betl-side.sh but adds a
# per-iteration TRUNCATE of the target table — postgres.upsert and
# postgres.copy both want a clean target each run, but only the
# latter handles truncation itself (via truncate: true).
#
# Usage:
#   bench/ssis/run-pg-side.sh <yaml> <target_table> [runs=6] [warmup=1]
#
# Env:
#   BETL_TEST_PG_BENCH_DSN — required, libpq URI to the betl_bench DB
#   BETL_BIN               — defaults to ./build/betl
#
# Truncation uses a tiny betl pipeline that runs "TRUNCATE TABLE <tbl>"
# via postgres.exec — keeps the bench runner from needing psql.
# postgres.exec is row-bound so we drive it with a single gen_int64
# row whose value is unused.
#
# Output: same shape as run-betl-side.sh — one CSV line per iter +
# min/p50/max summary.

set -euo pipefail

yaml="${1:?usage: $0 <yaml> <target_table> [runs] [warmup]}"
target="${2:?usage: $0 <yaml> <target_table> [runs] [warmup]}"
runs="${3:-6}"
warmup="${4:-1}"

bin="${BETL_BIN:-./build/betl}"
[[ -x "$bin" ]] || { echo "betl binary not found at $bin" >&2; exit 1; }
[[ -n "${BETL_TEST_PG_BENCH_DSN:-}" ]] || {
    echo "BETL_TEST_PG_BENCH_DSN unset" >&2; exit 1;
}

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
export LD_LIBRARY_PATH="$repo_root/deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Generate a one-shot truncate pipeline. postgres.exec takes a row from
# the source as the trigger; we don't bind any parameters.
truncate_yml="$(mktemp --suffix=.yml)"
cat > "$truncate_yml" <<EOF
betl: 1
name: bench-pg-truncate
connections:
  bench:
    type: postgres
    dsn: \${env.BETL_TEST_PG_BENCH_DSN}
pipeline:
  - id: stage_one
    type: dataflow
    steps:
      - id: source
        type: betl.gen_int64
        row_count: 1
      - id: trunc
        type: postgres.exec
        from: source
        connection: bench
        sql: 'TRUNCATE TABLE ${target}'
        parameters: []
      - id: sink
        type: betl.count_rows
        from: trunc
        expect: 1
EOF
trap 'rm -f "$truncate_yml"' EXIT

times_ms=()
for ((i=1; i<=runs; i++)); do
    "$bin" run "$truncate_yml" >/dev/null 2>&1 || {
        echo "iter $i: truncate failed" >&2; exit 1;
    }
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
