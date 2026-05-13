# betl — Better ETL

A cross-platform, open-source, text-first ETL system. Pipelines are
plain YAML you can diff, review, and merge — no GUID-stamped XML, no
binary blobs, no IDE lock-in.

> **Status:** v0.2 landed 2026-05-12. The pipeline file format,
> provider ABI (`include/betl/provider.h`), and expression-engine
> sub-ABI are stable in shape but may gain fields before v1.0.
>
> The runtime-neutral contract is documented in
> [`SPEC_CORE.md`](SPEC_CORE.md). This repository hosts **betl-legacy**
> — the first reference implementation (C engine + Lua scripting). A
> second reference implementation, `betl.dotnet` (headless .NET 8,
> hosts compiled SSIS components on Linux), is planned. See
> [SPEC_CORE.md §14](SPEC_CORE.md).

## Why betl

- **Pipelines as plain text.** A pipeline is a YAML file. Two engineers
  editing different parts produce a clean three-way merge — no GUIDs,
  no embedded XML coordinates, no binary blobs.
- **Open spec, not just an implementation.** The pipeline file format
  is defined by [`SPEC_CORE.md`](SPEC_CORE.md) — a runtime-neutral
  contract any conforming implementation can target. The C/Lua engine
  in this repo is one such implementation; more are planned.
- **Apache Arrow types end-to-end.** Cross-component data is passed
  as Arrow record batches via the C Data Interface; no serialization
  between steps inside a pipeline.
- **Pluggable.** Sources, sinks, transforms, expression languages, and
  language hosts are providers loaded at runtime via a stable C ABI.
- **Embedded scripting.** Inline Lua expressions for `where:` /
  `expr:`; full Lua scripts for `lua.task` / `lua.map` / `lua.script`
  (1:N async). C#/VB.NET via `dotnet.task` / `dotnet.script` (NativeAOT
  compile-on-validate). Other languages (Python, SQL host, …) can be
  added without touching the core.

This repository's full design lives in `SPEC.md`; the runtime-neutral
contract that other implementations target lives in `SPEC_CORE.md`.
The provider ABI is documented in `include/betl/provider.h`; the
expression-engine sub-ABI is in `docs/EXPR_ABI.md`.

## A small but real pipeline

```yaml
betl: 1
name: orders-daily-ingest

connections:
  warehouse:
    type: postgres
    dsn: ${env.WAREHOUSE_DSN}

pipeline:
  - id: ingest_orders
    type: dataflow
    steps:
      - id: read
        type: csv.read
        path: ${params.src_dir}/orders-${params.load_date}.csv
        schema:
          columns:
            - { name: order_id,    type: int64 }
            - { name: customer_id, type: int64 }
            - { name: sku,         type: utf8  }

      - id: clean
        type: lua.map
        from: read
        script: |
          row.sku = (row.sku or ""):upper()
          return row

      - id: drop_unknown
        type: filter
        from: clean
        where: "row.sku ~= ''"

      - id: load
        type: postgres.upsert
        from: drop_unknown
        connection: warehouse
        table: stg.orders
        key: [order_id]
```

Run it:

```sh
betl run pipeline.betl.yml \
    --param src_dir=fixtures \
    --param load_date=2026-05-04
```

More examples live under `examples/`: CSV-to-Postgres, sales-star
build with a star-schema lookup join, CRM migration with multi-stage
upserts, and SSIS-style date enrichment with `ssisexpr`.

## Migrating from SSIS

SSIS migration is one supported use case, not the headline. betl-legacy
ships three pieces aimed at it:

- **`betl-dtsx2yaml`** (in `tools/`) — a C# console converter that reads
  `.dtsx` packages and emits betl YAML. Handles OLEDB / Flat File sources
  and sinks, Execute SQL Task, the standard SSIS transforms (Conditional
  Split, Aggregate, Sort, Distinct, Lookup, Merge Join, Union All,
  Multicast, Derived Column, Data Conversion, Pivot, Unpivot, plus the
  rest of the SSIS default set), Script Task and Script Component (with
  VB.NET auto-translated to C#), and the wider control-flow surface
  (containers, precedence constraints, file / process / bulk-insert
  tasks).
- **`ssisexpr`** — the SSIS Expression Language, fully implemented as a
  betl expression provider. Migrated pipelines keep their SSIS derived-
  column / conditional-split expressions verbatim — typed `(DT_*)` casts
  (incl. NUMERIC / GUID / dates), 3VL nulls, ~30 SSIS functions. See
  [`docs/SSISEXPR.md`](docs/SSISEXPR.md).
- **`dotnet.task` / `dotnet.script`** — C# / VB.NET Script Task and
  Script Component analogues, with NativeAOT compile-on-validate. The
  designer-free runtime side of SSIS scripting.

The longer-term plan is for `betl.dotnet` (planned, see
[SPEC_CORE.md §14](SPEC_CORE.md)) to host **compiled SSIS
PipelineComponents** in-process via a `Betl.Ssis.PipelineCompat.dll`
shim — recompile a third-party C# component against the shim, drop the
`.dll` into a plugins directory, run it on Linux without SSDT or SQL
Server.

## What betl-legacy ships (v0.1 + v0.2)

| Kind | Component | Status | Notes |
|---|---|---|---|
| SOURCE | `csv.read` | ✓ | Streaming; RFC 4180; types: int(8/16/32/64) / float(32/64) / utf8 / date / timestamp / timestamptz / time / uuid / decimal(p,s) / binary (hex) |
| SOURCE | `postgres.read` | ✓ | libpq cursor; int / text / float / DATE / TIMESTAMP[TZ] / TIME / NUMERIC / uuid / BYTEA; nulls supported |
| SOURCE | `mssql.read` | ✓ | unixODBC + FreeTDS; int / varchar / float / DATE / DATETIME2 / DATETIMEOFFSET / TIME / DECIMAL / UNIQUEIDENTIFIER / VARBINARY; nulls supported |
| SOURCE | `betl.gen_int64` / `betl.gen_strings` | ✓ | Test generators |
| SINK | `csv.write` | ✓ | RFC 4180; renders all source types as ISO 8601 / canonical text (binary as lower-case hex) |
| SINK | `postgres.upsert` | ✓ | INSERT…ON CONFLICT; 4 conflict modes; binds every source type incl. NUMERIC / TIMESTAMPTZ / uuid / TIME / BYTEA |
| SINK | `mssql.upsert` | ✓ | MERGE; 4 conflict modes; binds DATE / DATETIME2 / DATETIMEOFFSET / DECIMAL / UNIQUEIDENTIFIER / TIME via SQL_C_CHAR text; VARBINARY via SQL_C_BINARY |
| SINK | `betl.count_rows` | ✓ | Smoke / assertion sink |
| TRANSFORM | `filter` | ✓ | Predicate via the expression engine |
| TRANSFORM | `map` | ✓ | `add:` (append) and `select:` (project / rename) |
| TRANSFORM | `aggregate` | ✓ | `group_by` + count / sum / min / max |
| TRANSFORM | `sort` | ✓ | Multi-key, asc / desc, stable |
| TRANSFORM | `join` | ✓ | inner / left / outer; multi-key; null-aware output |
| TRANSFORM | `union` | ✓ | N-input vertical concat; schemas must match |
| TRANSFORM | `distinct` | ✓ | Drop duplicate rows; optional `keys:` subset |
| TRANSFORM | `limit` | ✓ | Keep first N rows |
| TRANSFORM | `conditional_split` | ✓ | Multi-output router; `from: split:case_name` |
| TRANSFORM | `unpivot` | ✓ | Wide → long; `value_cols` collapse into `name_col`/`value_col` |
| TRANSFORM | `pivot` | ✓ | Long → wide on declared `pivot_keys`; sorted-input contract |
| TRANSFORM | `postgres.lookup` | ✓ | Cached SELECT + linear probe; on_miss error/null/drop |
| TRANSFORM | `mssql.lookup` | ✓ | Same model over ODBC |
| TRANSFORM | `multicast` | ✓ | 1-in-N-out fan-out via refcounted shared batches (zero-copy) |
| TRANSFORM | `lua.map` | ✓ | Per-row Lua script; mutate `row` and return (synchronous, 1:1) |
| TRANSFORM | `lua.script` | ✓ | Stateful Lua: `on_row`/`on_eof` + `emit()`; SSIS async script component (1:N, N:1, windowed) |
| TRANSFORM | `dotnet.script` | ✓ | Stateful C# / VB.NET async script component; NativeAOT compile-on-validate; same protocol as `lua.script` |
| TASK | `lua.task` | ✓ | Standalone Lua script with host bridges |
| TASK | `dotnet.task` | ✓ | Standalone C# / VB.NET Script Task analogue; NativeAOT compile-on-validate; Params / Connection / Log bridges |
| TOOL | `betl-dtsx2yaml` | ✓ | DTSX → betl YAML converter; ships in `tools/`, runs separately from `betl run` |
| ENGINE | `literal` | ✓ | Constant expressions (built-in) |
| ENGINE | `lua` | ✓ | Lua 5.4 (provider plugin) |
| ENGINE | `ssisexpr` | ✓ | SSIS Expression Language — typed `(DT_*)` casts (incl. NUMERIC / GUID / dates), 3VL nulls, ~30 functions, decimal+uuid comparisons (provider plugin); see [`docs/SSISEXPR.md`](docs/SSISEXPR.md) |

### Missing on purpose at v0.x

- No `parquet.*` — slated for **v0.3**. Bare parquet is well-defined;
  Delta / Iceberg landing (Snowflake / Databricks / Fabric) is a
  separate conversation.
- No `kafka.*`, no window functions.
- No scheduler. Wire betl into cron / systemd / Airflow as you
  already do.

## Pipeline parallelism

The executor wraps every dataflow edge in a producer-thread + bounded
queue, so adjacent steps overlap their I/O. A typical
`postgres.read → mssql.upsert` chain has the source and sink running
concurrently.

This is **on by default**. Tunable via env vars:

- `BETL_PARALLEL=off` — fall back to the single-threaded path
  (useful for debugging or memory-constrained runs).
- `BETL_PARALLEL_DEPTH=N` — per-edge ring-buffer capacity in batches.
  Default 4. Memory bound is `depth × n_edges × max_batch_size`.

Components don't need to be thread-safe themselves: each component's
state is still touched by exactly one thread — the parallelism happens
*between* components, on the edges.

Measured numbers live in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md);
regenerate with `make bench`. Headline: a 4-stage CPU-bound chain
hits ~3.5× speedup at default depth=4; trivial single-stage shapes
are roughly flat (the handoff costs what the work is worth).

## Provider plugins

The `betl-lua` and `betl-ssisexpr` engines (and any future plugin —
postgres replication source, parquet reader, …) ship as separate
shared libraries. `betl run` auto-loads them from three places, in
order:

1. **`<exe_dir>/providers/PLUGIN/betl-PLUGIN.so`** — the dev build
   tree layout. `cmake --build build` puts each plugin under
   `build/providers/betl-PLUGIN/`, and `build/betl` finds them by
   convention. No flags needed.
2. **`$BETL_PROVIDER_DIR`** — colon-separated list of directories
   (same shape as `LD_LIBRARY_PATH`). For each entry, every
   `betl-*.so` is loaded. Use for production installs.
3. **`--provider <path>`** — explicit, repeatable. For one-off
   overrides or out-of-tree plugins.

A plugin that fails to load emits a stderr warning but doesn't abort
the run — pipelines that don't reference the broken plugin keep
working. Missing engines surface at evaluate time with a clear error.

## Building from source

Prerequisites:

- A C11 compiler (tested with gcc 13), CMake ≥ 3.20, ninja or make.
- `libyaml` (always required), via `pkg-config`.
- *Optional:* `libpq` (for `postgres.*` components),
  `unixODBC` (for `mssql.*` components), `liblua5.4` (for the
  `betl-lua` provider).

On Debian / Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
                 libyaml-dev libpq-dev liblua5.4-dev \
                 unixodbc-dev libodbc2 tdsodbc libodbcinst2
```

Build:

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Each optional dependency is detected at configure time. If `libpq` is
absent the build skips the postgres components; same for `unixODBC`
and `lua5.4`.

## Project layout

```
include/betl/             Public C ABI: provider.h, version.h
src/cli/                  betl binary (run, validate)
src/loader/               provider registry + dlopen
src/pipeline/             YAML → in-memory pipeline AST
src/runtime/              executor, builtins, transforms, db sinks/lookups
src/yaml/                 libyaml-backed parser
providers/betl-lua/       Lua provider plugin (+expression engine)
providers/betl-ssisexpr/  SSIS Expression Language engine
providers/betl-dotnet/    dotnet.task / dotnet.script provider
tools/betl-dtsx2yaml/     DTSX → betl YAML converter (C# console app)
tests/                    C tests; ctest harness; integration tests gated on
                          a sibling Postgres / MSSQL
examples/                 Runnable pipelines with fixtures
schemas/                  JSON Schema for the YAML pipeline format
docs/EXPR_ABI.md          Expression-engine ABI reference
docs/SSISEXPR.md          SSIS-EL function reference (for `.dtsx` migrations)
SPEC.md                   Full betl-legacy design (text-first, types, ABI, …)
SPEC_CORE.md              Runtime-neutral contract; what conforming
                          implementations must honor
```

## Learn more

- [`SPEC_CORE.md`](SPEC_CORE.md) — the runtime-neutral contract: file
  format, step types, type system, placeholder mechanism, validation
  rules, conformance levels. What any betl implementation must honor.
- [`SPEC.md`](SPEC.md) — full betl-legacy design and rationale,
  including the type system (Apache Arrow), the provider model, and
  v0.x vs. v1 commitments.
- `include/betl/provider.h` — authoritative C ABI for components and
  expression engines.
- `docs/EXPR_ABI.md` — companion guide for engine authors.
- `docs/SSISEXPR.md` — SSIS Expression Language reference: supported
  casts, function set, NULL semantics, and what's deferred to v2.
- `examples/` — end-to-end pipelines covering CSV ingest, star build,
  CRM migration, and SSIS-style date enrichment.

## License

Apache-2.0.
