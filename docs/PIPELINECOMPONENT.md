# `dotnet.pipelinecomponent` — hosting SSIS PipelineComponents in betl

This step type runs user-supplied C# source that derives from
`Microsoft.SqlServer.Dts.Pipeline.PipelineComponent`. The source compiles
via NativeAOT at validate time; at run time the AOT'd `.so` is dlopened
and betl drives the SSIS-style lifecycle.

Use it when you're migrating a custom SSIS PipelineComponent and want to
keep the original API surface — `ProcessInput(int, PipelineBuffer)`,
`BufferManager.FindColumnByLineageID`, `DirectErrorRow`, the lot. If you
just need a per-row callback with a clean DX, prefer `dotnet.script`
(simpler protocol, smaller compile time).

`dotnet.script` and `dotnet.pipelinecomponent` cover two distinct SSIS
artifacts:

| SSIS artifact          | Authored by | betl step               |
|------------------------|-------------|-------------------------|
| Script Component       | The designer scaffolds it; user fills in row handlers | `dotnet.script` |
| Custom PipelineComponent | Developer hand-writes the whole lifecycle | `dotnet.pipelinecomponent` |

## Quick example

```yaml
- id: enrich
  type: dotnet.pipelinecomponent
  from: source
  lang: csharp
  output_schema:
    - { name: id,      type: l }
    - { name: doubled, type: l }
  source: |
    using Microsoft.SqlServer.Dts.Pipeline;
    using Microsoft.SqlServer.Dts.Pipeline.Wrapper;
    namespace Betl;
    public class UserComponent : PipelineComponent {
      int idIdx = -1, doubledIdx = -1;
      public override void PreExecute() {
        base.PreExecute();
        var input  = ComponentMetaData.InputCollection[(object)0];
        var output = ComponentMetaData.OutputCollection[(object)0];
        foreach (IDTSInputColumn100 c in input.InputColumnCollection)
          if (c.Name == "id")
            idIdx = BufferManager.FindColumnByLineageID(input.Buffer, c.LineageID);
        foreach (IDTSOutputColumn100 c in output.OutputColumnCollection)
          if (c.Name == "doubled")
            doubledIdx = BufferManager.FindColumnByLineageID(input.Buffer, c.LineageID);
      }
      public override void ProcessInput(int inputID, PipelineBuffer buffer) {
        while (buffer.NextRow())
          buffer.SetInt64(doubledIdx, buffer.GetInt64(idIdx) * 2);
      }
    }
```

The user class **must** be named `UserComponent` and live in the `Betl`
namespace. The plugin instantiates it by that fully-qualified name.

## Modes

Three modes, controlled by two boolean YAML flags (`async`, `error_output`):

| `async` | `error_output` | What you get |
|---------|----------------|--------------|
| false (default) | false (default) | **Sync, single output.** One column space defined by `output_schema`. Same-named input columns pre-populate cells; `Get`/`Set` operate on the same row. `NextRow()` advances. One output port (`out`). |
| false | true | **Sync + error routing.** Two output ports (`out` and `error_out`). User calls `DirectErrorRow(rowIdx, errorCode, errorColumn)` to redirect the current row to `error_out`; non-redirected rows go to `out`. Error port's schema = `output_schema` + `ErrorCode` (i32) + `ErrorColumn` (i32). |
| true | false | **Async transform.** Separate input and output buffers. `PrimeOutput` receives an output buffer; user calls `AddRow()` on it inside `ProcessInput`. Filter / aggregate / fan-in patterns. |
| true | true | not currently supported (Phase 2 deferred async error routing). |

Wire the error port like:

```yaml
- id: bad_rows
  type: csv.write
  from: enrich:error_out
```

## Supported types

Internal format chars and their YAML / Arrow / SSIS DataType / C# accessor mapping:

| YAML `type:` | Arrow format | SSIS DataType | C# accessor |
|--------------|--------------|---------------|-------------|
| `l`          | `l`          | DT_I8         | `GetInt64` / `SetInt64` |
| `L`          | `L`          | DT_UI8        | `GetInt64` (unsigned bytes) |
| `i`          | `i`          | DT_I4         | `GetInt32` / `SetInt32` |
| `I`          | `I`          | DT_UI4        | `GetUInt32` / `SetUInt32` |
| `s`          | `s`          | DT_I2         | `GetInt16` / `SetInt16` |
| `S`          | `S`          | DT_UI2        | `GetUInt16` / `SetUInt16` |
| `c`          | `c`          | DT_I1         | `GetSByte` / `SetSByte` |
| `C`          | `C`          | DT_UI1        | `GetByte` / `SetByte` |
| `g`          | `g`          | DT_R8         | `GetDouble` / `SetDouble` |
| `f`          | `f`          | DT_R4         | `GetSingle` / `SetSingle` |
| `b`          | `b`          | DT_BOOL       | `GetBoolean` / `SetBoolean` |
| `u`          | `u`          | DT_WSTR       | `GetString` / `SetString` |
| `D`          | `tdD`        | DT_DBDATE     | `GetDate` / `SetDate` (DateTime, midnight UTC) |
| `T`          | `tsu:[<tz>]` | DT_DBTIMESTAMP2 / DT_DBTIMESTAMPOFFSET | `GetDate` (DateTime) or `GetDateTimeOffset` |
| `M`          | `ttu`        | DT_DBTIME2    | `GetTime` (TimeSpan) or raw `GetInt64` (µs) |
| `z`          | `z`          | DT_BYTES      | `GetBytes` / `SetBytes` |
| `G`          | `w:16`       | DT_GUID       | `GetGuid` / `SetGuid` |
| `E`          | `d:38,<scale>` | DT_NUMERIC  | `GetDecimal` / `SetDecimal` |

Decimal columns take an optional `scale: N` (default 0). Timestamp
columns take an optional `tz: "<tag>"` (default no tz). Examples:

```yaml
output_schema:
  - { name: id,     type: l }
  - { name: price,  type: E, scale: 4 }
  - { name: when,   type: T, tz: "UTC" }
  - { name: bytes,  type: z }
```

### Type precision caveats

- `GetDecimal` returns `System.Decimal`, which has a 96-bit mantissa.
  Values from `decimal128` exceeding that throw `BetlPipelineException`.
- `System.Decimal` scale is capped at 28; columns declared with scale > 28
  throw on `GetDecimal`.
- `GetDate` for `T` columns returns `DateTime` in UTC ticks; the tz tag
  is preserved structurally but the value itself is always µs-since-epoch.
  Use `GetDateTimeOffset` if you need the tz info on the C# side.
- Resolution variants (`tdm`, `tts`, `ttm`, `ttn`, `tss:`, `tsm:`, `tsn:`)
  are not yet supported — but nothing in the betl ecosystem emits them.
  Add them when an external Arrow source with non-µs precision needs to
  feed in.

## Shim API surface

The shim assembly `Betl.Shim.dll` provides these types in the
`Microsoft.SqlServer.Dts.Pipeline` and
`Microsoft.SqlServer.Dts.Pipeline.Wrapper` namespaces — enough to compile
typical SSIS PipelineComponent source against:

- `PipelineComponent` — abstract base. Lifecycle methods listed below.
- `PipelineBuffer` — typed Get/Set/NextRow/AddRow/SetEndOfRowset/IsNull/
  SetNull/DirectErrorRow.
- `IDTSComponentMetaData100` — populated read-only from the YAML config.
  Exposes `InputCollection`, `OutputCollection`, `CustomPropertyCollection`
  (empty), `RuntimeConnectionCollection` (lazy lookup), plus `FireError` /
  `FireWarning` / `FireInformation`.
- `IDTSInput100` / `IDTSOutput100` / `IDTSInputColumn100` /
  `IDTSOutputColumn100` / `IDTSCustomProperty100` / `IDTSRuntimeConnection100`.
- `IDTSBufferManager100` — `FindColumnByLineageID(bufferID, lineageID)`
  returns the index. In Phase 1 betl uses identity-mapped lineage IDs
  (LineageID == position within the column collection).
- `DataType` enum — full SSIS value set (DT_I1..DT_FILETIME), with the
  runtime supporting the subset listed above.
- `DTSValidationStatus` enum.
- `BetlPipelineException` — what the shim throws on invalid use.

## Lifecycle

Methods driven by the betl runtime (override if you need them):

| Method | When |
|--------|------|
| `PreExecute()` | Once before the first `ProcessInput`. |
| `PrimeOutput(int outputs, int[] outputIDs, PipelineBuffer[] buffers)` | Once, after `PreExecute`. **Only meaningful in async mode**; sync mode calls it but with empty buffers. |
| `ProcessInput(int inputID, PipelineBuffer buffer)` | Once per upstream batch. **abstract** — must override. |
| `PostExecute()` | After upstream EOF. In async mode, this is where aggregator-style components emit final rows via `AddRow()`. |
| `Cleanup()` | Last. Free anything you allocated. |

Methods NOT driven by the betl runtime (they exist in the shim so
ported source compiles, but betl never calls them):

- `ProvideComponentProperties()` — designer-side, sets up the metadata
  graph. betl populates `ComponentMetaData` directly from the YAML.
- `Validate()` — designer-side.
- `ReinitializeMetaData()` — designer-side.
- `SetComponentProperty(name, value)` — designer-side.
- `PerformUpgrade(pipelineVersion)` — versioning across SSIS releases;
  there's no analogue here.

Components that rely on these for runtime correctness will need
adaptation. The standard pattern — design-time scaffolding done by SSDT,
runtime code in `PreExecute` / `ProcessInput` — ports without changes.

## Connection Managers

`ComponentMetaData.RuntimeConnectionCollection["foo"]` looks up the
connection named `foo` from the pipeline's `connections:` block.
`AcquireConnection(transaction)` returns the connection's raw JSON blob
as a `string`. Ported SSIS source that originally did:

```csharp
var cn = (OleDbConnection)cm.AcquireConnection(null);
```

becomes:

```csharp
var json = (string)cm.AcquireConnection(null);
// parse json and construct OleDbConnection / SqlConnection / etc. yourself
```

Lazy lookup: indexed access by name always succeeds (synthesizing an
entry on demand). The existence check fires inside `AcquireConnection`
— if the name isn't in the pipeline's `connections:` block, it throws
with a clear message.

## Performance

Measured on a 16-core x86_64 box with .NET 8.0.404 NativeAOT,
100k rows × 5 iterations per shape, steady-state (post-warmup).
Full numbers and reproduction instructions in
[`BENCHMARKS.md`](BENCHMARKS.md).

### Throughput

| shape                     | wall (min) | throughput     |
|---------------------------|-----------:|---------------:|
| 1-col passthrough         | ~7 ms      | ~14 M rows/s   |
| 10-col passthrough        | ~24 ms     |  ~4 M rows/s   |
| error_output, 10% tagged  | ~7 ms      | ~14 M rows/s   |
| 5 decimal cells/row       | ~33 ms     |  ~3 M rows/s   |
| async aggregator (N → 1)  |  ~3 ms     | ~30 M rows/s   |
| 1-col passthrough (lua.script comparison) | ~19 ms | ~5 M rows/s |

Per-cell throughput scales roughly linearly with column count —
each `Get*` / `Set*` is its own function-pointer crossing into C.
Error routing costs effectively nothing at low tag rates.
Decimal128 is ~5× slower than int64 because of the per-cell
`new byte[16]` heap allocation that flows through the staging
buffer (the 128-bit arithmetic itself stays in registers via
`System.Int128` / `UInt128`); a previous `BigInteger`-based
implementation was ~10× slower than int64.

The async aggregator pattern (no per-row `Set*` in the hot loop)
shows the FFI ceiling: ~30 M rows/s when nothing crosses back
into C per row.

### Startup cost

| state | wall time | notes |
|-------|----------:|-------|
| **Cold** (cache empty, NuGet warm)   | ~1.5 s | `dotnet publish -p:PublishAot=true` runs from scratch |
| **Warm** (per-source-hash cache hit) | ~0.6 ms | just `dlopen` + lifecycle methods |

The cold cost is dominated by `dotnet publish` (Roslyn compile +
NativeAOT link). Each unique `(source, lang, schema)` tuple gets
its own cache entry under `$HOME/.cache/betl/dotnet/<hash>.so`;
re-running an unchanged component reuses the cached `.so` and
pays only the per-process `dlopen` cost. Cold cost grows with
component complexity — a real SSIS-migration PipelineComponent
with multiple referenced assemblies will be 5–15 s cold.

First-ever AOT compile on a clean machine (NuGet cache empty too)
is slower than the 1.5 s above — NuGet has to download packages.
Subsequent compiles share the system-wide NuGet cache.

### Parallel mode

`BETL_PARALLEL` doesn't materially affect the `pc-*` shapes:
in a 3-stage pipeline (`gen → pc → sink`) the dotnet component
is the single hot stage and the pipeline-parallel executor has
little to overlap. Long chains with `dotnet.pipelinecomponent`
sandwiched between other CPU-bound stages would see the usual
overlap gains.

### Comparison vs SSIS

`docs/BENCHMARKS.md` carries the full measured comparison
against real SSIS (SQL Server 2022 dtexec on Linux, via the
`mcp-ssis` service). Quick summary on a stock 10-col read +
10-col derived workload, end-to-end wall time:

- **100k rows** (typical small batch / cron-style ETL): betl
  ~70 ms vs SSIS ~2.2 s → **~31× faster**. At this scale most
  of SSIS's wall time is process startup, CLR JIT, and
  pipeline validation.
- **1M rows** (startup amortised, steady-state data-flow
  throughput dominates): betl ~540 ms (~1.86 M rows/s) vs
  SSIS ~3.1 s (~318 k rows/s) → **~5.8× faster**.

These are stock-component shapes — they don't exercise
`dotnet.pipelinecomponent` at all. They establish the
*minimum* advantage you'd see migrating to betl. For
SSIS Script Components specifically (the case where
`PipelineBuffer`'s COM-RCW per-cell marshalling tax is at
its worst), the structural argument says the gap should be
larger again — but SSIS-on-Linux doesn't ship the Script
Component assemblies, so that A/B still needs a Windows host
with SSDT. Treat the Script-Component-specific multiplier as
a hypothesis until then.

**Trade-off:** SSIS wins on cold start (no AOT compile
penalty). See `pc-startup` for cold/warm timings.

## Limitations

| Area | What's missing |
|------|----------------|
| `Validate` / `ProvideComponentProperties` | Not driven by the runtime; stubs only. |
| `PerformUpgrade` | No analogue. |
| Variable mutation | `Variables` are read-only via the params bridge. SSIS-style `vars["x"].Value = ...` won't work. |
| Typed connections | `AcquireConnection` returns the JSON blob (string), not `OleDbConnection`/`SqlConnection`. |
| Async error routing | Async mode (`async: true`) has one output buffer; `error_output: true` only works in sync mode. |
| Resolution variants | Only µs precision for timestamps + time; only day precision for dates. Arrow variants `tdm`/`tts`/`ttm`/`ttn`/`tss:`/`tsm:`/`tsn:` not yet supported. |
| DT_FILETIME | No native support. Convert in your component if needed. |
| `CustomPropertyCollection` | Returns empty collection. Custom properties from SSIS aren't carried through. |

## Porting recipe for SSIS source

1. Drop your `PipelineComponent` subclass into a YAML
   `dotnet.pipelinecomponent` step's `source:` field. Rename the class
   to `UserComponent` and put it in namespace `Betl`.
2. Translate connection acquisition: `(OleDbConnection)cm.AcquireConnection(null)`
   → `(string)cm.AcquireConnection(null)` + your own connection construction.
3. Replace any variable writes with read-only param access.
4. Convert SSIS column declarations into `output_schema:` YAML entries,
   using the type/scale/tz mapping above.
5. `betl validate` your pipeline — it AOT-compiles on validate, so
   syntax errors surface immediately.
6. `betl run` to test against your data.

The shim's API surface is documented inline in
`providers/betl-dotnet/shim/pipelinecomponent/`; the C-side runtime is
in `providers/betl-dotnet/dotnet_provider.c`.
