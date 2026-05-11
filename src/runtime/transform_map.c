/* `map` TRANSFORM (SPEC §4.3) — `add:` and `select:` modes.
 *
 *   add:    appends new columns derived from expressions, preserving
 *           input columns. Output schema = input + add.
 *   select: replaces the column set with a list of pass-through /
 *           rename / computed entries. Same input column may appear
 *           multiple times (deep-copied).
 *
 * Engines are resolved via betl_get_expr_engine at first batch and
 * compiled against the upstream schema. v0.1 supports int64 and utf8
 * input columns and "l" / "u" / "b" desired_format for outputs.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/transforms_internal.h"


typedef struct {
    char *name;          /* output column name */
    char *lang;          /* engine language ("lua" default) */
    char *source;        /* expr text or literal value as a string */
    char *out_format;    /* "l", "u", "b" — defaults to "u" */

    const BetlExprEngine *engine;
    void                 *handle;
    int                   handle_ready;
} MapAdd;

typedef enum {
    SEL_PASS   = 1,
    SEL_RENAME = 2,
    SEL_EXPR   = 3,
} SelKind;

typedef struct {
    SelKind kind;
    char   *out_name;
    char   *from_name;
    int     from_idx;
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

    MapAdd     *adds;
    size_t      n_adds;

    SelEntry   *sels;
    size_t      n_sels;

    struct ArrowArrayStream input;
    int                     have_input;

    int          schema_cached;
    size_t       n_input_cols;
    struct ArrowSchema input_schema;
    int                input_schema_owned;
    char       **input_col_names;
    char        *input_col_fmts;

    char         last_err[256];
} MapState;

static void mset_err(MapState *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->last_err, sizeof m->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(m->ctx, "%s", m->last_err);
}

typedef struct { MapState *m; int err; } AddCtx;

static int add_visit(const char *key, const char *value, size_t value_len,
                     void *user) {
    AddCtx *c = user;
    MapState *m = c->m;
    if (value_len == 0 || value[0] != '{') {
        mset_err(m, "map: add column '%s' must be a {lang, expr|value} object", key);
        c->err = 1;
        return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *lang = NULL, *expr = NULL, *vstr = NULL, *type = NULL;
    betl_tx_json_string_at(vbuf, "lang",  &lang);
    betl_tx_json_string_at(vbuf, "expr",  &expr);
    betl_tx_json_value_to_string(vbuf, "value", &vstr);
    betl_tx_json_string_at(vbuf, "type",  &type);
    free(vbuf);

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
    const char *add = betl_tx_json_value_after(cfg, "add");
    if (!add || *add != '{') {
        mset_err(m, "map: requires an `add:` map");
        return BETL_ERR_INVALID;
    }
    AddCtx c = { .m = m, .err = 0 };
    if (betl_tx_json_walk_object(add, add_visit, &c) != 0 || c.err) {
        return BETL_ERR_INVALID;
    }
    if (m->n_adds == 0) {
        mset_err(m, "map: `add:` is empty");
        return BETL_ERR_INVALID;
    }
    return BETL_OK;
}

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
        char *name = NULL;
        if (betl_tx_json_decode_str(value, &name) != 0 || !name) {
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

    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';

    char *name = NULL, *from = NULL, *expr = NULL, *vstr = NULL;
    char *type = NULL, *lang = NULL;
    betl_tx_json_string_at(vbuf, "name", &name);
    betl_tx_json_string_at(vbuf, "from", &from);
    betl_tx_json_string_at(vbuf, "expr", &expr);
    betl_tx_json_value_to_string(vbuf, "value", &vstr);
    betl_tx_json_string_at(vbuf, "type", &type);
    betl_tx_json_string_at(vbuf, "lang", &lang);
    free(vbuf);

    if (!name) {
        free(from); free(expr); free(vstr); free(type); free(lang);
        mset_err(m, "map: select: object entry must have a `name` field");
        c->err = 1; return -1;
    }

    if (expr || vstr) {
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
    const char *sel = betl_tx_json_value_after(cfg, "select");
    if (!sel || *sel != '[') {
        mset_err(m, "map: `select:` must be a list");
        return BETL_ERR_INVALID;
    }
    SelCtx c = { .m = m, .err = 0 };
    if (betl_tx_json_walk_array(sel, sel_visit, &c) != 0 || c.err) {
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

    const char *add_at = betl_tx_json_value_after(cfg, "add");
    const char *sel_at = betl_tx_json_value_after(cfg, "select");
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
    map_state_free(m);
}

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

    m->input_col_names = calloc(m->n_input_cols, sizeof *m->input_col_names);
    m->input_col_fmts  = calloc(m->n_input_cols, 1);
    if (!m->input_col_names || !m->input_col_fmts) {
        mset_err(m, "map: out of memory"); return -1;
    }
    for (size_t i = 0; i < m->n_input_cols; ++i) {
        struct ArrowSchema *c = m->input_schema.children[i];
        const char *fmt = (c && c->format) ? c->format : NULL;
        int is_fixed = fmt && betl_tx_fixed_width_for_fmt(fmt[0]) != 0
                       && (fmt[0] == 'c' || fmt[0] == 'C' ||
                           fmt[0] == 's' || fmt[0] == 'S' ||
                           fmt[0] == 'i' || fmt[0] == 'I' ||
                           fmt[0] == 'l' || fmt[0] == 'L' ||
                           fmt[0] == 'f' || fmt[0] == 'g');
        int is_utf8  = fmt && strcmp(fmt, "u") == 0;
        if (!is_fixed && !is_utf8) {
            mset_err(m, "map: input column '%s' has unsupported format '%s' "
                        "(supports fixed-width primitive ints/floats and utf8)",
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
    } else {
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

/* Map a desired_format string to an Arrow leaf schema's `format`
 * string. Accepts every primitive format the in-tree engines can
 * produce — and currently they all happen to be identity-mapped. */
static const char *desired_to_format(const char *desired) {
    if (strcmp(desired, "l") == 0) return "l";
    if (strcmp(desired, "L") == 0) return "L";
    if (strcmp(desired, "c") == 0) return "c";
    if (strcmp(desired, "C") == 0) return "C";
    if (strcmp(desired, "s") == 0) return "s";
    if (strcmp(desired, "S") == 0) return "S";
    if (strcmp(desired, "i") == 0) return "i";
    if (strcmp(desired, "I") == 0) return "I";
    if (strcmp(desired, "f") == 0) return "f";
    if (strcmp(desired, "g") == 0) return "g";
    if (strcmp(desired, "u") == 0) return "u";
    if (strcmp(desired, "b") == 0) return "b";
    return NULL;
}

/* Deep-copy a fixed-width primitive leaf, applying src's offset so the
 * destination starts at offset 0. Used by select mode (where the same
 * input column may be referenced more than once and ownership cannot be
 * moved). Element width given by `elem_size`. */
static int deepcopy_fixed_leaf(struct ArrowArray *dst,
                               const struct ArrowArray *src,
                               size_t elem_size) {
    if (elem_size == 0) return -1;
    int64_t length = src->length;
    int64_t offset = src->offset;
    int64_t null_count = src->null_count;

    uint8_t *vals = malloc((size_t)(length ? length : 1) * elem_size);
    if (!vals) return -1;
    if (length > 0) {
        const uint8_t *svals = src->buffers[1];
        memcpy(vals,
               svals + (size_t)offset * elem_size,
               (size_t)length * elem_size);
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
    dst->release    = betl_tx_release_int64_leaf;  /* same shape: 2 buffers */
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
    dst->release    = betl_tx_release_utf8_leaf;
    return 0;
}

static int map_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    MapState *m = st->private_data;
    if (!m) return EINVAL;
    if (map_ensure_ready(m) != 0) return EIO;
    memset(out, 0, sizeof *out);

    if (m->mode == MAP_MODE_ADD) {
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
            struct ArrowSchema *c = betl_tx_new_leaf_schema(a->name, fmt);
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
        out->release    = betl_tx_release_schema_struct_owned;
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
        } else {
            char ch = m->input_col_fmts[s->from_idx];
            switch (ch) {
                case 'l': fmt = "l"; break;
                case 'L': fmt = "L"; break;
                case 'c': fmt = "c"; break;
                case 'C': fmt = "C"; break;
                case 's': fmt = "s"; break;
                case 'S': fmt = "S"; break;
                case 'i': fmt = "i"; break;
                case 'I': fmt = "I"; break;
                case 'f': fmt = "f"; break;
                case 'g': fmt = "g"; break;
                default:  fmt = "u"; break;
            }
        }
        if (!fmt) {
            for (size_t k = 0; k < i; ++k) {
                if (kids[k] && kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids); return EINVAL;
        }
        kids[i] = betl_tx_new_leaf_schema(s->out_name, fmt);
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
    out->release    = betl_tx_release_schema_struct_owned;
    return 0;
}

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
        for (size_t i = 0; i < m->n_input_cols; ++i) {
            kids[i] = in_arr.children[i];
            in_arr.children[i] = NULL;
        }
    } else {
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
                char ch = m->input_col_fmts[s->from_idx];
                size_t w = betl_tx_fixed_width_for_fmt(ch);
                if (w != 0) {
                    rc = deepcopy_fixed_leaf(leaf, src, w);
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
    out->release    = betl_tx_release_struct;

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

int betl_tx_register_map(BetlRegistry *r) {
    return betl_registry_register(r, &map_provider, "<builtin:map>");
}
