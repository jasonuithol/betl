# betl — Better ETL

A cross-platform, open-source, text-first ETL runtime aimed at being a
modern replacement for SQL Server Integration Services (SSIS).

> **Status:** v0.1, alpha. Useful for experimentation and small ingest
> jobs against the supported sources/sinks. The C ABI for providers
> (`include/betl/provider.h`) is stable in shape but may gain fields
> before v1.0.

## What it is

- **Pipelines as plain text.** A pipeline is a YAML file. Two engineers
  editing different parts produce a clean three-way merge — no GUIDs,
  no embedded XML coordinates, no binary blobs.
- **Engine in C, types from Apache Arrow.** Cross-component data is
  passed as Arrow record batches via the C Data Interface; no
  serialization between steps inside a pipeline.
- **Pluggable.** Sources, sinks, transforms, expression languages, and
  language hosts are providers loaded at runtime via a stable C ABI.
- **Embedded scripting.** Inline Lua expressions for `where:` /
  `expr:`; full Lua scripts for `lua.task` / `lua.map`. SSIS users
  migrating off `.dtsx` files can keep their Data Flow expressions
  verbatim via the **`ssisexpr`** engine (`MONTH`, `DATEADD`, typed
  `(DT_*)` casts, 3VL nulls). Other languages (Python, SQL host, …)
  can be added without touching the core.

The full design lives in `SPEC.md`. The provider ABI is documented in
`include/betl/provider.h`; the expression-engine sub-ABI is in
`docs/EXPR_ABI.md`.

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

## What's in v0.1

| Kind | Component | Status | Notes |
|---|---|---|---|
| SOURCE | `csv.read` | ✓ | Streaming; RFC 4180 quoted fields incl. multi-line; int64 / utf8 / date / timestamp |
| SOURCE | `postgres.read` | ✓ | Run a SELECT, stream rows; libpq cursor; int / text / DATE / TIMESTAMP; nulls supported |
| SOURCE | `mssql.read` | ✓ | Run a SELECT, stream rows; unixODBC + FreeTDS; int / varchar / DATE / DATETIME2; nulls supported |
| SOURCE | `betl.gen_int64` / `betl.gen_strings` | ✓ | Test generators |
| SINK | `csv.write` | ✓ | RFC 4180 quoting, header / delimiter; date / timestamp rendered as ISO 8601 |
| SINK | `postgres.upsert` | ✓ | INSERT…ON CONFLICT; 4 conflict modes; libpq; binds DATE / TIMESTAMP |
| SINK | `mssql.upsert` | ✓ | MERGE; 4 conflict modes; unixODBC + FreeTDS; binds DATE / DATETIME2 |
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
| TRANSFORM | `postgres.lookup` | ✓ | Cached SELECT + linear probe; on_miss error/null/drop |
| TRANSFORM | `mssql.lookup` | ✓ | Same model over ODBC |
| TRANSFORM | `lua.map` | ✓ | Per-row Lua script; mutate `row` and return |
| TASK | `lua.task` | ✓ | Standalone Lua script with host bridges |
| ENGINE | `literal` | ✓ | Constant expressions (built-in) |
| ENGINE | `lua` | ✓ | Lua 5.4 (provider plugin) |
| ENGINE | `ssisexpr` | ✓ | SSIS Expression Language — typed `(DT_*)` casts, 3VL nulls, ~30 functions incl. dates (provider plugin); see [`docs/SSISEXPR.md`](docs/SSISEXPR.md) |

### Missing on purpose at v0.1

- No `parquet.*`, no `kafka.*`.
- No `pivot` / `unpivot` reshape, no window functions.
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
include/betl/        Public C ABI: provider.h, version.h
src/cli/             betl binary (run, validate)
src/loader/          provider registry + dlopen
src/pipeline/        YAML → in-memory pipeline AST
src/runtime/         executor, builtins, transforms, db sinks/lookups
src/yaml/            libyaml-backed parser
providers/betl-lua/      Lua provider plugin (+expression engine)
providers/betl-ssisexpr/ SSIS Expression Language engine
tests/                   C tests; ctest harness; integration tests gated on
                         a sibling Postgres / MSSQL
examples/                Four runnable pipelines with fixtures
schemas/                 JSON Schema for the YAML pipeline format
docs/EXPR_ABI.md         Expression-engine ABI reference
docs/SSISEXPR.md         SSIS-EL function reference (for `.dtsx` migrations)
SPEC.md                  Full project design (text-first, types, ABI, …)
```

## Learn more

- `SPEC.md` — design and rationale, including the type system
  (Apache Arrow), the provider model, and v0.1 vs. v1 commitments.
- `include/betl/provider.h` — authoritative C ABI for components and
  expression engines.
- `docs/EXPR_ABI.md` — companion guide for engine authors.
- `docs/SSISEXPR.md` — SSIS Expression Language reference: supported
  casts, function set, NULL semantics, and what's deferred to v2.
- `examples/` — four end-to-end pipelines covering CSV ingest, star
  build, CRM migration, and SSIS-style date enrichment.

## License

Apache-2.0.
