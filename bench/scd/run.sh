#!/usr/bin/env bash
# bench/scd/run.sh — drive the SCD type-2 recipe at a given row count
# and report timings. Self-contained: drives setup + seed via betl
# itself, no psql binary required.
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

# Drive setup + per-iteration seed via betl's `sql.execute` task.
# Tmpfile gets rewritten each iteration with the current N / dimN baked
# in via shell substitution — sql.execute's `sql:` is parsed at YAML
# load time, so the values can't come from `--param`.
tmp="$(mktemp --suffix=.yml)"
trap 'rm -f "$tmp"' EXIT

run_sql() {
    local title="$1"; shift
    local sql="$*"
    cat > "$tmp" <<EOF
betl: 1
name: bench-scd-step
connections:
  warehouse:
    type: postgres
    dsn: \${env.BETL_TEST_PG_BENCH_DSN}
pipeline:
  - id: stmt
    type: sql.execute
    connection: warehouse
    sql: |
$(printf '      %s\n' "$sql" | sed 's/^/      /')
EOF
    "$bin" run "$tmp" >/dev/null 2>&1 || {
        echo "$title: betl run failed" >&2
        cat "$tmp" >&2
        return 1
    }
}

# ---- One-time setup. -----------------------------------------------------
echo "==> setup schema"
$bin run "$script_dir/yaml/setup.yml" >/dev/null 2>&1 \
    || { echo "setup pipeline failed" >&2; exit 1; }

# ---- Result-count probe via a betl pipeline.
# Captures counts to stdout via the count_rows log line.
read_counts() {
    cat > "$tmp" <<EOF
betl: 1
name: bench-scd-counts
connections:
  warehouse:
    type: postgres
    dsn: \${env.BETL_TEST_PG_BENCH_DSN}
pipeline:
  - id: probe
    type: dataflow
    steps:
      - id: src
        type: postgres.read
        connection: warehouse
        query: |
          SELECT
            (SELECT count(*) FROM scdbench_dim.customer WHERE is_current)::bigint     AS cur,
            (SELECT count(*) FROM scdbench_dim.customer WHERE NOT is_current)::bigint AS closed
      - id: out
        type: csv.write
        from: src
        path: ${tmp}.counts
        header: false
EOF
    "$bin" run "$tmp" >/dev/null 2>&1
    awk -F, '{printf "current=%s closed=%s", $1, $2}' "${tmp}.counts" 2>/dev/null
    rm -f "${tmp}.counts"
}

times_ms=()
for ((i = 1; i <= runs; i++)); do
    # Re-seed each iteration so every run is the same workload.
    run_sql "truncate" "TRUNCATE scdbench_dim.customer; TRUNCATE scdbench_stg.customer;"
    run_sql "seed-stg" "
        INSERT INTO scdbench_stg.customer (customer_id, name, email, address)
        SELECT g, 'Name_' || g, 'e' || g || '@x',
               CASE WHEN g <= ($dimn * 0.5)::int          THEN 'Lane ' || g
                    WHEN g <=  $dimn AND (g % 4) <> 0      THEN 'CHANGED Lane ' || g
                    WHEN g >   $dimn                       THEN 'Lane ' || g
                    ELSE NULL END
          FROM generate_series(1, $n) AS g
         WHERE NOT (g > ($dimn * 0.5)::int AND g <= $dimn AND (g % 4) = 0);"
    if (( dimn > 0 )); then
        run_sql "seed-dim" "
            INSERT INTO scdbench_dim.customer
                (customer_id, name, email, address, valid_from, is_current)
            SELECT g, 'Name_' || g, 'e' || g || '@x', 'Lane ' || g,
                   '2026-01-01T00:00:00Z', TRUE
              FROM generate_series(1, $dimn) AS g;"
    fi

    t0=$(date +%s%N)
    "$bin" run "$script_dir/yaml/scd.yml" \
           --param "batch_ts=2026-05-15T00:00:00+00" >/dev/null
    t1=$(date +%s%N)
    elapsed_ms=$(( (t1 - t0) / 1000000 ))

    printf "iter=%d elapsed_ms=%d  %s\n" "$i" "$elapsed_ms" "$(read_counts)"
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
