# bench/ssis — real-SSIS comparison harness

This directory contains the inputs and harness for the
SSIS-vs-betl end-to-end comparison documented in
`docs/BENCHMARKS.md` under "SSIS comparison".

## Layout

| path | contents |
|------|----------|
| `sql/schema.sql`         | One-time DDL + seed for `betl_bench` SQL Server database |
| `packages/*.dtsx`        | Hand-crafted SSIS packages (OLE DB Source → optional Derived Column → Row Count) |
| `yaml/betl-*.yml`        | Equivalent betl pipelines (`mssql.read → map → count_rows`) |
| `gen-dtsx.py`            | Generator for the `.dtsx` files — edit it, not the XML |
| `run-betl-side.sh`       | Wrapper that times `betl run` over N iterations with warmup |
| `run-comparison.sh`      | Drives the betl side and prints the comparison table |

## Why these specific shapes

- `A-1col`, `A-10col` — pure read paths. Isolate the
  source/buffer overhead with no transforms.
- `B-derived-10col` — read + a stock derived-column transform
  on every column. This is the fair stock-vs-stock test.
- `B-derived-10col-1m` — 1M-row variant of `B`. At this size
  SSIS's ~1.9 s startup is <10% of wall, so the ratio
  approximates steady-state data-flow throughput.

## Running it

```
# 1. One-time: seed the SQL Server.
sqlcmd -S host.containers.internal -U sa -P '<pwd>' -d master \
       -i bench/ssis/sql/schema.sql
# or via mcp-mssql workbench.

# 2. Stage the .dtsx files where mcp-ssis can see them.
cp bench/ssis/packages/*.dtsx /path/to/mcp-ssis/packages/

# 3. betl side.
export BETL_TEST_MSSQL_DSN="<your ODBC dsn for betl_bench>"
bench/ssis/run-comparison.sh

# 4. SSIS side — call mcp-ssis benchmark_package for each
# of the four .dtsx files (runs=6 warmup=1).
```

## Linux SSIS subset caveat

SSIS-on-Linux ships only a subset of components. Things that
worked on the SQL Server 2022 dtexec we tested against:
- OLE DB Source (with `MSOLEDBSQL` provider)
- Derived Column
- Row Count

Things that **did not** load:
- ADO.NET Source (`Microsoft.SqlServer.ADONETSrc` assembly
  not present)
- Script Component (likely — not attempted, but the Script
  Task assemblies generally aren't shipped on Linux)

The Script Component case is the one where the
`PipelineComponent`-vs-shim COM-RCW argument would land most
strongly. To benchmark that specifically you still need a
Windows host with SSDT.

## Authoring new shapes

Edit `gen-dtsx.py` and add a new entry to the `pkgs` list in
`main()`. The generator handles all four required block types
(main output / external metadata / error output / row count
input columns), and (for shapes with Derived Column) the
required error-output + dispositions that SSIS validation
demands.
