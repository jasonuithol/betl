#include "runtime/builtins.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"

#include "runtime/date_util.h"
#include "runtime/decimal_util.h"
#include "runtime/literal_expr.h"
#include "runtime/transforms.h"

#ifdef BETL_HAVE_LIBPQ
#include "runtime/postgres_upsert.h"
#include "runtime/postgres_lookup.h"
#include "runtime/postgres_read.h"
#endif

#ifdef BETL_HAVE_ODBC
#include "runtime/mssql_upsert.h"
#include "runtime/mssql_lookup.h"
#include "runtime/mssql_read.h"
#endif

/* ============================================================== *
 *  Tiny JSON value extractor                                       *
 *                                                                  *
 *  The configs we need to read are flat objects with int / string  *
 *  values. The YAML->JSON converter produces compact, well-formed  *
 *  output, so the lookups can be done with substring + strtoll /   *
 *  span-to-quote. Doesn't handle nested objects in lookups or      *
 *  escaped quotes inside string values. Sufficient for v0.1; will  *
 *  be replaced when we adopt a real JSON parser.                   *
 * ============================================================== */

static const char *json_value_after(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char needle[64];
    int n = snprintf(needle, sizeof needle, "\"%s\":", key);
    if (n < 0 || (size_t)n >= sizeof needle) return NULL;
    const char *p = strstr(json, needle);
    if (!p) return NULL;
    p += (size_t)n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

static int json_int64(const char *json, const char *key, int64_t *out) {
    const char *v = json_value_after(json, key);
    if (!v) return -1;
    char *end = NULL;
    long long parsed = strtoll(v, &end, 10);
    if (end == v) return -1;
    *out = (int64_t)parsed;
    return 0;
}

static int json_string(const char *json, const char *key, char **out) {
    const char *v = json_value_after(json, key);
    if (!v || *v != '"') return -1;
    ++v;
    const char *end = strchr(v, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - v);
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, v, len);
    s[len] = '\0';
    *out = s;
    return 0;
}

/* ============================================================== *
 *  Arrow release helpers                                           *
 *                                                                  *
 *  Each component owns the buffers it allocates for its emitted    *
 *  arrays and schemas. The Arrow C Data / Stream Interface release *
 *  callback is the standard way for the consumer to hand them      *
 *  back. We allocate child structs with calloc and child pointer   *
 *  arrays with malloc; the release callbacks free both.            *
 * ============================================================== */

static void release_schema_named(struct ArrowSchema *sch) {
    /* Leaf schema with a strdup'd `name`; format is a static literal. */
    free((void *)sch->name);
    sch->release = NULL;
}

static void release_schema_named_owned_format(struct ArrowSchema *sch) {
    /* Variant for decimal columns: `format` was strdup'd (e.g. "d:12,2")
     * and needs freeing alongside `name`. */
    free((void *)sch->name);
    free((void *)sch->format);
    sch->release = NULL;
}

static void release_schema_struct(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i] && sch->children[i]->release) {
            sch->children[i]->release(sch->children[i]);
        }
        free(sch->children[i]);
    }
    free(sch->children);
    sch->release = NULL;
}

static void release_array_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_array_date32_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_array_decimal128_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_array_utf8_leaf(struct ArrowArray *arr) {
    /* utf8 leaf has 3 buffers: validity (may be NULL), int32 offsets,
     * raw byte data. */
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_array_struct(struct ArrowArray *arr) {
    for (int64_t i = 0; i < arr->n_children; ++i) {
        if (arr->children[i] && arr->children[i]->release) {
            arr->children[i]->release(arr->children[i]);
        }
        free(arr->children[i]);
    }
    free(arr->children);
    free(arr->buffers);
    arr->release = NULL;
}

/* ============================================================== *
 *  betl.gen_int64 — SOURCE                                         *
 *                                                                  *
 *  Config:                                                         *
 *    row_count  (int, required)   number of rows to emit           *
 *    column     (string, default "id")    column name              *
 *    start      (int, default 0)         first emitted value       *
 *                                                                  *
 *  Output: struct array, single int64 child, all rows in one batch.*
 * ============================================================== */

typedef struct {
    BetlContext *ctx;
    int64_t      row_count;
    int64_t      start;
    char        *column;
    int          emitted;       /* 0 = not yet, 1 = batch sent */
    char         err[128];
} GenState;

static int gen_init(BetlContext *ctx, const char *cfg, void **state) {
    GenState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;

    if (json_int64(cfg, "row_count", &s->row_count) != 0 || s->row_count < 0) {
        betl_set_error(ctx, "gen_int64: missing or invalid `row_count`");
        free(s);
        return BETL_ERR_INVALID;
    }
    int64_t start;
    s->start = (json_int64(cfg, "start", &start) == 0) ? start : 0;

    char *col = NULL;
    if (json_string(cfg, "column", &col) == 0 && col) {
        s->column = col;
    } else {
        s->column = strdup("id");
        if (!s->column) { free(s); return BETL_ERR_INTERNAL; }
    }
    *state = s;
    return BETL_OK;
}

static void gen_destroy(void *state) {
    GenState *s = state;
    if (!s) return;
    free(s->column);
    free(s);
}

/* Stream callbacks. The stream's private_data is the GenState. */

static int gen_stream_get_schema(struct ArrowArrayStream *st,
                                 struct ArrowSchema *out) {
    GenState *s = st->private_data;
    memset(out, 0, sizeof *out);

    struct ArrowSchema *child = calloc(1, sizeof *child);
    if (!child) return 1;

    char *name = strdup(s->column);
    if (!name) { free(child); return 1; }

    child->format  = "l";   /* int64 */
    child->name    = name;
    child->flags   = ARROW_FLAG_NULLABLE;
    child->release = release_schema_named;

    struct ArrowSchema **kids = malloc(sizeof *kids);
    if (!kids) {
        if (child->release) child->release(child);
        free(child);
        return 1;
    }
    kids[0] = child;

    out->format     = "+s";
    out->n_children = 1;
    out->children   = kids;
    out->release    = release_schema_struct;
    return 0;
}

static int gen_stream_get_next(struct ArrowArrayStream *st,
                               struct ArrowArray *out) {
    GenState *s = st->private_data;
    memset(out, 0, sizeof *out);

    if (s->emitted) {
        /* Empty array signals end-of-stream: release == NULL. */
        return 0;
    }

    int64_t  n   = s->row_count;
    int64_t *vals = malloc((size_t)((n > 0 ? n : 1)) * sizeof *vals);
    if (!vals) return 1;
    for (int64_t i = 0; i < n; ++i) vals[i] = s->start + i;

    /* Inner int64 leaf array. */
    struct ArrowArray *child = calloc(1, sizeof *child);
    const void **child_bufs = malloc(2 * sizeof *child_bufs);
    if (!child || !child_bufs) {
        free(vals); free(child); free(child_bufs); return 1;
    }
    child_bufs[0] = NULL;          /* validity (no nulls) */
    child_bufs[1] = vals;
    child->length     = n;
    child->null_count = 0;
    child->offset     = 0;
    child->n_buffers  = 2;
    child->n_children = 0;
    child->buffers    = child_bufs;
    child->children   = NULL;
    child->dictionary = NULL;
    child->release    = release_array_int64_leaf;

    /* Outer struct array. */
    struct ArrowArray **kids = malloc(sizeof *kids);
    const void **outer_bufs   = malloc(1 * sizeof *outer_bufs);
    if (!kids || !outer_bufs) {
        if (child->release) child->release(child);
        free(child); free(kids); free(outer_bufs); return 1;
    }
    kids[0] = child;
    outer_bufs[0] = NULL;          /* validity */

    out->length     = n;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = 1;
    out->buffers    = outer_bufs;
    out->children   = kids;
    out->dictionary = NULL;
    out->release    = release_array_struct;
    out->private_data = NULL;

    s->emitted = 1;
    return 0;
}

static const char *gen_stream_get_last_error(struct ArrowArrayStream *st) {
    GenState *s = st->private_data;
    return (s && s->err[0]) ? s->err : NULL;
}

static void gen_stream_release(struct ArrowArrayStream *st) {
    /* State is owned by the component, freed in destroy. We just
     * mark the stream as released. */
    st->private_data = NULL;
    st->release      = NULL;
}

static int gen_attach_output(void *state, int port,
                             struct ArrowArrayStream *out) {
    (void)port;
    GenState *s = state;
    out->get_schema     = gen_stream_get_schema;
    out->get_next       = gen_stream_get_next;
    out->get_last_error = gen_stream_get_last_error;
    out->release        = gen_stream_release;
    out->private_data   = s;
    return BETL_OK;
}

static const BetlPortDef gen_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DERIVED, .doc = "int64 rows" },
};

static const BetlComponentDef gen_components[] = {
    { .name               = "betl.gen_int64",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_DETERMINISTIC,
      .outputs            = gen_outputs,
      .output_count       = 1,
      .init               = gen_init,
      .destroy            = gen_destroy,
      .attach_output      = gen_attach_output },
};

static const BetlProvider gen_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-gen",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = gen_components,
    .component_count = sizeof gen_components / sizeof gen_components[0],
};

/* ============================================================== *
 *  betl.gen_strings — SOURCE                                       *
 *                                                                  *
 *  Two-column generator for testing utf8 paths end-to-end:          *
 *    id    int64    (start + i)                                     *
 *    name  utf8     (prefix .. i,  e.g. "row_0", "row_1", ...)     *
 *                                                                  *
 *  Config:                                                          *
 *    row_count  (int,    required)                                  *
 *    start      (int,    default 0)                                 *
 *    prefix     (string, default "row_")                            *
 *    id_column  (string, default "id")                              *
 *    name_column(string, default "name")                            *
 *                                                                  *
 *  Emits one batch with all rows.                                   *
 * ============================================================== */

typedef struct {
    BetlContext *ctx;
    int64_t      row_count;
    int64_t      start;
    char        *prefix;
    char        *id_col;
    char        *name_col;
    int          emitted;
} GenStrState;

static int gens_init(BetlContext *ctx, const char *cfg, void **state) {
    GenStrState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;

    if (json_int64(cfg, "row_count", &s->row_count) != 0 || s->row_count < 0) {
        betl_set_error(ctx, "gen_strings: missing or invalid `row_count`");
        free(s);
        return BETL_ERR_INVALID;
    }
    int64_t start;
    s->start = (json_int64(cfg, "start", &start) == 0) ? start : 0;

    char *p = NULL;
    s->prefix   = (json_string(cfg, "prefix",      &p) == 0 && p) ? p : strdup("row_");
    p = NULL;
    s->id_col   = (json_string(cfg, "id_column",   &p) == 0 && p) ? p : strdup("id");
    p = NULL;
    s->name_col = (json_string(cfg, "name_column", &p) == 0 && p) ? p : strdup("name");

    if (!s->prefix || !s->id_col || !s->name_col) {
        free(s->prefix); free(s->id_col); free(s->name_col); free(s);
        return BETL_ERR_INTERNAL;
    }
    *state = s;
    return BETL_OK;
}

static void gens_destroy(void *state) {
    GenStrState *s = state;
    if (!s) return;
    free(s->prefix); free(s->id_col); free(s->name_col);
    free(s);
}

static int gens_stream_get_schema(struct ArrowArrayStream *st,
                                  struct ArrowSchema *out) {
    GenStrState *s = st->private_data;
    memset(out, 0, sizeof *out);

    struct ArrowSchema **kids = malloc(2 * sizeof *kids);
    if (!kids) return 1;
    kids[0] = NULL; kids[1] = NULL;

    /* int64 child */
    struct ArrowSchema *id = calloc(1, sizeof *id);
    char *id_name = strdup(s->id_col);
    if (!id || !id_name) { free(id); free(id_name); free(kids); return 1; }
    id->format  = "l";
    id->name    = id_name;
    id->flags   = ARROW_FLAG_NULLABLE;
    id->release = release_schema_named;
    kids[0] = id;

    /* utf8 child */
    struct ArrowSchema *nm = calloc(1, sizeof *nm);
    char *nm_name = strdup(s->name_col);
    if (!nm || !nm_name) {
        free(nm); free(nm_name);
        if (kids[0]->release) kids[0]->release(kids[0]);
        free(kids[0]); free(kids); return 1;
    }
    nm->format  = "u";
    nm->name    = nm_name;
    nm->flags   = ARROW_FLAG_NULLABLE;
    nm->release = release_schema_named;
    kids[1] = nm;

    out->format     = "+s";
    out->n_children = 2;
    out->children   = kids;
    out->release    = release_schema_struct;
    return 0;
}

static int gens_stream_get_next(struct ArrowArrayStream *st,
                                struct ArrowArray *out) {
    GenStrState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (s->emitted) return 0;

    int64_t  n   = s->row_count;
    size_t   N   = (size_t)(n > 0 ? n : 0);

    /* int64 column */
    int64_t *id_vals = malloc((N ? N : 1) * sizeof *id_vals);
    if (!id_vals) return 1;
    for (size_t i = 0; i < N; ++i) id_vals[i] = s->start + (int64_t)i;

    /* utf8 column: build offsets + concatenated data buffer. */
    int32_t *offsets = malloc((N + 1) * sizeof *offsets);
    if (!offsets) { free(id_vals); return 1; }
    /* Conservative initial cap: prefix_len * N + 16 * N (digits) + 1. */
    size_t prefix_len = strlen(s->prefix);
    size_t cap = prefix_len * (N ? N : 1) + 16 * (N ? N : 1) + 1;
    char *data = malloc(cap);
    if (!data) { free(id_vals); free(offsets); return 1; }
    size_t pos = 0;
    offsets[0] = 0;
    for (size_t i = 0; i < N; ++i) {
        char idx[32];
        int idl = snprintf(idx, sizeof idx, "%lld", (long long)(s->start + (int64_t)i));
        if (idl < 0) { free(id_vals); free(offsets); free(data); return 1; }
        size_t need = pos + prefix_len + (size_t)idl;
        if (need > cap) {
            size_t nc = cap * 2;
            while (nc < need) nc *= 2;
            char *nd = realloc(data, nc);
            if (!nd) { free(id_vals); free(offsets); free(data); return 1; }
            data = nd; cap = nc;
        }
        memcpy(data + pos,             s->prefix, prefix_len);
        memcpy(data + pos + prefix_len, idx,      (size_t)idl);
        pos += prefix_len + (size_t)idl;
        offsets[i + 1] = (int32_t)pos;
    }

    /* Build int64 leaf. */
    struct ArrowArray *id_arr = calloc(1, sizeof *id_arr);
    const void **id_bufs = malloc(2 * sizeof *id_bufs);
    if (!id_arr || !id_bufs) {
        free(id_arr); free(id_bufs);
        free(id_vals); free(offsets); free(data); return 1;
    }
    id_bufs[0] = NULL;
    id_bufs[1] = id_vals;
    id_arr->length     = n;
    id_arr->null_count = 0;
    id_arr->n_buffers  = 2;
    id_arr->buffers    = id_bufs;
    id_arr->release    = release_array_int64_leaf;

    /* Build utf8 leaf. */
    struct ArrowArray *nm_arr = calloc(1, sizeof *nm_arr);
    const void **nm_bufs = malloc(3 * sizeof *nm_bufs);
    if (!nm_arr || !nm_bufs) {
        if (id_arr->release) id_arr->release(id_arr);
        free(id_arr); free(nm_arr); free(nm_bufs);
        free(offsets); free(data); return 1;
    }
    nm_bufs[0] = NULL;       /* validity */
    nm_bufs[1] = offsets;
    nm_bufs[2] = data;
    nm_arr->length     = n;
    nm_arr->null_count = 0;
    nm_arr->n_buffers  = 3;
    nm_arr->buffers    = nm_bufs;
    nm_arr->release    = release_array_utf8_leaf;

    /* Outer struct array. */
    struct ArrowArray **kids = malloc(2 * sizeof *kids);
    const void **outer_bufs   = malloc(1 * sizeof *outer_bufs);
    if (!kids || !outer_bufs) {
        if (id_arr->release) id_arr->release(id_arr);
        if (nm_arr->release) nm_arr->release(nm_arr);
        free(id_arr); free(nm_arr); free(kids); free(outer_bufs);
        return 1;
    }
    kids[0] = id_arr;
    kids[1] = nm_arr;
    outer_bufs[0] = NULL;

    out->length     = n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = 2;
    out->buffers    = outer_bufs;
    out->children   = kids;
    out->release    = release_array_struct;

    s->emitted = 1;
    return 0;
}

static const char *gens_stream_get_last_error(struct ArrowArrayStream *st) {
    (void)st; return NULL;
}

static void gens_stream_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int gens_attach_output(void *state, int port,
                              struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = gens_stream_get_schema;
    out->get_next       = gens_stream_get_next;
    out->get_last_error = gens_stream_get_last_error;
    out->release        = gens_stream_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef gens_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DERIVED,
      .doc = "(id int64, name utf8)" },
};

static const BetlComponentDef gens_components[] = {
    { .name               = "betl.gen_strings",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_DETERMINISTIC,
      .outputs            = gens_outputs,
      .output_count       = 1,
      .init               = gens_init,
      .destroy            = gens_destroy,
      .attach_output      = gens_attach_output },
};

static const BetlProvider gens_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-gen-strings",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = gens_components,
    .component_count = sizeof gens_components / sizeof gens_components[0],
};

/* ============================================================== *
 *  betl.count_rows — SINK                                          *
 *                                                                  *
 *  Config:                                                         *
 *    expect (int, optional) — fail if total counted != expect      *
 *                                                                  *
 *  Side effect: logs the final total at INFO. The final count is   *
 *  written into ctx->last_error (via betl_set_error) so a host     *
 *  with no log-stream access can still observe it.                 *
 * ============================================================== */

typedef struct {
    BetlContext           *ctx;
    int                    have_expect;
    int64_t                expect;
    struct ArrowArrayStream input;
    int                    have_input;
    int64_t                total_rows;
} CountState;

static int count_init(BetlContext *ctx, const char *cfg, void **state) {
    CountState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    int64_t v;
    if (json_int64(cfg, "expect", &v) == 0) {
        s->expect      = v;
        s->have_expect = 1;
    }
    *state = s;
    return BETL_OK;
}

static int count_attach_input(void *state, int port,
                              struct ArrowArrayStream *in) {
    (void)port;
    CountState *s = state;
    /* Take ownership: copy the stream struct, then zero the source so
     * it can't be released twice. */
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static int count_sink_run(void *state) {
    CountState *s = state;
    if (!s->have_input) {
        betl_set_error(s->ctx, "count_rows: sink_run without attached input");
        return BETL_ERR_INVALID;
    }
    /* Pull schema once. We don't validate the shape — any schema is fine.
     * On failure surface the upstream's get_last_error() if it has one
     * so callers can see the underlying cause (e.g. an unknown column
     * in a map step) rather than a generic message. */
    struct ArrowSchema schema = {0};
    if (s->input.get_schema && s->input.get_schema(&s->input, &schema) != 0) {
        const char *e = s->input.get_last_error
                            ? s->input.get_last_error(&s->input) : NULL;
        betl_set_error(s->ctx, "count_rows: get_schema failed: %s",
                       e ? e : "(no detail)");
        return BETL_ERR_IO;
    }
    if (schema.release) schema.release(&schema);

    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            betl_set_error(s->ctx, "count_rows: cancelled by host");
            return BETL_ERR_CANCELLED;
        }
        struct ArrowArray arr = {0};
        if (s->input.get_next(&s->input, &arr) != 0) {
            betl_set_error(s->ctx, "count_rows: get_next failed: %s",
                           s->input.get_last_error
                               ? s->input.get_last_error(&s->input)
                               : "(no detail)");
            return BETL_ERR_IO;
        }
        if (!arr.release) break;     /* end of stream */
        s->total_rows += arr.length;
        arr.release(&arr);
    }

    betl_log(s->ctx, BETL_LOG_INFO, "count_rows: %lld rows",
             (long long)s->total_rows);
    /* Always record the count so the host can introspect it. */
    betl_set_error(s->ctx, "count_rows: counted %lld rows",
                   (long long)s->total_rows);

    if (s->have_expect && s->expect != s->total_rows) {
        betl_set_error(s->ctx,
            "count_rows: expected %lld rows but counted %lld",
            (long long)s->expect, (long long)s->total_rows);
        return BETL_ERR_INTERNAL;
    }
    return BETL_OK;
}

static void count_destroy(void *state) {
    CountState *s = state;
    if (!s) return;
    if (s->have_input && s->input.release) {
        s->input.release(&s->input);
    }
    free(s);
}

static const BetlPortDef count_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "any rows" },
};

static const BetlComponentDef count_components[] = {
    { .name               = "betl.count_rows",
      .kind               = BETL_KIND_SINK,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_DETERMINISTIC,
      .inputs             = count_inputs,
      .input_count        = 1,
      .init               = count_init,
      .destroy            = count_destroy,
      .attach_input       = count_attach_input,
      .sink_run           = count_sink_run },
};

static const BetlProvider count_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-count",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = count_components,
    .component_count = sizeof count_components / sizeof count_components[0],
};

/* ============================================================== *
 *  csv.read — SOURCE                                               *
 *                                                                  *
 *  v0.1 capabilities:                                              *
 *    - streams the file: emits one Arrow batch per `batch_size`    *
 *      rows (default 1024) instead of materializing the whole file *
 *    - column types: int64 and utf8 (set per column via `schema:`) *
 *    - if `schema:` is omitted, every column defaults to int64     *
 *    - delimiter: single char, default ','                         *
 *    - header: bool, default true. When true, the first line       *
 *      provides column names (schema may override). When false,    *
 *      a schema is required.                                       *
 *    - RFC 4180 quoted fields: a field starting with `"` runs to   *
 *      the matching `"`, with `""` decoded as a literal quote.     *
 *      Quoted fields may contain the delimiter and/or newlines;    *
 *      records that span multiple lines are reassembled before     *
 *      type coercion.                                              *
 *                                                                  *
 *  Config:                                                         *
 *    path        (string, required)                                *
 *    delimiter   (string, optional, default ",")                   *
 *    header      (bool,   optional, default true)                  *
 *    batch_size  (int,    optional, default 1024)                  *
 *    schema:                                                       *
 *      columns:                                                    *
 *        - { name: id,    type: int64 }                            *
 *        - { name: name,  type: utf8  }                            *
 * ============================================================== */

typedef enum {
    CSV_T_INT64         = 1,
    CSV_T_UTF8          = 2,
    CSV_T_DATE32        = 3,  /* Arrow tdD — int32 days since 1970-01-01 */
    CSV_T_TIMESTAMP_US  = 4,  /* Arrow tsu: — int64 micros since 1970-01-01 */
    CSV_T_DECIMAL128    = 5,  /* Arrow d:p,s — int128 + (precision, scale) */
    CSV_T_TIMESTAMP_TZ  = 6,  /* Arrow tsu:UTC — int64 micros normalised to UTC */
} CsvType;

/* Per-column: schema fields (name, type) are set at init and live for
 * the whole stream; the buffer fields below are per-batch staging that
 * gets reused across get_next calls. */
typedef struct {
    char    *name;
    CsvType  type;
    int      dec_precision;  /* decimal columns only */
    int      dec_scale;      /* decimal columns only */
    char    *fmt_string;     /* heap-owned: "d:p,s" for decimal cols, NULL otherwise */
    int64_t *i64_vals;       /* [row_cap] — int64 + timestamp_us */
    int32_t *d32_vals;       /* [row_cap] — date32 */
    betl_dec128 *d128_vals;  /* [row_cap] — decimal128 */
    char   **u8_strs;        /* [row_cap] heap strings (NUL-terminated) */
    size_t  *u8_lens;        /* [row_cap] cached string lengths */
} CsvCol;

typedef struct {
    BetlContext *ctx;
    char        *path;
    char         delim;
    int          header;
    size_t       batch_size;

    /* Streaming reader state. */
    FILE        *fp;
    char        *rec_buf;        /* growable scratch for one logical record */
    size_t       rec_cap;
    int          eof;
    size_t       line_no;        /* 1-based file line, for error messages */

    /* Schema + per-batch staging. row_cap is allocated once to batch_size;
     * n_rows is the row count of the *current* batch and resets between
     * get_next calls. */
    CsvCol *cols;
    size_t  n_cols;
    size_t  n_rows;
    size_t  row_cap;
} CsvState;

/* ---- Record reader (RFC 4180 quote-aware) ------------------------------- */

static int csv_rec_grow(CsvState *s, size_t need) {
    if (need < s->rec_cap) return 0;
    size_t nc = s->rec_cap ? s->rec_cap : 256;
    while (nc <= need) nc *= 2;
    char *p = realloc(s->rec_buf, nc);
    if (!p) return -1;
    s->rec_buf = p;
    s->rec_cap = nc;
    return 0;
}

/* Read one logical CSV record into s->rec_buf. The record covers
 * everything up to a newline that is OUTSIDE quotes (or EOF). Embedded
 * quotes (`""`) and quoted-field contents are stored verbatim — field
 * unescaping happens in parse_field below.
 *
 * Returns:
 *    0 + sets *out_len  — got a record (possibly empty if blank line)
 *   -1                  — clean EOF (no more records)
 *   -2                  — read or OOM error                            */
static int csv_read_record(CsvState *s, size_t *out_len) {
    if (s->eof) return -1;
    size_t n = 0;
    int in_quote = 0;
    int saw_any  = 0;
    for (;;) {
        int c = fgetc(s->fp);
        if (c == EOF) {
            s->eof = 1;
            if (!saw_any) return -1;
            break;
        }
        saw_any = 1;
        if (csv_rec_grow(s, n + 2) != 0) return -2;
        if (in_quote) {
            if (c == '"') {
                int next = fgetc(s->fp);
                if (next == '"') {
                    /* Escaped quote: keep both bytes; parse_field will
                     * collapse them to a single quote during unescape. */
                    s->rec_buf[n++] = '"';
                    s->rec_buf[n++] = '"';
                    continue;
                }
                /* Closing quote. */
                s->rec_buf[n++] = '"';
                in_quote = 0;
                if (next == EOF)  { s->eof = 1; break; }
                if (next == '\n') { s->line_no++; break; }
                if (next == '\r') {
                    int c2 = fgetc(s->fp);
                    if (c2 != EOF && c2 != '\n') ungetc(c2, s->fp);
                    s->line_no++;
                    break;
                }
                if (csv_rec_grow(s, n + 1) != 0) return -2;
                s->rec_buf[n++] = (char)next;
                continue;
            }
            if (c == '\n') s->line_no++;     /* track lines inside quoted */
            s->rec_buf[n++] = (char)c;
            continue;
        }
        /* not in quote */
        if (c == '"') {
            in_quote = 1;
            s->rec_buf[n++] = '"';
            continue;
        }
        if (c == '\n') { s->line_no++; break; }
        if (c == '\r') {
            int c2 = fgetc(s->fp);
            if (c2 != EOF && c2 != '\n') ungetc(c2, s->fp);
            s->line_no++;
            break;
        }
        s->rec_buf[n++] = (char)c;
    }
    if (csv_rec_grow(s, n + 1) != 0) return -2;
    s->rec_buf[n] = '\0';
    *out_len = n;
    return 0;
}

/* ---- Field parser ------------------------------------------------------- */

/* Parse one field from [*p, end). Allocates *out + sets *out_len with
 * the unquoted/unescaped value. Advances *p past the trailing delimiter
 * (or to `end` if this was the last field).
 *
 * Returns:
 *    1 — consumed a delimiter (caller should expect another field)
 *    0 — hit end-of-record (no more fields)
 *   -1 — malformed (e.g. text after a closing quote that isn't `delim`) */
static int csv_parse_field(const char **p, const char *end, char delim,
                           char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    const char *q = *p;
    int quoted = 0;
    if (q < end && *q == '"') { quoted = 1; ++q; }

    if (!quoted) {
        const char *start = q;
        while (q < end && *q != delim) ++q;
        size_t len = (size_t)(q - start);
        char *dup = malloc(len + 1);
        if (!dup) return -1;
        if (len) memcpy(dup, start, len);
        dup[len] = '\0';
        *out = dup;
        *out_len = len;
        if (q < end && *q == delim) { *p = q + 1; return 1; }
        *p = q; return 0;
    }
    /* quoted: bytes between the opening `"` and the next unescaped `"`,
     * with `""` collapsed to a single `"`. We allocate worst-case
     * capacity (remaining bytes) to avoid a growth loop. */
    size_t cap = (size_t)(end - q) + 1;
    char *dup = malloc(cap);
    if (!dup) return -1;
    size_t n = 0;
    int closed = 0;
    while (q < end) {
        char c = *q++;
        if (c == '"') {
            if (q < end && *q == '"') { dup[n++] = '"'; ++q; continue; }
            closed = 1;
            break;
        }
        dup[n++] = c;
    }
    dup[n] = '\0';
    if (!closed) { free(dup); return -1; }      /* unterminated quote */
    *out = dup;
    *out_len = n;
    if (q == end) { *p = q; return 0; }
    if (*q == delim) { *p = q + 1; return 1; }
    /* Anything else after a closing quote (other than delim or EOR) is
     * a malformed field — RFC 4180 strict mode. */
    free(dup); *out = NULL; *out_len = 0;
    return -1;
}

/* Parse the *header* record: turn the bytes in s->rec_buf[0..len)
 * into a malloc'd array of NUL-terminated names. Caller takes
 * ownership of the array and each element. */
static int csv_parse_header(CsvState *s, size_t rec_len,
                            char ***out_names, size_t *out_n) {
    const char *p   = s->rec_buf;
    const char *end = s->rec_buf + rec_len;
    char  **arr = NULL;
    size_t  n   = 0, cap = 0;
    int     more = 1;
    while (more) {
        char  *field = NULL;
        size_t flen  = 0;
        int r = csv_parse_field(&p, end, s->delim, &field, &flen);
        if (r < 0) goto fail;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            char **np = realloc(arr, nc * sizeof *np);
            if (!np) { free(field); goto fail; }
            arr = np; cap = nc;
        }
        arr[n++] = field;
        more = (r == 1);
    }
    *out_names = arr;
    *out_n     = n;
    return 0;
fail:
    for (size_t i = 0; i < n; ++i) free(arr[i]);
    free(arr);
    *out_names = NULL;
    *out_n     = 0;
    return -1;
}

/* Pre-allocate per-column staging so the get_next loop doesn't grow
 * mid-batch. row_cap is set to batch_size; n_rows is reset between
 * batches. */
static int csv_alloc_staging(CsvState *s) {
    if (s->row_cap == s->batch_size) return 0;
    for (size_t c = 0; c < s->n_cols; ++c) {
        CsvCol *col = &s->cols[c];
        if (col->type == CSV_T_INT64 || col->type == CSV_T_TIMESTAMP_US
            || col->type == CSV_T_TIMESTAMP_TZ) {
            int64_t *p = realloc(col->i64_vals,
                                 s->batch_size * sizeof *p);
            if (!p) return -1;
            col->i64_vals = p;
        } else if (col->type == CSV_T_DATE32) {
            int32_t *p = realloc(col->d32_vals,
                                 s->batch_size * sizeof *p);
            if (!p) return -1;
            col->d32_vals = p;
        } else if (col->type == CSV_T_DECIMAL128) {
            betl_dec128 *p = realloc(col->d128_vals,
                                     s->batch_size * sizeof *p);
            if (!p) return -1;
            col->d128_vals = p;
        } else {
            char **sp = realloc(col->u8_strs,
                                s->batch_size * sizeof *sp);
            if (!sp) return -1;
            col->u8_strs = sp;
            size_t *lp = realloc(col->u8_lens,
                                 s->batch_size * sizeof *lp);
            if (!lp) return -1;
            col->u8_lens = lp;
        }
    }
    s->row_cap = s->batch_size;
    return 0;
}

/* Parse a *data* record into the per-column staging arrays at
 * row_idx. Returns 0 on success, -1 on parse / type / shape error. */
static int csv_parse_record_typed(CsvState *s, size_t rec_len,
                                  size_t row_idx) {
    const char *p   = s->rec_buf;
    const char *end = s->rec_buf + rec_len;
    size_t got  = 0;
    int    more = 1;
    while (more) {
        if (got >= s->n_cols) goto bad_shape;
        char  *field = NULL;
        size_t flen  = 0;
        int r = csv_parse_field(&p, end, s->delim, &field, &flen);
        if (r < 0) goto bad_field;
        CsvCol *col = &s->cols[got];
        if (col->type == CSV_T_INT64) {
            char *endp = NULL;
            long long v = strtoll(field, &endp, 10);
            if (endp == field || *endp != '\0') {
                free(field);
                goto bad_field;
            }
            col->i64_vals[row_idx] = (int64_t)v;
            free(field);
        } else if (col->type == CSV_T_DATE32) {
            int32_t days;
            if (betl_parse_iso_date(field, flen, &days) != 0) {
                free(field);
                goto bad_field;
            }
            col->d32_vals[row_idx] = days;
            free(field);
        } else if (col->type == CSV_T_TIMESTAMP_US) {
            int64_t us;
            if (betl_parse_iso_ts(field, flen, &us) != 0) {
                free(field);
                goto bad_field;
            }
            col->i64_vals[row_idx] = us;
            free(field);
        } else if (col->type == CSV_T_TIMESTAMP_TZ) {
            int64_t us;
            if (betl_parse_iso_tstz(field, flen, &us) != 0) {
                free(field);
                goto bad_field;
            }
            col->i64_vals[row_idx] = us;
            free(field);
        } else if (col->type == CSV_T_DECIMAL128) {
            betl_dec128 v;
            if (betl_dec128_parse(field, flen, col->dec_scale, &v) != 0) {
                free(field);
                goto bad_field;
            }
            col->d128_vals[row_idx] = v;
            free(field);
        } else {
            col->u8_strs[row_idx] = field;
            col->u8_lens[row_idx] = flen;
        }
        ++got;
        more = (r == 1);
    }
    if (got != s->n_cols) goto bad_shape;
    return 0;
bad_field:
bad_shape:
    /* Free any utf8 cells we already wrote into this row before the
     * error so the next attempt (or destroy) doesn't double-free. */
    for (size_t k = 0; k < got; ++k) {
        if (s->cols[k].type == CSV_T_UTF8) {
            free(s->cols[k].u8_strs[row_idx]);
            s->cols[k].u8_strs[row_idx] = NULL;
        }
    }
    return -1;
}

/* Free per-row utf8 strings stored in the staging arrays for the
 * current batch, AFTER the Arrow leaf builder has copied them. */
static void csv_free_batch_strings(CsvState *s) {
    for (size_t c = 0; c < s->n_cols; ++c) {
        CsvCol *col = &s->cols[c];
        if (col->type != CSV_T_UTF8 || !col->u8_strs) continue;
        for (size_t r = 0; r < s->n_rows; ++r) {
            free(col->u8_strs[r]);
            col->u8_strs[r] = NULL;
        }
    }
}

static void csv_state_clear_columns(CsvState *s) {
    if (s->cols) {
        for (size_t c = 0; c < s->n_cols; ++c) {
            CsvCol *col = &s->cols[c];
            free(col->name);
            free(col->fmt_string);
            free(col->i64_vals);
            free(col->d32_vals);
            free(col->d128_vals);
            if (col->u8_strs) {
                for (size_t r = 0; r < s->n_rows; ++r) free(col->u8_strs[r]);
                free(col->u8_strs);
            }
            free(col->u8_lens);
        }
        free(s->cols);
        s->cols = NULL;
    }
    s->n_cols = 0;
    s->n_rows = 0;
    s->row_cap = 0;
}

/* ---- Schema parsing ----------------------------------------------------- */

/* Walk a JSON array element-by-element. Same shape/spirit as the walkers
 * in transforms.c — duplicated here to keep builtins.c self-contained. */
typedef int (*csv_item_visit_fn)(const char *value, size_t value_len, void *user);

static int csv_walk_array_at(const char *p, csv_item_visit_fn cb, void *user) {
    if (!p || *p != '[') return -1;
    ++p;
    while (1) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ']' || *p == '\0') return 0;
        const char *vstart = p;
        int depth = 0, in_str = 0;
        while (*p) {
            if (in_str) {
                if (*p == '\\' && p[1]) { p += 2; continue; }
                if (*p == '"') in_str = 0;
                ++p;
                continue;
            }
            if (*p == '"') { in_str = 1; ++p; continue; }
            if (*p == '{' || *p == '[') { ++depth; ++p; continue; }
            if (*p == '}' || *p == ']') {
                if (depth == 0) break;
                --depth; ++p; continue;
            }
            if (*p == ',' && depth == 0) break;
            ++p;
        }
        size_t vlen = (size_t)(p - vstart);
        while (vlen > 0 && (vstart[vlen - 1] == ' '
                         || vstart[vlen - 1] == '\n'
                         || vstart[vlen - 1] == '\t'
                         || vstart[vlen - 1] == '\r')) --vlen;
        if (cb(vstart, vlen, user) != 0) return -1;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == ']' || *p == '\0') return 0;
        return -1;
    }
}

typedef struct {
    CsvState *s;
    int       err;
    char      err_msg[160];
} CsvSchemaCtx;

static int csv_schema_visit(const char *value, size_t value_len, void *user) {
    CsvSchemaCtx *c = user;
    if (value_len == 0 || value[0] != '{') {
        snprintf(c->err_msg, sizeof c->err_msg,
                 "schema entry must be {name, type}");
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *name = NULL, *type = NULL;
    json_string(vbuf, "name", &name);
    json_string(vbuf, "type", &type);
    free(vbuf);

    if (!name || !type) {
        free(name); free(type);
        snprintf(c->err_msg, sizeof c->err_msg,
                 "schema entry needs both 'name' and 'type'");
        c->err = 1; return -1;
    }
    CsvType t;
    int prec = 0, scale = 0;
    if      (strcmp(type, "int64")     == 0) t = CSV_T_INT64;
    else if (strcmp(type, "utf8")      == 0) t = CSV_T_UTF8;
    else if (strcmp(type, "date")      == 0) t = CSV_T_DATE32;
    else if (strcmp(type, "timestamp")   == 0) t = CSV_T_TIMESTAMP_US;
    else if (strcmp(type, "timestamptz") == 0) t = CSV_T_TIMESTAMP_TZ;
    else if (strcmp(type, "decimal")     == 0) {
        t = CSV_T_DECIMAL128;
        /* Required: precision + scale ints in the same {} block. */
        char *vbuf2 = malloc(value_len + 1);
        if (!vbuf2) { free(name); free(type); c->err = 1; return -1; }
        memcpy(vbuf2, value, value_len); vbuf2[value_len] = '\0';
        int64_t pv = 0, sv = 0;
        int ok = (json_int64(vbuf2, "precision", &pv) == 0)
              && (json_int64(vbuf2, "scale",     &sv) == 0);
        free(vbuf2);
        if (!ok || pv < 1 || pv > 38 || sv < 0 || sv > (int)pv) {
            snprintf(c->err_msg, sizeof c->err_msg,
                     "decimal column needs precision in [1,38] and "
                     "scale in [0,precision]");
            free(name); free(type); c->err = 1; return -1;
        }
        prec  = (int)pv;
        scale = (int)sv;
    }
    else {
        snprintf(c->err_msg, sizeof c->err_msg,
                 "unsupported schema type '%s' (supported: int64, utf8, date, "
                 "timestamp, decimal)", type);
        free(name); free(type); c->err = 1; return -1;
    }
    free(type);

    CsvState *s = c->s;
    CsvCol *grow = realloc(s->cols, (s->n_cols + 1) * sizeof *grow);
    if (!grow) { free(name); c->err = 1; return -1; }
    s->cols = grow;
    CsvCol *col = &s->cols[s->n_cols++];
    memset(col, 0, sizeof *col);
    col->name = name;
    col->type = t;
    col->dec_precision = prec;
    col->dec_scale     = scale;
    if (t == CSV_T_DECIMAL128) {
        char buf[24];
        snprintf(buf, sizeof buf, "d:%d,%d", prec, scale);
        col->fmt_string = strdup(buf);
        if (!col->fmt_string) { c->err = 1; return -1; }
    }
    return 0;
}

/* Parse `schema: { columns: [...] }` if present. Returns 0 on success
 * (which may mean "no schema given" — caller falls back to header-derived
 * all-int64). Returns -1 on a malformed schema. Sets c->err_msg on error. */
static int csv_parse_schema(CsvState *s, const char *cfg, CsvSchemaCtx *out) {
    out->s = s;
    out->err = 0;
    out->err_msg[0] = '\0';
    const char *sch = json_value_after(cfg, "schema");
    if (!sch || *sch != '{') return 0;     /* no schema specified */
    /* Find columns inside the schema object. The top-level json_value_after
     * doesn't enter nested objects, but our schema has columns at the same
     * top level as far as the strstr-based search is concerned. Caveat:
     * if some other key happened to be named "columns", we'd find it
     * instead. v0.1 limitation. */
    const char *cols = json_value_after(cfg, "columns");
    if (!cols || *cols != '[') {
        snprintf(out->err_msg, sizeof out->err_msg,
                 "schema must contain a `columns:` list");
        out->err = 1;
        return -1;
    }
    if (csv_walk_array_at(cols, csv_schema_visit, out) != 0 || out->err) {
        return -1;
    }
    if (s->n_cols == 0) {
        snprintf(out->err_msg, sizeof out->err_msg,
                 "schema columns: list is empty");
        out->err = 1;
        return -1;
    }
    return 0;
}

static int csv_init(BetlContext *ctx, const char *cfg, void **state) {
    CsvState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx        = ctx;
    s->delim      = ',';
    s->header     = 1;
    s->batch_size = 1024;
    s->line_no    = 0;

    if (json_string(cfg, "path", &s->path) != 0 || !s->path) {
        betl_set_error(ctx, "csv.read: missing required `path`");
        free(s);
        return BETL_ERR_INVALID;
    }
    char *delim_str = NULL;
    if (json_string(cfg, "delimiter", &delim_str) == 0 && delim_str) {
        if (delim_str[0] && delim_str[0] != '"') s->delim = delim_str[0];
        free(delim_str);
    }
    /* `header` is a bool literal in JSON — extract via the same value
     * scanner. */
    {
        const char *v = json_value_after(cfg, "header");
        if (v) {
            if (strncmp(v, "false", 5) == 0) s->header = 0;
            else if (strncmp(v, "true", 4) == 0) s->header = 1;
        }
    }
    /* `batch_size` int literal — anything <= 0 falls back to default. */
    {
        int64_t bs = 0;
        if (json_int64(cfg, "batch_size", &bs) == 0 && bs > 0) {
            s->batch_size = (size_t)bs;
        }
    }

    /* schema: { columns: [...] } — optional. If present, gives names+types
     * directly. If absent and header=true, names come from the header line
     * and every column defaults to int64. If absent and header=false, the
     * file has no way to tell us its columns and we error out. */
    CsvSchemaCtx schema_ctx = {0};
    if (csv_parse_schema(s, cfg, &schema_ctx) != 0) {
        betl_set_error(ctx, "csv.read: %s", schema_ctx.err_msg);
        csv_state_clear_columns(s);
        free(s->path); free(s);
        return BETL_ERR_INVALID;
    }
    int schema_provided = (s->n_cols > 0);

    s->fp = fopen(s->path, "r");
    if (!s->fp) {
        betl_set_error(ctx, "csv.read: cannot open %s", s->path);
        csv_state_clear_columns(s);
        free(s->path); free(s);
        return BETL_ERR_IO;
    }
    s->line_no = 1;

    if (s->header) {
        size_t hlen = 0;
        int rr = csv_read_record(s, &hlen);
        if (rr != 0) {
            betl_set_error(ctx, "csv.read: %s reading header in %s",
                           rr == -1 ? "empty file" : "read error", s->path);
            fclose(s->fp); s->fp = NULL;
            csv_state_clear_columns(s);
            free(s->path); free(s);
            return BETL_ERR_IO;
        }
        if (!schema_provided) {
            char **names = NULL;
            size_t n = 0;
            if (csv_parse_header(s, hlen, &names, &n) != 0 || n == 0) {
                betl_set_error(ctx, "csv.read: failed to parse header in %s",
                               s->path);
                fclose(s->fp); s->fp = NULL;
                csv_state_clear_columns(s);
                free(s->path); free(s);
                return BETL_ERR_IO;
            }
            s->cols = calloc(n, sizeof *s->cols);
            if (!s->cols) {
                for (size_t i = 0; i < n; ++i) free(names[i]);
                free(names);
                fclose(s->fp); s->fp = NULL;
                free(s->path); free(s);
                return BETL_ERR_INTERNAL;
            }
            s->n_cols = n;
            for (size_t i = 0; i < n; ++i) {
                s->cols[i].name = names[i];
                s->cols[i].type = CSV_T_INT64;
            }
            free(names);
        }
        /* If schema was provided, header was consumed but discarded. */
    } else if (!schema_provided) {
        betl_set_error(ctx, "csv.read: when header=false, a `schema:` is required");
        fclose(s->fp); s->fp = NULL;
        csv_state_clear_columns(s);
        free(s->path); free(s);
        return BETL_ERR_INVALID;
    }

    /* Allocate per-column staging once at batch_size capacity. */
    if (csv_alloc_staging(s) != 0) {
        fclose(s->fp); s->fp = NULL;
        csv_state_clear_columns(s);
        free(s->path); free(s);
        return BETL_ERR_INTERNAL;
    }

    *state = s;
    return BETL_OK;
}

static void csv_destroy(void *state) {
    CsvState *s = state;
    if (!s) return;
    if (s->fp) fclose(s->fp);
    free(s->rec_buf);
    free(s->path);
    csv_state_clear_columns(s);
    free(s);
}

static const char *csv_format_for_col(const CsvCol *c) {
    switch (c->type) {
        case CSV_T_INT64:        return "l";
        case CSV_T_DATE32:       return "tdD";
        case CSV_T_TIMESTAMP_US: return "tsu:";
        case CSV_T_TIMESTAMP_TZ: return "tsu:UTC";
        case CSV_T_UTF8:         return "u";
        case CSV_T_DECIMAL128:   return c->fmt_string ? c->fmt_string : "u";
    }
    return "u";
}

static int csv_stream_get_schema(struct ArrowArrayStream *st,
                                 struct ArrowSchema *out) {
    CsvState *s = st->private_data;
    memset(out, 0, sizeof *out);

    struct ArrowSchema **kids = malloc(s->n_cols * sizeof *kids);
    if (!kids) return 1;
    for (size_t i = 0; i < s->n_cols; ++i) kids[i] = NULL;

    for (size_t i = 0; i < s->n_cols; ++i) {
        struct ArrowSchema *child = calloc(1, sizeof *child);
        char *name = strdup(s->cols[i].name);
        if (!child || !name) {
            free(child); free(name);
            for (size_t j = 0; j < i; ++j) {
                if (kids[j]->release) kids[j]->release(kids[j]);
                free(kids[j]);
            }
            free(kids);
            return 1;
        }
        if (s->cols[i].type == CSV_T_DECIMAL128) {
            /* "d:p,s" needs its own allocation tied to the schema's
             * lifetime, since the stream may be released before the
             * downstream consumer is done with the schema. */
            child->format  = strdup(csv_format_for_col(&s->cols[i]));
            child->release = release_schema_named_owned_format;
            if (!child->format) {
                free(child); free(name);
                for (size_t j = 0; j < i; ++j) {
                    if (kids[j]->release) kids[j]->release(kids[j]);
                    free(kids[j]);
                }
                free(kids);
                return 1;
            }
        } else {
            child->format  = csv_format_for_col(&s->cols[i]);
            child->release = release_schema_named;
        }
        child->name    = name;
        child->flags   = ARROW_FLAG_NULLABLE;
        kids[i] = child;
    }

    out->format     = "+s";
    out->n_children = (int64_t)s->n_cols;
    out->children   = kids;
    out->release    = release_schema_struct;
    return 0;
}

/* Build a fresh utf8 leaf from per-row strings + lengths. Allocates
 * offsets + concatenated data buffer. */
static int csv_build_utf8_leaf(struct ArrowArray *out,
                               char **strs, size_t *lens, size_t n) {
    int32_t *offsets = malloc((n + 1) * sizeof *offsets);
    if (!offsets) return -1;
    size_t total = 0;
    offsets[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        total += lens[i];
        if (total > (size_t)INT32_MAX) { free(offsets); return -1; }
        offsets[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offsets); return -1; }
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        if (lens[i]) memcpy(data + pos, strs[i], lens[i]);
        pos += lens[i];
    }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offsets); free(data); return -1; }
    bufs[0] = NULL;
    bufs[1] = offsets;
    bufs[2] = data;

    out->length     = (int64_t)n;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 3;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = release_array_utf8_leaf;
    return 0;
}

static int csv_stream_get_next(struct ArrowArrayStream *st,
                               struct ArrowArray *out) {
    CsvState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (s->eof && s->n_rows == 0) return 0;   /* end-of-stream */

    /* Fill staging up to batch_size or EOF. Blank lines (a record with
     * length 0) are skipped, matching the previous behavior. */
    s->n_rows = 0;
    while (s->n_rows < s->batch_size && !s->eof) {
        size_t rec_len = 0;
        int rr = csv_read_record(s, &rec_len);
        if (rr == -1) break;        /* EOF — emit whatever we have */
        if (rr == -2) {
            betl_set_error(s->ctx, "csv.read: I/O error reading %s", s->path);
            return 1;
        }
        if (rec_len == 0) continue; /* blank line */
        if (csv_parse_record_typed(s, rec_len, s->n_rows) != 0) {
            betl_set_error(s->ctx,
                "csv.read: parse error in %s near line %zu "
                "(expected %zu fields, %s)",
                s->path, s->line_no, s->n_cols,
                "type or quoting mismatch");
            return 1;
        }
        s->n_rows++;
    }
    if (s->n_rows == 0) return 0;   /* clean EOF */

    int64_t n = (int64_t)s->n_rows;

    struct ArrowArray **kids = calloc(s->n_cols, sizeof *kids);
    if (!kids) return 1;
    for (size_t c = 0; c < s->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c]) {
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            return 1;
        }
        if (s->cols[c].type == CSV_T_INT64
            || s->cols[c].type == CSV_T_TIMESTAMP_US
            || s->cols[c].type == CSV_T_TIMESTAMP_TZ) {
            int64_t *vals = malloc((size_t)((n > 0 ? n : 1)) * sizeof *vals);
            if (!vals) {
                free(kids[c]);
                for (size_t k = 0; k < c; ++k) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return 1;
            }
            if (n > 0) memcpy(vals, s->cols[c].i64_vals,
                              (size_t)n * sizeof *vals);
            const void **bufs = malloc(2 * sizeof *bufs);
            if (!bufs) {
                free(vals); free(kids[c]);
                for (size_t k = 0; k < c; ++k) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return 1;
            }
            bufs[0] = NULL;
            bufs[1] = vals;
            kids[c]->length     = n;
            kids[c]->null_count = 0;
            kids[c]->n_buffers  = 2;
            kids[c]->buffers    = bufs;
            kids[c]->release    = release_array_int64_leaf;
        } else if (s->cols[c].type == CSV_T_DATE32) {
            int32_t *vals = malloc((size_t)((n > 0 ? n : 1)) * sizeof *vals);
            if (!vals) {
                free(kids[c]);
                for (size_t k = 0; k < c; ++k) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return 1;
            }
            if (n > 0) memcpy(vals, s->cols[c].d32_vals,
                              (size_t)n * sizeof *vals);
            const void **bufs = malloc(2 * sizeof *bufs);
            if (!bufs) {
                free(vals); free(kids[c]);
                for (size_t k = 0; k < c; ++k) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return 1;
            }
            bufs[0] = NULL;
            bufs[1] = vals;
            kids[c]->length     = n;
            kids[c]->null_count = 0;
            kids[c]->n_buffers  = 2;
            kids[c]->buffers    = bufs;
            kids[c]->release    = release_array_date32_leaf;
        } else if (s->cols[c].type == CSV_T_DECIMAL128) {
            betl_dec128 *vals = malloc((size_t)((n > 0 ? n : 1)) * sizeof *vals);
            if (!vals) {
                free(kids[c]);
                for (size_t k = 0; k < c; ++k) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return 1;
            }
            if (n > 0) memcpy(vals, s->cols[c].d128_vals,
                              (size_t)n * sizeof *vals);
            const void **bufs = malloc(2 * sizeof *bufs);
            if (!bufs) {
                free(vals); free(kids[c]);
                for (size_t k = 0; k < c; ++k) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return 1;
            }
            bufs[0] = NULL;
            bufs[1] = vals;
            kids[c]->length     = n;
            kids[c]->null_count = 0;
            kids[c]->n_buffers  = 2;
            kids[c]->buffers    = bufs;
            kids[c]->release    = release_array_decimal128_leaf;
        } else {
            if (csv_build_utf8_leaf(kids[c], s->cols[c].u8_strs,
                                    s->cols[c].u8_lens, (size_t)n) != 0) {
                free(kids[c]);
                for (size_t k = 0; k < c; ++k) {
                    if (kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return 1;
            }
        }
    }

    const void **outer_bufs = malloc(1 * sizeof *outer_bufs);
    if (!outer_bufs) {
        for (size_t c = 0; c < s->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        return 1;
    }
    outer_bufs[0] = NULL;

    out->length     = n;
    out->n_buffers  = 1;
    out->n_children = (int64_t)s->n_cols;
    out->buffers    = outer_bufs;
    out->children   = kids;
    out->release    = release_array_struct;

    /* csv_build_utf8_leaf copied each row's bytes into the Arrow data
     * buffer, so the per-row scratch strings are no longer needed.
     * Free them and reset the pointers ahead of the next batch. */
    csv_free_batch_strings(s);
    return 0;
}

static const char *csv_stream_get_last_error(struct ArrowArrayStream *st) {
    (void)st; return NULL;
}

static void csv_stream_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int csv_attach_output(void *state, int port,
                             struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = csv_stream_get_schema;
    out->get_next       = csv_stream_get_next;
    out->get_last_error = csv_stream_get_last_error;
    out->release        = csv_stream_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef csv_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DERIVED, .doc = "csv rows" },
};

/* ============================================================== *
 *  csv.write — SINK                                                *
 *                                                                  *
 *  v0.1 capabilities:                                              *
 *    - writes the entire input stream to a single text file        *
 *    - column types: int64 (`l`) and utf8 (`u`); other Arrow       *
 *      formats are rejected at sink_run time                       *
 *    - delimiter: single char, default ','                         *
 *    - header: bool, default true                                  *
 *    - utf8 fields are RFC 4180 quoted on demand: a value          *
 *      containing the delimiter, a double-quote, CR, or LF is      *
 *      wrapped in "..." with internal quotes doubled. (csv.read    *
 *      v0.1 doesn't yet round-trip quoted fields — that asymmetry  *
 *      is documented; csv.write produces the standard form so      *
 *      external consumers behave correctly.)                       *
 *    - NULL cells are emitted as the empty string                  *
 *                                                                  *
 *  Config:                                                         *
 *    path       (string, required)                                 *
 *    delimiter  (string, optional, default ",")                    *
 *    header     (bool,   optional, default true)                   *
 * ============================================================== */

typedef struct {
    BetlContext              *ctx;
    char                     *path;
    char                      delim;
    int                       header;
    struct ArrowArrayStream   input;
    int                       have_input;
} CsvWriteState;

static int csv_write_init(BetlContext *ctx, const char *cfg, void **state) {
    CsvWriteState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx    = ctx;
    s->delim  = ',';
    s->header = 1;

    if (json_string(cfg, "path", &s->path) != 0 || !s->path) {
        betl_set_error(ctx, "csv.write: missing required `path`");
        free(s);
        return BETL_ERR_INVALID;
    }
    char *delim_str = NULL;
    if (json_string(cfg, "delimiter", &delim_str) == 0 && delim_str) {
        if (delim_str[0] && delim_str[0] != '"') s->delim = delim_str[0];
        free(delim_str);
    }
    {
        const char *v = json_value_after(cfg, "header");
        if (v) {
            if (strncmp(v, "false", 5) == 0) s->header = 0;
            else if (strncmp(v, "true", 4) == 0) s->header = 1;
        }
    }
    *state = s;
    return BETL_OK;
}

static void csv_write_destroy(void *state) {
    CsvWriteState *s = state;
    if (!s) return;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    free(s->path);
    free(s);
}

static int csv_write_attach_input(void *state, int port,
                                  struct ArrowArrayStream *in) {
    (void)port;
    CsvWriteState *s = state;
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

/* True if the validity bitmap marks (col, row) as null. NULL bitmap
 * means "all valid". Mirrors the helper in postgres_upsert.c. */
static int csv_write_is_null(const struct ArrowArray *a, int64_t row) {
    if (a->n_buffers < 1 || !a->buffers[0]) return 0;
    const uint8_t *v = a->buffers[0];
    int64_t bit = row + a->offset;
    return ((v[bit / 8] >> (bit % 8)) & 1) == 0;
}

/* Emit a utf8 cell with RFC 4180 quoting when needed. */
static int csv_write_utf8_cell(FILE *fp, char delim,
                               const char *data, size_t len) {
    int needs_quote = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = data[i];
        if (c == delim || c == '"' || c == '\n' || c == '\r') {
            needs_quote = 1;
            break;
        }
    }
    if (!needs_quote) {
        if (len && fwrite(data, 1, len, fp) != len) return -1;
        return 0;
    }
    if (fputc('"', fp) == EOF) return -1;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '"') {
            if (fputs("\"\"", fp) == EOF) return -1;
        } else {
            if (fputc(data[i], fp) == EOF) return -1;
        }
    }
    if (fputc('"', fp) == EOF) return -1;
    return 0;
}

static int csv_write_render_cell(FILE *fp, char delim,
                                 const struct ArrowArray *col,
                                 const char *fmt, int64_t row) {
    if (csv_write_is_null(col, row)) return 0; /* empty field */
    int64_t off = col->offset + row;
    if (strcmp(fmt, "l") == 0) {
        const int64_t *vals = col->buffers[1];
        if (fprintf(fp, "%" PRId64, vals[off]) < 0) return -1;
        return 0;
    }
    if (strcmp(fmt, "u") == 0) {
        const int32_t *offs = col->buffers[1];
        const char    *data = col->buffers[2];
        int32_t start = offs[off];
        int32_t end   = offs[off + 1];
        return csv_write_utf8_cell(fp, delim, data + start,
                                   (size_t)(end - start));
    }
    if (strcmp(fmt, "tdD") == 0) {
        const int32_t *vals = col->buffers[1];
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(vals[off], &y, &m, &d);
        if (fprintf(fp, "%04d-%02u-%02u", y, m, d) < 0) return -1;
        return 0;
    }
    if (strcmp(fmt, "tsu:") == 0) {
        const int64_t *vals = col->buffers[1];
        int32_t days; int64_t us_of_day;
        betl_split_ts(vals[off], &days, &us_of_day);
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(days, &y, &m, &d);
        int hh   = (int)(us_of_day / 3600000000LL);
        int mm   = (int)((us_of_day / 60000000LL) % 60);
        int ss   = (int)((us_of_day / 1000000LL) % 60);
        int frac = (int)(us_of_day % 1000000LL);
        int rc;
        if (frac == 0) {
            rc = fprintf(fp, "%04d-%02u-%02u %02d:%02d:%02d",
                         y, m, d, hh, mm, ss);
        } else {
            rc = fprintf(fp, "%04d-%02u-%02u %02d:%02d:%02d.%06d",
                         y, m, d, hh, mm, ss, frac);
        }
        return rc < 0 ? -1 : 0;
    }
    if (strcmp(fmt, "tsu:UTC") == 0) {
        /* UTC-pinned timestamp: same layout as `tsu:`, render with a
         * trailing 'Z'. */
        const int64_t *vals = col->buffers[1];
        int32_t days; int64_t us_of_day;
        betl_split_ts(vals[off], &days, &us_of_day);
        int y = 0; unsigned m = 0, d = 0;
        betl_civil_from_days(days, &y, &m, &d);
        int hh   = (int)(us_of_day / 3600000000LL);
        int mm   = (int)((us_of_day / 60000000LL) % 60);
        int ss   = (int)((us_of_day / 1000000LL) % 60);
        int frac = (int)(us_of_day % 1000000LL);
        int rc;
        if (frac == 0) {
            rc = fprintf(fp, "%04d-%02u-%02u %02d:%02d:%02dZ",
                         y, m, d, hh, mm, ss);
        } else {
            rc = fprintf(fp, "%04d-%02u-%02u %02d:%02d:%02d.%06dZ",
                         y, m, d, hh, mm, ss, frac);
        }
        return rc < 0 ? -1 : 0;
    }
    if (strncmp(fmt, "d:", 2) == 0) {
        int p = 0, s = 0;
        if (sscanf(fmt + 2, "%d,%d", &p, &s) != 2) return -2;
        const betl_dec128 *vals = col->buffers[1];
        char buf[48];
        int n = betl_dec128_format(vals[off], s, buf, sizeof buf);
        if (n < 0) return -1;
        return csv_write_utf8_cell(fp, delim, buf, (size_t)n);
    }
    return -2; /* unsupported format */
}

static int csv_write_sink_run(void *state) {
    CsvWriteState *s = state;
    if (!s->have_input) {
        betl_set_error(s->ctx, "csv.write: sink_run without attached input");
        return BETL_ERR_INVALID;
    }

    struct ArrowSchema schema = {0};
    if (s->input.get_schema(&s->input, &schema) != 0) {
        const char *up = s->input.get_last_error
            ? s->input.get_last_error(&s->input) : NULL;
        betl_set_error(s->ctx, "csv.write: get_schema failed%s%s",
                       up ? ": " : "", up ? up : "");
        return BETL_ERR_IO;
    }
    if (!schema.format || strcmp(schema.format, "+s") != 0) {
        betl_set_error(s->ctx,
            "csv.write: input must be a struct stream (got '%s')",
            schema.format ? schema.format : "(null)");
        if (schema.release) schema.release(&schema);
        return BETL_ERR_TYPE;
    }
    int64_t n_cols = schema.n_children;
    if (n_cols <= 0) {
        betl_set_error(s->ctx, "csv.write: input stream has no columns");
        schema.release(&schema);
        return BETL_ERR_TYPE;
    }
    /* Validate every column's format up front. Bail before opening the
     * file so a bad type doesn't leave a half-written artefact behind. */
    for (int64_t i = 0; i < n_cols; ++i) {
        const char *fmt = schema.children[i]->format;
        int ok = fmt && (strcmp(fmt, "l")        == 0 ||
                         strcmp(fmt, "u")        == 0 ||
                         strcmp(fmt, "tdD")      == 0 ||
                         strcmp(fmt, "tsu:")     == 0 ||
                         strcmp(fmt, "tsu:UTC")  == 0 ||
                         strncmp(fmt, "d:", 2)   == 0);
        if (!ok) {
            betl_set_error(s->ctx,
                "csv.write: column '%s' has unsupported Arrow format '%s' "
                "(supported: int64 'l', utf8 'u', date 'tdD', timestamp 'tsu:', "
                "decimal 'd:p,s')",
                schema.children[i]->name ? schema.children[i]->name : "?",
                fmt ? fmt : "(null)");
            schema.release(&schema);
            return BETL_ERR_TYPE;
        }
    }

    FILE *fp = fopen(s->path, "w");
    if (!fp) {
        betl_set_error(s->ctx, "csv.write: cannot open %s for writing",
                       s->path);
        schema.release(&schema);
        return BETL_ERR_IO;
    }

    int rc = BETL_OK;

    if (s->header) {
        for (int64_t i = 0; i < n_cols; ++i) {
            if (i > 0 && fputc(s->delim, fp) == EOF) goto io_err;
            const char *nm = schema.children[i]->name;
            size_t nlen = nm ? strlen(nm) : 0;
            if (csv_write_utf8_cell(fp, s->delim, nm ? nm : "", nlen) != 0) {
                goto io_err;
            }
        }
        if (fputc('\n', fp) == EOF) goto io_err;
    }

    /* Capture per-column formats now so we don't dereference schema
     * children during cell rendering. */
    const char **fmts = malloc((size_t)n_cols * sizeof *fmts);
    if (!fmts) { rc = BETL_ERR_INTERNAL; goto cleanup; }
    for (int64_t i = 0; i < n_cols; ++i) fmts[i] = schema.children[i]->format;

    while (1) {
        struct ArrowArray batch = {0};
        if (s->input.get_next(&s->input, &batch) != 0) {
            const char *up = s->input.get_last_error
                ? s->input.get_last_error(&s->input) : NULL;
            betl_set_error(s->ctx, "csv.write: get_next failed%s%s",
                           up ? ": " : "", up ? up : "");
            rc = BETL_ERR_IO;
            free(fmts);
            goto cleanup;
        }
        if (!batch.release) break;          /* end-of-stream */
        if (batch.n_children != n_cols) {
            betl_set_error(s->ctx,
                "csv.write: batch has %" PRId64 " columns, schema declared %" PRId64,
                batch.n_children, n_cols);
            batch.release(&batch);
            rc = BETL_ERR_TYPE;
            free(fmts);
            goto cleanup;
        }
        for (int64_t r = 0; r < batch.length; ++r) {
            for (int64_t c = 0; c < n_cols; ++c) {
                if (c > 0 && fputc(s->delim, fp) == EOF) {
                    batch.release(&batch);
                    free(fmts);
                    goto io_err;
                }
                int crc = csv_write_render_cell(fp, s->delim,
                                                batch.children[c],
                                                fmts[c], r);
                if (crc != 0) {
                    betl_set_error(s->ctx,
                        crc == -2
                          ? "csv.write: column '%s' format changed mid-stream"
                          : "csv.write: write error on %s",
                        crc == -2
                          ? schema.children[c]->name
                          : s->path);
                    batch.release(&batch);
                    free(fmts);
                    rc = (crc == -2) ? BETL_ERR_TYPE : BETL_ERR_IO;
                    goto cleanup;
                }
            }
            if (fputc('\n', fp) == EOF) {
                batch.release(&batch);
                free(fmts);
                goto io_err;
            }
        }
        batch.release(&batch);
    }
    free(fmts);

cleanup:
    if (fclose(fp) != 0 && rc == BETL_OK) {
        betl_set_error(s->ctx, "csv.write: fclose failed on %s", s->path);
        rc = BETL_ERR_IO;
    }
    schema.release(&schema);
    return rc;

io_err:
    betl_set_error(s->ctx, "csv.write: write error on %s", s->path);
    fclose(fp);
    schema.release(&schema);
    return BETL_ERR_IO;
}

static const BetlPortDef csv_write_inputs[] = {
    { .name = "in", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to write" },
};

static const BetlComponentDef csv_components[] = {
    { .name               = "csv.read",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = BETL_FLAG_DETERMINISTIC,
      .outputs            = csv_outputs,
      .output_count       = 1,
      .init               = csv_init,
      .destroy            = csv_destroy,
      .attach_output      = csv_attach_output },
    { .name               = "csv.write",
      .kind               = BETL_KIND_SINK,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = csv_write_inputs,
      .input_count        = 1,
      .init               = csv_write_init,
      .destroy            = csv_write_destroy,
      .attach_input       = csv_write_attach_input,
      .sink_run           = csv_write_sink_run },
};

static const BetlProvider csv_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-csv",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = csv_components,
    .component_count = sizeof csv_components / sizeof csv_components[0],
};

/* ============================================================== *
 *  Registry helper                                                 *
 * ============================================================== */

int betl_register_builtins(BetlRegistry *r) {
    int rc;
    rc = betl_registry_register(r, &gen_provider,   "<builtin:gen>");
    if (rc != BETL_OK) return rc;
    rc = betl_registry_register(r, &gens_provider,  "<builtin:gen-strings>");
    if (rc != BETL_OK) return rc;
    rc = betl_registry_register(r, &count_provider, "<builtin:count>");
    if (rc != BETL_OK) return rc;
    rc = betl_registry_register(r, &csv_provider,   "<builtin:csv>");
    if (rc != BETL_OK) return rc;
    rc = betl_register_literal_engine(r);
    if (rc != BETL_OK) return rc;
    rc = betl_register_transforms(r);
    if (rc != BETL_OK) return rc;
#ifdef BETL_HAVE_LIBPQ
    rc = betl_register_postgres(r);
    if (rc != BETL_OK) return rc;
    rc = betl_register_postgres_lookup(r);
    if (rc != BETL_OK) return rc;
    rc = betl_register_postgres_read(r);
    if (rc != BETL_OK) return rc;
#endif
#ifdef BETL_HAVE_ODBC
    rc = betl_register_mssql(r);
    if (rc != BETL_OK) return rc;
    rc = betl_register_mssql_lookup(r);
    if (rc != BETL_OK) return rc;
    rc = betl_register_mssql_read(r);
    if (rc != BETL_OK) return rc;
#endif
    return rc;
}
