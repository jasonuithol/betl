/* xlsx.read — SOURCE that reads rows from a single sheet in an .xlsx
 * file via libxlsxio. SSIS Excel Source parity.
 *
 * Config:
 *   path        string, required
 *   sheet       string, optional — sheet name (default: first sheet)
 *   header      bool,   optional, default true — first row supplies
 *                                                column names
 *   columns     list[string], optional — explicit column names when
 *                                        header: false
 *   batch_size  int,    optional, default 1024
 *
 * Type coverage v0.1: every column is emitted as Arrow utf8. Excel
 * doesn't carry schema-level types per column anyway — coerce to
 * int/decimal/date via `map` downstream when needed. */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xlsxio_read.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/transforms_internal.h"
#include "runtime/xlsx_read.h"

typedef struct {
    BetlContext *ctx;

    char        *path;
    char        *sheet_name;       /* may be NULL → first sheet */
    int          header;
    size_t       batch_size;
    char       **explicit_cols;
    size_t       n_explicit_cols;

    xlsxioreader      reader;
    xlsxioreadersheet sheet;
    int               opened;

    /* Resolved column names (strdup'd). */
    char       **col_names;
    size_t       n_cols;

    int          eof;

    char         last_err[400];
} XrState;

static void xrset_err(XrState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* ============================================================== *
 *  Config helpers                                                  *
 * ============================================================== */

typedef struct { XrState *s; char ***out; size_t *n_out; int err; } StrArrCtx;

static int str_visit(const char *value, size_t value_len, void *user) {
    StrArrCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        xrset_err(c->s, "xlsx.read: `columns:` entries must be strings");
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';
    char *name = NULL;
    if (betl_tx_json_decode_str(vbuf, &name) != 0 || !name) {
        free(vbuf); c->err = 1; return -1;
    }
    free(vbuf);
    size_t n = *c->n_out;
    char **grow = realloc(*c->out, (n + 1) * sizeof *grow);
    if (!grow) { free(name); c->err = 1; return -1; }
    *c->out = grow;
    grow[n] = name;
    *c->n_out = n + 1;
    return 0;
}

static int parse_cols(XrState *s, const char *cfg) {
    const char *pos = betl_tx_json_value_after(cfg, "columns");
    if (!pos) return 0;
    if (*pos != '[') {
        xrset_err(s, "xlsx.read: `columns:` must be a list");
        return -1;
    }
    StrArrCtx c = { .s = s, .out = &s->explicit_cols,
                    .n_out = &s->n_explicit_cols, .err = 0 };
    if (betl_tx_json_walk_array(pos, str_visit, &c) != 0 || c.err) return -1;
    return 0;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int xr_init(BetlContext *ctx, const char *cfg, void **state) {
    XrState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    s->header = 1;
    s->batch_size = 1024;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "path", &s->path) != 0 || !s->path) {
        xrset_err(s, "xlsx.read: missing required `path`");
        free(s); return BETL_ERR_INVALID;
    }
    if (betl_tx_json_string_at(cfg, "sheet", &s->sheet_name) != 0) {
        s->sheet_name = NULL;
    }
    {
        char *h = NULL;
        if (betl_tx_json_value_to_string(cfg, "header", &h) == 0 && h) {
            s->header = (strcmp(h, "false") != 0);
            free(h);
        }
    }
    {
        char *bs = NULL;
        if (betl_tx_json_value_to_string(cfg, "batch_size", &bs) == 0 && bs) {
            char *end = NULL;
            long v = strtol(bs, &end, 10);
            if (end != bs && *end == '\0' && v > 0) s->batch_size = (size_t)v;
            free(bs);
        }
    }
    if (parse_cols(s, cfg) != 0) {
        free(s->path); free(s->sheet_name); free(s);
        return BETL_ERR_INVALID;
    }

    *state = s;
    return BETL_OK;
}

static void xr_destroy(void *state) {
    if (!state) return;
    XrState *s = state;
    if (s->sheet)  xlsxioread_sheet_close(s->sheet);
    if (s->reader) xlsxioread_close(s->reader);
    if (s->col_names) {
        for (size_t i = 0; i < s->n_cols; ++i) free(s->col_names[i]);
        free(s->col_names);
    }
    for (size_t i = 0; i < s->n_explicit_cols; ++i) free(s->explicit_cols[i]);
    free(s->explicit_cols);
    free(s->sheet_name);
    free(s->path);
    free(s);
}

/* Open file + sheet and resolve column names. Sets s->col_names / n_cols. */
static int xr_open(XrState *s) {
    if (s->opened) return BETL_OK;
    s->reader = xlsxioread_open(s->path);
    if (!s->reader) {
        xrset_err(s, "xlsx.read: failed to open '%s'", s->path);
        return BETL_ERR_IO;
    }
    s->sheet = xlsxioread_sheet_open(s->reader,
        (s->sheet_name && *s->sheet_name) ? s->sheet_name : NULL,
        XLSXIOREAD_SKIP_EMPTY_ROWS);
    if (!s->sheet) {
        xrset_err(s, "xlsx.read: failed to open sheet '%s' in '%s'",
                  s->sheet_name ? s->sheet_name : "<first>", s->path);
        return BETL_ERR_IO;
    }

    if (s->header) {
        /* First row supplies column names. */
        if (!xlsxioread_sheet_next_row(s->sheet)) {
            xrset_err(s, "xlsx.read: file '%s' has no header row", s->path);
            return BETL_ERR_INVALID;
        }
        char *cell;
        size_t cap = 8, n = 0;
        char **names = calloc(cap, sizeof *names);
        if (!names) { xrset_err(s, "xlsx.read: out of memory"); return BETL_ERR_INTERNAL; }
        while ((cell = xlsxioread_sheet_next_cell(s->sheet)) != NULL) {
            if (n == cap) {
                cap *= 2;
                char **gr = realloc(names, cap * sizeof *names);
                if (!gr) {
                    for (size_t i = 0; i < n; ++i) free(names[i]);
                    free(names); free(cell);
                    xrset_err(s, "xlsx.read: out of memory");
                    return BETL_ERR_INTERNAL;
                }
                names = gr;
            }
            names[n++] = cell;       /* take ownership; caller must free */
        }
        if (n == 0) {
            free(names);
            xrset_err(s, "xlsx.read: header row in '%s' is empty", s->path);
            return BETL_ERR_INVALID;
        }
        s->col_names = names;
        s->n_cols    = n;
    } else if (s->n_explicit_cols > 0) {
        s->n_cols = s->n_explicit_cols;
        s->col_names = calloc(s->n_cols, sizeof *s->col_names);
        if (!s->col_names) {
            xrset_err(s, "xlsx.read: out of memory");
            return BETL_ERR_INTERNAL;
        }
        for (size_t i = 0; i < s->n_cols; ++i) {
            s->col_names[i] = strdup(s->explicit_cols[i]);
            if (!s->col_names[i]) {
                xrset_err(s, "xlsx.read: out of memory");
                return BETL_ERR_INTERNAL;
            }
        }
    } else {
        xrset_err(s, "xlsx.read: when header: false, `columns:` is required");
        return BETL_ERR_INVALID;
    }

    s->opened = 1;
    return BETL_OK;
}

/* ============================================================== *
 *  Stream                                                          *
 * ============================================================== */

static int xr_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    XrState *s = st->private_data;
    if (!s) return EINVAL;
    if (xr_open(s) != BETL_OK) return EIO;
    memset(out, 0, sizeof *out);
    struct ArrowSchema **kids = calloc(s->n_cols, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < s->n_cols; ++i) {
        kids[i] = betl_tx_new_leaf_schema(s->col_names[i], "u");
        if (!kids[i]) {
            for (size_t k = 0; k < i; ++k) {
                if (kids[k] && kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            return ENOMEM;
        }
    }
    out->format     = "+s";
    out->n_children = (int64_t)s->n_cols;
    out->children   = kids;
    out->release    = betl_tx_release_schema_struct_owned;
    return 0;
}

/* Build a utf8 leaf array from `cells` (an array of n_rows NUL-
 * terminated strings, NULL meaning the row had no value here). */
static int build_utf8_leaf(struct ArrowArray *out, char **cells, size_t n_rows) {
    size_t total = 0;
    int64_t null_count = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!cells[i]) { ++null_count; continue; }
        total += strlen(cells[i]);
    }

    int32_t *offs = malloc((n_rows + 1) * sizeof *offs);
    char    *data = malloc(total ? total : 1);
    size_t   vmap_bytes = (n_rows + 7) / 8;
    uint8_t *vmap = NULL;
    if (null_count > 0) {
        vmap = malloc(vmap_bytes ? vmap_bytes : 1);
        if (!vmap) { free(offs); free(data); return -1; }
        memset(vmap, 0xFF, vmap_bytes ? vmap_bytes : 1);
    }
    if (!offs || !data) { free(offs); free(data); free(vmap); return -1; }

    offs[0] = 0;
    size_t pos = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (!cells[i]) {
            if (vmap) vmap[i / 8] &= (uint8_t)~(1u << (i % 8));
        } else {
            size_t l = strlen(cells[i]);
            if (l) memcpy(data + pos, cells[i], l);
            pos += l;
        }
        offs[i + 1] = (int32_t)pos;
    }

    const void **bufs = malloc(3 * sizeof *bufs);
    if (!bufs) { free(offs); free(data); free(vmap); return -1; }
    bufs[0] = vmap;
    bufs[1] = offs;
    bufs[2] = data;
    out->length     = (int64_t)n_rows;
    out->null_count = null_count;
    out->offset     = 0;
    out->n_buffers  = 3;
    out->n_children = 0;
    out->buffers    = bufs;
    out->release    = betl_tx_release_utf8_leaf;
    return 0;
}

static int xr_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    XrState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (xr_open(s) != BETL_OK) return EIO;
    if (s->eof) return 0;

    /* Allocate per-column row buffers. Each entry is a NUL-terminated
     * string from xlsxio (heap-owned), or NULL for missing/empty. */
    char ***rows = calloc(s->n_cols, sizeof *rows);
    if (!rows) { xrset_err(s, "xlsx.read: out of memory"); return EIO; }
    for (size_t c = 0; c < s->n_cols; ++c) {
        rows[c] = calloc(s->batch_size, sizeof **rows);
        if (!rows[c]) {
            for (size_t k = 0; k < c; ++k) free(rows[k]);
            free(rows);
            xrset_err(s, "xlsx.read: out of memory");
            return EIO;
        }
    }

    size_t n_rows = 0;
    while (n_rows < s->batch_size) {
        if (betl_should_cancel(s->ctx)) {
            for (size_t c = 0; c < s->n_cols; ++c) {
                for (size_t r = 0; r < n_rows; ++r) free(rows[c][r]);
                free(rows[c]);
            }
            free(rows);
            xrset_err(s, "xlsx.read: cancelled");
            return EIO;
        }
        if (!xlsxioread_sheet_next_row(s->sheet)) { s->eof = 1; break; }
        for (size_t c = 0; c < s->n_cols; ++c) {
            char *v = xlsxioread_sheet_next_cell(s->sheet);
            rows[c][n_rows] = v;   /* may be NULL → row stays sparse here */
        }
        /* Drain any extra cells beyond declared column count. */
        char *extra;
        while ((extra = xlsxioread_sheet_next_cell(s->sheet)) != NULL) free(extra);
        ++n_rows;
    }

    if (n_rows == 0) {
        for (size_t c = 0; c < s->n_cols; ++c) free(rows[c]);
        free(rows);
        return 0;
    }

    /* Build a struct batch with N utf8 leaves. */
    struct ArrowArray **kids = calloc(s->n_cols, sizeof *kids);
    if (!kids) {
        for (size_t c = 0; c < s->n_cols; ++c) {
            for (size_t r = 0; r < n_rows; ++r) free(rows[c][r]);
            free(rows[c]);
        }
        free(rows);
        xrset_err(s, "xlsx.read: out of memory");
        return EIO;
    }
    for (size_t c = 0; c < s->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c] || build_utf8_leaf(kids[c], rows[c], n_rows) != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            for (size_t cc = 0; cc < s->n_cols; ++cc) {
                for (size_t r = 0; r < n_rows; ++r) free(rows[cc][r]);
                free(rows[cc]);
            }
            free(rows);
            xrset_err(s, "xlsx.read: out of memory");
            return EIO;
        }
    }
    /* Free the per-cell strings now that they've been copied into the
     * Arrow leaf buffers. */
    for (size_t c = 0; c < s->n_cols; ++c) {
        for (size_t r = 0; r < n_rows; ++r) free(rows[c][r]);
        free(rows[c]);
    }
    free(rows);

    const void **outer = malloc(1 * sizeof *outer);
    if (!outer) {
        for (size_t c = 0; c < s->n_cols; ++c) {
            if (kids[c]->release) kids[c]->release(kids[c]);
            free(kids[c]);
        }
        free(kids);
        xrset_err(s, "xlsx.read: out of memory");
        return EIO;
    }
    outer[0] = NULL;
    out->length     = (int64_t)n_rows;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)s->n_cols;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;
    return 0;
}

static const char *xr_get_last_error(struct ArrowArrayStream *st) {
    XrState *s = st->private_data;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}

static void xr_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int xr_attach_output(void *state, int port,
                            struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = xr_get_schema;
    out->get_next       = xr_get_next;
    out->get_last_error = xr_get_last_error;
    out->release        = xr_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef xr_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows read from xlsx" },
};

static const BetlComponentDef xr_components[] = {
    { .name               = "xlsx.read",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = 0,
      .outputs            = xr_outputs,
      .output_count       = 1,
      .init               = xr_init,
      .destroy            = xr_destroy,
      .attach_output      = xr_attach_output },
};

static const BetlProvider xr_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-xlsx-read",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = xr_components,
    .component_count = sizeof xr_components / sizeof xr_components[0],
};

int betl_register_xlsx_read(BetlRegistry *r) {
    return betl_registry_register(r, &xr_provider, "<builtin:xlsx-read>");
}
