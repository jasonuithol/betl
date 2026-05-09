/* Standard data-flow transforms: filter and map.
 *
 * Both compile their expressions through the BetlExprEngine ABI from
 * SPEC §7. The engine is resolved via betl_get_expr_engine() at first
 * batch, after we know the upstream schema. v0.1 supports int64 and
 * utf8 input columns and produces those plus boolean output (filter
 * predicate) and int64 / utf8 / boolean (map add columns).
 */

#include "runtime/transforms.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"


/* ============================================================== *
 *  Tiny JSON value extractor (same shape as builtins.c)            *
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

/* Decode a JSON string value at *p (pointing at the opening quote)
 * into a freshly malloc'd NUL-terminated buffer. Handles \" \\ \n \t
 * \r \b \f \/ — the same escapes pipeline.c emits. */
static int json_decode_str(const char *p, char **out) {
    *out = NULL;
    if (!p || *p != '"') return -1;
    ++p;
    size_t cap = strlen(p) + 1;
    char *buf = malloc(cap);
    if (!buf) return -1;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p != '\\') { buf[i++] = *p++; continue; }
        ++p;
        if (!*p) { free(buf); return -1; }
        switch (*p) {
            case '"':  buf[i++] = '"';  ++p; break;
            case '\\': buf[i++] = '\\'; ++p; break;
            case '/':  buf[i++] = '/';  ++p; break;
            case 'n':  buf[i++] = '\n'; ++p; break;
            case 't':  buf[i++] = '\t'; ++p; break;
            case 'r':  buf[i++] = '\r'; ++p; break;
            case 'b':  buf[i++] = '\b'; ++p; break;
            case 'f':  buf[i++] = '\f'; ++p; break;
            default: free(buf); return -1;
        }
    }
    if (*p != '"') { free(buf); return -1; }
    buf[i] = '\0';
    *out = buf;
    return 0;
}

static int json_string_at(const char *json, const char *key, char **out) {
    return json_decode_str(json_value_after(json, key), out);
}

/* Coerce the next JSON value (whatever it is) at *p into a string —
 * used when the user wrote `value: 42` (number) or `value: true` (bool)
 * and we want to feed the raw string form to the engine. Reads up to
 * the next `,` `}` `]` or end-of-string. */
static int json_value_to_string(const char *json, const char *key, char **out) {
    const char *p = json_value_after(json, key);
    if (!p) return -1;
    if (*p == '"') return json_decode_str(p, out);
    /* Bare token: number, true, false, null. Consume up to delimiter. */
    const char *end = p;
    while (*end && *end != ',' && *end != '}' && *end != ']'
           && *end != ' ' && *end != '\n' && *end != '\r' && *end != '\t') {
        ++end;
    }
    size_t len = (size_t)(end - p);
    if (len == 0) return -1;
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, p, len);
    s[len] = '\0';
    *out = s;
    return 0;
}


/* ============================================================== *
 *  Arrow leaf release helpers (filter / map output)                *
 * ============================================================== */

static void release_int64_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 2 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_utf8_leaf(struct ArrowArray *arr) {
    if (arr->n_buffers >= 3 && arr->buffers) {
        free((void *)arr->buffers[0]);
        free((void *)arr->buffers[1]);
        free((void *)arr->buffers[2]);
    }
    free(arr->buffers);
    arr->release = NULL;
}

static void release_struct(struct ArrowArray *arr) {
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
 *  Bit helpers                                                     *
 * ============================================================== */

static int bit_at(const uint8_t *bm, size_t i) {
    return (bm[i / 8] >> (i % 8)) & 1u;
}


/* ============================================================== *
 *  filter — TRANSFORM                                              *
 *                                                                  *
 *  Config:                                                         *
 *    where  (string | {lang, expr})  required                      *
 *                                                                  *
 *  Output schema = input schema. Output rows = those for which the *
 *  predicate evaluated to true. Null predicate values drop the row *
 *  (treated as false).                                             *
 * ============================================================== */

typedef struct {
    BetlContext *ctx;
    char        *lang;       /* default "lua" */
    char        *expr_src;

    const BetlExprEngine *engine;
    void                 *engine_handle;
    int                   handle_ready;

    struct ArrowArrayStream input;
    int                     have_input;

    /* Cached schema info. */
    int          schema_cached;
    size_t       n_cols;
    char       **col_names;       /* heap copies */
    char        *col_fmts;        /* one char per col: 'l' or 'u' */

    char         last_err[256];
} FilterState;

static void fset_err(FilterState *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->last_err, sizeof f->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(f->ctx, "%s", f->last_err);
}

static int parse_where(FilterState *f, const char *cfg) {
    /* Try shorthand: where: "string". */
    char *s = NULL;
    if (json_string_at(cfg, "where", &s) == 0 && s) {
        f->expr_src = s;
        f->lang     = strdup("lua");
        return f->lang ? BETL_OK : BETL_ERR_INTERNAL;
    }
    /* Full form: search for "lang" / "expr" / "value" at top level. The
     * strstr-based json_value_after finds them inside the where:{...}
     * object as well as anywhere else; for flat configs that's fine.
     * `value` is accepted in addition to `expr` so the literal engine
     * works (it spells its source field "value"). */
    char *lang = NULL, *src = NULL;
    json_string_at(cfg, "lang", &lang);
    if (json_string_at(cfg, "expr", &src) != 0 || !src) {
        free(src); src = NULL;
        json_value_to_string(cfg, "value", &src);
    }
    if (!lang || !src) {
        free(lang); free(src);
        fset_err(f,
            "filter: 'where' must be a string shorthand or {lang, expr|value} map");
        return BETL_ERR_INVALID;
    }
    f->lang     = lang;
    f->expr_src = src;
    return BETL_OK;
}

static int filter_init(BetlContext *ctx, const char *cfg, void **state) {
    FilterState *f = calloc(1, sizeof *f);
    if (!f) return BETL_ERR_INTERNAL;
    f->ctx = ctx;
    int rc = parse_where(f, cfg ? cfg : "{}");
    if (rc != BETL_OK) { free(f->lang); free(f->expr_src); free(f); return rc; }
    *state = f;
    return BETL_OK;
}

static int filter_attach_input(void *state, int port,
                               struct ArrowArrayStream *in) {
    (void)port;
    FilterState *f = state;
    f->input      = *in;
    f->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void filter_destroy(void *state) {
    if (!state) return;
    FilterState *f = state;
    if (f->have_input && f->input.release) f->input.release(&f->input);
    if (f->handle_ready && f->engine && f->engine_handle) {
        f->engine->release(f->engine_handle);
    }
    free(f->expr_src);
    free(f->lang);
    if (f->col_names) {
        for (size_t i = 0; i < f->n_cols; ++i) free(f->col_names[i]);
        free(f->col_names);
    }
    free(f->col_fmts);
    free(f);
}

/* Fetch upstream schema, validate columns are int64/utf8 only, cache
 * names and types. Idempotent. The caller still owns `*sch_keep` and
 * is responsible for releasing it (we copy what we need). */
static int ensure_schema(FilterState *f, struct ArrowSchema *sch_keep) {
    if (f->schema_cached) return 0;
    if (!f->have_input || !f->input.get_schema) {
        fset_err(f, "filter: input has no get_schema");
        return -1;
    }
    if (f->input.get_schema(&f->input, sch_keep) != 0) {
        fset_err(f, "filter: upstream get_schema failed");
        return -1;
    }
    if (!sch_keep->format || strcmp(sch_keep->format, "+s") != 0
        || sch_keep->n_children <= 0) {
        fset_err(f, "filter: input must be a struct with >=1 child");
        return -1;
    }
    size_t n = (size_t)sch_keep->n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) { free(names); free(fmts);
        fset_err(f, "filter: out of memory"); return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch_keep->children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            fset_err(f, "filter: column '%s' has unsupported format '%s' "
                        "(v0.1 supports int64 and utf8)",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            return -1;
        }
        fmts[i]  = fmt[0];
        names[i] = strdup(c->name ? c->name : "");
        if (!names[i]) {
            for (size_t k = 0; k < i; ++k) free(names[k]);
            free(names); free(fmts);
            fset_err(f, "filter: out of memory"); return -1;
        }
    }
    f->n_cols     = n;
    f->col_names  = names;
    f->col_fmts   = fmts;
    f->schema_cached = 1;
    return 0;
}

static int ensure_compiled(FilterState *f, struct ArrowSchema *schema) {
    if (f->handle_ready) return 0;
    f->engine = betl_get_expr_engine(f->ctx, f->lang);
    if (!f->engine) {
        fset_err(f, "filter: no expression engine for lang '%s'", f->lang);
        return -1;
    }
    int rc = f->engine->compile(f->ctx, f->expr_src, schema, &f->engine_handle);
    if (rc != BETL_OK) return -1;     /* engine has set ctx error */
    f->handle_ready = 1;
    return 0;
}

/* Build a length-N int64 leaf containing only rows from `src` where
 * keep[i] != 0 (n_kept rows total). */
static int build_int64_filtered(struct ArrowArray *out,
                                const struct ArrowArray *src,
                                const uint8_t *keep, size_t n_rows,
                                size_t n_kept) {
    int64_t *vals = malloc((n_kept ? n_kept : 1) * sizeof *vals);
    if (!vals) return -1;
    size_t bytes = (n_kept + 7) / 8;
    uint8_t *vmap = malloc(bytes ? bytes : 1);
    if (!vmap) { free(vals); return -1; }
    memset(vmap, 0xFF, bytes ? bytes : 1);

    int64_t null_count = 0;
    const uint8_t *src_valid = (src->null_count > 0) ? src->buffers[0] : NULL;
    const int64_t *src_vals  = src->buffers[1];
    size_t off = (size_t)src->offset;
    size_t w = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!keep[i]) continue;
        size_t row = i + off;
        int is_null = src_valid && !bit_at(src_valid, row);
        if (is_null) {
            vmap[w / 8] &= (uint8_t)~(1u << (w % 8));
            ++null_count;
        } else {
            vals[w] = src_vals[row];
        }
        ++w;
    }
    if (null_count == 0) { free(vmap); vmap = NULL; }

    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = vals;

    out->length     = (int64_t)n_kept;
    out->null_count = null_count;
    out->offset     = 0;
    out->n_buffers  = 2;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = release_int64_leaf;
    return 0;
}

/* Same shape for utf8: walk kept rows, copy strings into a fresh data
 * buffer, build offsets[n_kept+1]. */
static int build_utf8_filtered(struct ArrowArray *out,
                               const struct ArrowArray *src,
                               const uint8_t *keep, size_t n_rows,
                               size_t n_kept) {
    const uint8_t *src_valid = (src->null_count > 0) ? src->buffers[0] : NULL;
    const int32_t *src_off   = src->buffers[1];
    const char    *src_data  = src->buffers[2];
    size_t off = (size_t)src->offset;

    /* First pass: total bytes for kept non-null strings. */
    size_t total = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!keep[i]) continue;
        size_t row = i + off;
        if (src_valid && !bit_at(src_valid, row)) continue;
        total += (size_t)(src_off[row + 1] - src_off[row]);
    }

    int32_t *offs = malloc((n_kept + 1) * sizeof *offs);
    char    *data = malloc(total ? total : 1);
    size_t bytes  = (n_kept + 7) / 8;
    uint8_t *vmap = malloc(bytes ? bytes : 1);
    if (!offs || !data || !vmap) {
        free(offs); free(data); free(vmap); return -1;
    }
    memset(vmap, 0xFF, bytes ? bytes : 1);
    offs[0] = 0;

    int64_t null_count = 0;
    size_t  pos = 0;
    size_t  w   = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!keep[i]) continue;
        size_t row = i + off;
        if (src_valid && !bit_at(src_valid, row)) {
            vmap[w / 8] &= (uint8_t)~(1u << (w % 8));
            ++null_count;
        } else {
            size_t slen = (size_t)(src_off[row + 1] - src_off[row]);
            if (slen) memcpy(data + pos, src_data + src_off[row], slen);
            pos += slen;
        }
        offs[w + 1] = (int32_t)pos;
        ++w;
    }
    if (null_count == 0) { free(vmap); vmap = NULL; }

    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = offs;
    bufs[2] = data;

    out->length     = (int64_t)n_kept;
    out->null_count = null_count;
    out->offset     = 0;
    out->n_buffers  = 3;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = release_utf8_leaf;
    return 0;
}

static int filt_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    FilterState *f = st->private_data;
    if (!f || !f->have_input || !f->input.get_schema) return EINVAL;
    return f->input.get_schema(&f->input, out);
}

static int filt_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    FilterState *f = st->private_data;
    memset(out, 0, sizeof *out);
    if (!f) return EINVAL;

    /* Cache schema and compile expression on first call. */
    struct ArrowSchema sch = {0};
    if (ensure_schema(f, &sch) != 0) return EIO;
    if (ensure_compiled(f, &sch) != 0) {
        if (sch.release) sch.release(&sch);
        return EIO;
    }
    if (sch.release) sch.release(&sch);

    /* Pull upstream batch. */
    struct ArrowArray in_arr = {0};
    if (f->input.get_next(&f->input, &in_arr) != 0) {
        const char *e = f->input.get_last_error
                            ? f->input.get_last_error(&f->input) : NULL;
        fset_err(f, "filter: upstream get_next failed: %s",
                 e ? e : "(no detail)");
        return EIO;
    }
    if (!in_arr.release) return 0;          /* end-of-stream */

    if (in_arr.n_children != (int64_t)f->n_cols) {
        long long got = (long long)in_arr.n_children;
        size_t expected = f->n_cols;
        in_arr.release(&in_arr);
        fset_err(f, "filter: upstream batch has %lld cols, expected %zu",
                 got, expected);
        return EIO;
    }
    size_t length = (size_t)in_arr.length;

    /* Evaluate predicate -> bool leaf. */
    struct ArrowArray pred = {0};
    int rc = f->engine->evaluate(f->engine_handle, &in_arr, "b", &pred);
    if (rc != BETL_OK) {
        in_arr.release(&in_arr);
        return EIO;
    }
    if (pred.length != (int64_t)length || pred.n_buffers < 2 || !pred.buffers[1]) {
        if (pred.release) pred.release(&pred);
        in_arr.release(&in_arr);
        fset_err(f, "filter: engine returned malformed bool array");
        return EIO;
    }

    /* Build keep[]: 1=row passes, 0=drop. Null predicate -> drop. */
    uint8_t *keep = calloc(length ? length : 1, 1);
    if (!keep) {
        if (pred.release) pred.release(&pred);
        in_arr.release(&in_arr);
        fset_err(f, "filter: out of memory");
        return EIO;
    }
    const uint8_t *pred_valid = (pred.null_count > 0) ? pred.buffers[0] : NULL;
    const uint8_t *pred_vals  = pred.buffers[1];
    size_t n_kept = 0;
    for (size_t i = 0; i < length; ++i) {
        size_t row = i + (size_t)pred.offset;
        if (pred_valid && !bit_at(pred_valid, row)) continue;
        if (bit_at(pred_vals, row)) { keep[i] = 1; ++n_kept; }
    }
    if (pred.release) pred.release(&pred);

    /* Per-column copy with selection. */
    struct ArrowArray **kids = calloc(f->n_cols, sizeof *kids);
    if (!kids) {
        free(keep); in_arr.release(&in_arr);
        fset_err(f, "filter: out of memory");
        return EIO;
    }
    for (size_t c = 0; c < f->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        int crc;
        if (f->col_fmts[c] == 'l') {
            crc = build_int64_filtered(kids[c], in_arr.children[c],
                                       keep, length, n_kept);
        } else {
            crc = build_utf8_filtered (kids[c], in_arr.children[c],
                                       keep, length, n_kept);
        }
        if (crc != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); free(keep); in_arr.release(&in_arr);
            fset_err(f, "filter: build column '%s' failed", f->col_names[c]);
            return EIO;
        }
    }
    free(keep);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < f->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids); in_arr.release(&in_arr);
        fset_err(f, "filter: out of memory");
        return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n_kept;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)f->n_cols;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = release_struct;

    in_arr.release(&in_arr);
    return 0;
}

static const char *filt_get_last_error(struct ArrowArrayStream *st) {
    FilterState *f = st->private_data;
    return (f && f->last_err[0]) ? f->last_err : NULL;
}

static void filt_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int filter_attach_output(void *state, int port,
                                struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = filt_get_schema;
    out->get_next       = filt_get_next;
    out->get_last_error = filt_get_last_error;
    out->release        = filt_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef filter_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "any rows" },
};
static const BetlPortDef filter_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "subset of in" },
};

static const BetlComponentDef filter_components[] = {
    { .name               = "filter",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = filter_inputs,
      .input_count        = 1,
      .outputs            = filter_outputs,
      .output_count       = 1,
      .init               = filter_init,
      .destroy            = filter_destroy,
      .attach_input       = filter_attach_input,
      .attach_output      = filter_attach_output },
};

static const BetlProvider filter_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-filter",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = filter_components,
    .component_count = sizeof filter_components / sizeof filter_components[0],
};


/* ============================================================== *
 *  JSON object walker (for `add:`)                                 *
 *                                                                  *
 *  Walks a flat top-level object, calling cb(key, value_substr,    *
 *  value_len) per pair. Tracks string + brace nesting so we don't  *
 *  cut a value short at a comma inside a nested object.            *
 * ============================================================== */

typedef int (*kv_visit_fn)(const char *key,
                           const char *value, size_t value_len,
                           void *user);

static int json_walk_object_at(const char *p,
                               kv_visit_fn cb, void *user) {
    if (!p || *p != '{') return -1;
    ++p;
    while (1) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == '}' || *p == '\0') return 0;
        if (*p != '"') return -1;
        const char *key_start = p + 1;
        const char *key_end = strchr(key_start, '"');
        if (!key_end) return -1;
        size_t klen = (size_t)(key_end - key_start);
        char key[128];
        if (klen >= sizeof key) return -1;
        memcpy(key, key_start, klen);
        key[klen] = '\0';
        p = key_end + 1;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p != ':') return -1;
        ++p;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        const char *val_start = p;
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
        size_t vlen = (size_t)(p - val_start);
        if (cb(key, val_start, vlen, user) != 0) return -1;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == '}' || *p == '\0') return 0;
        return -1;
    }
}

/* Same shape for arrays. Calls cb(item_substr, item_len) per element. */
typedef int (*item_visit_fn)(const char *value, size_t value_len, void *user);

static int json_walk_array_at(const char *p,
                              item_visit_fn cb, void *user) {
    if (!p || *p != '[') return -1;
    ++p;
    while (1) {
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ']' || *p == '\0') return 0;
        const char *val_start = p;
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
        size_t vlen = (size_t)(p - val_start);
        /* Trim trailing whitespace from the slice. */
        while (vlen > 0 && (val_start[vlen - 1] == ' '
                         || val_start[vlen - 1] == '\n'
                         || val_start[vlen - 1] == '\t'
                         || val_start[vlen - 1] == '\r')) {
            --vlen;
        }
        if (cb(val_start, vlen, user) != 0) return -1;
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (*p == ',') { ++p; continue; }
        if (*p == ']' || *p == '\0') return 0;
        return -1;
    }
}


/* ============================================================== *
 *  map — TRANSFORM                                                 *
 *                                                                  *
 *  Config (v0.1, only `add:` mode):                                *
 *    add: { col_name: {lang, expr|value, type?} , ... }            *
 *                                                                  *
 *  Output schema = input columns followed by added columns. Each   *
 *  added column's Arrow type is the engine `desired_format`,       *
 *  defaulting to "u" (utf8); the user can override with `type:`.   *
 *  v0.1 supports types "l", "u", "b" (matching the lua engine).    *
 *                                                                  *
 *  `select:` mode (replacing the column set) is deferred.          *
 * ============================================================== */

typedef struct {
    char *name;          /* output column name */
    char *lang;          /* engine language ("lua" default) */
    char *source;        /* expr text or literal value as a string */
    char *out_format;    /* "l", "u", "b" — defaults to "u" */

    const BetlExprEngine *engine;
    void                 *handle;
    int                   handle_ready;
} MapAdd;

/* `select:` entries. Three forms map to three kinds. */
typedef enum {
    SEL_PASS   = 1,  /* bare string: pass-through with same name */
    SEL_RENAME = 2,  /* {name, from}: same data, different output name */
    SEL_EXPR   = 3,  /* {name, expr|value, lang?, type?} */
} SelKind;

typedef struct {
    SelKind kind;
    char   *out_name;
    char   *from_name;     /* PASS: same as out_name; RENAME: source col */
    int     from_idx;      /* resolved at first batch */
    /* SEL_EXPR fields */
    char   *lang;
    char   *source;
    char   *out_format;
    const BetlExprEngine *engine;
    void                 *handle;
    int                   handle_ready;
} SelEntry;

typedef enum { MAP_MODE_ADD = 1, MAP_MODE_SELECT = 2 } MapMode;

typedef struct {
    BetlContext *ctx;
    MapMode      mode;

    /* MAP_MODE_ADD */
    MapAdd     *adds;
    size_t      n_adds;

    /* MAP_MODE_SELECT */
    SelEntry   *sels;
    size_t      n_sels;

    struct ArrowArrayStream input;
    int                     have_input;

    int          schema_cached;
    size_t       n_input_cols;
    /* Cached input schema kept around for compile() calls. */
    struct ArrowSchema input_schema;
    int                input_schema_owned;
    /* Convenience: input column names indexed alongside input_schema's
     * children. Used to resolve from_name → from_idx for select. */
    char       **input_col_names;
    char        *input_col_fmts;     /* one char per col: 'l' or 'u' */

    char         last_err[256];
} MapState;

static void mset_err(MapState *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->last_err, sizeof m->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(m->ctx, "%s", m->last_err);
}

/* For json_walk_object_at: parse one entry of the `add:` object. The
 * value substring is a `{lang, expr|value, type?}` JSON object. */
typedef struct {
    MapState *m;
    int       err;
} AddCtx;

static int add_visit(const char *key, const char *value, size_t value_len,
                     void *user) {
    AddCtx *c = user;
    MapState *m = c->m;
    if (value_len == 0 || value[0] != '{') {
        mset_err(m, "map: add column '%s' must be a {lang, expr|value} object", key);
        c->err = 1;
        return -1;
    }
    /* Copy the value substring NUL-terminated so json_string_at works. */
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *lang = NULL, *expr = NULL, *vstr = NULL, *type = NULL;
    json_string_at(vbuf, "lang",  &lang);
    json_string_at(vbuf, "expr",  &expr);
    /* `value:` (literal) — use the value-to-string helper so unquoted
     * scalars (numbers, booleans) round-trip. */
    json_value_to_string(vbuf, "value", &vstr);
    json_string_at(vbuf, "type",  &type);
    free(vbuf);

    /* Pick one as the source. If both are present (mostly nonsense, but
     * possible if a user pastes both fields), prefer `expr` and discard
     * the literal value cleanly. */
    char *src_owned = NULL;
    if (expr) { src_owned = expr; expr = NULL; free(vstr); vstr = NULL; }
    else if (vstr) { src_owned = vstr; vstr = NULL; }
    else {
        free(lang); free(type);
        mset_err(m, "map: add column '%s' must declare 'expr' or 'value'", key);
        c->err = 1;
        return -1;
    }

    MapAdd *grow = realloc(m->adds, (m->n_adds + 1) * sizeof *grow);
    if (!grow) {
        free(src_owned); free(lang); free(type);
        c->err = 1; return -1;
    }
    m->adds = grow;
    MapAdd *a = &m->adds[m->n_adds++];
    memset(a, 0, sizeof *a);
    a->name       = strdup(key);
    a->lang       = lang ? lang : strdup("lua");
    a->source     = src_owned;
    a->out_format = type ? type : strdup("u");
    if (!a->name || !a->lang || !a->out_format) { c->err = 1; return -1; }
    return 0;
}

static int parse_add_block(MapState *m, const char *cfg) {
    const char *add = json_value_after(cfg, "add");
    if (!add || *add != '{') {
        mset_err(m, "map: requires an `add:` map");
        return BETL_ERR_INVALID;
    }
    AddCtx c = { .m = m, .err = 0 };
    if (json_walk_object_at(add, add_visit, &c) != 0 || c.err) {
        return BETL_ERR_INVALID;
    }
    if (m->n_adds == 0) {
        mset_err(m, "map: `add:` is empty");
        return BETL_ERR_INVALID;
    }
    return BETL_OK;
}

/* Per-item visitor for select: list. `value` is the raw JSON substring
 * for one entry — either a quoted string (passthrough) or an object
 * (rename / expr). */
typedef struct { MapState *m; int err; } SelCtx;

static int sel_visit(const char *value, size_t value_len, void *user) {
    SelCtx *c = user;
    MapState *m = c->m;
    if (value_len == 0) { c->err = 1; return -1; }

    SelEntry *grow = realloc(m->sels, (m->n_sels + 1) * sizeof *grow);
    if (!grow) { c->err = 1; return -1; }
    m->sels = grow;
    SelEntry *s = &m->sels[m->n_sels++];
    memset(s, 0, sizeof *s);
    s->from_idx = -1;

    if (value[0] == '"') {
        /* Passthrough: bare string. */
        char *name = NULL;
        if (json_decode_str(value, &name) != 0 || !name) {
            mset_err(m, "map: select: malformed passthrough entry"); c->err = 1; return -1;
        }
        s->kind     = SEL_PASS;
        s->out_name = name;
        s->from_name = strdup(name);
        if (!s->from_name) { c->err = 1; return -1; }
        return 0;
    }
    if (value[0] != '{') {
        mset_err(m, "map: select: entry must be a string or {name, ...} object");
        c->err = 1; return -1;
    }

    /* Object form. Copy the value substring NUL-terminated for
     * json_string_at to work. */
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *name = NULL, *from = NULL, *expr = NULL, *vstr = NULL;
    char *type = NULL, *lang = NULL;
    json_string_at(vbuf, "name", &name);
    json_string_at(vbuf, "from", &from);
    json_string_at(vbuf, "expr", &expr);
    json_value_to_string(vbuf, "value", &vstr);
    json_string_at(vbuf, "type", &type);
    json_string_at(vbuf, "lang", &lang);
    free(vbuf);

    if (!name) {
        free(from); free(expr); free(vstr); free(type); free(lang);
        mset_err(m, "map: select: object entry must have a `name` field");
        c->err = 1; return -1;
    }

    if (expr || vstr) {
        /* Computed entry. `from` (if present) is unused here — drop it
         * before any path that might early-return on OOM. */
        free(from); from = NULL;
        char *src = NULL;
        if (expr) { src = expr; expr = NULL; free(vstr); vstr = NULL; }
        else      { src = vstr; vstr = NULL; }
        s->kind       = SEL_EXPR;
        s->out_name   = name;
        s->lang       = lang ? lang : strdup("lua");
        s->source     = src;
        s->out_format = type ? type : strdup("u");
        if (!s->lang || !s->out_format) { c->err = 1; return -1; }
        return 0;
    }
    if (from) {
        s->kind       = SEL_RENAME;
        s->out_name   = name;
        s->from_name  = from;
        free(lang); free(type);
        return 0;
    }
    free(name); free(lang); free(type);
    mset_err(m, "map: select: entry must declare `from`, `expr`, or `value`");
    c->err = 1;
    return -1;
}

static int parse_select_block(MapState *m, const char *cfg) {
    const char *sel = json_value_after(cfg, "select");
    if (!sel || *sel != '[') {
        mset_err(m, "map: `select:` must be a list");
        return BETL_ERR_INVALID;
    }
    SelCtx c = { .m = m, .err = 0 };
    if (json_walk_array_at(sel, sel_visit, &c) != 0 || c.err) {
        return BETL_ERR_INVALID;
    }
    if (m->n_sels == 0) {
        mset_err(m, "map: `select:` is empty");
        return BETL_ERR_INVALID;
    }
    return BETL_OK;
}

static void map_state_free(MapState *m) {
    if (!m) return;
    for (size_t i = 0; i < m->n_adds; ++i) {
        free(m->adds[i].name); free(m->adds[i].lang);
        free(m->adds[i].source); free(m->adds[i].out_format);
    }
    free(m->adds);
    for (size_t i = 0; i < m->n_sels; ++i) {
        free(m->sels[i].out_name); free(m->sels[i].from_name);
        free(m->sels[i].lang); free(m->sels[i].source);
        free(m->sels[i].out_format);
    }
    free(m->sels);
    free(m);
}

static int map_init(BetlContext *ctx, const char *cfg, void **state) {
    MapState *m = calloc(1, sizeof *m);
    if (!m) return BETL_ERR_INTERNAL;
    m->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    /* `add:` xor `select:`. Detect which (or both, which is an error). */
    const char *add_at = json_value_after(cfg, "add");
    const char *sel_at = json_value_after(cfg, "select");
    if (add_at && sel_at) {
        mset_err(m, "map: only one of `add:` or `select:` may be set");
        free(m);
        return BETL_ERR_INVALID;
    }
    int rc;
    if (sel_at) {
        m->mode = MAP_MODE_SELECT;
        rc = parse_select_block(m, cfg);
    } else if (add_at) {
        m->mode = MAP_MODE_ADD;
        rc = parse_add_block(m, cfg);
    } else {
        mset_err(m, "map: requires `add:` or `select:`");
        rc = BETL_ERR_INVALID;
    }
    if (rc != BETL_OK) {
        map_state_free(m);
        return rc;
    }
    *state = m;
    return BETL_OK;
}

static int map_attach_input(void *state, int port,
                            struct ArrowArrayStream *in) {
    (void)port;
    MapState *m = state;
    m->input      = *in;
    m->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void map_destroy(void *state) {
    if (!state) return;
    MapState *m = state;
    if (m->have_input && m->input.release) m->input.release(&m->input);
    for (size_t i = 0; i < m->n_adds; ++i) {
        MapAdd *a = &m->adds[i];
        if (a->handle_ready && a->engine && a->handle) {
            a->engine->release(a->handle);
        }
    }
    for (size_t i = 0; i < m->n_sels; ++i) {
        SelEntry *s = &m->sels[i];
        if (s->handle_ready && s->engine && s->handle) {
            s->engine->release(s->handle);
        }
    }
    if (m->input_schema_owned && m->input_schema.release) {
        m->input_schema.release(&m->input_schema);
    }
    if (m->input_col_names) {
        for (size_t i = 0; i < m->n_input_cols; ++i) free(m->input_col_names[i]);
        free(m->input_col_names);
    }
    free(m->input_col_fmts);
    /* Free name/lang/source/etc strings via the shared helper. */
    map_state_free(m);
}

/* Cache upstream schema; compile expressions. Resolve from_name → idx
 * for select entries. */
static int map_ensure_ready(MapState *m) {
    if (m->schema_cached) return 0;
    if (!m->have_input || !m->input.get_schema) {
        mset_err(m, "map: input has no get_schema");
        return -1;
    }
    if (m->input.get_schema(&m->input, &m->input_schema) != 0) {
        mset_err(m, "map: upstream get_schema failed");
        return -1;
    }
    m->input_schema_owned = 1;
    if (!m->input_schema.format
        || strcmp(m->input_schema.format, "+s") != 0) {
        mset_err(m, "map: input must be a struct array");
        return -1;
    }
    m->n_input_cols = (size_t)m->input_schema.n_children;

    /* Cache input column names + formats so select can resolve from-refs. */
    m->input_col_names = calloc(m->n_input_cols, sizeof *m->input_col_names);
    m->input_col_fmts  = calloc(m->n_input_cols, 1);
    if (!m->input_col_names || !m->input_col_fmts) {
        mset_err(m, "map: out of memory"); return -1;
    }
    for (size_t i = 0; i < m->n_input_cols; ++i) {
        struct ArrowSchema *c = m->input_schema.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            mset_err(m, "map: input column '%s' has unsupported format '%s' "
                        "(v0.1 supports int64 and utf8)",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            return -1;
        }
        m->input_col_fmts[i] = fmt[0];
        m->input_col_names[i] = strdup((c && c->name) ? c->name : "");
        if (!m->input_col_names[i]) {
            mset_err(m, "map: out of memory"); return -1;
        }
    }

    if (m->mode == MAP_MODE_ADD) {
        for (size_t i = 0; i < m->n_adds; ++i) {
            MapAdd *a = &m->adds[i];
            a->engine = betl_get_expr_engine(m->ctx, a->lang);
            if (!a->engine) {
                mset_err(m, "map: add '%s': no engine for lang '%s'",
                         a->name, a->lang);
                return -1;
            }
            int rc = a->engine->compile(m->ctx, a->source, &m->input_schema,
                                        &a->handle);
            if (rc != BETL_OK) return -1;
            a->handle_ready = 1;
        }
    } else { /* MAP_MODE_SELECT */
        for (size_t i = 0; i < m->n_sels; ++i) {
            SelEntry *s = &m->sels[i];
            if (s->kind == SEL_EXPR) {
                s->engine = betl_get_expr_engine(m->ctx, s->lang);
                if (!s->engine) {
                    mset_err(m, "map: select '%s': no engine for lang '%s'",
                             s->out_name, s->lang);
                    return -1;
                }
                int rc = s->engine->compile(m->ctx, s->source,
                                            &m->input_schema, &s->handle);
                if (rc != BETL_OK) return -1;
                s->handle_ready = 1;
            } else {
                /* PASS / RENAME: resolve from_name to an input column index. */
                int idx = -1;
                for (size_t k = 0; k < m->n_input_cols; ++k) {
                    if (strcmp(m->input_col_names[k], s->from_name) == 0) {
                        idx = (int)k; break;
                    }
                }
                if (idx < 0) {
                    mset_err(m, "map: select '%s': unknown input column '%s'",
                             s->out_name, s->from_name);
                    return -1;
                }
                s->from_idx = idx;
            }
        }
    }
    m->schema_cached = 1;
    return 0;
}

/* Schema leaf release for map's add columns: name is strdup'd, format
 * is a static literal (no free needed). */
static void release_schema_named_leaf(struct ArrowSchema *sch) {
    free((void *)sch->name);
    sch->release = NULL;
}

/* Output schema release: we own the children pointer array but each
 * child may have come from upstream (transferred ownership via release
 * passthrough) or be a heap-allocated leaf for an add column. */
static void release_map_schema(struct ArrowSchema *sch) {
    for (int64_t i = 0; i < sch->n_children; ++i) {
        if (sch->children[i] && sch->children[i]->release) {
            sch->children[i]->release(sch->children[i]);
        }
        free(sch->children[i]);
    }
    free(sch->children);
    sch->release = NULL;
}

/* Map "l" / "u" / "b" desired_format to an Arrow leaf schema's
 * `format` string. */
static const char *desired_to_format(const char *desired) {
    if (strcmp(desired, "l") == 0) return "l";
    if (strcmp(desired, "u") == 0) return "u";
    if (strcmp(desired, "b") == 0) return "b";
    return NULL;
}

/* Deep-copy an int64 leaf, applying src's offset so the destination
 * starts at offset 0. Used by select mode (where the same input column
 * may be referenced more than once and ownership cannot be moved). */
static int deepcopy_int64_leaf(struct ArrowArray *dst,
                               const struct ArrowArray *src) {
    int64_t length = src->length;
    int64_t offset = src->offset;
    int64_t null_count = src->null_count;

    int64_t *vals = malloc((size_t)(length ? length : 1) * sizeof *vals);
    if (!vals) return -1;
    if (length > 0) {
        const int64_t *svals = src->buffers[1];
        memcpy(vals, svals + offset, (size_t)length * sizeof *vals);
    }

    uint8_t *vmap = NULL;
    if (null_count > 0 && src->buffers[0]) {
        size_t bytes = ((size_t)length + 7) / 8;
        vmap = malloc(bytes ? bytes : 1);
        if (!vmap) { free(vals); return -1; }
        memset(vmap, 0xFF, bytes ? bytes : 1);
        const uint8_t *sv = src->buffers[0];
        for (size_t i = 0; i < (size_t)length; ++i) {
            size_t srow = (size_t)offset + i;
            if (!((sv[srow / 8] >> (srow % 8)) & 1u)) {
                vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
            }
        }
    }

    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = vals;

    dst->length     = length;
    dst->null_count = null_count;
    dst->offset     = 0;
    dst->n_buffers  = 2;
    dst->n_children = 0;
    dst->buffers    = bufs;
    dst->release    = release_int64_leaf;
    return 0;
}

static int deepcopy_utf8_leaf(struct ArrowArray *dst,
                              const struct ArrowArray *src) {
    int64_t length = src->length;
    int64_t offset = src->offset;
    int64_t null_count = src->null_count;

    const int32_t *soff  = src->buffers[1];
    const char    *sdata = src->buffers[2];
    int32_t base  = soff[offset];
    int32_t total = soff[offset + length] - base;
    if (total < 0) return -1;

    int32_t *offs = malloc(((size_t)length + 1) * sizeof *offs);
    char    *data = malloc((size_t)(total ? total : 1));
    if (!offs || !data) { free(offs); free(data); return -1; }
    for (int64_t i = 0; i <= length; ++i) offs[i] = soff[offset + i] - base;
    if (total > 0) memcpy(data, sdata + base, (size_t)total);

    uint8_t *vmap = NULL;
    if (null_count > 0 && src->buffers[0]) {
        size_t bytes = ((size_t)length + 7) / 8;
        vmap = malloc(bytes ? bytes : 1);
        if (!vmap) { free(offs); free(data); return -1; }
        memset(vmap, 0xFF, bytes ? bytes : 1);
        const uint8_t *sv = src->buffers[0];
        for (size_t i = 0; i < (size_t)length; ++i) {
            size_t srow = (size_t)offset + i;
            if (!((sv[srow / 8] >> (srow % 8)) & 1u)) {
                vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
            }
        }
    }

    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = offs;
    bufs[2] = data;

    dst->length     = length;
    dst->null_count = null_count;
    dst->offset     = 0;
    dst->n_buffers  = 3;
    dst->n_children = 0;
    dst->buffers    = bufs;
    dst->release    = release_utf8_leaf;
    return 0;
}

/* Construct a leaf schema with strdup'd name, static format, nullable.
 * Used for both add columns (output name = add->name) and select-mode
 * computed/renamed columns. */
static struct ArrowSchema *new_leaf_schema(const char *name, const char *format) {
    struct ArrowSchema *c = calloc(1, sizeof *c);
    char *nm = strdup(name);
    if (!c || !nm) { free(c); free(nm); return NULL; }
    c->format  = format;
    c->name    = nm;
    c->flags   = ARROW_FLAG_NULLABLE;
    c->release = release_schema_named_leaf;
    return c;
}

static int map_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    MapState *m = st->private_data;
    if (!m) return EINVAL;
    if (map_ensure_ready(m) != 0) return EIO;
    memset(out, 0, sizeof *out);

    if (m->mode == MAP_MODE_ADD) {
        /* Pull a fresh schema from upstream (we hand out a struct the
         * consumer will release). */
        struct ArrowSchema up = {0};
        if (m->input.get_schema(&m->input, &up) != 0) return EIO;

        size_t n_total = m->n_input_cols + m->n_adds;
        struct ArrowSchema **kids = calloc(n_total, sizeof *kids);
        if (!kids) { if (up.release) up.release(&up); return ENOMEM; }

        for (size_t i = 0; i < m->n_input_cols; ++i) {
            kids[i] = up.children[i];
            up.children[i] = NULL;
        }
        if (up.release) up.release(&up);

        for (size_t j = 0; j < m->n_adds; ++j) {
            MapAdd *a = &m->adds[j];
            const char *fmt = desired_to_format(a->out_format);
            if (!fmt) {
                for (size_t k = 0; k < m->n_input_cols + j; ++k) {
                    if (kids[k] && kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids);
                return EINVAL;
            }
            struct ArrowSchema *c = new_leaf_schema(a->name, fmt);
            if (!c) {
                for (size_t k = 0; k < m->n_input_cols + j; ++k) {
                    if (kids[k] && kids[k]->release) kids[k]->release(kids[k]);
                    free(kids[k]);
                }
                free(kids); return ENOMEM;
            }
            kids[m->n_input_cols + j] = c;
        }

        out->format     = "+s";
        out->n_children = (int64_t)n_total;
        out->children   = kids;
        out->release    = release_map_schema;
        return 0;
    }

    /* MAP_MODE_SELECT: build N fresh leaf schemas, one per entry. */
    size_t n_total = m->n_sels;
    struct ArrowSchema **kids = calloc(n_total, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < n_total; ++i) {
        SelEntry *s = &m->sels[i];
        const char *fmt = NULL;
        if (s->kind == SEL_EXPR) {
            fmt = desired_to_format(s->out_format);
        } else { /* PASS or RENAME */
            char ch = m->input_col_fmts[s->from_idx];
            fmt = (ch == 'l') ? "l" : "u";
        }
        if (!fmt) {
            for (size_t k = 0; k < i; ++k) {
                if (kids[k] && kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return EINVAL;
        }
        kids[i] = new_leaf_schema(s->out_name, fmt);
        if (!kids[i]) {
            for (size_t k = 0; k < i; ++k) {
                if (kids[k] && kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
    }
    out->format     = "+s";
    out->n_children = (int64_t)n_total;
    out->children   = kids;
    out->release    = release_map_schema;
    return 0;
}

/* Free kids[k..n) plus the array itself; tolerates NULL slots. */
static void free_kids_partial(struct ArrowArray **kids, size_t k, size_t n) {
    for (size_t i = k; i < n; ++i) {
        if (kids[i] && kids[i]->release) kids[i]->release(kids[i]);
        free(kids[i]);
    }
    free(kids);
}

static int map_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    MapState *m = st->private_data;
    memset(out, 0, sizeof *out);
    if (!m) return EINVAL;
    if (map_ensure_ready(m) != 0) return EIO;

    struct ArrowArray in_arr = {0};
    if (m->input.get_next(&m->input, &in_arr) != 0) {
        const char *e = m->input.get_last_error
                            ? m->input.get_last_error(&m->input) : NULL;
        mset_err(m, "map: upstream get_next failed: %s", e ? e : "(no detail)");
        return EIO;
    }
    if (!in_arr.release) return 0;

    if (in_arr.n_children != (int64_t)m->n_input_cols) {
        long long got = (long long)in_arr.n_children;
        size_t expected = m->n_input_cols;
        in_arr.release(&in_arr);
        mset_err(m, "map: upstream batch has %lld cols, expected %zu",
                 got, expected);
        return EIO;
    }

    size_t n_total = (m->mode == MAP_MODE_ADD)
                        ? (m->n_input_cols + m->n_adds)
                        : m->n_sels;
    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        in_arr.release(&in_arr);
        mset_err(m, "map: out of memory"); return EIO;
    }

    if (m->mode == MAP_MODE_ADD) {
        /* Evaluate add columns FIRST (engines read in_arr->children). */
        for (size_t j = 0; j < m->n_adds; ++j) {
            MapAdd *a = &m->adds[j];
            struct ArrowArray *leaf = calloc(1, sizeof *leaf);
            if (!leaf) {
                free_kids_partial(kids, m->n_input_cols, m->n_input_cols + j);
                in_arr.release(&in_arr);
                mset_err(m, "map: out of memory"); return EIO;
            }
            int rc = a->engine->evaluate(a->handle, &in_arr, a->out_format, leaf);
            if (rc != BETL_OK) {
                free(leaf);
                free_kids_partial(kids, m->n_input_cols, m->n_input_cols + j);
                in_arr.release(&in_arr);
                return EIO;
            }
            kids[m->n_input_cols + j] = leaf;
        }
        /* Now safe to take ownership of input children. */
        for (size_t i = 0; i < m->n_input_cols; ++i) {
            kids[i] = in_arr.children[i];
            in_arr.children[i] = NULL;
        }
    } else {
        /* MAP_MODE_SELECT: per entry, deep-copy or evaluate. */
        for (size_t i = 0; i < m->n_sels; ++i) {
            SelEntry *s = &m->sels[i];
            struct ArrowArray *leaf = calloc(1, sizeof *leaf);
            if (!leaf) {
                free_kids_partial(kids, 0, i);
                in_arr.release(&in_arr);
                mset_err(m, "map: out of memory"); return EIO;
            }
            int rc;
            if (s->kind == SEL_EXPR) {
                rc = s->engine->evaluate(s->handle, &in_arr,
                                         s->out_format, leaf);
                if (rc != BETL_OK) {
                    free(leaf);
                    free_kids_partial(kids, 0, i);
                    in_arr.release(&in_arr);
                    return EIO;
                }
            } else {
                const struct ArrowArray *src = in_arr.children[s->from_idx];
                if (m->input_col_fmts[s->from_idx] == 'l') {
                    rc = deepcopy_int64_leaf(leaf, src);
                } else {
                    rc = deepcopy_utf8_leaf(leaf, src);
                }
                if (rc != 0) {
                    free(leaf);
                    free_kids_partial(kids, 0, i);
                    in_arr.release(&in_arr);
                    mset_err(m, "map: select '%s': deep-copy failed",
                             s->out_name);
                    return EIO;
                }
            }
            kids[i] = leaf;
        }
    }

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        free_kids_partial(kids, 0, n_total);
        in_arr.release(&in_arr);
        mset_err(m, "map: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = in_arr.length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = release_struct;

    in_arr.release(&in_arr);
    return 0;
}

static const char *map_get_last_error(struct ArrowArrayStream *st) {
    MapState *m = st->private_data;
    return (m && m->last_err[0]) ? m->last_err : NULL;
}

static void map_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int map_attach_output(void *state, int port,
                             struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = map_get_schema;
    out->get_next       = map_get_next;
    out->get_last_error = map_get_last_error;
    out->release        = map_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef map_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "any rows" },
};
static const BetlPortDef map_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "in + add cols" },
};

static const BetlComponentDef map_components[] = {
    { .name               = "map",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = map_inputs,
      .input_count        = 1,
      .outputs            = map_outputs,
      .output_count       = 1,
      .init               = map_init,
      .destroy            = map_destroy,
      .attach_input       = map_attach_input,
      .attach_output      = map_attach_output },
};

static const BetlProvider map_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-map",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = map_components,
    .component_count = sizeof map_components / sizeof map_components[0],
};


/* ============================================================== *
 *  aggregate — TRANSFORM                                           *
 *                                                                  *
 *  Config (v0.1):                                                  *
 *    group_by:  list of input column names (int64, ≥1)             *
 *    compute:   map of output_name → {agg, over?}                  *
 *               agg ∈ {count, sum, min, max}                       *
 *               over: required for sum/min/max (int64 input col)   *
 *                                                                  *
 *  Output schema = group_by columns followed by compute columns,   *
 *  all int64. One row per distinct group.                          *
 *                                                                  *
 *  Implementation: pull every upstream batch, group rows linearly  *
 *  (small-n; no hashmap yet), emit a single output batch on the    *
 *  first downstream get_next.                                      *
 *                                                                  *
 *  Nulls: a null in any group_by column is currently a hard error  *
 *  (SQL would treat as a distinct group; we'll add that later).    *
 *  A null in an `over:` column is silently skipped.                *
 * ============================================================== */

typedef enum {
    AGG_COUNT = 1,
    AGG_SUM   = 2,
    AGG_MIN   = 3,
    AGG_MAX   = 4,
} AggKind;

typedef struct {
    char  *out_name;
    AggKind kind;
    char  *over_name;     /* NULL for count */
    int    over_idx;      /* resolved at first batch */
} ComputeSpec;

/* Per-group accumulator state. One slot per ComputeSpec. */
typedef struct {
    int64_t count;        /* row count contributing (all rows for COUNT;
                             non-null over: rows for SUM/MIN/MAX) */
    int64_t sum;
    int64_t mn;
    int64_t mx;
    int     seen;         /* at least one non-null observation */
} Accum;

/* One group: tuple of group_by key values + accumulators per compute. */
typedef struct {
    int64_t *key_vals;    /* n_group_by entries */
    Accum   *accs;        /* n_computes entries */
} Group;

typedef struct {
    BetlContext *ctx;

    char **group_by_names;
    size_t n_group_by;

    ComputeSpec *computes;
    size_t n_computes;

    struct ArrowArrayStream input;
    int                     have_input;

    int          schema_cached;
    size_t       n_input_cols;
    char       **input_col_names;
    char        *input_col_fmts;
    int         *group_by_idx;       /* resolved column index per group_by */

    Group  *groups;
    size_t  n_groups;
    size_t  groups_cap;
    int     materialized;
    int     emitted;

    char    last_err[256];
} AggState;

static void aset_err(AggState *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->last_err, sizeof a->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(a->ctx, "%s", a->last_err);
}

/* JSON visitor for compute: each value is `{agg: "...", over?: "..."}`. */
typedef struct { AggState *a; int err; } AggCtx;

static int compute_visit(const char *key, const char *value, size_t value_len,
                         void *user) {
    AggCtx *c = user;
    AggState *a = c->a;
    if (value_len == 0 || value[0] != '{') {
        aset_err(a, "aggregate: compute['%s'] must be an object", key);
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *agg = NULL, *over = NULL;
    json_string_at(vbuf, "agg",  &agg);
    json_string_at(vbuf, "over", &over);
    free(vbuf);

    if (!agg) {
        free(over);
        aset_err(a, "aggregate: compute['%s'] missing 'agg'", key);
        c->err = 1; return -1;
    }
    AggKind kind;
    if      (strcmp(agg, "count") == 0) kind = AGG_COUNT;
    else if (strcmp(agg, "sum")   == 0) kind = AGG_SUM;
    else if (strcmp(agg, "min")   == 0) kind = AGG_MIN;
    else if (strcmp(agg, "max")   == 0) kind = AGG_MAX;
    else {
        aset_err(a, "aggregate: compute['%s'] unsupported agg '%s' "
                    "(v0.1: count, sum, min, max)", key, agg);
        free(agg); free(over); c->err = 1; return -1;
    }
    free(agg);
    if (kind != AGG_COUNT && !over) {
        aset_err(a, "aggregate: compute['%s'] requires 'over:' for non-count agg", key);
        c->err = 1; return -1;
    }
    if (kind == AGG_COUNT) { free(over); over = NULL; }

    ComputeSpec *grow = realloc(a->computes,
                                (a->n_computes + 1) * sizeof *grow);
    if (!grow) { free(over); c->err = 1; return -1; }
    a->computes = grow;
    ComputeSpec *cs = &a->computes[a->n_computes++];
    cs->out_name  = strdup(key);
    cs->kind      = kind;
    cs->over_name = over;
    cs->over_idx  = -1;
    if (!cs->out_name) { c->err = 1; return -1; }
    return 0;
}

/* JSON visitor for group_by: items must be quoted strings. */
typedef struct { AggState *a; int err; } GbCtx;

static int group_by_visit(const char *value, size_t value_len, void *user) {
    GbCtx *c = user;
    AggState *a = c->a;
    if (value_len == 0 || value[0] != '"') {
        aset_err(a, "aggregate: group_by entries must be column-name strings");
        c->err = 1; return -1;
    }
    char *s = NULL;
    if (json_decode_str(value, &s) != 0 || !s) { c->err = 1; return -1; }
    char **grow = realloc(a->group_by_names,
                          (a->n_group_by + 1) * sizeof *grow);
    if (!grow) { free(s); c->err = 1; return -1; }
    a->group_by_names = grow;
    a->group_by_names[a->n_group_by++] = s;
    return 0;
}

static int agg_init(BetlContext *ctx, const char *cfg, void **state) {
    AggState *a = calloc(1, sizeof *a);
    if (!a) return BETL_ERR_INTERNAL;
    a->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    /* group_by: must be a non-empty list of strings. */
    const char *gb = json_value_after(cfg, "group_by");
    if (!gb || *gb != '[') {
        aset_err(a, "aggregate: `group_by` must be a list of column names");
        free(a); return BETL_ERR_INVALID;
    }
    GbCtx gbc = { .a = a, .err = 0 };
    if (json_walk_array_at(gb, group_by_visit, &gbc) != 0 || gbc.err
        || a->n_group_by == 0) {
        if (a->last_err[0] == '\0')
            aset_err(a, "aggregate: `group_by` is empty");
        for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
        free(a->group_by_names);
        free(a);
        return BETL_ERR_INVALID;
    }

    /* compute: must be an object. */
    const char *cmp = json_value_after(cfg, "compute");
    if (!cmp || *cmp != '{') {
        aset_err(a, "aggregate: `compute` must be a map");
        for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
        free(a->group_by_names);
        free(a);
        return BETL_ERR_INVALID;
    }
    AggCtx cc = { .a = a, .err = 0 };
    if (json_walk_object_at(cmp, compute_visit, &cc) != 0 || cc.err
        || a->n_computes == 0) {
        if (a->last_err[0] == '\0')
            aset_err(a, "aggregate: `compute` is empty");
        for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
        free(a->group_by_names);
        for (size_t i = 0; i < a->n_computes; ++i) {
            free(a->computes[i].out_name);
            free(a->computes[i].over_name);
        }
        free(a->computes);
        free(a);
        return BETL_ERR_INVALID;
    }
    *state = a;
    return BETL_OK;
}

static int agg_attach_input(void *state, int port,
                            struct ArrowArrayStream *in) {
    (void)port;
    AggState *a = state;
    a->input      = *in;
    a->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void agg_destroy(void *state) {
    if (!state) return;
    AggState *a = state;
    if (a->have_input && a->input.release) a->input.release(&a->input);
    for (size_t i = 0; i < a->n_group_by; ++i) free(a->group_by_names[i]);
    free(a->group_by_names);
    for (size_t i = 0; i < a->n_computes; ++i) {
        free(a->computes[i].out_name);
        free(a->computes[i].over_name);
    }
    free(a->computes);
    if (a->input_col_names) {
        for (size_t i = 0; i < a->n_input_cols; ++i) free(a->input_col_names[i]);
        free(a->input_col_names);
    }
    free(a->input_col_fmts);
    free(a->group_by_idx);
    for (size_t i = 0; i < a->n_groups; ++i) {
        free(a->groups[i].key_vals);
        free(a->groups[i].accs);
    }
    free(a->groups);
    free(a);
}

/* Resolve group_by + over: column references against the upstream
 * schema. v0.1 requires every referenced column to be int64. */
static int agg_resolve_schema(AggState *a) {
    if (a->schema_cached) return 0;
    if (!a->have_input || !a->input.get_schema) {
        aset_err(a, "aggregate: input has no get_schema");
        return -1;
    }
    struct ArrowSchema sch = {0};
    if (a->input.get_schema(&a->input, &sch) != 0) {
        aset_err(a, "aggregate: upstream get_schema failed");
        return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        aset_err(a, "aggregate: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) {
        free(names); free(fmts);
        aset_err(a, "aggregate: out of memory");
        goto done;
    }
    int ok = 1;
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        names[i] = strdup((c && c->name) ? c->name : "");
        if (!names[i] || !fmt) {
            aset_err(a, "aggregate: input column %zu malformed", i);
            ok = 0; break;
        }
        fmts[i] = fmt[0];
    }
    if (!ok) {
        for (size_t i = 0; i < n; ++i) free(names[i]);
        free(names); free(fmts);
        goto done;
    }
    a->n_input_cols    = n;
    a->input_col_names = names;
    a->input_col_fmts  = fmts;

    a->group_by_idx = calloc(a->n_group_by, sizeof *a->group_by_idx);
    if (!a->group_by_idx) { aset_err(a, "aggregate: out of memory"); goto done; }
    for (size_t i = 0; i < a->n_group_by; ++i) {
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], a->group_by_names[i]) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            aset_err(a, "aggregate: group_by column '%s' not found",
                     a->group_by_names[i]);
            goto done;
        }
        if (fmts[idx] != 'l') {
            aset_err(a, "aggregate: group_by column '%s' must be int64 (got '%c')",
                     a->group_by_names[i], fmts[idx]);
            goto done;
        }
        a->group_by_idx[i] = idx;
    }
    for (size_t i = 0; i < a->n_computes; ++i) {
        ComputeSpec *cs = &a->computes[i];
        if (cs->kind == AGG_COUNT) { cs->over_idx = -1; continue; }
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], cs->over_name) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            aset_err(a, "aggregate: compute['%s'] over: '%s' not found",
                     cs->out_name, cs->over_name);
            goto done;
        }
        if (fmts[idx] != 'l') {
            aset_err(a, "aggregate: compute['%s'] over: '%s' must be int64",
                     cs->out_name, cs->over_name);
            goto done;
        }
        cs->over_idx = idx;
    }
    a->schema_cached = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

/* Find a group whose key matches `key`, or create a new one. */
static int agg_find_or_create(AggState *a, const int64_t *key, size_t *out_idx) {
    for (size_t i = 0; i < a->n_groups; ++i) {
        int eq = 1;
        for (size_t k = 0; k < a->n_group_by; ++k) {
            if (a->groups[i].key_vals[k] != key[k]) { eq = 0; break; }
        }
        if (eq) { *out_idx = i; return 0; }
    }
    /* New group. */
    if (a->n_groups == a->groups_cap) {
        size_t nc = a->groups_cap ? a->groups_cap * 2 : 8;
        Group *g = realloc(a->groups, nc * sizeof *g);
        if (!g) return -1;
        a->groups = g;
        a->groups_cap = nc;
    }
    Group *g = &a->groups[a->n_groups];
    /* n_group_by is validated > 0 in init, but the analyzer can't see
     * that — guard the malloc size to keep it quiet. */
    g->key_vals = malloc((a->n_group_by ? a->n_group_by : 1) * sizeof *g->key_vals);
    g->accs     = calloc(a->n_computes, sizeof *g->accs);
    if (!g->key_vals || !g->accs) {
        free(g->key_vals); free(g->accs);
        return -1;
    }
    memcpy(g->key_vals, key, a->n_group_by * sizeof *key);
    /* Initialize MIN/MAX accumulators with sentinels not yet meaningful;
     * accs[].seen guards. */
    for (size_t c = 0; c < a->n_computes; ++c) {
        g->accs[c].mn = INT64_MAX;
        g->accs[c].mx = INT64_MIN;
    }
    *out_idx = a->n_groups++;
    return 0;
}

/* Read a single int64 cell with offset+null awareness. Returns 0 if
 * the cell is non-null and *out is set; returns 1 if the cell is null
 * (out untouched). */
static int read_int64_cell(const struct ArrowArray *col, size_t row_idx,
                           int64_t *out) {
    size_t row = row_idx + (size_t)col->offset;
    if (col->null_count > 0 && col->buffers[0]) {
        const uint8_t *valid = col->buffers[0];
        if (!(valid[row / 8] & (1u << (row % 8)))) return 1;
    }
    *out = ((const int64_t *)col->buffers[1])[row];
    return 0;
}

/* Pull every upstream batch, fold rows into groups. */
static int agg_materialize(AggState *a) {
    if (a->materialized) return 0;
    if (agg_resolve_schema(a) != 0) return -1;

    int64_t *key = malloc(a->n_group_by * sizeof *key);
    if (!key) { aset_err(a, "aggregate: out of memory"); return -1; }

    for (;;) {
        if (betl_should_cancel(a->ctx)) {
            free(key);
            aset_err(a, "aggregate: cancelled"); return -1;
        }
        struct ArrowArray batch = {0};
        if (a->input.get_next(&a->input, &batch) != 0) {
            const char *e = a->input.get_last_error
                                ? a->input.get_last_error(&a->input) : NULL;
            free(key);
            aset_err(a, "aggregate: upstream get_next failed: %s",
                     e ? e : "(no detail)");
            return -1;
        }
        if (!batch.release) break;       /* end-of-stream */
        if (batch.n_children != (int64_t)a->n_input_cols) {
            int64_t got = batch.n_children;
            size_t expected = a->n_input_cols;
            batch.release(&batch); free(key);
            aset_err(a, "aggregate: batch has %lld cols, expected %zu",
                     (long long)got, expected);
            return -1;
        }
        size_t length = (size_t)batch.length;
        for (size_t r = 0; r < length; ++r) {
            int row_failed = 0;
            for (size_t k = 0; k < a->n_group_by; ++k) {
                int64_t v;
                if (read_int64_cell(batch.children[a->group_by_idx[k]], r, &v) != 0) {
                    aset_err(a,
                        "aggregate: null in group_by column '%s' (row %zu) "
                        "is not supported in v0.1",
                        a->group_by_names[k], r);
                    row_failed = 1;
                    break;
                }
                key[k] = v;
            }
            if (row_failed) {
                batch.release(&batch); free(key); return -1;
            }
            size_t gi;
            if (agg_find_or_create(a, key, &gi) != 0) {
                batch.release(&batch); free(key);
                aset_err(a, "aggregate: out of memory");
                return -1;
            }
            Group *g = &a->groups[gi];
            for (size_t c = 0; c < a->n_computes; ++c) {
                ComputeSpec *cs = &a->computes[c];
                Accum *acc = &g->accs[c];
                if (cs->kind == AGG_COUNT) { ++acc->count; acc->seen = 1; continue; }
                int64_t v;
                if (read_int64_cell(batch.children[cs->over_idx], r, &v) != 0) {
                    /* null over: skip this contribution. */
                    continue;
                }
                ++acc->count;
                acc->seen = 1;
                switch (cs->kind) {
                    case AGG_SUM: acc->sum += v; break;
                    case AGG_MIN: if (v < acc->mn) acc->mn = v; break;
                    case AGG_MAX: if (v > acc->mx) acc->mx = v; break;
                    case AGG_COUNT: break;     /* unreachable */
                }
            }
        }
        batch.release(&batch);
    }
    free(key);
    a->materialized = 1;
    return 0;
}

static int agg_get_schema(struct ArrowArrayStream *st,
                          struct ArrowSchema *out) {
    AggState *a = st->private_data;
    memset(out, 0, sizeof *out);
    if (!a) return EINVAL;
    if (agg_resolve_schema(a) != 0) return EIO;

    size_t n_total = a->n_group_by + a->n_computes;
    struct ArrowSchema **kids = calloc(n_total, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < a->n_group_by; ++i) {
        kids[i] = new_leaf_schema(a->group_by_names[i], "l");
        if (!kids[i]) {
            for (size_t k = 0; k < i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
    }
    for (size_t i = 0; i < a->n_computes; ++i) {
        kids[a->n_group_by + i] = new_leaf_schema(a->computes[i].out_name, "l");
        if (!kids[a->n_group_by + i]) {
            for (size_t k = 0; k < a->n_group_by + i; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return ENOMEM;
        }
    }
    out->format     = "+s";
    out->n_children = (int64_t)n_total;
    out->children   = kids;
    out->release    = release_map_schema;
    return 0;
}

static int build_int64_const_leaf(struct ArrowArray *out,
                                  const int64_t *vals, size_t length) {
    int64_t *v = malloc((length ? length : 1) * sizeof *v);
    if (!v) return -1;
    if (length) memcpy(v, vals, length * sizeof *v);
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(v); return -1; }
    bufs[0] = NULL;     /* aggregates never produce nulls in v0.1 */
    bufs[1] = v;
    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 2;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = release_int64_leaf;
    return 0;
}

static int agg_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    AggState *a = st->private_data;
    memset(out, 0, sizeof *out);
    if (!a) return EINVAL;
    if (agg_materialize(a) != 0) return EIO;
    if (a->emitted) return 0;       /* end-of-stream after the one batch */

    size_t length  = a->n_groups;
    size_t n_total = a->n_group_by + a->n_computes;
    /* Single flat buffer indexed [col * length + row]. One allocation
     * means the analyzer can prove no NULL deref under indexing. */
    size_t cells = n_total * (length ? length : 1);
    int64_t *flat = malloc(cells * sizeof *flat);
    if (!flat) { aset_err(a, "aggregate: out of memory"); return EIO; }

    for (size_t r = 0; r < length; ++r) {
        for (size_t k = 0; k < a->n_group_by; ++k) {
            flat[k * length + r] = a->groups[r].key_vals[k];
        }
        for (size_t c = 0; c < a->n_computes; ++c) {
            const ComputeSpec *cs = &a->computes[c];
            const Accum *acc = &a->groups[r].accs[c];
            int64_t v = 0;
            switch (cs->kind) {
                case AGG_COUNT: v = acc->count; break;
                case AGG_SUM:   v = acc->sum;   break;
                case AGG_MIN:   v = acc->seen ? acc->mn : 0; break;
                case AGG_MAX:   v = acc->seen ? acc->mx : 0; break;
            }
            flat[(a->n_group_by + c) * length + r] = v;
        }
    }

    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        free(flat);
        aset_err(a, "aggregate: out of memory"); return EIO;
    }
    for (size_t c = 0; c < n_total; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c] || build_int64_const_leaf(kids[c],
                                               flat + c * length,
                                               length) != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            free(flat);
            aset_err(a, "aggregate: out of memory"); return EIO;
        }
    }
    free(flat);     /* per-column leaves keep their own copies */

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < n_total; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        aset_err(a, "aggregate: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)length;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = release_struct;

    a->emitted = 1;
    return 0;
}

static const char *agg_get_last_error(struct ArrowArrayStream *st) {
    AggState *a = st->private_data;
    return (a && a->last_err[0]) ? a->last_err : NULL;
}

static void agg_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int agg_attach_output(void *state, int port,
                             struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = agg_get_schema;
    out->get_next       = agg_get_next;
    out->get_last_error = agg_get_last_error;
    out->release        = agg_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef agg_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to group" },
};
static const BetlPortDef agg_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "one row per group" },
};

static const BetlComponentDef agg_components[] = {
    { .name               = "aggregate",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = agg_inputs,
      .input_count        = 1,
      .outputs            = agg_outputs,
      .output_count       = 1,
      .init               = agg_init,
      .destroy            = agg_destroy,
      .attach_input       = agg_attach_input,
      .attach_output      = agg_attach_output },
};

static const BetlProvider agg_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-aggregate",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = agg_components,
    .component_count = sizeof agg_components / sizeof agg_components[0],
};


/* ============================================================== *
 *  sort — TRANSFORM                                                *
 *                                                                  *
 *  Config (v0.1):                                                  *
 *    by:  list of {col, dir?}; dir defaults to "asc".              *
 *                                                                  *
 *  Materializes the entire input then emits it in sorted order. v0.1*
 *  supports int64 and utf8 columns on both the keys and the rest of *
 *  the schema. Stable: equal keys preserve original order via the   *
 *  index-pair compare.                                              *
 * ============================================================== */

typedef struct {
    char *col_name;
    int   asc;          /* 1 = ascending, 0 = descending */
    int   col_idx;      /* resolved at first batch */
    char  fmt;          /* 'l' or 'u' — same as input col */
} SortKey;

typedef struct {
    BetlContext *ctx;

    SortKey *keys;
    size_t   n_keys;

    struct ArrowArrayStream input;
    int                     have_input;

    int    schema_cached;
    size_t n_input_cols;
    char **col_names;
    char  *col_fmts;

    /* Per-column staged storage. For each input column, exactly one of
     * i64_vals or (u8_strs + u8_lens) is populated based on col_fmts. */
    int64_t **i64_vals;       /* [n_input_cols][row_cap] */
    char  ***u8_strs;         /* [n_input_cols][row_cap] */
    size_t **u8_lens;         /* [n_input_cols][row_cap] */
    size_t   n_rows;
    size_t   row_cap;

    int materialized;
    int emitted;

    char last_err[256];
} SortState;

static void sset_err(SortState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* Visitor for sort `by:` list. Each entry is either a quoted string
 * (column name, asc default) or {col, dir}. */
typedef struct { SortState *s; int err; } SortCtx;

static int sort_by_visit(const char *value, size_t value_len, void *user) {
    SortCtx *c = user;
    SortState *s = c->s;
    SortKey *grow = realloc(s->keys, (s->n_keys + 1) * sizeof *grow);
    if (!grow) { c->err = 1; return -1; }
    s->keys = grow;
    SortKey *k = &s->keys[s->n_keys++];
    memset(k, 0, sizeof *k);
    k->asc = 1;
    k->col_idx = -1;

    if (value_len == 0) { c->err = 1; return -1; }
    if (value[0] == '"') {
        if (json_decode_str(value, &k->col_name) != 0 || !k->col_name) {
            c->err = 1; return -1;
        }
        return 0;
    }
    if (value[0] != '{') {
        sset_err(s, "sort: by entry must be a string or {col, dir} object");
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';
    char *col = NULL, *dir = NULL;
    json_string_at(vbuf, "col", &col);
    json_string_at(vbuf, "dir", &dir);
    free(vbuf);
    if (!col) {
        free(dir);
        sset_err(s, "sort: by entry needs a `col` field");
        c->err = 1; return -1;
    }
    k->col_name = col;
    if (dir) {
        if (strcmp(dir, "asc") == 0)        k->asc = 1;
        else if (strcmp(dir, "desc") == 0)  k->asc = 0;
        else {
            sset_err(s, "sort: dir must be 'asc' or 'desc' (got '%s')", dir);
            free(dir); c->err = 1; return -1;
        }
        free(dir);
    }
    return 0;
}

static int sort_init(BetlContext *ctx, const char *cfg, void **state) {
    SortState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";
    const char *by = json_value_after(cfg, "by");
    if (!by || *by != '[') {
        sset_err(s, "sort: requires a `by:` list");
        free(s); return BETL_ERR_INVALID;
    }
    SortCtx c = { .s = s, .err = 0 };
    if (json_walk_array_at(by, sort_by_visit, &c) != 0 || c.err
        || s->n_keys == 0) {
        if (s->last_err[0] == '\0') sset_err(s, "sort: `by:` is empty");
        for (size_t i = 0; i < s->n_keys; ++i) free(s->keys[i].col_name);
        free(s->keys); free(s);
        return BETL_ERR_INVALID;
    }
    *state = s;
    return BETL_OK;
}

static int sort_attach_input(void *state, int port,
                             struct ArrowArrayStream *in) {
    (void)port;
    SortState *s = state;
    s->input      = *in;
    s->have_input = 1;
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void sort_destroy(void *state) {
    if (!state) return;
    SortState *s = state;
    if (s->have_input && s->input.release) s->input.release(&s->input);
    for (size_t i = 0; i < s->n_keys; ++i) free(s->keys[i].col_name);
    free(s->keys);
    if (s->col_names) {
        for (size_t i = 0; i < s->n_input_cols; ++i) free(s->col_names[i]);
        free(s->col_names);
    }
    free(s->col_fmts);
    if (s->i64_vals) {
        for (size_t c = 0; c < s->n_input_cols; ++c) free(s->i64_vals[c]);
        free(s->i64_vals);
    }
    if (s->u8_strs) {
        for (size_t c = 0; c < s->n_input_cols; ++c) {
            if (s->u8_strs[c]) {
                for (size_t r = 0; r < s->n_rows; ++r) free(s->u8_strs[c][r]);
                free(s->u8_strs[c]);
            }
        }
        free(s->u8_strs);
    }
    if (s->u8_lens) {
        for (size_t c = 0; c < s->n_input_cols; ++c) free(s->u8_lens[c]);
        free(s->u8_lens);
    }
    free(s);
}

/* Cache the schema, allocate per-column staging, resolve sort keys. */
static int sort_resolve(SortState *s) {
    if (s->schema_cached) return 0;
    if (!s->have_input || !s->input.get_schema) {
        sset_err(s, "sort: input has no get_schema"); return -1;
    }
    struct ArrowSchema sch = {0};
    if (s->input.get_schema(&s->input, &sch) != 0) {
        sset_err(s, "sort: upstream get_schema failed"); return -1;
    }
    int rc = -1;
    if (!sch.format || strcmp(sch.format, "+s") != 0 || sch.n_children <= 0) {
        sset_err(s, "sort: input must be a struct array");
        goto done;
    }
    size_t n = (size_t)sch.n_children;
    char **names = calloc(n, sizeof *names);
    char  *fmts  = calloc(n, 1);
    if (!names || !fmts) { free(names); free(fmts);
        sset_err(s, "sort: out of memory"); goto done;
    }
    int ok = 1;
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            sset_err(s, "sort: input column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            ok = 0; break;
        }
        fmts[i]  = fmt[0];
        names[i] = strdup((c && c->name) ? c->name : "");
        if (!names[i]) { sset_err(s, "sort: out of memory"); ok = 0; break; }
    }
    if (!ok) {
        for (size_t i = 0; i < n; ++i) free(names[i]);
        free(names); free(fmts);
        goto done;
    }
    s->n_input_cols = n;
    s->col_names    = names;
    s->col_fmts     = fmts;

    s->i64_vals = calloc(n, sizeof *s->i64_vals);
    s->u8_strs  = calloc(n, sizeof *s->u8_strs);
    s->u8_lens  = calloc(n, sizeof *s->u8_lens);
    if (!s->i64_vals || !s->u8_strs || !s->u8_lens) {
        sset_err(s, "sort: out of memory"); goto done;
    }

    /* Resolve sort keys against the input schema. */
    for (size_t i = 0; i < s->n_keys; ++i) {
        int idx = -1;
        for (size_t k = 0; k < n; ++k) {
            if (strcmp(names[k], s->keys[i].col_name) == 0) { idx = (int)k; break; }
        }
        if (idx < 0) {
            sset_err(s, "sort: by column '%s' not found in input",
                     s->keys[i].col_name);
            goto done;
        }
        s->keys[i].col_idx = idx;
        s->keys[i].fmt     = fmts[idx];
    }

    s->schema_cached = 1;
    rc = 0;
done:
    if (sch.release) sch.release(&sch);
    return rc;
}

static int sort_grow_rows(SortState *s) {
    size_t nc = s->row_cap ? s->row_cap * 2 : 64;
    for (size_t c = 0; c < s->n_input_cols; ++c) {
        if (s->col_fmts[c] == 'l') {
            int64_t *p = realloc(s->i64_vals[c], nc * sizeof *p);
            if (!p) return -1;
            s->i64_vals[c] = p;
        } else {
            char **sp = realloc(s->u8_strs[c], nc * sizeof *sp);
            if (!sp) return -1;
            s->u8_strs[c] = sp;
            size_t *lp = realloc(s->u8_lens[c], nc * sizeof *lp);
            if (!lp) return -1;
            s->u8_lens[c] = lp;
        }
    }
    s->row_cap = nc;
    return 0;
}

/* Read a typed cell from an upstream column at row_idx; copy into our
 * staging at our own row index `r`. Returns 0/-1. */
static int sort_stash_cell(SortState *s, size_t c,
                           const struct ArrowArray *src,
                           size_t row_idx, size_t r) {
    size_t row = row_idx + (size_t)src->offset;
    if (s->col_fmts[c] == 'l') {
        s->i64_vals[c][r] = ((const int64_t *)src->buffers[1])[row];
        return 0;
    }
    /* utf8 */
    const int32_t *off = src->buffers[1];
    const char    *dat = src->buffers[2];
    size_t len = (size_t)(off[row + 1] - off[row]);
    char *dup = malloc(len + 1);
    if (!dup) return -1;
    if (len) memcpy(dup, dat + off[row], len);
    dup[len] = '\0';
    s->u8_strs[c][r] = dup;
    s->u8_lens[c][r] = len;
    return 0;
}

static int sort_materialize(SortState *s) {
    if (s->materialized) return 0;
    if (sort_resolve(s) != 0) return -1;
    for (;;) {
        if (betl_should_cancel(s->ctx)) {
            sset_err(s, "sort: cancelled"); return -1;
        }
        struct ArrowArray batch = {0};
        if (s->input.get_next(&s->input, &batch) != 0) {
            const char *e = s->input.get_last_error
                                ? s->input.get_last_error(&s->input) : NULL;
            sset_err(s, "sort: upstream get_next failed: %s",
                     e ? e : "(no detail)");
            return -1;
        }
        if (!batch.release) break;
        if (batch.n_children != (int64_t)s->n_input_cols) {
            int64_t got = batch.n_children;
            size_t expected = s->n_input_cols;
            batch.release(&batch);
            sset_err(s, "sort: batch has %lld cols, expected %zu",
                     (long long)got, expected);
            return -1;
        }
        size_t length = (size_t)batch.length;
        for (size_t r = 0; r < length; ++r) {
            if (s->n_rows == s->row_cap && sort_grow_rows(s) != 0) {
                batch.release(&batch);
                sset_err(s, "sort: out of memory"); return -1;
            }
            for (size_t c = 0; c < s->n_input_cols; ++c) {
                if (sort_stash_cell(s, c, batch.children[c], r, s->n_rows) != 0) {
                    batch.release(&batch);
                    sset_err(s, "sort: out of memory"); return -1;
                }
            }
            ++s->n_rows;
        }
        batch.release(&batch);
    }
    s->materialized = 1;
    return 0;
}

/* qsort comparator. Uses a thread-local pointer to the active state;
 * v0.1 betl runs single-threaded so this is safe. */
static _Thread_local SortState *cmp_state;

static int sort_cmp_rows(const void *a, const void *b) {
    SortState *s = cmp_state;
    size_t ra = *(const size_t *)a;
    size_t rb = *(const size_t *)b;
    for (size_t k = 0; k < s->n_keys; ++k) {
        SortKey *key = &s->keys[k];
        int dir = key->asc ? 1 : -1;
        if (key->fmt == 'l') {
            int64_t va = s->i64_vals[key->col_idx][ra];
            int64_t vb = s->i64_vals[key->col_idx][rb];
            if (va < vb) return -1 * dir;
            if (va > vb) return  1 * dir;
        } else {
            const char *sa = s->u8_strs[key->col_idx][ra];
            const char *sb = s->u8_strs[key->col_idx][rb];
            size_t la = s->u8_lens[key->col_idx][ra];
            size_t lb = s->u8_lens[key->col_idx][rb];
            size_t mn = la < lb ? la : lb;
            int c = memcmp(sa, sb, mn);
            if (c != 0) return c * dir;
            if (la != lb) return (la < lb ? -1 : 1) * dir;
        }
    }
    /* Stable tie-break by original row index. */
    if (ra < rb) return -1;
    if (ra > rb) return  1;
    return 0;
}

/* Build an int64 leaf from a column, indexed by `order`. */
static int sort_build_int64(struct ArrowArray *out,
                            const int64_t *src, const size_t *order, size_t n) {
    int64_t *vals = malloc((n ? n : 1) * sizeof *vals);
    if (!vals) return -1;
    for (size_t i = 0; i < n; ++i) vals[i] = src[order[i]];
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(vals); return -1; }
    bufs[0] = NULL; bufs[1] = vals;
    out->length = (int64_t)n;
    out->null_count = 0;
    out->n_buffers = 2;
    out->buffers = bufs;
    out->release = release_int64_leaf;
    return 0;
}

/* Build a utf8 leaf from per-row strings indexed by `order`. */
static int sort_build_utf8(struct ArrowArray *out,
                           char *const *strs, const size_t *lens,
                           const size_t *order, size_t n) {
    int32_t *offs = malloc((n + 1) * sizeof *offs);
    if (!offs) return -1;
    size_t total = 0;
    offs[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        total += lens[order[i]];
        if (total > (size_t)INT32_MAX) { free(offs); return -1; }
        offs[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offs); return -1; }
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t k = order[i];
        if (lens[k]) memcpy(data + pos, strs[k], lens[k]);
        pos += lens[k];
    }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); return -1; }
    bufs[0] = NULL; bufs[1] = offs; bufs[2] = data;
    out->length = (int64_t)n;
    out->null_count = 0;
    out->n_buffers = 3;
    out->buffers = bufs;
    out->release = release_utf8_leaf;
    return 0;
}

static int sort_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    SortState *s = st->private_data;
    if (!s) return EINVAL;
    if (sort_resolve(s) != 0) return EIO;
    /* Sort doesn't change the schema — forward upstream's. */
    return s->input.get_schema(&s->input, out);
}

static int sort_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    SortState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (sort_materialize(s) != 0) return EIO;
    if (s->emitted) return 0;

    size_t n = s->n_rows;
    size_t *order = malloc((n ? n : 1) * sizeof *order);
    if (!order) { sset_err(s, "sort: out of memory"); return EIO; }
    for (size_t i = 0; i < n; ++i) order[i] = i;
    cmp_state = s;
    qsort(order, n, sizeof *order, sort_cmp_rows);
    cmp_state = NULL;

    size_t n_total = s->n_input_cols;
    struct ArrowArray **kids = calloc(n_total ? n_total : 1, sizeof *kids);
    if (!kids) { free(order); sset_err(s, "sort: out of memory"); return EIO; }
    for (size_t c = 0; c < n_total; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        int rc;
        if (s->col_fmts[c] == 'l') {
            rc = sort_build_int64(kids[c], s->i64_vals[c], order, n);
        } else {
            rc = sort_build_utf8(kids[c], s->u8_strs[c], s->u8_lens[c], order, n);
        }
        if (!kids[c] || rc != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); free(order);
            sset_err(s, "sort: failed to build output column");
            return EIO;
        }
    }
    free(order);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < n_total; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        sset_err(s, "sort: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = release_struct;

    s->emitted = 1;
    return 0;
}

static const char *sort_get_last_error(struct ArrowArrayStream *st) {
    SortState *s = st->private_data;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}

static void sort_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int sort_attach_output(void *state, int port,
                              struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = sort_get_schema;
    out->get_next       = sort_get_next;
    out->get_last_error = sort_get_last_error;
    out->release        = sort_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef sort_inputs[]  = {
    { .name = "in",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "rows to sort" },
};
static const BetlPortDef sort_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "sorted rows" },
};

static const BetlComponentDef sort_components[] = {
    { .name               = "sort",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = sort_inputs,
      .input_count        = 1,
      .outputs            = sort_outputs,
      .output_count       = 1,
      .init               = sort_init,
      .destroy            = sort_destroy,
      .attach_input       = sort_attach_input,
      .attach_output      = sort_attach_output },
};

static const BetlProvider sort_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-sort",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = sort_components,
    .component_count = sizeof sort_components / sizeof sort_components[0],
};


/* ============================================================== *
 *  join — TRANSFORM                                                *
 *                                                                  *
 *  Two-stream inner join. v0.1 config:                             *
 *    from:  [<left>, <right>]   # port 0 = left, port 1 = right    *
 *    on:    { left_col: right_col, ... }   # >=1 pair              *
 *    kind:  inner               # only inner in v0.1               *
 *                                                                  *
 *  Output schema = left columns followed by right columns. Right   *
 *  column names that collide with left names are NOT renamed in    *
 *  v0.1 — users should `map: select:` first to disambiguate.       *
 *                                                                  *
 *  Implementation: pull every row from each side into per-column   *
 *  staging, then for each left row scan the right side linearly    *
 *  for matches, emitting one output row per match.                 *
 * ============================================================== */

/* Per-side materialized table. */
typedef struct {
    size_t   n_cols;
    char   **names;
    char    *fmts;
    int64_t **i64;
    char  ***u8s;
    size_t **u8l;
    size_t   n_rows;
    size_t   row_cap;
} JTab;

static void jtab_free(JTab *t) {
    if (t->names) {
        for (size_t i = 0; i < t->n_cols; ++i) free(t->names[i]);
        free(t->names);
    }
    free(t->fmts);
    if (t->i64) {
        for (size_t c = 0; c < t->n_cols; ++c) free(t->i64[c]);
        free(t->i64);
    }
    if (t->u8s) {
        for (size_t c = 0; c < t->n_cols; ++c) {
            if (t->u8s[c]) {
                for (size_t r = 0; r < t->n_rows; ++r) free(t->u8s[c][r]);
                free(t->u8s[c]);
            }
        }
        free(t->u8s);
    }
    if (t->u8l) {
        for (size_t c = 0; c < t->n_cols; ++c) free(t->u8l[c]);
        free(t->u8l);
    }
    memset(t, 0, sizeof *t);
}

/* Cache a side's schema in `t`. Allocates arrays sized to n_cols. */
static int jtab_set_schema(JTab *t, const struct ArrowSchema *sch,
                           char *err, size_t err_cap) {
    if (!sch->format || strcmp(sch->format, "+s") != 0 || sch->n_children <= 0) {
        snprintf(err, err_cap, "input must be a struct array");
        return -1;
    }
    size_t n = (size_t)sch->n_children;
    t->names = calloc(n, sizeof *t->names);
    t->fmts  = calloc(n, 1);
    t->i64   = calloc(n, sizeof *t->i64);
    t->u8s   = calloc(n, sizeof *t->u8s);
    t->u8l   = calloc(n, sizeof *t->u8l);
    if (!t->names || !t->fmts || !t->i64 || !t->u8s || !t->u8l) {
        snprintf(err, err_cap, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; ++i) {
        struct ArrowSchema *c = sch->children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        if (!fmt || (strcmp(fmt, "l") != 0 && strcmp(fmt, "u") != 0)) {
            snprintf(err, err_cap, "column '%s' has unsupported format '%s'",
                     (c && c->name) ? c->name : "?", fmt ? fmt : "(none)");
            return -1;
        }
        t->fmts[i]  = fmt[0];
        t->names[i] = strdup((c && c->name) ? c->name : "");
        if (!t->names[i]) { snprintf(err, err_cap, "out of memory"); return -1; }
    }
    t->n_cols = n;
    return 0;
}

static int jtab_grow_rows(JTab *t) {
    size_t nc = t->row_cap ? t->row_cap * 2 : 64;
    for (size_t c = 0; c < t->n_cols; ++c) {
        if (t->fmts[c] == 'l') {
            int64_t *p = realloc(t->i64[c], nc * sizeof *p);
            if (!p) return -1;
            t->i64[c] = p;
        } else {
            char **sp = realloc(t->u8s[c], nc * sizeof *sp);
            if (!sp) return -1;
            t->u8s[c] = sp;
            size_t *lp = realloc(t->u8l[c], nc * sizeof *lp);
            if (!lp) return -1;
            t->u8l[c] = lp;
        }
    }
    t->row_cap = nc;
    return 0;
}

static int jtab_stash(JTab *t, size_t c,
                      const struct ArrowArray *src,
                      size_t row_idx, size_t r) {
    size_t row = row_idx + (size_t)src->offset;
    if (t->fmts[c] == 'l') {
        t->i64[c][r] = ((const int64_t *)src->buffers[1])[row];
        return 0;
    }
    const int32_t *off = src->buffers[1];
    const char    *dat = src->buffers[2];
    size_t len = (size_t)(off[row + 1] - off[row]);
    char *dup = malloc(len + 1);
    if (!dup) return -1;
    if (len) memcpy(dup, dat + off[row], len);
    dup[len] = '\0';
    t->u8s[c][r] = dup;
    t->u8l[c][r] = len;
    return 0;
}

static int jtab_pull(JTab *t, struct ArrowArrayStream *in,
                     char *err, size_t err_cap) {
    for (;;) {
        struct ArrowArray batch = {0};
        if (in->get_next(in, &batch) != 0) {
            const char *e = in->get_last_error ? in->get_last_error(in) : NULL;
            snprintf(err, err_cap, "get_next failed: %s", e ? e : "(no detail)");
            return -1;
        }
        if (!batch.release) return 0;
        if (batch.n_children != (int64_t)t->n_cols) {
            int64_t got = batch.n_children;
            size_t expected = t->n_cols;
            batch.release(&batch);
            snprintf(err, err_cap, "batch has %lld cols, expected %zu",
                     (long long)got, expected);
            return -1;
        }
        size_t length = (size_t)batch.length;
        for (size_t r = 0; r < length; ++r) {
            if (t->n_rows == t->row_cap && jtab_grow_rows(t) != 0) {
                batch.release(&batch);
                snprintf(err, err_cap, "out of memory");
                return -1;
            }
            for (size_t c = 0; c < t->n_cols; ++c) {
                if (jtab_stash(t, c, batch.children[c], r, t->n_rows) != 0) {
                    batch.release(&batch);
                    snprintf(err, err_cap, "out of memory");
                    return -1;
                }
            }
            ++t->n_rows;
        }
        batch.release(&batch);
    }
}

typedef struct {
    char *left_col;
    char *right_col;
    int   left_idx;
    int   right_idx;
    char  fmt;
} JoinKey;

typedef struct {
    BetlContext *ctx;

    JoinKey *keys;
    size_t   n_keys;

    /* `kind` is parsed but only "inner" supported in v0.1. */
    char *kind;

    struct ArrowArrayStream left_in;
    int                     have_left;
    struct ArrowArrayStream right_in;
    int                     have_right;

    JTab L;
    JTab R;

    int materialized;
    int emitted;

    char last_err[256];
} JoinState;

static void jset_err(JoinState *j, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->last_err, sizeof j->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(j->ctx, "%s", j->last_err);
}

typedef struct { JoinState *j; int err; } JoinOnCtx;

static int join_on_visit(const char *key, const char *value, size_t value_len,
                         void *user) {
    JoinOnCtx *c = user;
    JoinState *j = c->j;
    if (value_len == 0 || value[0] != '"') {
        jset_err(j, "join: on['%s'] must be a string (right column name)", key);
        c->err = 1; return -1;
    }
    char *rcol = NULL;
    if (json_decode_str(value, &rcol) != 0 || !rcol) { c->err = 1; return -1; }

    JoinKey *grow = realloc(j->keys, (j->n_keys + 1) * sizeof *grow);
    if (!grow) { free(rcol); c->err = 1; return -1; }
    j->keys = grow;
    JoinKey *k = &j->keys[j->n_keys++];
    memset(k, 0, sizeof *k);
    k->left_col  = strdup(key);
    k->right_col = rcol;
    k->left_idx  = -1;
    k->right_idx = -1;
    if (!k->left_col) { c->err = 1; return -1; }
    return 0;
}

static int join_init(BetlContext *ctx, const char *cfg, void **state) {
    JoinState *j = calloc(1, sizeof *j);
    if (!j) return BETL_ERR_INTERNAL;
    j->ctx = ctx;
    cfg = cfg ? cfg : "{}";

    char *kind = NULL;
    json_string_at(cfg, "kind", &kind);
    j->kind = kind ? kind : strdup("inner");
    if (!j->kind) { free(j); return BETL_ERR_INTERNAL; }
    if (strcmp(j->kind, "inner") != 0) {
        jset_err(j, "join: only 'inner' supported in v0.1 (got '%s')", j->kind);
        free(j->kind); free(j); return BETL_ERR_UNSUPPORTED;
    }

    const char *on = json_value_after(cfg, "on");
    if (!on || *on != '{') {
        jset_err(j, "join: requires `on:` map of {left_col: right_col}");
        free(j->kind); free(j); return BETL_ERR_INVALID;
    }
    JoinOnCtx oc = { .j = j, .err = 0 };
    if (json_walk_object_at(on, join_on_visit, &oc) != 0 || oc.err
        || j->n_keys == 0) {
        if (j->last_err[0] == '\0') jset_err(j, "join: `on:` is empty");
        for (size_t i = 0; i < j->n_keys; ++i) {
            free(j->keys[i].left_col); free(j->keys[i].right_col);
        }
        free(j->keys); free(j->kind); free(j);
        return BETL_ERR_INVALID;
    }
    *state = j;
    return BETL_OK;
}

static int join_attach_input(void *state, int port,
                             struct ArrowArrayStream *in) {
    JoinState *j = state;
    if (port == 0) {
        j->left_in   = *in;
        j->have_left = 1;
    } else if (port == 1) {
        j->right_in   = *in;
        j->have_right = 1;
    } else {
        return BETL_ERR_INVALID;
    }
    memset(in, 0, sizeof *in);
    return BETL_OK;
}

static void join_destroy(void *state) {
    if (!state) return;
    JoinState *j = state;
    if (j->have_left  && j->left_in.release)  j->left_in.release(&j->left_in);
    if (j->have_right && j->right_in.release) j->right_in.release(&j->right_in);
    for (size_t i = 0; i < j->n_keys; ++i) {
        free(j->keys[i].left_col); free(j->keys[i].right_col);
    }
    free(j->keys);
    free(j->kind);
    jtab_free(&j->L);
    jtab_free(&j->R);
    free(j);
}

/* Schema: left + right concatenated. Forward by pulling fresh schemas
 * from both upstreams and gluing them together. */
static int join_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    JoinState *j = st->private_data;
    memset(out, 0, sizeof *out);
    if (!j || !j->have_left || !j->have_right) return EINVAL;

    struct ArrowSchema sl = {0}, sr = {0};
    if (j->left_in.get_schema(&j->left_in, &sl) != 0) return EIO;
    if (j->right_in.get_schema(&j->right_in, &sr) != 0) {
        if (sl.release) sl.release(&sl);
        return EIO;
    }
    int64_t nl = sl.n_children, nr = sr.n_children;
    int64_t n_total = nl + nr;
    struct ArrowSchema **kids = calloc((size_t)n_total, sizeof *kids);
    if (!kids) {
        if (sl.release) sl.release(&sl);
        if (sr.release) sr.release(&sr);
        return ENOMEM;
    }
    for (int64_t i = 0; i < nl; ++i) { kids[i] = sl.children[i]; sl.children[i] = NULL; }
    for (int64_t i = 0; i < nr; ++i) { kids[nl + i] = sr.children[i]; sr.children[i] = NULL; }
    if (sl.release) sl.release(&sl);
    if (sr.release) sr.release(&sr);

    out->format     = "+s";
    out->n_children = n_total;
    out->children   = kids;
    out->release    = release_map_schema;
    return 0;
}

/* Resolve key columns + materialize both sides. */
static int join_materialize(JoinState *j) {
    if (j->materialized) return 0;
    if (!j->have_left || !j->have_right) {
        jset_err(j, "join: both inputs (left and right) must be attached");
        return -1;
    }
    /* Schemas. */
    struct ArrowSchema sl = {0}, sr = {0};
    if (j->left_in.get_schema(&j->left_in, &sl) != 0) {
        jset_err(j, "join: left get_schema failed"); return -1;
    }
    char err[160];
    if (jtab_set_schema(&j->L, &sl, err, sizeof err) != 0) {
        if (sl.release) sl.release(&sl);
        jset_err(j, "join: left: %s", err); return -1;
    }
    if (sl.release) sl.release(&sl);
    if (j->right_in.get_schema(&j->right_in, &sr) != 0) {
        jset_err(j, "join: right get_schema failed"); return -1;
    }
    if (jtab_set_schema(&j->R, &sr, err, sizeof err) != 0) {
        if (sr.release) sr.release(&sr);
        jset_err(j, "join: right: %s", err); return -1;
    }
    if (sr.release) sr.release(&sr);

    /* Resolve every key pair and check format compatibility. */
    for (size_t i = 0; i < j->n_keys; ++i) {
        JoinKey *k = &j->keys[i];
        for (size_t c = 0; c < j->L.n_cols; ++c) {
            if (strcmp(j->L.names[c], k->left_col) == 0) { k->left_idx = (int)c; break; }
        }
        for (size_t c = 0; c < j->R.n_cols; ++c) {
            if (strcmp(j->R.names[c], k->right_col) == 0) { k->right_idx = (int)c; break; }
        }
        if (k->left_idx < 0) {
            jset_err(j, "join: left column '%s' not found", k->left_col);
            return -1;
        }
        if (k->right_idx < 0) {
            jset_err(j, "join: right column '%s' not found", k->right_col);
            return -1;
        }
        if (j->L.fmts[k->left_idx] != j->R.fmts[k->right_idx]) {
            jset_err(j, "join: key '%s'/'%s' types differ ('%c' vs '%c')",
                     k->left_col, k->right_col,
                     j->L.fmts[k->left_idx], j->R.fmts[k->right_idx]);
            return -1;
        }
        k->fmt = j->L.fmts[k->left_idx];
    }

    /* Pull both sides. */
    if (jtab_pull(&j->L, &j->left_in,  err, sizeof err) != 0) {
        jset_err(j, "join: left: %s",  err); return -1;
    }
    if (jtab_pull(&j->R, &j->right_in, err, sizeof err) != 0) {
        jset_err(j, "join: right: %s", err); return -1;
    }
    j->materialized = 1;
    return 0;
}

static int row_keys_equal(const JoinState *j, size_t lr, size_t rr) {
    for (size_t k = 0; k < j->n_keys; ++k) {
        const JoinKey *key = &j->keys[k];
        if (key->fmt == 'l') {
            if (j->L.i64[key->left_idx][lr] != j->R.i64[key->right_idx][rr]) {
                return 0;
            }
        } else {
            size_t la = j->L.u8l[key->left_idx][lr];
            size_t lb = j->R.u8l[key->right_idx][rr];
            if (la != lb) return 0;
            if (la && memcmp(j->L.u8s[key->left_idx][lr],
                             j->R.u8s[key->right_idx][rr], la) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

/* Build an int64 leaf from a JTab column, with a pair of (row_idx, side) lists. */
static int join_build_int64_leaf(struct ArrowArray *out,
                                 JTab *side, int col_idx,
                                 const size_t *rows, size_t n) {
    int64_t *v = malloc((n ? n : 1) * sizeof *v);
    if (!v) return -1;
    for (size_t i = 0; i < n; ++i) v[i] = side->i64[col_idx][rows[i]];
    const void **bufs = malloc(2 * sizeof *bufs);
    if (!bufs) { free(v); return -1; }
    bufs[0] = NULL; bufs[1] = v;
    out->length = (int64_t)n;
    out->n_buffers = 2;
    out->buffers = bufs;
    out->release = release_int64_leaf;
    return 0;
}

static int join_build_utf8_leaf(struct ArrowArray *out,
                                JTab *side, int col_idx,
                                const size_t *rows, size_t n) {
    int32_t *offs = malloc((n + 1) * sizeof *offs);
    if (!offs) return -1;
    size_t total = 0;
    offs[0] = 0;
    for (size_t i = 0; i < n; ++i) {
        total += side->u8l[col_idx][rows[i]];
        if (total > (size_t)INT32_MAX) { free(offs); return -1; }
        offs[i + 1] = (int32_t)total;
    }
    char *data = malloc(total ? total : 1);
    if (!data) { free(offs); return -1; }
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        size_t len = side->u8l[col_idx][rows[i]];
        if (len) memcpy(data + pos, side->u8s[col_idx][rows[i]], len);
        pos += len;
    }
    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); return -1; }
    bufs[0] = NULL; bufs[1] = offs; bufs[2] = data;
    out->length = (int64_t)n;
    out->n_buffers = 3;
    out->buffers = bufs;
    out->release = release_utf8_leaf;
    return 0;
}

static int join_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    JoinState *j = st->private_data;
    memset(out, 0, sizeof *out);
    if (!j) return EINVAL;
    if (join_materialize(j) != 0) return EIO;
    if (j->emitted) return 0;

    /* Find all match pairs (left_row, right_row). Linear scan v0.1. */
    size_t cap = 16, n = 0;
    size_t *lr = malloc(cap * sizeof *lr);
    size_t *rr = malloc(cap * sizeof *rr);
    if (!lr || !rr) { free(lr); free(rr); jset_err(j, "join: out of memory"); return EIO; }

    for (size_t li = 0; li < j->L.n_rows; ++li) {
        if (betl_should_cancel(j->ctx)) {
            free(lr); free(rr);
            jset_err(j, "join: cancelled"); return EIO;
        }
        for (size_t ri = 0; ri < j->R.n_rows; ++ri) {
            if (!row_keys_equal(j, li, ri)) continue;
            if (n == cap) {
                cap *= 2;
                size_t *nlr = realloc(lr, cap * sizeof *lr);
                size_t *nrr = realloc(rr, cap * sizeof *rr);
                if (!nlr || !nrr) {
                    free(nlr ? nlr : lr); free(nrr ? nrr : rr);
                    jset_err(j, "join: out of memory"); return EIO;
                }
                lr = nlr; rr = nrr;
            }
            lr[n] = li; rr[n] = ri; ++n;
        }
    }

    size_t n_total = j->L.n_cols + j->R.n_cols;
    if (n_total == 0) {
        free(lr); free(rr);
        jset_err(j, "join: no output columns");
        return EIO;
    }
    struct ArrowArray **kids = calloc(n_total, sizeof *kids);
    if (!kids) {
        free(lr); free(rr);
        jset_err(j, "join: out of memory"); return EIO;
    }
    for (size_t c = 0; c < n_total; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        int rc;
        if (c < j->L.n_cols) {
            int idx = (int)c;
            rc = (j->L.fmts[idx] == 'l')
                 ? join_build_int64_leaf(kids[c], &j->L, idx, lr, n)
                 : join_build_utf8_leaf (kids[c], &j->L, idx, lr, n);
        } else {
            int idx = (int)(c - j->L.n_cols);
            rc = (j->R.fmts[idx] == 'l')
                 ? join_build_int64_leaf(kids[c], &j->R, idx, rr, n)
                 : join_build_utf8_leaf (kids[c], &j->R, idx, rr, n);
        }
        if (!kids[c] || rc != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); free(lr); free(rr);
            jset_err(j, "join: failed to build output column");
            return EIO;
        }
    }
    free(lr); free(rr);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < n_total; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        jset_err(j, "join: out of memory"); return EIO;
    }
    outer[0] = NULL;

    out->length     = (int64_t)n;
    out->null_count = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)n_total;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = release_struct;

    j->emitted = 1;
    return 0;
}

static const char *join_get_last_error(struct ArrowArrayStream *st) {
    JoinState *j = st->private_data;
    return (j && j->last_err[0]) ? j->last_err : NULL;
}

static void join_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int join_attach_output(void *state, int port,
                              struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = join_get_schema;
    out->get_next       = join_get_next;
    out->get_last_error = join_get_last_error;
    out->release        = join_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef join_inputs[]  = {
    { .name = "left",  .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "left rows" },
    { .name = "right", .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "right rows" },
};
static const BetlPortDef join_outputs[] = {
    { .name = "out",   .schema_mode = BETL_SCHEMA_DYNAMIC, .doc = "joined rows" },
};

static const BetlComponentDef join_components[] = {
    { .name               = "join",
      .kind               = BETL_KIND_TRANSFORM,
      .config_schema_json = "{}",
      .flags              = 0,
      .inputs             = join_inputs,
      .input_count        = 2,
      .outputs            = join_outputs,
      .output_count       = 1,
      .init               = join_init,
      .destroy            = join_destroy,
      .attach_input       = join_attach_input,
      .attach_output      = join_attach_output },
};

static const BetlProvider join_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-join",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = join_components,
    .component_count = sizeof join_components / sizeof join_components[0],
};


/* ============================================================== *
 *  Registry                                                        *
 * ============================================================== */

int betl_register_transforms(BetlRegistry *r) {
    int rc;
    rc = betl_registry_register(r, &filter_provider, "<builtin:filter>");
    if (rc != BETL_OK) return rc;
    rc = betl_registry_register(r, &map_provider,    "<builtin:map>");
    if (rc != BETL_OK) return rc;
    rc = betl_registry_register(r, &agg_provider,    "<builtin:aggregate>");
    if (rc != BETL_OK) return rc;
    rc = betl_registry_register(r, &sort_provider,   "<builtin:sort>");
    if (rc != BETL_OK) return rc;
    rc = betl_registry_register(r, &join_provider,   "<builtin:join>");
    if (rc != BETL_OK) return rc;
    return BETL_OK;
}
