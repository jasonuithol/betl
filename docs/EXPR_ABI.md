# Expression engine ABI

This document specifies the C ABI third-party providers use to plug a
new language into betl's expression slots (`where:` predicates,
`expr:` cells inside `map`'s `add:` / `select:`, etc.). It complements
the generative comments in `include/betl/provider.h` — that header is
authoritative, this page expands on the contract with worked examples
and lifetime rules.

If you are writing a brand-new component (a SOURCE / SINK / TRANSFORM /
TASK), see SPEC §6 instead. Engines are different: they don't process
batches end-to-end, they evaluate user-supplied expressions on rows
the host has already materialized.

## When to use an engine

Use an engine when the *user* supplies code in YAML and you want betl's
built-in transforms (`filter`, `map`) to drive that code on each row.
Examples:

```yaml
# `where:` shorthand defaults to lang: lua
- type: filter
  from: src
  where: "row.amount > 100"

# Explicit lang, compiled once and re-evaluated per batch
- type: map
  from: src
  add:
    discount: { lang: lua, expr: "row.price * 0.1", type: g }
```

The first reaches into the `lua` engine; the second names it
explicitly. Both routes go through the same `BetlExprEngine` lookup.

If you want a *component* (e.g. `lua.task`, `lua.map`) you do that
separately — engines and components are orthogonal. A provider may
expose any combination of the two; betl-lua exposes both.

## Where engines come from

Engines are advertised by providers, the same shared libraries that
contribute components:

```c
static const BetlExprEngine my_engine = {
    .lang     = "duckdb-sql",
    .compile  = my_compile,
    .evaluate = my_evaluate,
    .release  = my_release,
};

static const BetlProvider my_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-duckdb-sql",
    .version         = "0.1.0",
    .license         = "MIT",
    .components      = my_components,
    .component_count = sizeof my_components / sizeof my_components[0],
    .expr_engine     = &my_engine,    /* optional — NULL if no engine */
};
```

`BETL_EXPORT const BetlProvider *betl_provider_entry(void)` returns the
provider; the host loader picks up the engine via the `expr_engine`
field. Built-in engines (`literal`, `lua`) follow the same pattern,
just compiled into `betl_core` rather than dlopen'd.

Two engines registering the same `lang` is a registry-load error. The
host walks providers in registration order; the first to claim a lang
wins.

## The struct

```c
struct BetlExprEngine {
    const char *lang;

    int  (*compile)(BetlContext *ctx,
                    const char *source,
                    const struct ArrowSchema *input_schema,
                    void **handle);

    int  (*evaluate)(void *handle,
                     const struct ArrowArray *input_struct,
                     const char *desired_format,
                     struct ArrowArray *out_array);

    void (*release)(void *handle);

    void *_reserved[4];  /* zero-initialize */
};
```

### `lang`

A stable, non-NULL string. Used as the lookup key for `lang: foo` in
YAML and for `betl_get_expr_engine(ctx, "foo")` from C. Convention:
ASCII lowercase, hyphen-separated.

The pointer's lifetime must be at least as long as the loaded library
— string literals work, malloc'd strings need to outlive the provider.
Built-in engines use a static literal; that's the recommended pattern.

### `compile`

```c
int compile(BetlContext *ctx,
            const char *source,
            const struct ArrowSchema *input_schema,
            void **handle);
```

Called once per `(expression, schema)` pair. The host pre-validates:
- `source` is non-NULL and NUL-terminated.
- `input_schema` is a `+s` (struct) schema describing the row layout
  the engine will see at evaluate time. Children give per-column names
  and Arrow format strings.

On success, write a heap-allocated, opaque pointer to `*handle` and
return `BETL_OK`. The engine owns whatever the handle points to.

On failure, set a human-readable error via `betl_set_error(ctx, ...)`
and return a `BETL_ERR_*` code. The host treats every non-zero return
as fatal for the step.

Compile is the right place to do schema-dependent work: parsing,
type-checking, building a fast accessor for columns by name. Don't
defer this to evaluate — for `filter`/`map`, evaluate runs once per
batch.

### `evaluate`

```c
int evaluate(void *handle,
             const struct ArrowArray *input_struct,
             const char *desired_format,
             struct ArrowArray *out_array);
```

Called once per upstream batch with:
- `handle` — what compile returned.
- `input_struct` — a borrowed `+s` array. The struct's children are
  the columns (`l`, `u`, `g`, `b`, ...). Read but DO NOT free; do not
  store the pointer past the call.
- `desired_format` — the Arrow leaf format the host wants back. For
  `filter`'s predicate, the host requests `"b"` (bool). For `map`'s
  `add:` cells, it's whatever the YAML's `type:` field said.
- `out_array` — caller-allocated `struct ArrowArray` (zero-initialized).
  The engine fills it in.

Write a single Arrow leaf array of length `input_struct->length`. Set
its `release` callback so the host can free it later. Return `BETL_OK`.

`desired_format` coercion:
- The engine SHOULD coerce its natural result type to `desired_format`
  where the conversion is unambiguous. Lua's number → `l` (int64) and
  Lua's boolean → `b` are both required.
- Where coercion is lossy or undefined, return `BETL_ERR_TYPE` and set
  an error message.
- Recognized formats today: `"l"` (int64), `"u"` (utf8), `"b"` (bool),
  `"g"` (float64). Other formats may be added later — engines are not
  required to support every future format, but should reject unknown
  formats explicitly.

### `release`

```c
void release(void *handle);
```

Called exactly once per successful compile, with the same handle.
After release returns, the host will not touch the handle again. Free
all of the handle's memory here.

The host calls release in the destroy path of whatever component
holds the handle — typically when the pipeline tears down. If your
engine uses a runtime (Lua state, Python interpreter), this is where
you tear it down.

## Output array ownership

After `evaluate` returns successfully:
- The host reads from `out_array` (length, buffers, validity bitmap).
- The host calls `out_array->release(out_array)` exactly once, and
  expects all internally-owned memory to be freed by that callback.

The contract is the standard Arrow C Data Interface: `release` is
responsible for every allocation referenced through `out_array`,
including `out_array->buffers[i]` and the `buffers` array itself.

The host does NOT free `out_array` itself (it lives on the stack /
caller frame). Don't write a release that calls `free(arr)` — only
free what `arr` points at.

A reference release for an `int64` leaf:

```c
static void release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);  /* validity bitmap (may be NULL) */
        free((void *)arr->buffers[1]);  /* int64 values */
    }
    free(arr->buffers);
    arr->release = NULL;
}
```

## Looking up an engine from a component

A custom component that wants to host user expressions itself can call:

```c
const BetlExprEngine *eng = betl_get_expr_engine(ctx, "lua");
if (!eng) { /* not registered */ }
void *h = NULL;
int rc = eng->compile(ctx, "row.x + 1", schema, &h);
/* ... evaluate per batch ... */
eng->release(h);
```

This is how the standard `filter` / `map` transforms reach into Lua
without depending on lua.h directly. See `src/runtime/transform_filter.c`
and `src/runtime/transform_map.c` for working callers.

## Reference engines

Two engines ship with betl and serve as worked examples:

- **`literal`** (`src/runtime/literal_expr.c`) — accepts a constant
  string in `value:`, returns it for every row coerced to the
  requested format. Trivial; good starting point for reading the ABI.
- **`lua`** (`providers/betl-lua/lua_provider.c`) — a real engine
  backed by Lua 5.4. Wraps the user's source in a per-row closure,
  builds typed column accumulators, then materializes them as Arrow
  leaves.

Both follow the same lifecycle and the same output ownership rules
described above.

## Versioning + forward compatibility

The struct ends in `void *_reserved[4]`; zero-initialize it. Future
fields may be appended in that block (e.g. an `analyze` callback for
plan-time type inference). Old engines compiled against the current
header will continue to load — the host treats absent reserved slots
as NULL.

ABI-breaking changes bump `BETL_ABI_VERSION`. Providers compiled
against an older `provider.h` get rejected at load time, before any
engine callbacks are invoked.
