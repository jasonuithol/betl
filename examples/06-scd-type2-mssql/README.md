# Example 06 — SCD type-2 dimension load (SQL Server)

Sibling of [example 05](../05-scd-type2/README.md) — the same recipe
adapted for SQL Server. The pattern is identical; only the
engine-specific bits change.

## What's different from example 05

| Concern | Postgres (05) | SQL Server (06) |
|---|---|---|
| Surrogate key | `BIGSERIAL PRIMARY KEY` | `BIGINT IDENTITY(1,1) PRIMARY KEY` |
| Timestamp type | `TIMESTAMPTZ` | `DATETIME2(6)` |
| Boolean type | `BOOLEAN` | `BIT` (1 / 0) |
| Source/sink components | `postgres.read` / `postgres.exec` | `mssql.read` / `mssql.exec` |
| SQL placeholders | `$1`, `$2`, …  | `?` positional |
| `is_current` filter | `WHERE is_current` | `WHERE is_current = 1` |

Everything else — the join + classify + conditional_split + union +
INSERT/UPDATE shape — is byte-identical to the Postgres recipe.

## Setup

```sh
export WAREHOUSE_DSN="DRIVER={FreeTDS};SERVER=...;DATABASE=warehouse;UID=...;PWD=..."
sqlcmd -S <server> -d warehouse -i sql/dim_setup.sql
```

## Run

```sh
betl run pipeline.betl.yml --param batch_ts=2026-05-15T00:00:00
```

## Notes

- The pipeline uses inline-literal injection for `valid_from` /
  `is_current` (e.g. `'${params.batch_ts}'`) rather than ODBC
  parameter binding, because ODBC binds positional `?` placeholders
  in order and we want column values to come from the input row.
  Substitution happens once per run, not per row.
- The `dim.customer_current` view filters with `WHERE is_current = 1`
  to match MSSQL's BIT type. The Postgres view uses bare
  `WHERE is_current` against the BOOLEAN.
- See `examples/05-scd-type2/README.md` for limitations that apply
  equally to both flavors (no delete handling, no type-1 / fixed
  attribute handling, no inferred-member dimension management).
