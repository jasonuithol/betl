# betl — Spec Core

**Version: 1 (draft)**

This document defines the **runtime-neutral contract** that any conforming
betl implementation must honor. It is deliberately smaller than the full
`SPEC.md` reference, which describes the betl-legacy (C/Lua) implementation
in detail. SPEC.md is one valid implementation of this contract; future
implementations (e.g. betl.dotnet) implement the same contract differently.

## Status of this document

- **Authoritative** for: the pipeline file format, step type names and
  semantics, the placeholder mechanism, the connection contract, the type
  system, and validation behavior.
- **Not authoritative** for: expression language semantics, embedded
  scripting language semantics, provider implementation details, runtime
  architecture, CLI surface, tooling.

If `SPEC.md` and this document disagree on something that this document
covers, this document wins and `SPEC.md` is wrong.

---

## 1. What betl is, in one paragraph

A betl pipeline is a directed acyclic graph of steps, declared in plain
text (YAML or JSON), executed by a conforming runtime. Pipelines describe
*what* should happen to data — sources, transforms, sinks, tasks — using
a small set of spec-defined step types. Where steps need application
logic (expressions or scripts), the pipeline declares the language and
text; the runtime supplies the evaluator. The same pipeline file is
valid input to any conforming runtime that supports the step types,
provider connections, and expression languages it uses.

## 2. What this spec does and does not promise

### Promises
- The structural part of a pipeline — graph, step types, ports, types,
  validation rules, connection contract — is portable across all
  conforming runtimes.
- A pipeline that uses only spec-defined step types, the spec-defined
  type system, and the designated portable expression language
  (§7.3) runs identically on every conforming runtime.

### Does not promise
- That a pipeline using runtime-specific scripts (`type: dotnet.script`,
  `type: lua.script`, …) runs on any runtime other than the one whose
  language it targets. Such pipelines are *partially portable*: the
  structure transports, the script does not.
- That expression languages have identical semantics across runtimes
  unless they are the portable LCD (§7.3).
- That all runtimes ship every provider. A pipeline that uses
  `type: mssql.read` requires a runtime that ships an mssql provider.

## 3. Conformance

A **conforming runtime** MUST:

1. Parse pipeline files in the format defined in §4.
2. Implement every step type in §6 with the semantics specified.
3. Implement the Arrow-based type system in §5.
4. Honor the `lang:` placeholder mechanism in §7.
5. Implement at least one expression language and advertise which.
6. Reject any pipeline that references a step type, provider, type, or
   `lang:` value it does not support — with a clear error naming the
   missing piece — before executing any step.
7. Validate pipelines completely before execution (`betl validate`
   semantics) and refuse to run a pipeline that does not validate.

A conforming runtime MAY:

- Ship additional step types beyond the spec floor (`kafka.consume`,
  `s3.put`, etc.). These are provider extensions; using them locks the
  pipeline to runtimes that ship the same provider.
- Implement additional expression languages beyond its declared
  default.
- Reject features in §6 that it does not implement, as long as the
  rejection is at validate time with a clear error.

A conforming runtime MUST NOT:

- Silently ignore unknown fields. Unknown fields are validate-time
  errors, not warnings.
- Mutate the pipeline file. The text is the source of truth and the
  runtime is read-only with respect to it.
- Coerce values across types at port boundaries without an explicit
  cast step in the pipeline.

## 4. Pipeline file format

### 4.1 Surface syntax

- **Primary:** YAML 1.2.
- **Alternative:** JSON. JSON files MUST produce the same logical
  document as their YAML equivalent. Runtimes MUST accept both.
- File extensions: `.yml`, `.yaml`, `.json`. Selection of parser is
  by extension.

### 4.2 Top-level shape

A pipeline file is a single mapping with these top-level keys:

| Key            | Required | Meaning                                              |
|----------------|----------|------------------------------------------------------|
| `betl`         | yes      | Spec version integer. This document defines `1`.     |
| `name`         | yes      | Pipeline identifier (lower-snake-case recommended).  |
| `connections`  | no       | Map of named connection definitions.                 |
| `parameters`   | no       | Map of named pipeline parameters with types/defaults.|
| `pipeline`     | yes      | Ordered list of top-level steps.                     |
| `include`      | no       | List of file paths to merge in (semantics in 4.5).   |

The `betl: <int>` value declares which version of this spec the file
targets. Future incompatible changes to the spec bump the integer.
Runtimes MUST reject pipelines whose declared version they do not
implement.

### 4.3 Steps

A step is a mapping with these reserved keys, plus type-specific body:

| Key             | Type         | Notes                                              |
|-----------------|--------------|----------------------------------------------------|
| `id`            | string       | Required. Unique within the enclosing scope.       |
| `type`          | string       | Required. See §6 for the spec-defined values.      |
| `after`         | list[string] | Step IDs that must complete before this one runs.  |
| `on_failure`    | enum         | `stop` (default), `continue`, `retry`.             |
| `retries`       | int          | With `on_failure: retry`.                          |
| `retry_backoff` | string       | E.g. `"1s"`, `"5s,10s,30s"`.                       |
| `timeout`       | string       | Per-attempt wall-clock cap, e.g. `"30s"`, `"5m"`.  |
| `condition`     | expression   | Skip this step if the predicate is false (§7).     |
| `description`   | string       | Free-text doc.                                     |

Anything not listed above is type-specific body.

### 4.4 Two layers: control flow and data flow

Pipelines have two graph layers, intentionally separated:

- **Control flow** — orchestration. Steps are *tasks*: run SQL, run a
  shell command, copy a file, run a data flow. Edges express ordering
  via `after:`.
- **Data flow** — row streaming. Steps are *components*: sources,
  transforms, sinks. Edges express row streams via `from:`. A data
  flow is a single node in the control flow with `type: dataflow` and
  `steps:` containing the component list.

Mixing the two at the same level is not permitted.

### 4.5 Includes

`include:` MAY be specified at the top level as a list of relative
file paths. Runtimes MUST merge included documents into the current
one as if their contents were inlined. Conflicting top-level keys are
validate-time errors except for `connections` and `parameters`, which
merge key-by-key.

## 5. Type system

### 5.1 Apache Arrow as the type contract

The data contract at every port between steps is **Apache Arrow logical
types**. Schemas declare columns as `(name, type, nullable)`.

A runtime MUST accept and pass through any data that conforms to the
declared schema. It MAY implement a subset of Arrow's full type system;
it MUST advertise which subset it supports and reject pipelines that
require unsupported types at validate time.

### 5.2 Type spelling in YAML

Types are spelled using Arrow's canonical logical-type names:
`int8`, `int16`, `int32`, `int64`, `uint8`, …, `float32`, `float64`,
`bool`, `string`, `binary`, `date32`, `date64`, `time32[unit]`,
`time64[unit]`, `timestamp[unit, tz]`, `decimal128(p, s)`,
`decimal256(p, s)`, `list<T>`, `struct<…>`, `dictionary<I, T>`,
`map<K, V>`.

Type names containing `[`, `<`, `,`, or `>` MUST be quoted when written
inside a YAML flow mapping. Block-style and single-token names need
no quoting.

### 5.3 No implicit coercion

A row passed between two ports whose declared schemas disagree on
type is a validate-time error. Explicit type conversion is a `map`
step with a cast expression.

## 6. Step types (the spec floor)

Every conforming runtime MUST implement these step types with the
semantics described. Runtimes MAY add more; the floor is the minimum.

### 6.1 Data flow — components

#### Transforms

| `type:`       | Inputs | Outputs | Purpose                                           |
|---------------|--------|---------|---------------------------------------------------|
| `filter`      | 1      | 1       | Drop rows where `where:` is false.                |
| `map`         | 1      | 1       | Add/replace/project columns via expressions.      |
| `lookup`      | 1      | 1       | One-sided indexed join against a reference table. |
| `join`        | 2      | 1       | Two-stream join with explicit kind.               |
| `aggregate`   | 1      | 1       | Group-by with `compute:` aggregates.              |
| `sort`        | 1      | 1       | Order by keys.                                    |
| `union`       | N      | 1       | Concatenate same-schema streams.                  |
| `distinct`    | 1      | 1       | Deduplicate by all columns or by `by:`.           |
| `limit`       | 1      | 1       | Take at most N rows.                              |
| `split`       | 1      | N       | Route rows to N named output ports by predicate.  |
| `pivot`       | 1      | 1       | Long-form to wide-form on a pivot key.            |
| `unpivot`     | 1      | 1       | Wide-form to long-form on a column set.           |
| `multicast`   | 1      | N       | Duplicate each input row to N named output taps.  |

Body fields for each are spelled out in §6.3 (deferred — see SPEC.md
for the v0.1 reference; the body of this document promotes those
field names to spec status once stabilized).

#### Sources and sinks

Sources and sinks are provider-defined: `csv.read`, `csv.write`,
`mssql.read`, `mssql.upsert`, `postgres.read`, `postgres.upsert`, etc.
The spec defines the **naming convention** (`<provider>.<verb>`) and
the **port contract** (sources have 0 inputs and 1+ outputs; sinks
have 1+ inputs and 0 outputs). The provider list a runtime ships is
not part of the core spec.

### 6.2 Control flow — tasks

| `type:`         | Purpose                                              |
|-----------------|------------------------------------------------------|
| `sql.execute`   | Run SQL against a named connection.                  |
| `shell`         | Run a command. `argv:` only — no shell-injection.    |
| `http`          | Issue an HTTP request.                               |
| `file.copy`     | Copy a local file.                                   |
| `file.move`     | Move/rename a local file.                            |
| `file.delete`   | Delete a local file.                                 |
| `dataflow`      | Embed a data flow as a single control-flow node.     |

### 6.3 Field-level reference (deferred)

For v1 of this spec, the per-step body fields and their YAML shapes
are documented in `SPEC.md` §4.3–4.4. They are promoted to spec
status by reference. A future revision of this document will inline
the field-level reference once the field set is stable across
implementations.

## 7. Placeholders: expressions and scripts

The spec defines structure. Anywhere the pipeline needs *application
logic* — a predicate, a derived value, a per-row callback, a whole
script — the spec declares the slot and the pipeline declares the
language. The runtime supplies the evaluator.

### 7.1 The three flavors of placeholder

| Flavor              | Example                                  | Portability                                                  |
|---------------------|------------------------------------------|--------------------------------------------------------------|
| Inline expression   | `where: { lang: lua, expr: "…" }`        | Portable across runtimes that implement the declared lang.   |
| Embedded script     | `type: dotnet.script` + body             | Step type is itself runtime-bound by convention.             |
| Step type           | `type: filter`                           | Spec-defined; portable across all conforming runtimes.       |

### 7.2 The `lang:` mechanism

Anywhere a placeholder accepts logic, the spec uses this shape:

```yaml
where:
  lang: <language-id>
  expr: "<source text>"
```

Or for literal values:

```yaml
value:
  lang: literal
  value: <yaml scalar>
```

`<language-id>` is a string the runtime maps to an evaluator. Reserved
values:

| Value       | Meaning                                                          |
|-------------|------------------------------------------------------------------|
| `literal`   | Constant value, no expression. The accompanying key is `value:`. |
| `ssisexpr`  | The portable LCD expression language (§7.3).                     |
| `lua`       | Lua 5.4 expression.                                              |
| `csharp`    | C# expression.                                                   |
| `python`    | Python expression.                                               |
| `sql`       | A SQL expression evaluated by the connection's engine.           |

A runtime that does not implement a given `lang:` value MUST reject
pipelines that reference it at validate time.

A runtime MAY pick a default `lang:` for the short-string shorthand
(e.g. `where: "row.amount > 0"` resolves to the runtime's default).
The choice of default is implementation-specific and is NOT portable
— pipelines that need to be portable MUST spell `lang:` explicitly.

### 7.3 The portable LCD: `ssisexpr`

The **portable expression language** is `ssisexpr`. It is the lowest
common denominator: small grammar, restricted semantics, well-specified.
All conforming runtimes MUST implement `ssisexpr`.

A pipeline that uses **only** `lang: ssisexpr` (or `lang: literal`)
plus spec-defined step types is the **guaranteed portable subset** —
it runs on every conforming runtime that ships the providers it uses.

The `ssisexpr` grammar and semantics are documented in `docs/SSISEXPR.md`
in the betl-legacy repository and promoted to spec status by reference.
The name acknowledges its origin (SSIS expressions); the semantics are
documented independently of any SSIS implementation.

Rationale for picking `ssisexpr` over inventing one or picking a SQL
flavor: it already exists, it's small, it's documented, it has a
working reference implementation, and our SSIS-migration path emits
it natively. The trade-off is that it is verbose by modern standards
(`[col] + " " + [other]`). A future spec revision may add a second
portable language; for v1 there is one.

### 7.4 Embedded scripts

Step types of the form `<runtime>.script` or `<runtime>.task`
(`lua.script`, `dotnet.script`, `python.script`, …) host a chunk of
runtime-bound code. These are explicitly *not* portable across
runtimes whose host language differs. The spec defines the *shape* of
such steps (`source:` or `file:`, `output_schema:` for transforms,
etc.) so a converter or analyzer can recognize and inspect them, but
not their runtime behavior.

## 8. Connections

A `connections:` block names one or more connections, each with a
`type:` and type-specific fields. Connection types are
provider-defined (`mssql`, `postgres`, `mysql`, `oledb`, …). The spec
defines the **referencing contract**:

- A step references a connection by name via `connection: <name>`.
- The runtime resolves the name to a provider implementation at
  validate time.
- Unknown connection names are validate-time errors.
- A runtime that does not ship a provider for a referenced connection
  type MUST reject the pipeline at validate time, naming the missing
  provider.

The set of fields each connection type carries is provider-defined,
not spec-defined.

## 9. Parameters

A `parameters:` block declares pipeline-level parameters:

```yaml
parameters:
  load_date:
    type: date
    default: 2026-01-01
  region:
    type: string
```

- Parameter values MAY be referenced anywhere a literal is valid via
  `${params.<name>}`.
- Runtimes MUST supply a CLI mechanism for setting parameter values
  at invocation. The flag shape is implementation-specific.
- Parameters are immutable within a pipeline run. There is no
  mid-pipeline parameter assignment (SSIS-style mutable package
  variables are explicitly excluded).

## 10. Validation contract

Every conforming runtime exposes a validation operation that:

1. Parses the file.
2. Verifies the top-level shape (§4.2).
3. Resolves every `type:` against the runtime's step-type registry.
4. Resolves every `connection:` reference against the
   `connections:` block.
5. Resolves every `lang:` reference against the runtime's expression
   registry.
6. Type-checks port-to-port schemas where they can be inferred.
7. Reports every problem found (not just the first), with file:line
   pointers where the parser supports them.

Validation MUST NOT execute any step. Validation results MUST be
deterministic given the same input file and the same runtime version.

## 11. Versioning

- Spec version is declared in the file as `betl: <integer>`.
- This document defines version `1`.
- Backwards-compatible additions (new step types, new optional fields,
  new `lang:` values) do not bump the version.
- Backwards-incompatible changes bump the version. Runtimes MUST
  reject pipelines whose declared version they do not implement.

## 12. Conformance levels

A runtime declares its level of conformance:

| Level | Required                                                                |
|-------|-------------------------------------------------------------------------|
| **L1** | All §6 step types (transforms + tasks). `ssisexpr`. Type system §5.    |
| **L2** | L1 + at least one additional `lang:` (e.g. `lua` / `csharp` / `python`).|
| **L3** | L2 + one or more provider extensions beyond the spec floor.             |

A pipeline targeting L1 is portable across all conforming runtimes.
Pipelines targeting L2+ are portable across the subset of runtimes
that implement the languages and providers they use.

## 13. What this spec deliberately leaves to implementations

- **Default `lang:` for shorthand expressions.** Each runtime picks
  its own default — explicit `lang:` is the portable shape.
- **CLI surface.** `betl validate`, `betl run`, `betl fmt` etc. are
  conventions, not spec.
- **Tooling.** Linters, formatters, IDE plugins, GUIs.
- **Engine architecture.** Threading model, batch sizes, memory
  management, spill-to-disk policy, parallelism — all implementation
  concerns.
- **Provider catalog.** Which providers a runtime ships is its choice.
- **Packaging.** How a runtime is distributed (single binary, .NET
  package, container image, …) is implementation-specific.

## 14. Known reference implementations

| Runtime         | Status     | Languages              | Notes                              |
|-----------------|------------|------------------------|------------------------------------|
| **betl-legacy** | live       | `ssisexpr`, `lua`      | C engine, Lua scripting, AOT plugins. First implementation; the "in-spirit" name acknowledges that the destination is a multi-runtime family, not this one binary. |
| **betl.dotnet** | planned    | `ssisexpr`, `csharp`   | Headless .NET 8 engine. Hosts compiled SSIS PipelineComponents. |

## 15. Glossary

- **Pipeline.** A YAML/JSON document conforming to this spec.
- **Step.** A node in the pipeline DAG; a task (control flow) or a
  component (data flow).
- **Component.** A data-flow step — source, transform, or sink.
- **Task.** A control-flow step — anything with side effects that
  isn't a row pipeline.
- **Port.** A typed input or output edge between components.
- **Schema.** Ordered list of `(name, type, nullable)` describing
  the columns flowing through a port.
- **Provider.** A named extension supplying connection types and/or
  step types beyond the spec floor (`mssql`, `postgres`, `kafka`, …).
- **Conforming runtime.** An implementation of this spec at L1, L2,
  or L3 (§12).
- **Portable subset.** Pipelines using only spec-floor step types,
  `ssisexpr` / `literal`, and providers shipped by the target runtime.
