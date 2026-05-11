# SSIS Expression Language

The `ssisexpr` engine implements a subset of [SQL Server Integration
Services Expression Language][ssis-el-docs] tight enough that real
SSIS Data Flow `Derived Column`, `Conditional Split`, and `Variable`
expressions can usually be lifted verbatim from a `.dtsx` package
into a betl pipeline.

The engine ships as a provider plugin under `providers/betl-ssisexpr`,
built as `build/providers/betl-ssisexpr/betl-ssisexpr.so`. `betl run`
auto-loads it from the dev tree (see the "Provider plugins" section
of the top-level [README](../README.md)).

[ssis-el-docs]: https://learn.microsoft.com/en-us/sql/integration-services/expressions/integration-services-ssis-expressions

## When to use it

- **Migrating an SSIS package.** Keep the original `Derived Column`
  formulae intact so the diff against the legacy project is minimal.
  Anything that won't lift cleanly is documented under "Not yet
  supported" below — convert those expressions to Lua at
  migration time.
- **You want typed casts.** `(DT_I8) col` and `(DT_DBTIMESTAMP) "2026-05-11"`
  are clearer than Lua's `tonumber` / `os.time` for ETL-shaped
  conversions.
- **You want three-valued null logic everywhere by default.** SSIS-EL's
  AND / OR / comparison rules treat NULL the SQL way (Unknown,
  propagated, asymmetric short-circuit). Lua's `nil` doesn't.

For row-level scripting that doesn't lift from SSIS — anything you'd
have written in a `Script Component` rather than a `Derived Column` —
use the Lua engine instead.

## Using it in a pipeline

`ssisexpr` is a `lang:` value, the same way `lua` and `literal` are.
You can use it anywhere an expression engine is accepted: `filter`'s
`where:`, `map`'s `add:` columns, `conditional_split`'s `cases:`, and
so on.

```yaml
- id: enrich
  type: map
  from: source
  add:
    order_month:
      lang: ssisexpr
      expr: MONTH(order_date)
    is_overdue:
      lang: ssisexpr
      expr: (DT_BOOL) (DATEDIFF("day", delivered_at, GETDATE()) > 30)
```

Column references can be bare (`order_date`) or bracketed
(`[order_date]`); both forms are case-insensitive, matching SSIS.

See `examples/04-ssis-orders-by-month/pipeline.betl.yml` for a full
worked pipeline.

## Type system

SSIS DT_* type names appear in two places: typed NULLs (`NULL(DT_*)`)
and explicit casts (`(DT_*)`). Each maps onto one of betl's
underlying Arrow value kinds.

| SSIS type        | Arrow leaf format | Internal kind     | Notes |
|---|---|---|---|
| `DT_I1`          | `l` (int64) | int64 | All integer widths collapse to int64. |
| `DT_I2`          | `l` (int64) | int64 | |
| `DT_I4`          | `l` (int64) | int64 | |
| `DT_I8`          | `l` (int64) | int64 | |
| `DT_R4`          | `g` (float64) | float64 | Both float widths collapse to float64. |
| `DT_R8`          | `g` (float64) | float64 | |
| `DT_BOOL`        | `b` (bit) | bool | |
| `DT_WSTR`        | `u` (utf8) | utf8 | `(DT_WSTR, N)` accepted; the length is parsed and ignored — betl strings are variable-length. |
| `DT_STR`         | `u` (utf8) | utf8 | `(DT_STR, N, cp)` accepted; cp parsed and ignored. |
| `DT_DBDATE`      | `tdD` (date32) | date32 | Days since 1970-01-01 (int32). |
| `DT_DBTIMESTAMP` | `tsu:` (timestamp_us) | timestamp_us | Microseconds since 1970-01-01 UTC. |
| `DT_DBTIMESTAMP2`| `tsu:` (timestamp_us) | timestamp_us | Alias for `DT_DBTIMESTAMP`. |

Not yet supported (the cast / typed-NULL will produce a parse error):

- `DT_NUMERIC(p, s)` and `DT_DECIMAL(s)` — no fixed-precision decimal
  yet. Use `DT_R8` for now, or store as `DT_I8` cents.
- `DT_DBTIME` / `DT_DBTIME2` — time-of-day without a date.
- `DT_DBTIMESTAMPOFFSET` — timestamp with tz offset.
- `DT_GUID`, `DT_IMAGE`, `DT_NTEXT`, `DT_BYTES`, `DT_DATE` (the
  obsolete float-of-days form).

## Casts

```
(DT_xx) value
(DT_WSTR, N) value
(DT_STR, N, cp) value
```

| From → To | Behaviour |
|---|---|
| numeric → numeric | Normal C-style truncation / promotion. |
| string → numeric  | `strtoll` / `strtod` of the full string; an unparseable suffix is an error. |
| numeric → bool    | `0` → FALSE, anything else → TRUE. |
| string → date     | Parses `YYYY-MM-DD`. Other forms error. |
| string → timestamp | Parses `YYYY-MM-DD HH:MM:SS[.uuuuuu]`; `T` also accepted as separator. Trailing tz (e.g. `Z` or `+02:00`) errors — see "Not yet supported". |
| date → timestamp  | Midnight on that date (00:00:00.000000). |
| timestamp → date  | Truncates time-of-day. |
| numeric / bool / date / timestamp → string | ISO 8601 for date/timestamp; `%g` for float; "True" / "False" for bool. |
| date / timestamp → numeric | **Error** — not implicit. Pull the part you want with `DATEPART` / `YEAR` / `MONTH` / `DAY`. |

## NULL semantics (three-valued logic)

Comparisons against NULL return NULL, and NULL propagates through
arithmetic and string concat. AND / OR follow the standard 3VL
truth tables:

```
AND |  T  |  F  |  N            OR  |  T  |  F  |  N
----+-----+-----+-----          ----+-----+-----+-----
 T  |  T  |  F  |  N             T  |  T  |  T  |  T
 F  |  F  |  F  |  F             F  |  T  |  F  |  N
 N  |  N  |  F  |  N             N  |  T  |  N  |  N
```

Both AND and OR are short-circuiting where the truth value is fully
determined by the left operand (`F && _` and `T || _`).

`ISNULL(x)` and `REPLACENULL(check, replacement)` are the two
functions that *observe* NULL rather than propagating it. Every other
function in the table below returns NULL whenever any of its inputs
is NULL.

## Operators

| Operator | Notes |
|---|---|
| `-x`, `!x`, `~x` | Unary negate, logical NOT, bitwise NOT (int only). |
| `*` `/` `%` | Multiplicative. Integer divide-by-zero is an error; float `0/0` follows IEEE 754. |
| `+` `-` | Additive. **`+` on two strings is concatenation**, matching SSIS. |
| `<` `<=` `>` `>=` | Numeric / string / temporal compare; mixed temporal (date vs timestamp) promotes the date to midnight. |
| `==` `!=` | Equality, same rules as ordering. |
| `&&` `\|\|` | 3VL logical AND / OR (see above). |
| `? :` | Ternary: cond must be DT_BOOL (or NULL → result is NULL). |

Bitwise `&` / `|` / `^` / `<<` / `>>` are reserved syntactically but
not implemented (parser rejects with a clear message).

## Functions

Function names are case-insensitive (`LEN` ≡ `Len` ≡ `len`).
NULL-propagation behaviour is as described above unless noted.

### String

| Function | Signature | Behaviour |
|---|---|---|
| `LEN(s)` | `(DT_WSTR) → DT_I8` | Byte length. UTF-8 codepoint counting is a v2 enhancement. |
| `SUBSTRING(s, start, length)` | `(DT_WSTR, DT_I8, DT_I8) → DT_WSTR` | 1-based; `start < 1` errors; `start` past end gives `""`; `length` clamped to remaining. |
| `LEFT(s, n)` | `(DT_WSTR, DT_I8) → DT_WSTR` | n ≤ 0 → `""`. |
| `RIGHT(s, n)` | `(DT_WSTR, DT_I8) → DT_WSTR` | n ≤ 0 → `""`. |
| `TRIM(s)`, `LTRIM(s)`, `RTRIM(s)` | `(DT_WSTR) → DT_WSTR` | Trim ASCII whitespace. |
| `LOWER(s)`, `UPPER(s)` | `(DT_WSTR) → DT_WSTR` | ASCII-only (v1). |
| `REPLACE(s, find, replacement)` | `(DT_WSTR, DT_WSTR, DT_WSTR) → DT_WSTR` | Replace all non-overlapping matches. `find = ""` returns `s` unchanged. |
| `FINDSTRING(s, needle, occurrence)` | `(DT_WSTR, DT_WSTR, DT_I8) → DT_I8` | 1-based position of the N-th match; `0` if not found. |
| `REVERSE(s)` | `(DT_WSTR) → DT_WSTR` | Byte-level reverse. |

### NULL handling

| Function | Signature | Behaviour |
|---|---|---|
| `ISNULL(x)` | `(any) → DT_BOOL` | Does not propagate NULL — returns `TRUE` iff the input is NULL. |
| `REPLACENULL(check, replacement)` | `(any, any) → any` | Returns `replacement` when `check` is NULL, else `check`. |

### Numeric

| Function | Signature | Behaviour |
|---|---|---|
| `ABS(n)` | `(num) → same kind` | Preserves int vs float. |
| `POWER(b, e)` | `(num, num) → DT_R8` | |
| `SQUARE(n)` | `(num) → DT_R8` | |
| `SQRT(n)` | `(num) → DT_R8` | |
| `ROUND(n, d)` | `(num, DT_I8) → DT_R8` | d in `[0, 15]`. |
| `CEILING(n)`, `FLOOR(n)` | `(num) → DT_R8` | |
| `SIGN(n)` | `(num) → DT_I8` | -1 / 0 / 1. |

### Date / timestamp

All temporal functions accept either `DT_DBDATE` or `DT_DBTIMESTAMP`
inputs unless noted; sub-day units require a timestamp.

| Function | Signature | Behaviour |
|---|---|---|
| `GETDATE()` | `() → DT_DBTIMESTAMP` | Wall-clock now from `clock_gettime(CLOCK_REALTIME)`, microsecond precision. |
| `YEAR(d)`, `MONTH(d)`, `DAY(d)` | `(date\|ts) → DT_I8` | |
| `DATEPART(part, d)` | `(DT_WSTR, date\|ts) → DT_I8` | See "Part names" below. |
| `DATEADD(part, n, d)` | `(DT_WSTR, DT_I8, date\|ts) → same kind` | Result type follows input. Sub-day parts on a date input error. Year / quarter / month clamp the day to the last valid day of the target month (e.g. Jan 31 + 1 month = Feb 28 in a non-leap year). |
| `DATEDIFF(part, start, end)` | `(DT_WSTR, date\|ts, date\|ts) → DT_I8` | Truncates to whole units. Day/week count whole calendar boundaries; hour/minute/second use full elapsed micros. |

#### Part names

Each part has full-name + short-form aliases matching SSIS / T-SQL:

| Unit       | Aliases |
|---|---|
| year       | `year`, `yyyy`, `yy` |
| quarter    | `quarter`, `qq`, `q` |
| month      | `month`, `mm`, `m` |
| dayofyear  | `dayofyear`, `dy`, `y` |
| day        | `day`, `dd`, `d` |
| week       | `week`, `wk`, `ww` |
| weekday    | `weekday`, `dw` *(1 = Sunday, 7 = Saturday)* |
| hour       | `hour`, `hh` |
| minute     | `minute`, `mi`, `n` |
| second     | `second`, `ss`, `s` |

The `millisecond` / `microsecond` / `nanosecond` parts are not yet
implemented.

## Not yet supported

These tend to be the high-value migration blockers. They're tracked
for v2.

- **Variables.** `@[User::Foo]` and `@[$Project::Bar]` parse-fail. For
  now, expose project variables as pipeline parameters and reference
  them with `${params.foo}` substitution before the expression is
  compiled.
- **`DT_NUMERIC(p, s)` / `DT_DECIMAL(s)`** — no fixed-precision
  decimal type in betl. Workaround: store currency as `DT_I8` cents,
  do display rounding in the reporting layer.
- **Bitwise operators** — `&` `|` `^` `<<` `>>`.
- **`TOKEN(str, delim, n)`, `TOKENCOUNT(str, delim)`** — string
  splitting.
- **`HEX(n)`, `CODEPOINT(s)`** — value introspection.
- **Locale-aware parsing** — date / number parsers are ASCII /
  invariant-culture only. SSIS' `LocaleID` setting doesn't apply.
- **Time-zone-aware timestamps** — input strings can't carry a `Z`
  or `+HH:MM` suffix. Cast `TIMESTAMPTZ` columns to `TIMESTAMP` in
  your `postgres.read` query.
- **`DT_DBTIME` / `DT_DBTIME2`** — time-of-day type. Workaround:
  embed in a timestamp at a fixed reference date.

## Error handling

- **Compile-time errors** surface from `betl validate` and at run
  start, before any data is touched. Unknown column names, unknown
  function names, wrong arity, and unsupported `DT_*` types all
  belong here.
- **Run-time errors** (e.g. `strtoll` failing on a malformed string
  in `(DT_I8) col`) surface from the first row that hits the bad
  value. The pipeline halts on that row; no rows from the same batch
  are emitted past the failure.

## See also

- `tests/test_ssisexpr.c` — 34 subtests covering literals, casts,
  3VL operators, every function, NULL propagation, and parse / type
  errors. The most precise spec of what the engine accepts.
- `examples/04-ssis-orders-by-month/` — a complete pipeline using
  `ssisexpr` end-to-end (csv.read → ssisexpr → postgres.upsert).
- `docs/EXPR_ABI.md` — the C ABI the engine implements. Read this
  if you want to write a new engine, not if you're just consuming
  one.
