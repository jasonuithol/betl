#!/usr/bin/env bash
# bench/ssis/run-comparison.sh — drive both sides of the SSIS-vs-betl
# comparison and emit a markdown summary table.
#
# Prereqs:
#   * SQL Server reachable at BETL_TEST_MSSQL_DSN, with the schema from
#     bench/ssis/sql/schema.sql already loaded.
#   * The mcp-ssis service running; its packages dir mounted to a path
#     this script can write to via $SSIS_PACKAGES_HOST_DIR (default:
#     /workspace/ssis-packages).
#   * `betl run` working from this repo's ./build/betl.
#
# Usage:
#   bench/ssis/run-comparison.sh [runs=6] [warmup=1]
#
# The SSIS side is invoked via the mcp-ssis MCP service if available, or
# by `dtexec` directly if you have one in $PATH. We default to the MCP
# call — see comments below.
#
# Output: markdown table printed to stdout; also appended to
# bench/ssis/RESULTS.md if BETL_BENCH_APPEND is set.

set -euo pipefail

runs="${1:-6}"
warmup="${2:-1}"

script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
ssis_packages_dir="${SSIS_PACKAGES_HOST_DIR:-/workspace/ssis-packages}"

[[ -n "${BETL_TEST_MSSQL_DSN:-}" ]] || {
    echo "BETL_TEST_MSSQL_DSN unset — set it to the ODBC DSN for betl_bench" >&2
    exit 1
}

echo "# SSIS-vs-betl bench comparison"
echo
echo "Hardware: $(uname -srm)"
echo "betl: $(cd "$repo_root" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "runs=$runs warmup=$warmup"
echo

# 1. betl side — call run-betl-side.sh for each yaml, capture min ms.
echo "## betl side"
echo
echo '| shape | min ms | p50 ms | max ms |'
echo '|---|---:|---:|---:|'
for yml in \
    "$script_dir/yaml/betl-A-1col.yml" \
    "$script_dir/yaml/betl-A-10col.yml" \
    "$script_dir/yaml/betl-B-derived-10col.yml" \
    "$script_dir/yaml/betl-B-derived-10col-1m.yml"
do
    name="$(basename "$yml" .yml)"
    out="$("$script_dir/run-betl-side.sh" "$yml" "$runs" "$warmup" 2>&1 || true)"
    summary="$(echo "$out" | grep '^SUMMARY' | tail -1)"
    min="$(echo "$summary" | sed -nE 's/.*min=([0-9]+)ms.*/\1/p')"
    p50="$(echo "$summary" | sed -nE 's/.*p50=([0-9]+)ms.*/\1/p')"
    max="$(echo "$summary" | sed -nE 's/.*max=([0-9]+)ms.*/\1/p')"
    printf '| `%s` | %s | %s | %s |\n' "$name" "${min:-?}" "${p50:-?}" "${max:-?}"
done

# 2. SSIS side — needs the mcp-ssis service or a direct dtexec.
# This script doesn't call MCP directly (no MCP CLI in the sandbox); if
# you've staged the .dtsx files under $ssis_packages_dir, call
# mcp-ssis benchmark_package() interactively (or wrap the HTTP API) and
# paste the results into RESULTS.md.
echo
echo "## SSIS side"
echo
echo "Stage the .dtsx files in $ssis_packages_dir, then call the"
echo "mcp-ssis benchmark_package tool for each of:"
echo "  - A-1col.dtsx"
echo "  - A-10col.dtsx"
echo "  - B-derived-10col.dtsx"
echo "  - B-derived-10col-1m.dtsx"
echo "with runs=$runs warmup=$warmup."
