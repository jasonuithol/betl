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

SSIS variable references — `@[User::Foo]`, `@[$Project::Bar]` — are
resolved at compile time against the pipeline's parameter table.
Set the value with `betl_context_set_param(ctx, "User::Foo", "...")`
(or the YAML `params:` block, which goes through the same table).
Variables fold into string literals — cast at the call site if a
different type is wanted: `(DT_I8) @[User::Count] + 1`.

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
| `DT_DBTIMESTAMP` | `tsu:` (timestamp_us) | timestamp_us | Microseconds since 1970-01-01 UTC (or `tsu:UTC` for explicitly-tz columns; same int64 layout). |
| `DT_DBTIMESTAMP2`| `tsu:` (timestamp_us) | timestamp_us | Alias for `DT_DBTIMESTAMP`. |
| `DT_NUMERIC`     | `d:p,s` (decimal128) | decimal128 | int128 + scale. `(DT_NUMERIC, p, s)` cast accepts string / int / float / existing decimal (with rescale). `DT_DECIMAL` is an accepted alias. |
| `DT_GUID`        | `w:16` (fixed_binary[16]) | uuid | 16 raw bytes; canonical `xxxxxxxx-xxxx-...` text form on cast to/from string. |
| `DT_BYTES`       | `z` (binary) | bytes | Variable-length byte arrays. Lower-case hex on cast to/from string. `DT_IMAGE` is an accepted alias. |
| `DT_DBTIME`      | `ttu` (time_us) | int64 | Time-of-day cast — parses `HH:MM:SS[.uuuuuu]` to int64 micros-of-day. Result is plain int64 (no distinct time kind), so stringifying via `(DT_WSTR)` gives the raw micros; pipe to a `ttu` sink for `HH:MM:SS` output. `DT_DBTIME2` is an accepted alias. |

Time-zone-aware timestamps flow into the engine as the same int64
microseconds-since-epoch UTC values as plain timestamps (Arrow
`tsu:UTC`); the tz annotation lives on the schema, not the row data.
`DT_DBTIMESTAMPOFFSET` is an accepted alias for `DT_DBTIMESTAMP`
(the offset is normalised at provider boundary).

`DT_NTEXT` is an accepted alias for `DT_WSTR`; `DT_TEXT` an alias
for `DT_STR`.

Not yet supported (the cast / typed-NULL will produce a parse error):

- `DT_DATE` (the obsolete float-of-days form) — use `DT_DBDATE`.

## Casts

```
(DT_xx) value
(DT_WSTR, N) value
(DT_STR, N, cp) value
(DT_NUMERIC, p, s) value
```

| From → To | Behaviour |
|---|---|
| numeric → numeric | Normal C-style truncation / promotion. |
| string → numeric  | `strtoll` / `strtod` of the full string; an unparseable suffix is an error. |
| numeric → bool    | `0` → FALSE, anything else → TRUE. |
| decimal → bool    | `0` → FALSE, anything else → TRUE. |
| string → date     | Parses `YYYY-MM-DD`. Other forms error. |
| string → timestamp | Parses `YYYY-MM-DD HH:MM:SS[.uuuuuu]`; `T` also accepted as separator. A trailing tz suffix (`Z`, `+HH:MM`, `+HHMM`, `+HH`) is accepted and normalised to UTC. |
| date → timestamp  | Midnight on that date (00:00:00.000000). |
| timestamp → date  | Truncates time-of-day. |
| string → decimal  | `(DT_NUMERIC, p, s)` parses with the target scale; over-scale digits must be zero or the cast errors (no silent rounding). |
| int / float → decimal | Promoted to the target scale. Float goes via `printf("%.*f", scale, …)` so libc rounding rules apply. |
| decimal → decimal | Rescaled. Widening scale multiplies by 10ⁿ; narrowing truncates. |
| decimal → int     | Truncates fractional part. |
| decimal → float   | Lossy via int128 → double / 10ˢᶜᵃˡᵉ. |
| decimal → string  | Renders with exactly the scale's fractional digits (zero-padded). |
| string → uuid     | Parses the canonical dashed-hex 36-char form (case-insensitive). |
| uuid → string     | Renders lower-case dashed-hex. |
| numeric / bool / date / timestamp / decimal / uuid → string | ISO 8601 for date/timestamp; `%g` for float; "True" / "False" for bool; canonical text for decimal / uuid. |
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
| `*` `/` `%` | Multiplicative. Integer divide-by-zero is an error; float `0/0` follows IEEE 754. Decimal `*` produces scale = sa + sb (clipped at 38); decimal `/` lands at scale = max(sa, 6) with half-away-from-zero rounding; decimal `%` aligns to the wider scale. |
| `+` `-` | Additive. **`+` on two strings is concatenation**, matching SSIS. Decimal `+` / `-` align operand scales to the wider side; result fits in i128 or errors. Decimal mixed with int promotes the int to a scale-0 decimal; decimal mixed with float falls back to IEEE 754 doubles. |
| `<` `<=` `>` `>=` | Numeric / string / temporal / decimal / uuid compare. Mixed temporal (date vs timestamp) promotes the date to midnight; mixed decimal scales promote to the wider scale; decimal vs int / float falls back to a double-precision compare (~16 decimal digits of precision). UUIDs compare byte-wise. |
| `==` `!=` | Equality, same rules as ordering. |
| `&` `^` `\|` | Bitwise AND / XOR / OR on int64. C-style precedence: tighter than `&&` / `\|\|` but looser than `==` / `!=`, so `(5 & 6) == 4` needs the parens. Non-int operands error at runtime. |
| `~` (unary) | Bitwise NOT on int64. |
| `&&` `\|\|` | 3VL logical AND / OR (see above). |
| `? :` | Ternary: cond must be DT_BOOL (or NULL → result is NULL). |

`<<` and `>>` are not part of SSIS-EL; the parser rejects them.

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
| `TOKEN(s, delim, n)` | `(DT_WSTR, DT_WSTR, DT_I8) → DT_WSTR` | 1-based N-th token. Split on ANY character in `delim`. Consecutive delimiters do not create empty tokens; leading/trailing delimiters are skipped. N past end → `""`. `n < 1` errors. Empty delimiter list returns the whole string as token #1. |
| `TOKENCOUNT(s, delim)` | `(DT_WSTR, DT_WSTR) → DT_I8` | Same splitting rules as `TOKEN`. |
| `HEX(n)` | `(DT_I8) → DT_WSTR` | Uppercase hex, no leading zeros, no prefix. `HEX(0) = "0"`. Negative input errors (no two's-complement convention). |
| `CODEPOINT(s)` | `(DT_WSTR) → DT_I8` | UTF-8 codepoint of the first character. Empty string errors. |

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

- **Locale-aware parsing** — date / number parsers are ASCII /
  invariant-culture only. SSIS' `LocaleID` setting doesn't apply.
- **Narrow-width Arrow integers.** `int8`/`int16`/`int32` are
  accepted at the schema layer and round-trip correctly through
  databases (target column widths are preserved by the driver) but
  widen to int64 in betl's in-memory representation.

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

- `tests/test_ssisexpr.c` — the comprehensive subtest suite, covering
  literals, casts, 3VL operators (incl. decimal arithmetic), every
  function, NULL propagation, and parse / type errors. The most
  precise spec of what the engine accepts.
- `examples/04-ssis-orders-by-month/` — a complete pipeline using
  `ssisexpr` end-to-end (csv.read → ssisexpr → postgres.upsert).
- `docs/EXPR_ABI.md` — the C ABI the engine implements. Read this
  if you want to write a new engine, not if you're just consuming
  one.
