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
  `expr:`; full Lua scripts for `lua.task` / `lua.map`. Other
  languages (Python, SQL host, …) can be added without touching the
  core.

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

More examples live under `examples/` (CSV-to-Postgres, sales-star
build with a star-schema lookup join, CRM migration with multi-stage
upserts).

## What's in v0.1

| Kind | Component | Status | Notes |
|---|---|---|---|
| SOURCE | `csv.read` | ✓ | Streaming; RFC 4180 quoted fields incl. multi-line; int64 / utf8 |
| SOURCE | `mssql.read` | ✓ | Run a SELECT, stream rows; unixODBC + FreeTDS; nulls supported |
| SOURCE | `betl.gen_int64` / `betl.gen_strings` | ✓ | Test generators |
| SINK | `csv.write` | ✓ | RFC 4180 quoting, header / delimiter |
| SINK | `postgres.upsert` | ✓ | INSERT…ON CONFLICT; 4 conflict modes; libpq |
| SINK | `mssql.upsert` | ✓ | MERGE; 4 conflict modes; unixODBC + FreeTDS |
| SINK | `betl.count_rows` | ✓ | Smoke / assertion sink |
| TRANSFORM | `filter` | ✓ | Predicate via the expression engine |
| TRANSFORM | `map` | ✓ | `add:` (append) and `select:` (project / rename) |
| TRANSFORM | `aggregate` | ✓ | `group_by` + count / sum / min / max |
| TRANSFORM | `sort` | ✓ | Multi-key, asc / desc, stable |
| TRANSFORM | `join` | ✓ | inner / left / outer; multi-key; null-aware output |
| TRANSFORM | `postgres.lookup` | ✓ | Cached SELECT + linear probe; on_miss error/null/drop |
| TRANSFORM | `mssql.lookup` | ✓ | Same model over ODBC |
| TRANSFORM | `lua.map` | ✓ | Per-row Lua script; mutate `row` and return |
| TASK | `lua.task` | ✓ | Standalone Lua script with host bridges |
| ENGINE | `literal` | ✓ | Constant expressions (built-in) |
| ENGINE | `lua` | ✓ | Lua 5.4 (provider plugin) |

### Missing on purpose at v0.1

- No `parquet.*`, no `kafka.*`, no `postgres.read`.
- No scheduler. Wire betl into cron / systemd / Airflow as you
  already do.

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
providers/betl-lua/  Lua provider plugin (+expression engine)
tests/               C tests; ctest harness; integration tests gated on
                     a sibling Postgres / MSSQL
examples/            Three runnable pipelines with fixtures
schemas/             JSON Schema for the YAML pipeline format
docs/EXPR_ABI.md     Expression-engine ABI reference
SPEC.md              Full project design (text-first, types, ABI, …)
```

## Learn more

- `SPEC.md` — design and rationale, including the type system
  (Apache Arrow), the provider model, and v0.1 vs. v1 commitments.
- `include/betl/provider.h` — authoritative C ABI for components and
  expression engines.
- `docs/EXPR_ABI.md` — companion guide for engine authors.
- `examples/` — three end-to-end pipelines covering CSV ingest, star
  build, and CRM migration.

## License

Apache-2.0.
