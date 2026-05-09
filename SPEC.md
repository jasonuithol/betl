# Better ETL (betl) — Spec (v0.1)

**One-line:** A cross-platform, open-source, text-first ETL runtime intended to replace SQL Server Integration Services (SSIS).

Engine in C, pipelines in YAML, types from Apache Arrow, providers as shared libraries with a C ABI, custom logic in Lua / Python / (later) C# / Java / SQL. Decisions resolved so far are listed in §15; remaining open questions in §16.

---

## 1. Goals

1. **Source-control native.** A pipeline is a plain text file. Two engineers editing different parts produce a clean three-way merge. No GUIDs, no XML layout coordinates, no embedded binary blobs.
2. **Linux-first, cross-platform later.** Linux is the only supported target for v1. Native macOS and native Windows are post-v1; the v1 story for both is "run it under Docker or WSL2," which is acceptable. Wherever the engine eventually runs, it has no dependency on SQL Server, the Windows registry, COM, .NET, or Visual Studio.
3. **Built on open standards.** Where there is a well-supported open format (YAML, JSON, XML, CSV) or open interface (ODBC, ADBC, Arrow), betl uses it directly rather than inventing a parallel one. The pipeline file format itself is published, versioned, and free for any tool to read or write.
4. **Don't reinvent the wheel.** Use existing permissively-licensed C libraries for parsing, drivers, and data interchange. Only write our own when the existing ecosystem is genuinely missing a piece.
5. **Pluggable.** Sources, transforms, and sinks are providers loaded at runtime. Adding "read from Kafka" or "write to Snowflake" does not require recompiling the core. Custom processing logic can be written in any embedded scripting language we support (Lua and Python at v1; C# / Java / others later via language-host plugins).
6. **Testable.** Pipelines (and individual components) can be unit-tested without a database, a network, or a GUI.
7. **CLI-first.** Everything the GUI can do, the CLI can do. The GUI (if any) is a thin layer over the CLI and the file format — never the other way around.

## 2. Non-goals (v1)

- **Not a scheduler.** Use cron, systemd timers, Airflow, or whatever you already have. betl runs a pipeline; it does not decide when.
- **Not a BI suite.** No reports, no cubes, no dashboards. SSAS/SSRS analogues are out of scope.
- **Not distributed (yet).** v1 runs on a single machine. Sharding/distribution is a v2 concern; the data model should not preclude it but v1 will not deliver it.
- **Not a perfect SSIS importer.** A best-effort .dtsx → betl converter is desirable but lossy by design — we are not committing to bug-for-bug compatibility with a system whose bugs are the reason this project exists.

## 3. Design principles

- **Text is the source of truth.** The runtime never writes back to the pipeline file. Tools may read it, lint it, render it, but the human owns it.
- **Declarative pipeline, imperative providers.** *What* the pipeline does is declarative YAML/whatever. *How* a provider talks to Kafka is ordinary code in whatever language the provider author chose.
- **Boring formats win.** Prefer YAML over a custom DSL. Prefer JSON / XML / CSV over a custom serialization. Prefer Arrow over a custom in-memory layout. Prefer ODBC / ADBC over a custom DB driver protocol.
- **Vendor with care, link with care.** Permissive licenses (MIT, BSD, Apache-2.0, ISC) for anything we statically link or ship. LGPL is acceptable for dynamic linking (covers most ODBC drivers). GPL/AGPL is incompatible with our intended distribution and will not be linked into the core.
- **No magic globals.** SSIS "package variables" that mutate across components are banned. Data flows through typed ports; configuration flows through typed parameters.
- **Fail loud, fail early.** Pipelines validate before they run. Unknown fields are errors, not warnings. Type mismatches at port boundaries are errors.

## 4. Conceptual model

A pipeline is a directed acyclic graph (DAG) of **steps**. There are two graph layers, intentionally separated (this is where SSIS got it right and we keep it):

- **Control flow** — orchestration. Steps are tasks: "run this data flow", "execute this SQL", "run this shell command", "branch on result". Edges express ordering and conditional dependency (`on_success`, `on_failure`, `always`).
- **Data flow** — row streaming. Steps are sources, transforms, and sinks. Edges express row streams between them. A data flow is itself a single node in the control flow.

This split keeps "I want to run three SQL scripts then a CSV-to-Postgres load" from being modeled as the same kind of thing as "for each row, uppercase the email column".

### 4.1 Components

Three component categories at the data-flow layer:

| Category    | Inputs        | Outputs       | Examples                                  |
|-------------|---------------|---------------|-------------------------------------------|
| Source      | none          | 1+ row streams| `csv.read`, `postgres.query`, `kafka.consume` |
| Transform   | 1+ row streams| 1+ row streams| `filter`, `map`, `lookup`, `join`, `aggregate`, `pivot` |
| Sink        | 1+ row streams| none          | `csv.write`, `postgres.upsert`, `s3.put` |

The standard transforms — those that every conforming engine ships — are spelled out in §4.3.

At the control-flow layer there are **tasks** — anything with side effects that isn't a row pipeline: `sql.execute`, `shell`, `http`, `file.copy`, `dataflow` (which embeds a data flow), etc. See §4.4.

### 4.2 Schemas and ports

Every row stream has a schema: an ordered list of `(name, type, nullable)`. Schemas are checked at validation time wherever they can be inferred. Where they can't (e.g. a generic SQL query), the user can pin them with a `schema:` block and the runtime will verify at runtime.

**Type system: Apache Arrow logical types.** The schema YAML uses Arrow logical-type names (`int64`, `string`, `decimal128(p,s)`, `timestamp[us, UTC]`, `date32`, `list<T>`, `struct<...>`, `dictionary<I,T>`, etc.). We adopt Arrow as both the *specification* of what types mean and the *in-memory representation* between components.

Why this choice:
- It's a published, multi-language, Apache-2.0-licensed standard with stable semantics for the hard cases (decimal precision/scale, timezone-aware timestamps, nested types, nullability).
- The **Arrow C Data Interface** and **Arrow C Stream Interface** define a tiny stable C ABI for handing arrays and streams across a shared-library boundary with no copy and no dependency on libarrow itself. This is the load-bearing piece that makes our shared-library plugin model practical (see §6).
- It makes future ADBC integration (Arrow-native database drivers) trivial.

For v1 the engine may implement a subset of Arrow types. The spec lists the full set; implementations declare which subset they support and reject pipelines that need types they don't.

> **YAML quoting note.** Arrow type names that contain `[`, `<`, `,`, or `>` MUST be quoted when written inside a YAML *flow* mapping (e.g. `{ name: ts, type: "timestamp[us, UTC]" }`). The unquoted form is parsed as a nested flow sequence. Block-style schema entries do not need quoting. `betl fmt` auto-quotes; `betl validate` rejects unquoted-but-ambiguous types with a clear error.

### 4.3 Standard transform set

These transforms are part of the spec and ship with every conforming engine. Provider authors may add more; these are the floor.

#### `filter`

Drops rows where the predicate is false. Schema unchanged.

```yaml
- id: keep_paid
  type: filter
  from: read
  where: "row.amount > 0 and row.status == 'paid'"   # default lang: lua
```

`where:` accepts a string (shorthand for `{lang: lua, expr: <string>}`) or a full expression-engine map (`{lang: ..., expr: ...}`).

#### `map`

Adds, replaces, or projects columns. Two modes — pick exactly one per step:

**`add:`** *(additive)* — keeps every input column and appends the listed ones.

```yaml
- id: tag_load
  type: map
  from: drop_zero_qty
  add:
    load_date:   { lang: literal, value: ${params.load_date} }
    is_high_val: { lang: lua,     expr:  "row.amount > 1000" }
```

**`select:`** *(replacing the column set)* — the output schema is exactly the listed columns, in this order. Anything not listed is dropped. Three column-spec forms:

```yaml
- id: project
  type: map
  from: enrich
  select:
    - customer_id                                            # pass-through
    - { name: full_name,   from: name }                      # rename only
    - { name: status,      expr: "string.upper(row.status or 'UNKNOWN')" }
    - { name: load_date,   lang: literal, value: ${params.load_date} }
```

A `map` step uses `add:` xor `select:`, never both.

#### `lookup`

Resolves a foreign key against a reference table — a one-sided indexed join, distinct from the general `join`. Used heavily for surrogate-key resolution in star-schema loads.

```yaml
- id: lookup_customer_sk
  type: lookup
  from: read_transactions
  connection: olap
  table: olap.dim_customer
  match:    { nk_customer: nk_customer }   # input col : lookup col
  select:   { sk_customer: sk_customer }   # output col : lookup col
  on_miss: error                           # error | null | drop
```

Differences from `join`:
- One-sided: the right side is a static table, not a stream.
- Single-row result: `match` resolves to at most one row per input row; multi-match is an error.
- `on_miss` policy: explicitly named.
- The runtime is free to cache the right side in memory, push the lookup into the source DB as a JOIN, or use whatever strategy fits.

#### `join`

Two-stream join. Distinct from `lookup` in that the right side is itself a stream and may produce multiple matches per left row.

```yaml
- id: enrich_orders
  type: join
  left:  read_orders
  right: read_customers
  on:    { customer_id: id }
  kind:  inner          # inner | left | right | full
```

#### `aggregate`

Groups rows and computes per-group aggregates. End-of-stream produces one row per group.

```yaml
- id: daily_grain
  type: aggregate
  from: project
  group_by: [sk_date, sk_customer, sk_product]
  compute:
    units_sold:    { agg: sum,   over: quantity   }
    gross_revenue: { agg: sum,   over: line_total }
    line_count:    { agg: count }
```

Built-in aggregations: `sum`, `count`, `count_distinct`, `min`, `max`, `avg`, `first`, `last`. Provider-supplied custom aggregations are addressed via the expression engine system.

#### `pivot` / `unpivot`

Reshape long-form to wide-form and back. Spec'd in v0.2; v0.1 engines may omit.

#### `union`

Concatenates rows from multiple input streams that share a schema.

```yaml
- id: all_orders
  type: union
  from: [orders_us, orders_eu, orders_jp]
```

Schemas must match; mismatches are validate-time errors.

#### `sort`

Orders rows by one or more keys. Materializes the full stream — cheap for small batches, memory-heavy for large ones. The runtime may spill to disk.

```yaml
- id: by_date
  type: sort
  from: orders
  by:
    - { col: order_date, dir: asc }
    - { col: order_id,   dir: asc }
```

### 4.4 Tasks (control-flow)

A task is a control-flow step — a unit of work with side effects, not a row pipeline. Every task supports the common control-flow keys (`after:`, `on_failure:`, `retries:`, `timeout:`) regardless of type.

#### `sql.execute`

Runs SQL against a connection. Two forms — `sql:` for inline, `file:` for external:

```yaml
- id: refresh_marts
  type: sql.execute
  connection: olap
  file: sql/refresh_marts.sql
  after: [load_fact_sales]
  on_failure: stop
```

```yaml
- id: row_count_check
  type: sql.execute
  connection: warehouse
  sql: |
    SELECT COUNT(*) AS rows_loaded
    FROM stg.orders
    WHERE load_date = :load_date
  params:
    load_date: ${params.load_date}
  expect:
    rows_loaded: { min: 1 }
```

Parameters are bound by name with `:name` placeholders and supplied via the `params:` map. The provider performs proper parameter binding — never string concatenation — so this is safe against injection.

`expect:` is an optional post-condition guard. If the query returns rows, named columns are matched against expectations:

| Form                          | Meaning                                  |
|-------------------------------|------------------------------------------|
| `{name: <value>}`             | exact equality                           |
| `{name: {min: N}}`            | column ≥ N                               |
| `{name: {max: N}}`            | column ≤ N                               |
| `{name: {between: [N, M]}}`   | N ≤ column ≤ M                           |
| `{name: {not_null: true}}`    | column not null                          |
| `{name: {one_of: [a, b, c]}}` | column ∈ set                             |

Expectations are evaluated against the first row by default; on multi-row results the user passes `expect.row: all` to require every row to satisfy.

#### `shell`

Runs a shell command. The command is the argv array, *never* a single string fed to a shell — no shell-injection surface.

```yaml
- id: snapshot_done
  type: shell
  argv: ["touch", "${params.dest_dir}/.complete"]
  after: [load_fact_sales]
```

`stdout:` and `stderr:` may be `capture | discard | inherit`.

#### `http`

Issues an HTTP request — used for webhooks, status pings, kicking off downstream services.

```yaml
- id: notify_done
  type: http
  url: ${env.SLACK_WEBHOOK}
  method: POST
  body_json: { text: "Pipeline ${pipeline.name} done in ${run.duration_s}s" }
  after: [refresh_marts]
  on_failure: continue            # don't fail the pipeline if Slack is down
```

#### `file.copy`, `file.move`, `file.delete`

Local filesystem ops. Cross-host transfers go through provider-specific tasks (`s3.put`, `sftp.put`, etc.).

#### `dataflow`

The wrapper task that embeds a data flow as a single node in the control flow. See the example in §5.

### 4.5 Common control-flow keys

Every step at the top level (i.e. every direct child of `pipeline:`) supports these:

| Key            | Type         | Meaning                                                              |
|----------------|--------------|----------------------------------------------------------------------|
| `id`           | string       | Required. Unique within the pipeline.                                |
| `type`         | string       | Required. Component or task type (see §4.3, §4.4, or any provider).  |
| `after`        | list[string] | Step IDs that must succeed before this one runs.                     |
| `on_failure`   | enum         | `stop` (default) \| `continue` \| `retry`                            |
| `retries`      | int          | When `on_failure: retry` — attempts after the first.                 |
| `retry_backoff`| string       | When retrying — `"1s"`, `"5s,10s,30s"` for fixed-then-escalating.    |
| `timeout`      | string       | Per-attempt wall-clock cap, e.g. `"30s"`, `"5m"`, `"2h"`.            |
| `condition`    | expression   | Skip this step if the predicate is false.                            |
| `description`  | string       | Free-text doc, surfaced by `betl plan`.                              |

## 5. Pipeline file format

**Surface syntax: YAML.** Universally familiar, well-tooled, diffs cleanly given a `betl fmt` canonicalizer, and consistent with the rest of the modern data tooling (Argo, GitHub Actions, dbt, Meltano, NiFi flow definitions).

Parser: **libyaml** (MIT) for the engine. Implementations in other languages use whatever Apache-2.0/MIT/BSD YAML library their ecosystem provides.

Sketch:

```yaml
betl: 1                                 # spec version

parameters:
  src_dir:   { type: string, required: true }
  load_date: { type: date,   default: today }

connections:
  warehouse:
    type: postgres
    dsn: ${env.WAREHOUSE_DSN}           # secrets via env, never inline

pipeline:
  - id: ingest_orders
    type: dataflow
    steps:
      - id: read
        type: csv.read
        path: ${params.src_dir}/orders-${params.load_date}.csv
        schema:
          - { name: order_id,   type: int64 }
          - { name: customer_id,type: int64 }
          - { name: amount,     type: decimal(12,2) }
          - { name: ordered_at, type: timestamp }
      - id: tag_load
        type: map
        from: read
        add:
          load_date: ${params.load_date}
      - id: write
        type: postgres.upsert
        from: tag_load
        connection: warehouse
        table: stg.orders
        key: [order_id]

  - id: refresh_marts
    type: sql.execute
    connection: warehouse
    file: sql/refresh_marts.sql
    after: [ingest_orders]
    on_failure: stop
```

Key properties of this format:

- **No IDs but the ones you wrote.** No GUIDs, no auto-generated keys. Renaming a step is a real edit, not a database migration.
- **No layout.** No `x:`, `y:`, `width:`. A renderer can lay it out automatically; the source file does not carry visual coordinates.
- **Edges are by name, not by index.** `from: tag_load` is stable across edits.
- **Secrets are never inline.** Connections must reference `${env.X}`, `${secret.X}`, or a file path; literal passwords are a lint error.
- **One file or many.** A pipeline can `include:` other files. Encouraged for connection bundles, schema bundles, reusable subgraphs.

### 5.1 File identification and extensions

A file is identified as a betl document by **content, not by extension**. The runtime considers a YAML file to be a betl document if its top-level mapping contains a recognized betl discriminator key:

| Key             | Document kind                                        |
|-----------------|------------------------------------------------------|
| `betl: <ver>`   | Pipeline definition (the primary kind)               |
| `betl_connections: <ver>` | Standalone connection bundle (future)      |
| `betl_schema: <ver>`      | Standalone schema bundle (future)          |

The value is the spec version. Files without a discriminator key are silently ignored by `betl run <dir>` even if they have a betl-looking extension.

Recommended file-naming conventions (not enforced):

- **`.betl.yml`** — preferred when betl files live alongside non-betl YAML (e.g. in a repo that also has `docker-compose.yml`, CI workflow files, etc.). Editors highlight it as YAML for free; globs stay clean.
- **Plain `.yml` / `.yaml`** — fine in repos where everything is betl. Day-one editor support, no plugin required.
- **`.btl` and other custom extensions** — accepted but discouraged. You lose YAML tooling (yamllint, yq, Spectral, editor highlighting) for no real gain, since the discriminator already provides identification.

`betl run <dir>` walks the directory, reads the head of each `*.yml` / `*.yaml` / `*.btl` candidate, and selects files whose top-level mapping contains a recognized discriminator. Other YAML in the same directory is left alone.

### 5.2 Parameters

Pipelines declare a `parameters:` block at the top level. Parameters are typed; values come from the CLI (`--param name=value`), environment, or defaults.

```yaml
parameters:
  src_dir:
    type: string
    required: true
    doc: Where the daily drop files land.
  load_date:
    type: date
    default: today          # recognized literal default; see below
  retries:
    type: int32
    default: 3
  dry_run:
    type: bool
    default: false
```

Per-parameter keys:

| Key        | Meaning                                                                  |
|------------|--------------------------------------------------------------------------|
| `type`     | Required. Any Arrow logical type, plus `bool`, `int32`, `int64`, `string`, `date`, `timestamp`, `decimal128(p,s)`. |
| `required` | Default false. If true, the runtime errors out unless a value is supplied. |
| `default`  | A literal value or one of the recognized sentinel literals (below).      |
| `doc`      | One-line description, surfaced by `betl plan` and `betl run --help`.     |
| `enum`     | Optional list of allowed values; mismatch is a validate-time error.       |

**Sentinel default literals** for time-typed parameters:

| Sentinel | Type            | Resolves to                                              |
|----------|-----------------|----------------------------------------------------------|
| `today`  | `date`          | The local date at run start (or UTC date with `--utc`).  |
| `now`    | `timestamp[us]` | The wall-clock time at run start, microsecond precision. |

These are evaluated once at run start; every reference to `${params.load_date}` within the run sees the same value.

CLI override:

```
betl run pipeline.betl.yml --param src_dir=/data --param load_date=2026-05-04
```

Within the pipeline, parameters are referenced as `${params.<name>}`.

### 5.3 Connections

A `connections:` block at the top level declares named connections. The `dsn:` field is the only required attribute on every connection; provider-specific attributes (`pool_max:`, `application_name:`, etc.) are namespaced by the provider.

```yaml
connections:
  warehouse:
    type: postgres
    dsn: ${env.WAREHOUSE_DSN}
    pool_max: 4               # provider-specific
```

**Secrets policy.** Inline literal credentials are a lint error. Acceptable sources:

- `${env.<NAME>}` — environment variable.
- `${secret.<NAME>}` — provider-pluggable secret store (Vault, AWS Secrets Manager, age-encrypted file, etc.).
- A file path to a connection-info file outside the repo.

`betl validate` rejects pipelines whose `dsn:` looks like an unredacted credential.

#### Connection-bundle files

Connections can be split out into their own file with the `betl_connections:` discriminator. The file contains exactly one top-level key — `connections:` — with the same shape as the inline form:

```yaml
betl_connections: 1

connections:
  warehouse:
    type: postgres
    dsn: ${env.WAREHOUSE_DSN}
  reporting:
    type: mssql
    dsn: ${env.REPORTING_DSN}
```

Use a connection bundle when:
- The same connections are referenced by several pipelines.
- A test environment overrides a subset of connections without touching the pipeline.
- A team wants the connections file under different review than the pipeline.

A pipeline pulls in a bundle via `include:` (next section).

### 5.4 Includes

A pipeline may incorporate other betl files via a top-level `include:` directive:

```yaml
betl: 1

include:
  - ./connections.betl.yml          # path relative to this file
  - ../shared/schemas.betl.yml

# ... rest of pipeline
```

**Resolution rules:**

- Paths are resolved relative to the file declaring the `include:`.
- Includes are processed before the rest of the file: top-level keys from the included file are merged into the parent's top-level keys.
- For mapping-valued keys (`connections:`, `parameters:`, etc.) the include's entries are added to the parent's; **a key collision is a validate-time error** — silent override is too dangerous in this domain.
- For list-valued keys (`pipeline:`) the include's entries are appended to the parent's. (Rarely useful at the pipeline level; common for shared `parameters:` / `connections:`.)
- Includes may include other includes; cycles are a validate-time error.
- An included file's discriminator must be compatible: a `betl_connections:` file may only contribute `connections:`; a `betl: <ver>` file may contribute anything.

## 6. Provider model

A provider supplies one or more component types. Providers live outside the core and are loaded at runtime.

**Provider interface: in-process shared library** (`.so` / `.dylib` / `.dll`). The engine `dlopen`s providers at startup and calls into them via a C ABI. Data crosses the boundary as Arrow C Data Interface arrays and Arrow C Stream Interface streams — a tiny pair of headers, no libarrow dependency on either side, zero-copy batches.

We acknowledge the risks (ABI versioning, in-process crashes affecting the run) and accept them for v1 in exchange for the perf and the simplicity of the data path. If they bite us hard, we can introduce a subprocess transport later **without changing the component-author API** — the boundary will be Arrow either way.

### 6.1 Provider ABI (sketch)

A provider exports a fixed entry point that returns a manifest plus vtable:

```c
/* betl-provider.h, simplified */
typedef struct BetlComponentDef {
    const char *name;             /* e.g. "postgres.upsert"                 */
    BetlComponentKind kind;       /* SOURCE | TRANSFORM | SINK | TASK       */
    const char *config_schema;    /* JSON Schema for the YAML config block  */
    /* lifecycle */
    int (*init)   (BetlContext *ctx, const BetlConfig *cfg, void **state);
    int (*start)  (void *state);
    /* data path: ports use Arrow C Stream Interface (struct ArrowArrayStream)*/
    int (*open_input) (void *state, int port_idx, struct ArrowArrayStream *in);
    int (*open_output)(void *state, int port_idx, struct ArrowArrayStream *out);
    int (*step)   (void *state);                  /* may be called repeatedly */
    int (*finish) (void *state);
    void (*destroy)(void *state);
} BetlComponentDef;

typedef struct BetlProvider {
    uint32_t abi_version;                          /* must match host        */
    const char *name;
    const char *version;
    const char *license;
    const BetlComponentDef *components;
    size_t component_count;
} BetlProvider;

const BetlProvider *betl_provider_entry(void);    /* the only required symbol */
```

The host:
1. Reads `BETL_PATH` (and OS-standard library dirs) for `betl-*.{so,dylib,dll}`.
2. Loads each, calls `betl_provider_entry`, checks `abi_version`.
3. Indexes components by name; pipeline references like `type: postgres.upsert` resolve here.

ABI policy: the host promises forward compatibility within an ABI major version. Adding fields to vtable structs is a major bump; appending new vtable structs is a minor bump. Providers declare which ABI they were built against; mismatched majors refuse to load with a clear error.

### 6.2 Language-host providers

Custom processing logic in a non-C language is implemented as a **language-host provider** — a normal C shared library that embeds an interpreter and exposes user scripts as components.

v1:
- `betl-lua` — embeds Lua 5.4 (MIT). Lua is small, sandboxable, embeds beautifully in C, handles row-at-a-time logic without ceremony. Best fit for inline expressions and simple per-row transforms.
- `betl-python` — embeds CPython via the stable C API (Python license, GPL-compatible, OK for our distribution). Best fit when the user wants to reach for pandas / numpy / requests / etc. Heavier process footprint than Lua.

The user's pipeline calls them by name:

```yaml
- id: clean_name
  type: lua.map        # provided by betl-lua
  from: read
  script: |
    row.full_name = row.first .. " " .. row.last
    return row

- id: enrich
  type: python.transform   # provided by betl-python
  from: clean_name
  module: enrichments.geocode
  function: geocode_batch
```

Later: `betl-mono` (C# via Mono — LGPL, dynamic-link OK), `betl-jvm` (Java/Kotlin via JNI). Same pattern; same ABI.

#### `betl-lua` standard helper library

The `betl-lua` provider exposes a small standard helper module on top of vanilla Lua 5.4. Provider authors and pipeline authors can both rely on these being present:

| Helper                          | Purpose                                                  |
|---------------------------------|----------------------------------------------------------|
| `sha256(s)` / `sha1(s)` / `md5(s)` | Hash a string, return lowercase hex.                  |
| `base64.encode(s)` / `base64.decode(s)` | Standard Base64 codec.                          |
| `json.encode(t)` / `json.decode(s)` | JSON ↔ Lua table.                                    |
| `uuid.v4()` / `uuid.v7()`       | Random / time-ordered UUIDs.                              |
| `regex.match(s, pat)`           | PCRE-flavour regex (via libpcre2).                       |
| `time.now()` / `time.parse(s, fmt)` / `time.format(t, fmt)` | RFC-3339 / strftime time helpers. |
| `log.debug(s)` / `log.info(s)` / `log.warn(s)` / `log.error(s)` | Structured logs into the host's log pipeline. |

Pipeline authors can register additional helpers via a `lua_init:` block at the top of the pipeline that runs once per worker before any `lua.map` step:

```yaml
lua_init: |
  local rates = json.decode(io.open("/etc/betl/fx.json"):read("*a"))
  function fx_rate(from, to) return rates[from .. "_" .. to] or error("no rate") end
```

The host enforces a wall-clock and memory cap on every Lua execution regardless of what the script does.

### 6.3 Provider manifest (YAML, alongside the .so)

For tooling and documentation; the runtime can also extract it via the C entry point.

```yaml
provider: betl-postgres
version: 0.3.1
license: Apache-2.0
abi: 1
components:
  - name: postgres.query
    kind: source
    config_schema: { ... }
    output_schema: dynamic    # determined at validate-time from the SQL
  - name: postgres.upsert
    kind: sink
    config_schema: { ... }
    input_schema: required
```

### 6.4 Standard provider conventions

Components are nominally per-provider, but a handful of operations are common enough across sinks and sources that consistent naming and semantics matter. Conforming providers SHOULD follow these conventions where applicable:

#### Upsert sinks

A sink whose name ends in `.upsert` (e.g. `postgres.upsert`, `mssql.upsert`, `mysql.upsert`, `sqlite.upsert`) accepts the following config:

| Key            | Required | Meaning                                                              |
|----------------|----------|----------------------------------------------------------------------|
| `connection`   | yes      | Connection name to write through.                                    |
| `table`        | yes      | Schema-qualified target table.                                       |
| `key`          | yes      | List of column names that uniquely identify a row (the upsert key). |
| `on_conflict`  | no       | One of `update` (default), `update_if_changed`, `ignore`, `error`.   |
| `columns`      | no       | Explicit column list to write; defaults to the input schema.         |
| `batch_size`   | no       | Hint; the runtime may override.                                      |

`on_conflict` semantics:

| Mode                   | When key exists                                                              |
|------------------------|------------------------------------------------------------------------------|
| `update` (default)     | Overwrite all non-key columns with the incoming row.                         |
| `update_if_changed`    | Diff incoming vs. existing; UPDATE only when at least one non-key col differs. Avoids spurious row rewrites and useless triggering of `updated_at` columns. |
| `ignore`               | Leave the existing row alone.                                                |
| `error`                | Fail the step.                                                               |

#### Query sources

A source whose name ends in `.query` (e.g. `postgres.query`, `mssql.query`) accepts:

| Key          | Required | Meaning                                                              |
|--------------|----------|----------------------------------------------------------------------|
| `connection` | yes      | Connection name to read through.                                     |
| `sql`        | one of   | Inline SQL.                                                          |
| `file`       | one of   | Path to a `.sql` file (relative to the pipeline file).               |
| `params`     | no       | Map of `:name` placeholder → value. Bound by name, not concatenated. |
| `schema`     | no       | Optional Arrow schema to pin the output; runtime verifies.           |

#### File sources / sinks

`csv.read` / `csv.write` / `json.read` / `json.write` / `xml.read` / `xml.write` accept a `path:` (which may include `${params.X}` substitutions) and a `schema:` block (required for sources unless the file is self-describing).

These are conventions, not enforcements — a provider may name its components however it likes. But consistency across providers is a usability win, so first-party providers will follow these names and the spec recommends third parties do too.

## 7. Expressions

Expression evaluation is needed for: parameter defaults, `map` columns, `filter` predicates, conditional control-flow edges. Different users will reasonably want different languages for this.

**Expression engines are pluggable.** An expression engine is a flavour of provider that exposes the C ABI from §6.1 with one extra vtable: evaluate-expression-against-row-batch. The pipeline tags each expression with the engine it should be evaluated by:

```yaml
- id: keep_big_orders
  type: filter
  from: read
  where:
    lang: lua                       # which engine
    expr: "row.amount > 1000"

- id: tag_load
  type: map
  from: keep_big_orders
  add:
    region:
      lang: python
      expr: "lookup_region(row.country)"
    load_date:
      lang: literal                 # constant — no engine needed
      value: ${params.load_date}
```

A pipeline-wide default engine can be set so users who only use one don't have to write `lang:` everywhere.

v1 engines:
- **`literal`** — built-in. YAML scalars, parameter substitution, string interpolation. No code execution. Used everywhere user input is structurally fixed.
- **`lua`** — same `betl-lua` provider as §6.2. Lightweight, fast per-row, sandboxable. **This is the default `lang:` for any `expr:` block that doesn't declare one.**
- **`python`** — same `betl-python` provider as §6.2. Heavy but powerful; gives you pandas, regex, datetime, requests, the lot. Opt-in via `lang: python`.

The default is Lua because (a) inline expressions are usually one-liners that don't justify a 25 MB CPython process, (b) Lua's per-thread `lua_State` model lets us hand a fresh sandbox to each worker without the GIL, and (c) Lua's interpreter init is microseconds vs. CPython's tens to hundreds of milliseconds — relevant for short-lived runs and the test loop. Python remains a first-class citizen for the cases where it earns its weight.

Later, on the same expression-engine plumbing:
- **`csharp`** — via `betl-mono`.
- **`java`** — via `betl-jvm`.
- **`sql`** — a SQL-alike row-expression engine. Probably built on **DuckDB** (MIT) since it already has a complete row-expression evaluator with Arrow-native input. Predicates and projections in SQL syntax with no separate query planner.

Sandboxing and resource limits are the engine plugin's responsibility; the host enforces wall-clock and memory caps at the component level regardless.

## 8. Execution semantics

- **Streaming by default** at the data-flow layer. Sources emit batches; transforms process batches; sinks consume batches. Backpressure is the runtime's job.
- **Batch size is a runtime concern**, not a pipeline concern. The user does not pick "10000 rows per buffer" in the pipeline file. The runtime chooses; the user can hint via the `runtime:` block but defaults work.
- **At-least-once delivery is the default**, with optional **exactly-once** when both source and sink support transactional checkpoints (e.g. Kafka offsets + Postgres TX).
- **Failures are explicit.** Every step has `on_failure: stop | continue | retry`, and when retrying, `retries: <N>` and optional `retry_backoff:`. Default `on_failure` is `stop`. No silent error sinks. See §4.5 for the full control-flow key set.
- **Idempotency is the user's problem**, but the runtime gives them the tools: deterministic batch IDs, transactional sinks where the underlying system supports them, and the `retry` failure mode.

### 8.1 Concurrency

Concurrency is **configurable**, with sensible defaults and three independent levers:

| Lever              | What it controls                                                | Default     |
|--------------------|-----------------------------------------------------------------|-------------|
| `control_workers`  | How many control-flow tasks may run in parallel (e.g. two unrelated dataflows that don't depend on each other) | `auto` (= number of CPU cores) |
| `dataflow_workers` | How many components within a single dataflow may run on different threads simultaneously | `auto` |
| `component_threads`| Per-component thread-pool size for components that internally parallelize (sort, hash-join build, etc.) | `auto` |

Configurable globally in the pipeline file:

```yaml
runtime:
  concurrency:
    control_workers:  4
    dataflow_workers: auto
    component_threads: 2
```

Overridable at the CLI:

```
betl run pipeline.yml --concurrency control_workers=1 --concurrency dataflow_workers=8
```

And per-component when a component opts in:

```yaml
- id: heavy_sort
  type: sort
  from: read
  by: [order_id]
  concurrency: 8        # override component_threads for this step only
```

A component's ABI declares whether it is thread-safe to invoke from multiple workers concurrently; the scheduler respects that. The default execution shape — one thread per active dataflow component, with batches flowing through bounded queues between them — is the same shape that gives SSIS its "data flow buffer" parallelism, but without SSIS's hidden buffer-allocation rules.

## 9. Observability

- Structured JSON logs to stdout by default. One log line per task start/end, with parameters, row counts, byte counts, duration, exit status.
- OpenTelemetry traces optional, off by default. One span per task, parent span per pipeline run.
- Metrics: rows-in / rows-out / bytes / duration / errors per component, exposed via Prometheus textfile or stdout.
- A run produces a **run manifest** — JSON file describing what ran, what its inputs were, what its outputs were, when, with what versions of which providers. This is the audit log SSIS doesn't really have.

## 10. Tooling

The reference implementation ships these CLI verbs:

| Command          | Purpose                                                       |
|------------------|---------------------------------------------------------------|
| `betl validate`  | Static check: parse, schema, connection refs, types.          |
| `betl plan`      | Render the resolved DAG (no execution).                       |
| `betl run`       | Execute the pipeline.                                         |
| `betl test`      | Run pipeline tests against fixtures.                          |
| `betl fmt`       | Canonicalize whitespace/key order so diffs stay clean.        |
| `betl providers` | List installed providers and their components.                |
| `betl import-dtsx` | Best-effort SSIS package import.                            |

A graphical renderer (web or desktop) is desirable but **strictly optional** and read-mostly: it visualizes the YAML, may scaffold edits, but the YAML wins on every conflict. No round-trip via a binary cache.

## 11. Testing

Two layers:

1. **Pipeline tests.** A `tests/` directory holds fixture inputs and expected outputs. `betl test` runs the pipeline against each fixture and diffs the output. Golden-file style.
2. **Component tests.** Provider authors write their own; not betl's problem.

Mockable connections: a connection can be redirected via `--connection warehouse=sqlite::memory:` at the CLI, so a Postgres pipeline can be tested against SQLite where the SQL dialect overlaps. Where it doesn't, the user runs against a real Postgres in a container — and we document the container recipe.

## 12. Packaging and deployment

- A pipeline is a directory: pipeline file(s), SQL files, fixtures, optional `betl.lock` pinning provider versions.
- `betl bundle` produces a tarball of that directory plus a manifest.
- `betl run path/to/dir` or `betl run path/to/bundle.tar.gz` are equivalent.
- No "deploy to server" step. If you want a server, run `betl run` on a server. The runtime is the deployment unit.

## 13. Implementation language and dependencies

**Engine: C** (C11, portable). Built with the c-build MCP toolchain (gcc 13 / clang, CMake or Meson), tested under valgrind / AddressSanitizer.

Anticipated upstream dependencies for the v1 engine — all permissively licensed, all dynamically or statically linkable:

| Concern                  | Library                          | License      | Notes                                           |
|--------------------------|----------------------------------|--------------|-------------------------------------------------|
| YAML parsing             | libyaml                          | MIT          | de-facto C YAML parser                          |
| JSON parsing             | cJSON or yyjson                  | MIT          | yyjson if perf matters, cJSON if simplicity does|
| XML parsing              | libxml2                          | MIT          | for XML data sources/sinks and dtsx import      |
| CSV parsing              | libcsv                           | LGPL-3.0     | dynamic link; or vendor a tiny BSD parser       |
| In-memory rows           | Arrow C Data Interface (headers) | Apache-2.0   | tiny header pair; engine ships its own copy     |
| Optional Arrow runtime   | nanoarrow                        | Apache-2.0   | minimal C Arrow lib, way smaller than libarrow  |
| ODBC                     | unixODBC / iODBC                 | LGPL-2.1     | dynamic link only                               |
| ADBC                     | apache/arrow-adbc (C)            | Apache-2.0   | preferred over ODBC where a driver exists       |
| Lua scripting            | Lua 5.4                          | MIT          | embedded by `betl-lua` provider                 |
| Python scripting         | CPython stable C API             | PSF (GPL-OK) | embedded by `betl-python` provider              |
| SQL expression engine    | DuckDB                           | MIT          | embedded by `betl-sql` engine (later)           |
| Logging                  | (built in, JSON to stdout)       | —            | no dep                                          |
| OpenTelemetry            | opentelemetry-c                  | Apache-2.0   | optional                                        |

**License rules:**
- The betl engine and first-party providers are **Apache-2.0**.
- Anything we statically link into the engine binary must be permissive (MIT / BSD / Apache-2.0 / ISC / Zlib).
- LGPL is acceptable for **dynamically loaded** libraries (this is how every distro ships unixODBC).
- GPL / AGPL: we will not link these into the core. A user is free to write a GPL provider; that's their licensing problem, not ours.
- Each shipped provider declares its license in the manifest so `betl providers` can show users what they're running.

## 14. Migration from SSIS

Out of scope for the v1 spec, but the design must not preclude it. `betl import-dtsx` is a separate tool that:

- Parses the .dtsx XML.
- Maps recognized components 1:1 where possible (Flat File Source → `csv.read`, OLE DB Destination → `postgres.upsert`/`mssql.upsert`/etc.).
- Emits a YAML pipeline plus a report listing every component it could not translate, with the original XML for those nodes attached as comments.

It is explicitly *not* a goal to be able to import every .dtsx. It is a goal to import the boring 80% and tell the human exactly where the other 20% is.

## 15. Decisions made

Recorded for posterity so we don't relitigate them:

- **Name:** **Better ETL**, "betl" for short. Binary: `betl`. File extension: TBD (`.betl.yml` or `.btl` — minor, defer).
- **Engine language:** C (C11). Portable, MCP-supported toolchain, embeds interpreters easily.
- **Primary target platform:** Linux only for v1. Both macOS and Windows are post-v1; v1 users on either run via Docker / WSL2.
- **Custom-code languages:** Lua and Python at v1 via language-host providers; C# (Mono) and Java later on the same plumbing.
- **Type system:** Apache Arrow logical types in the spec; Arrow C Data / Stream Interfaces across the plugin ABI.
- **Surface syntax:** YAML (libyaml in the engine).
- **Provider interface:** in-process shared library with C ABI. Subprocess transport reserved as a future option; component-author API will not change if we add it.
- **Expressions:** pluggable expression engines (`literal`, `lua`, `python` at v1; `sql`/`csharp`/`java` later). **Default engine is `lua`** for any `expr:` block that doesn't declare a `lang:`. Python is opt-in.
- **Concurrency:** configurable, three levers (`control_workers`, `dataflow_workers`, `component_threads`), all defaulting to `auto`. Per-component override allowed where the component declares thread-safety. See §8.1.
- **Standard formats supported v1:** YAML, JSON, XML, CSV in/out as data formats; ODBC and ADBC for relational drivers.
- **License posture:** engine and first-party providers Apache-2.0; permissive deps statically linked; LGPL deps dynamically linked only; no GPL/AGPL in the core.
- **Versioning:** the spec is v0.1; the engine starts at 0.x. Pre-1.0 we reserve the right to break things; once we ship 1.0, semver applies. No bikeshedding required at this stage.
- **File identification:** by **content** (top-level discriminator key — `betl:` / `betl_connections:` / `betl_schema:`), not by extension. Recommended convention is `.betl.yml`; plain `.yml` is fine; `.btl` is accepted but discouraged. See §5.1.
- **Standard transform set, standard task set, common control-flow keys:** documented in §4.3, §4.4, §4.5. Floor that every conforming engine ships: `filter`, `map`, `lookup`, `join`, `aggregate`, `union`, `sort`; `sql.execute`, `shell`, `http`, `file.*`, `dataflow`.
- **`map` modes:** `add:` (additive) xor `select:` (replacing the column set). `select:` supports three forms — pass-through, `{name, from}` rename, `{name, expr}` computed.
- **`sql.execute`:** inline `sql:` xor `file:`, named `:param` placeholders bound via `params:`, optional `expect:` post-conditions guard with min/max/between/not_null/one_of predicates.
- **Parameter sentinel literals:** `today` (date) and `now` (timestamp[us]) are recognized defaults. See §5.2.
- **Connections:** declared inline or in a `betl_connections:` bundle file pulled in via `include:`. See §5.3.
- **Includes:** top-level merge of mapping keys, append for list keys, conflicts are validate-time errors, cycles are validate-time errors. See §5.4.
- **Standard sink/source conventions:** `*.upsert` sinks and `*.query` sources have a defined config-key set; `on_conflict` modes are `update | update_if_changed | ignore | error`. See §6.4.
- **`betl-lua` standard helper library:** `sha256` / `base64` / `json` / `uuid` / `regex` / `time` / `log` namespaces, plus a per-pipeline `lua_init:` block for user-defined helpers. See §6.2.
- **YAML quoting rule:** Arrow types containing `[`, `<`, `,`, or `>` MUST be quoted in flow-mapping style. Block-style schema entries don't need it. `betl fmt` auto-quotes. See §4.2.

## 16. Still open

*(Nothing material — all v0.1 design decisions resolved. Subsequent work moves to building artifacts: the C provider ABI header (done), JSON Schema for the pipeline file, repo skeleton, and the C engine itself.)*
