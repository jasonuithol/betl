#!/usr/bin/env bash
# Drive betl_bench across (shape × mode) combinations and print a
# markdown table summary. Designed to be invoked by `make bench`,
# which substitutes the absolute path to the bench binary as $1.
#
# Each (shape, mode) runs in its own subprocess so async_stream's
# env-cached parallel toggle picks up the right setting fresh.

set -euo pipefail

bin="${1:-./build/bench/betl_bench}"
if [[ ! -x "$bin" ]]; then
    echo "betl_bench binary not found at: $bin" >&2
    exit 1
fi

# Default sweep parameters; override via env. ROWS_LARGE is the I/O-
# shape file size proxy — keep it large enough to swamp warmup noise.
# ROWS_PC defaults smaller because dotnet.pipelinecomponent has higher
# per-row overhead (C↔C# crossings + AOT path) and 100k rows is enough
# to swamp µs-level timing noise without taking a full minute per iter.
ITERS="${BETL_BENCH_ITERS:-5}"
ROWS_SMALL="${BETL_BENCH_ROWS:-1000000}"
ROWS_LARGE="${BETL_BENCH_CSV_ROWS:-1000000}"
ROWS_PC="${BETL_BENCH_PC_ROWS:-100000}"

# Provider library / deps load paths — providers/* sit next to bench/
# in the build tree; deps/lib carries libpq / libyaml / etc.
script_dir="$(cd "$(dirname "$0")" && pwd)"
deps_lib="$script_dir/../deps/lib"
export LD_LIBRARY_PATH="$deps_lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Each shape: name + rows + label. The csv-rt shape uses ROWS_LARGE
# (controls input file size); the rest use ROWS_SMALL.
declare -a SHAPES=(
    "filter-count|$ROWS_SMALL|gen → filter(true) → count"
    "map-arith|$ROWS_SMALL|gen → ssisexpr arithmetic → count"
    "sort|$ROWS_SMALL|gen → sort desc → count (materializes)"
    "chain|$ROWS_SMALL|gen → 4× ssisexpr map → count"
    "csv-rt|$ROWS_LARGE|csv.read → ssisexpr map → csv.write"
    "pc-passthrough-1col|$ROWS_PC|dotnet.pipelinecomponent 1-col passthrough"
    "pc-passthrough-10col|$ROWS_PC|dotnet.pipelinecomponent 10-col passthrough"
    "pc-error-route|$ROWS_PC|dotnet.pipelinecomponent error_output (10% tagged)"
    "pc-decimal|$ROWS_PC|dotnet.pipelinecomponent 5 decimal cells/row"
    "pc-async-aggregate|$ROWS_PC|dotnet.pipelinecomponent async N → 1 summary"
    "pc-vs-lua-script|$ROWS_PC|lua.script 1-col passthrough (baseline for pc)"
    "pc-startup|1|dotnet.pipelinecomponent cold + warm start (AOT compile cost)"
)

# Each mode: env settings + label.
declare -a MODES=(
    "serial|BETL_PARALLEL=off"
    "par1|BETL_PARALLEL=on BETL_PARALLEL_DEPTH=1"
    "par4|BETL_PARALLEL=on BETL_PARALLEL_DEPTH=4"
)

results_file="$(mktemp)"
trap 'rm -f "$results_file"' EXIT

# The pc-startup shape gets bespoke handling — it measures end-to-end
# cold-load + first-batch cost, not steady-state throughput. We run it
# with --no-warmup and a single timed iteration, once with the AOT
# compile cache pre-cleared (mode=cold) and once with the cache warm
# (mode=warm). Parallel mode is irrelevant to startup cost so the
# usual mode loop is skipped.
DOTNET_CACHE_DIR="${BETL_DOTNET_CACHE_DIR:-$HOME/.cache/betl/dotnet}"

run_pc_startup() {
    local shape="$1"
    echo "# $shape: cold + warm startup timing" >&2
    # Cold: clear the per-source-hash compile cache so the AOT publish
    # has to run from scratch. NuGet cache (~/.nuget) stays warm to
    # match the user-visible second-deploy case; first-ever-deploy on
    # a clean machine is even slower.
    rm -rf "$DOTNET_CACHE_DIR" 2>/dev/null || true
    set +e
    out=$(env -i HOME="$HOME" PATH="$PATH" \
          LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
          BETL_DOTNET_ROOT="${BETL_DOTNET_ROOT:-}" \
          BETL_PARALLEL=off \
          "$bin" "$shape" 1 --rows 1 --no-warmup --mode cold)
    rc=$?
    set -e
    if [[ $rc -eq 77 ]]; then
        echo "  cold: SKIPPED" >&2
    elif [[ $rc -ne 0 ]]; then
        echo "  cold: FAILED" >&2
    else
        echo "  cold: $out" >&2
        echo "$out" >> "$results_file"
    fi
    # Warm: don't clear cache; the cold run above populated it. Run
    # the same iter shape so timings are comparable.
    set +e
    out=$(env -i HOME="$HOME" PATH="$PATH" \
          LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
          BETL_DOTNET_ROOT="${BETL_DOTNET_ROOT:-}" \
          BETL_PARALLEL=off \
          "$bin" "$shape" 5 --rows 1 --no-warmup --mode warm)
    rc=$?
    set -e
    if [[ $rc -eq 77 ]]; then
        echo "  warm: SKIPPED" >&2
    elif [[ $rc -ne 0 ]]; then
        echo "  warm: FAILED" >&2
    else
        echo "  warm: $out" >&2
        echo "$out" >> "$results_file"
    fi
}

for shape_entry in "${SHAPES[@]}"; do
    IFS='|' read -r shape rows description <<<"$shape_entry"
    if [[ "$shape" == "pc-startup" ]]; then
        run_pc_startup "$shape"
        continue
    fi
    echo "# $shape ($rows rows): $description" >&2
    for mode_entry in "${MODES[@]}"; do
        IFS='|' read -r mode envs <<<"$mode_entry"
        # Build the env-prefix string and run in a subshell so the env
        # is fresh for each invocation (async_stream caches its flags).
        # BETL_DOTNET_ROOT propagated through env -i so the dotnet shapes
        # can find the SDK; benign for non-dotnet shapes.
        set +e
        out=$(env -i HOME="$HOME" PATH="$PATH" \
              LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
              BETL_DOTNET_ROOT="${BETL_DOTNET_ROOT:-}" \
              $envs "$bin" "$shape" "$ITERS" --rows "$rows" --mode "$mode")
        rc=$?
        set -e
        if [[ $rc -eq 77 ]]; then
            echo "  $mode: SKIPPED" >&2
            continue
        fi
        if [[ $rc -ne 0 ]]; then
            echo "  $mode: FAILED" >&2
            continue
        fi
        echo "  $mode: $out" >&2
        echo "$out" >> "$results_file"
    done
done

# Render markdown. Group rows by shape, columns by mode.
# Default output sits in the project's docs/ next to other reference
# docs, anchored to the script's own location so cwd doesn't matter.
project_root="$(cd "$script_dir/.." && pwd)"
out_md="${BETL_BENCH_OUT:-$project_root/docs/BENCHMARKS.md}"
mkdir -p "$(dirname "$out_md")"
{
    echo "# betl benchmarks"
    echo
    echo "Generated by \`bench/run.sh\`. Each cell is wall-clock"
    echo "milliseconds (min of $ITERS iterations after one warm-up run)."
    echo "Speedup column compares the best parallel mode to serial."
    echo
    echo "Host: \`$(uname -srm 2>/dev/null || echo unknown)\` —"
    echo "$(nproc 2>/dev/null || echo '?') logical CPUs."
    echo
    echo "## How to read these numbers"
    echo
    echo "betl's parallelism is **pipeline parallelism**: every"
    echo "producer-consumer edge between adjacent steps gets a"
    echo "background thread + ring buffer (depth set by"
    echo "\`BETL_PARALLEL_DEPTH\`). Steps in a chain run concurrently"
    echo "on different CPUs."
    echo
    echo "What that implies:"
    echo
    echo "- **Long chains of CPU-bound steps** speed up the most."
    echo "  Each stage's work overlaps with its neighbours' work."
    echo "  See \`chain\` below."
    echo "- **Trivial pipelines** are flat or slightly slower."
    echo "  The ring-buffer handoff adds ~µs of overhead per batch."
    echo "  See \`filter-count\` and \`map-arith\` below."
    echo "- **Materialising steps** (sort, aggregate) can't overlap"
    echo "  with downstream work — the whole input has to land first."
    echo "  See \`sort\` below."
    echo "- **I/O-bound pipelines** benefit modestly; the I/O step's"
    echo "  syscall time overlaps with the CPU step's compute."
    echo "  See \`csv-rt\` below."
    echo
    echo "### dotnet.pipelinecomponent shapes (\`pc-*\`)"
    echo
    echo "The \`pc-*\` shapes exercise the NativeAOT-compiled SSIS"
    echo "PipelineComponent path. Each first-run incurs a one-time AOT"
    echo "publish; see \`pc-startup\` below for the measured cold/warm"
    echo "split (typically ~1.5 s cold for a trivial component, sub-ms"
    echo "warm on cache hit). The per-row \`pc-*\` shapes' pre-timing"
    echo "warmup absorbs that compile, so the reported wall_min etc."
    echo "are steady-state per-row throughput, not cold-start."
    echo
    echo "Throughput notes:"
    echo
    echo "- **Single-cell I/O is cheap.** \`pc-passthrough-1col\` and"
    echo "  \`pc-async-aggregate\` show >10M rows/s — the C↔C# crossings"
    echo "  per cell aren't the bottleneck for narrow rows."
    echo "- **Throughput scales linearly with column count.** 10-col"
    echo "  passthrough is ~3× slower than 1-col — each Get/Set is its"
    echo "  own function-pointer call."
    echo "- **Error routing adds negligible overhead** when a small"
    echo "  fraction of rows are tagged (\`pc-error-route\` vs"
    echo "  \`pc-passthrough-1col\`)."
    echo "- **decimal128 is ~5× slower than int64**, with the bulk of"
    echo "  the cost now in the per-cell \`new byte[16]\` heap allocation"
    echo "  on the C# side rather than the 128-bit arithmetic itself"
    echo "  (the original BigInteger-based path was ~10× slower; the"
    echo "  switch to System.Int128 / UInt128 + power-of-10 lookup more"
    echo "  than doubled throughput). Further speedup would need a"
    echo "  staging refactor to use stackalloc Span<byte> instead of"
    echo "  heap-allocated byte[]."
    echo "- **Async aggregator is fastest.** Only one output row total,"
    echo "  no per-row Set in the user code."
    echo "- **Parallel mode is a wash** for these shapes — the dotnet"
    echo "  component is the single hot stage, with little for the"
    echo "  pipeline-parallel executor to overlap."
    echo "- **Comparison vs lua.script:** \`pc-vs-lua-script\` runs the"
    echo "  same 1-col passthrough through Lua's VM. dotnet (NativeAOT)"
    echo "  is roughly 2.5–3× faster than Lua here. The trade-off is"
    echo "  startup: lua.script has no AOT compile."
    echo
    echo "Reproduce: \`make bench\` (or \`bench/run.sh\` directly)."
    echo "Tunables: \`BETL_BENCH_ITERS\`, \`BETL_BENCH_ROWS\`,"
    echo "\`BETL_BENCH_CSV_ROWS\`, \`BETL_BENCH_PC_ROWS\`."
    echo
    echo "## TODO: Windows-side SSIS comparison"
    echo
    echo "The \`pc-*\` shapes give us numbers for"
    echo "\`dotnet.pipelinecomponent\`'s throughput on a Linux box."
    echo "We have an architectural argument that we should be faster"
    echo "than real SSIS on the component hot path — SSIS's"
    echo "\`PipelineBuffer\` is implemented in native C++ and managed"
    echo "code reaches it through COM RCWs, paying a marshalling tax"
    echo "per cell. Our shim's \`BetlPipelineBuffer\` is pure managed"
    echo "code with array-backed storage."
    echo
    echo "**We have not actually measured real SSIS.** Until we do,"
    echo "the \"5–30× faster per component\" claim in"
    echo "\`docs/PIPELINECOMPONENT.md\` is structural reasoning, not"
    echo "data. To turn it into a defensible number:"
    echo
    echo "1. Windows box with SQL Server Developer Edition + SSDT"
    echo "   (Visual Studio extension). Both free."
    echo "2. Pick a workload — same shape as one of our \`pc-*\` shapes."
    echo "   The 10-col passthrough or the decimal-heavy variant"
    echo "   are the most informative because they amplify per-cell"
    echo "   crossing cost."
    echo "3. Compile the same C# source two ways:"
    echo "   - Against the real Microsoft.SqlServer.Dts.Pipeline"
    echo "     assemblies via SSDT, deployed as a real SSIS package"
    echo "     and run under \`dtexec\`."
    echo "   - Against our \`Betl.Ssis.PipelineCompat\` shim and"
    echo "     run via \`betl run\`."
    echo "4. Same hardware, same input data, same row count, fair"
    echo "   source/sink setup."
    echo "5. Publish the numbers here as a new section."
    echo
    echo "The shim's API surface was made faithful to the SSIS one"
    echo "specifically so this A/B is possible without rewriting the"
    echo "user code. The runtime is the only variable."
    echo
    echo "Estimated effort: half a day once the Windows box is set up."
    echo "Until then, treat the per-component speedup claim as a"
    echo "hypothesis, not a benchmark."
    echo
    for shape_entry in "${SHAPES[@]}"; do
        IFS='|' read -r shape rows description <<<"$shape_entry"
        echo "## \`$shape\` — $description"
        echo
        if [[ "$shape" == "pc-startup" ]]; then
            echo "End-to-end wall time from \`bench/betl_bench\` launch to"
            echo "process exit on a 1-row pipeline. **Cold** clears the per-"
            echo "source-hash AOT cache (\`\$HOME/.cache/betl/dotnet\`) before"
            echo "the run; the AOT publish runs from scratch (NuGet cache is"
            echo "still warm). **Warm** reuses the cached .so."
            echo
            for mode in cold warm; do
                line=$(grep "^$shape,$mode," "$results_file" || true)
                if [[ -z "$line" ]]; then continue; fi
                wmin=$(echo "$line"  | awk -F, '{print $5}')
                wp50=$(echo "$line"  | awk -F, '{print $6}')
                wmax=$(echo "$line"  | awk -F, '{print $7}')
                maxrss=$(echo "$line" | awk -F, '{print $11}')
                printf -- "- **%s**: min=%s ms · p50=%s ms · max=%s ms · maxrss=%s KB\n" \
                    "$mode" "$wmin" "$wp50" "$wmax" "$maxrss"
            done
            echo
            continue
        fi
        echo "Rows: $rows"
        echo
        # Pull values for this shape.
        serial_ms=""
        par1_ms=""
        par4_ms=""
        for mode_entry in "${MODES[@]}"; do
            IFS='|' read -r mode _envs <<<"$mode_entry"
            line=$(grep "^$shape,$mode," "$results_file" || true)
            if [[ -z "$line" ]]; then continue; fi
            # CSV: shape,mode,iters,rows,wall_min,wall_p50,wall_max,wall_mean,user,sys,maxrss,rps
            wmin=$(echo "$line"  | awk -F, '{print $5}')
            wp50=$(echo "$line"  | awk -F, '{print $6}')
            wmax=$(echo "$line"  | awk -F, '{print $7}')
            rps=$(echo "$line"   | awk -F, '{print $12}')
            maxrss=$(echo "$line" | awk -F, '{print $11}')
            case "$mode" in
                serial) serial_ms="$wmin" ;;
                par1)   par1_ms="$wmin" ;;
                par4)   par4_ms="$wmin" ;;
            esac
            printf -- "- **%s**: min=%s ms · p50=%s ms · max=%s ms · %s rows/s · maxrss=%s KB\n" \
                "$mode" "$wmin" "$wp50" "$wmax" "$rps" "$maxrss"
        done
        if [[ -n "$serial_ms" && -n "$par4_ms" ]]; then
            speedup=$(awk -v s="$serial_ms" -v p="$par4_ms" 'BEGIN{ if (p>0) printf "%.2fx", s/p; else print "—" }')
            echo
            echo "**Speedup (serial → par4): $speedup**"
        fi
        echo
    done
} > "$out_md"

echo
echo "Wrote $out_md"
