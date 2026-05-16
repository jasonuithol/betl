/* json.read — SOURCE that reads NDJSON (one object per line) or a
 * JSON array of objects, and emits one Arrow row per object with all
 * columns rendered as utf8.
 *
 * Config:
 *   path        string, required
 *   format      "ndjson" (default) | "array"
 *   columns     list-of-string, required — output column names. Each
 *               row is the object's value for that key; missing keys
 *               become NULL.
 *   batch_size  int, optional, default 1024
 *
 * Per-cell rendering:
 *   null   → Arrow null bit
 *   string → bytes as-is
 *   number → minimal "%.17g" (preserves int round-trip up to 2^53)
 *   bool   → "true"/"false"
 *   object/array → cJSON_PrintUnformatted (lossless re-serialize)
 *
 * NDJSON is read line-by-line so even huge files work without
 * loading them into memory. Array format slurps the whole file
 * (cJSON has no streaming parser); fine for ≤ 100 MB.
 */

#include "runtime/json_read.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

#include "betl/provider.h"
#include "runtime/transforms_internal.h"

typedef enum {
    JR_FORMAT_NDJSON = 0,
    JR_FORMAT_ARRAY  = 1
} JrFormat;

typedef struct {
    BetlContext *ctx;

    char       *path;
    JrFormat    format;
    char      **col_names;
    size_t      n_cols;
    size_t      batch_size;

    /* NDJSON: line-by-line file pointer; line scratch grows. */
    FILE       *fp;
    char       *line_buf;
    size_t      line_cap;
    size_t      line_no;          /* 1-based, for error messages */
    int         eof;

    /* Array: parsed root + cursor. */
    cJSON      *root;             /* owns its tree */
    int         array_cursor;     /* index of next element */

    char        last_err[400];
} JrState;

static void jrset_err(JrState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* ============================================================== *
 *  Config                                                          *
 * ============================================================== */

typedef struct { JrState *s; int err; } NameCtx;

static int name_visit(const char *value, size_t value_len, void *user) {
    NameCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        jrset_err(c->s, "json.read: `columns:` entries must be strings");
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
    char **grow = realloc(c->s->col_names,
                          (c->s->n_cols + 1) * sizeof *grow);
    if (!grow) { free(name); c->err = 1; return -1; }
    c->s->col_names = grow;
    c->s->col_names[c->s->n_cols++] = name;
    return 0;
}

static int jr_init(BetlContext *ctx, const char *cfg, void **state) {
    JrState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx        = ctx;
    s->batch_size = 1024;
    s->format     = JR_FORMAT_NDJSON;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "path", &s->path) != 0 || !s->path) {
        jrset_err(s, "json.read: missing required `path`");
        free(s); return BETL_ERR_INVALID;
    }

    char *fmt = NULL;
    if (betl_tx_json_string_at(cfg, "format", &fmt) == 0 && fmt) {
        if (strcmp(fmt, "ndjson") == 0) {
            s->format = JR_FORMAT_NDJSON;
        } else if (strcmp(fmt, "array") == 0) {
            s->format = JR_FORMAT_ARRAY;
        } else {
            jrset_err(s, "json.read: `format:` must be \"ndjson\" or "
                         "\"array\" (got '%s')", fmt);
            free(fmt); free(s->path); free(s);
            return BETL_ERR_INVALID;
        }
        free(fmt);
    }

    const char *cols_at = betl_tx_json_value_after(cfg, "columns");
    if (!cols_at || *cols_at != '[') {
        jrset_err(s, "json.read: missing required `columns:` list");
        free(s->path); free(s);
        return BETL_ERR_INVALID;
    }
    NameCtx nc = { .s = s, .err = 0 };
    if (betl_tx_json_walk_array(cols_at, name_visit, &nc) != 0 || nc.err) {
        for (size_t i = 0; i < s->n_cols; ++i) free(s->col_names[i]);
        free(s->col_names);
        free(s->path); free(s);
        return BETL_ERR_INVALID;
    }
    if (s->n_cols == 0) {
        jrset_err(s, "json.read: `columns:` must contain at least one name");
        free(s->path); free(s);
        return BETL_ERR_INVALID;
    }

    int64_t bs;
    if (betl_tx_json_value_after(cfg, "batch_size")) {
        const char *p = betl_tx_json_value_after(cfg, "batch_size");
        char *end = NULL;
        long long v = strtoll(p, &end, 10);
        if (end != p && v > 0 && v <= 1024 * 1024) {
            s->batch_size = (size_t)v;
        }
        (void)bs;
    }

    *state = s;
    return BETL_OK;
}

static void jr_destroy(void *state) {
    if (!state) return;
    JrState *s = state;
    if (s->fp)        fclose(s->fp);
    if (s->root)      cJSON_Delete(s->root);
    free(s->line_buf);
    for (size_t i = 0; i < s->n_cols; ++i) free(s->col_names[i]);
    free(s->col_names);
    free(s->path);
    free(s);
}

/* ============================================================== *
 *  Open (deferred until first schema/get_next call)                *
 * ============================================================== */

static int jr_open(JrState *s) {
    if (s->fp || s->root) return BETL_OK;
    if (s->format == JR_FORMAT_NDJSON) {
        s->fp = fopen(s->path, "r");
        if (!s->fp) {
            jrset_err(s, "json.read: open(%s): %s", s->path, strerror(errno));
            return BETL_ERR_IO;
        }
        return BETL_OK;
    }
    /* Array form: slurp the whole file then parse. */
    FILE *f = fopen(s->path, "rb");
    if (!f) {
        jrset_err(s, "json.read: open(%s): %s", s->path, strerror(errno));
        return BETL_ERR_IO;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        jrset_err(s, "json.read: ftell(%s) failed", s->path);
        return BETL_ERR_IO;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); jrset_err(s, "json.read: out of memory"); return BETL_ERR_INTERNAL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    s->root = cJSON_Parse(buf);
    free(buf);
    if (!s->root) {
        const char *near = cJSON_GetErrorPtr();
        jrset_err(s, "json.read: parse failed near %.32s",
                  near ? near : "(start of file)");
        return BETL_ERR_INVALID;
    }
    if (!cJSON_IsArray(s->root)) {
        jrset_err(s, "json.read: format=array requires the document root "
                     "to be a JSON array");
        return BETL_ERR_INVALID;
    }
    return BETL_OK;
}

/* Read one logical line into s->line_buf. Returns the number of bytes
 * in the line (without trailing newline), or -1 at EOF, -2 on IO/OOM. */
static long jr_read_line(JrState *s) {
    if (s->eof) return -1;
    size_t n = 0;
    int saw = 0;
    for (;;) {
        int c = fgetc(s->fp);
        if (c == EOF) {
            s->eof = 1;
            if (!saw) return -1;
            break;
        }
        saw = 1;
        if (c == '\n') break;
        if (n + 2 >= s->line_cap) {
            size_t nc = s->line_cap ? s->line_cap * 2 : 256;
            char *p = realloc(s->line_buf, nc);
            if (!p) return -2;
            s->line_buf = p;
            s->line_cap = nc;
        }
        s->line_buf[n++] = (char)c;
    }
    if (n + 1 >= s->line_cap) {
        size_t nc = s->line_cap ? s->line_cap * 2 : 256;
        char *p = realloc(s->line_buf, nc);
        if (!p) return -2;
        s->line_buf = p;
        s->line_cap = nc;
    }
    s->line_buf[n] = '\0';
    return (long)n;
}

/* Get the next JSON object — either the next non-blank NDJSON line or
 * the next array element. Returns NULL at end-of-stream (without
 * setting error), or sets *err on failure. Caller must cJSON_Delete
 * NDJSON returns; array returns are owned by s->root and must NOT
 * be freed (sets *owned=0). */
static cJSON *jr_next_object(JrState *s, int *owned, int *err) {
    *err = 0;
    if (s->format == JR_FORMAT_NDJSON) {
        for (;;) {
            long L = jr_read_line(s);
            if (L == -1) return NULL;
            if (L == -2) {
                jrset_err(s, "json.read: IO error / OOM reading line");
                *err = 1; return NULL;
            }
            s->line_no++;
            /* skip blank / whitespace-only lines */
            char *p = s->line_buf;
            while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
            if (*p == '\0') continue;
            cJSON *obj = cJSON_Parse(s->line_buf);
            if (!obj) {
                jrset_err(s, "json.read: parse error on line %zu",
                          s->line_no);
                *err = 1; return NULL;
            }
            if (!cJSON_IsObject(obj)) {
                jrset_err(s, "json.read: NDJSON line %zu is not a JSON object",
                          s->line_no);
                cJSON_Delete(obj);
                *err = 1; return NULL;
            }
            *owned = 1;
            return obj;
        }
    }
    /* array */
    int total = cJSON_GetArraySize(s->root);
    if (s->array_cursor >= total) return NULL;
    cJSON *el = cJSON_GetArrayItem(s->root, s->array_cursor++);
    if (!cJSON_IsObject(el)) {
        jrset_err(s,
            "json.read: array element %d is not a JSON object",
            s->array_cursor - 1);
        *err = 1; return NULL;
    }
    *owned = 0;
    return el;
}

/* Render one cJSON cell to a malloc'd UTF-8 string (or return NULL for
 * SQL NULL). Returns NULL with *err=1 on OOM / encoding failure. */
static char *jr_render_cell(const cJSON *cell, int *err) {
    *err = 0;
    if (!cell || cJSON_IsNull(cell)) return NULL;
    if (cJSON_IsString(cell)) {
        const char *s = cJSON_GetStringValue(cell);
        if (!s) return NULL;
        return strdup(s);
    }
    if (cJSON_IsBool(cell)) {
        return strdup(cJSON_IsTrue(cell) ? "true" : "false");
    }
    if (cJSON_IsNumber(cell)) {
        double d = cell->valuedouble;
        char buf[40];
        /* Integer round-trip: if the number reads as a whole integer
         * within int64 range, render without decimal point. Otherwise
         * use %.17g for full double precision.
         *
         * The range check MUST come before the `(int64_t)d` cast and
         * comparison: casting a double outside INT64 range to int64
         * is undefined behaviour (UBSan: "X is outside the range of
         * representable values of type 'long'"). Found by fuzzing
         * json.read with double values like 6.22e+40. The 2^53 bound
         * (9007199254740992) is also where doubles stop being able
         * to represent consecutive integers, so anything past it
         * gets the %g treatment regardless. */
        if (d >= -9007199254740992.0
            && d <=  9007199254740992.0
            && d == (double)(int64_t)d) {
            snprintf(buf, sizeof buf, "%" PRId64, (int64_t)d);
        } else {
            snprintf(buf, sizeof buf, "%.17g", d);
        }
        return strdup(buf);
    }
    /* object / array → re-serialize losslessly */
    char *s = cJSON_PrintUnformatted(cell);
    if (!s) { *err = 1; return NULL; }
    return s;
}

/* ============================================================== *
 *  Stream                                                          *
 * ============================================================== */

static int jr_get_schema(struct ArrowArrayStream *st,
                         struct ArrowSchema *out) {
    JrState *s = st->private_data;
    if (!s) return EINVAL;
    if (jr_open(s) != BETL_OK) return EIO;
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

/* Build a utf8 leaf from a heap-owned cells[] array of strdup'd C
 * strings (NULL means SQL NULL). Frees the cell strings on success
 * and on failure. */
static int build_utf8_leaf(struct ArrowArray *out, char **cells, size_t n_rows) {
    int32_t *offs   = malloc((n_rows + 1) * sizeof *offs);
    size_t   nbytes = 0;
    int64_t  null_count = 0;
    for (size_t i = 0; i < n_rows; ++i) {
        if (cells[i]) nbytes += strlen(cells[i]);
        else ++null_count;
    }
    char *data = malloc(nbytes ? nbytes : 1);
    uint8_t *vmap = NULL;
    if (null_count > 0) {
        size_t vmap_bytes = (n_rows + 7) / 8;
        vmap = malloc(vmap_bytes);
        if (vmap) memset(vmap, 0xff, vmap_bytes);
    }
    if (!offs || !data || (null_count > 0 && !vmap)) {
        free(offs); free(data); free(vmap);
        for (size_t i = 0; i < n_rows; ++i) free(cells[i]);
        return -1;
    }
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
    /* Free the source cell strings — their content has been copied. */
    for (size_t i = 0; i < n_rows; ++i) free(cells[i]);

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

static int jr_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    JrState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (jr_open(s) != BETL_OK) return EIO;

    /* Stage one batch of up to batch_size rows. cells[col][row] is the
     * rendered cell (or NULL for SQL NULL). */
    char ***cells = calloc(s->n_cols, sizeof *cells);
    if (!cells) { jrset_err(s, "json.read: out of memory"); return EIO; }
    for (size_t c = 0; c < s->n_cols; ++c) {
        cells[c] = calloc(s->batch_size, sizeof **cells);
        if (!cells[c]) {
            for (size_t k = 0; k < c; ++k) free(cells[k]);
            free(cells);
            jrset_err(s, "json.read: out of memory");
            return EIO;
        }
    }

    size_t n_rows = 0;
    while (n_rows < s->batch_size) {
        if (betl_should_cancel(s->ctx)) {
            for (size_t c = 0; c < s->n_cols; ++c) {
                for (size_t k = 0; k < n_rows; ++k) free(cells[c][k]);
                free(cells[c]);
            }
            free(cells);
            jrset_err(s, "json.read: cancelled");
            return EIO;
        }
        int owned = 0, perr = 0;
        cJSON *obj = jr_next_object(s, &owned, &perr);
        if (perr) {
            for (size_t c = 0; c < s->n_cols; ++c) {
                for (size_t k = 0; k < n_rows; ++k) free(cells[c][k]);
                free(cells[c]);
            }
            free(cells);
            return EIO;
        }
        if (!obj) break;          /* end of stream */
        for (size_t c = 0; c < s->n_cols; ++c) {
            const cJSON *cell = cJSON_GetObjectItemCaseSensitive(
                obj, s->col_names[c]);
            int rerr = 0;
            cells[c][n_rows] = jr_render_cell(cell, &rerr);
            if (rerr) {
                if (owned) cJSON_Delete(obj);
                for (size_t cc = 0; cc < s->n_cols; ++cc) {
                    for (size_t k = 0; k <= n_rows; ++k)
                        free(cells[cc][k]);
                    free(cells[cc]);
                }
                free(cells);
                jrset_err(s, "json.read: cell render failed at row %zu",
                          n_rows);
                return EIO;
            }
        }
        if (owned) cJSON_Delete(obj);
        ++n_rows;
    }

    if (n_rows == 0) {
        for (size_t c = 0; c < s->n_cols; ++c) free(cells[c]);
        free(cells);
        return 0;                  /* end-of-stream */
    }

    struct ArrowArray **leaves = calloc(s->n_cols, sizeof *leaves);
    if (!leaves) {
        for (size_t c = 0; c < s->n_cols; ++c) {
            for (size_t k = 0; k < n_rows; ++k) free(cells[c][k]);
            free(cells[c]);
        }
        free(cells);
        return ENOMEM;
    }
    for (size_t c = 0; c < s->n_cols; ++c) {
        struct ArrowArray *leaf = calloc(1, sizeof *leaf);
        if (!leaf || build_utf8_leaf(leaf, cells[c], n_rows) != 0) {
            free(leaf);
            for (size_t cc = c + 1; cc < s->n_cols; ++cc) {
                for (size_t k = 0; k < n_rows; ++k) free(cells[cc][k]);
                free(cells[cc]);
            }
            for (size_t k = 0; k < c; ++k) {
                if (leaves[k] && leaves[k]->release) leaves[k]->release(leaves[k]);
                free(leaves[k]);
            }
            free(leaves);
            free(cells);
            jrset_err(s, "json.read: utf8 leaf build failed");
            return ENOMEM;
        }
        leaves[c] = leaf;
        free(cells[c]);
    }
    free(cells);

    const void **rootbufs = malloc(sizeof *rootbufs);
    if (!rootbufs) {
        for (size_t c = 0; c < s->n_cols; ++c) {
            if (leaves[c] && leaves[c]->release) leaves[c]->release(leaves[c]);
            free(leaves[c]);
        }
        free(leaves);
        return ENOMEM;
    }
    rootbufs[0] = NULL;
    out->length     = (int64_t)n_rows;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)s->n_cols;
    out->buffers    = rootbufs;
    out->children   = leaves;
    out->release    = betl_tx_release_struct;
    return 0;
}

static const char *jr_get_last_error(struct ArrowArrayStream *st) {
    JrState *s = st->private_data;
    return s ? s->last_err : NULL;
}

static void jr_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int jr_attach_output(void *state, int port,
                            struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = jr_get_schema;
    out->get_next       = jr_get_next;
    out->get_last_error = jr_get_last_error;
    out->release        = jr_release;
    out->private_data   = state;
    return BETL_OK;
}

static const BetlPortDef jr_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DERIVED,
      .doc = "one row per JSON object; columns as declared" },
};

static const BetlComponentDef jr_components[] = {
    { .name               = "json.read",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = 0,
      .outputs            = jr_outputs,
      .output_count       = 1,
      .init               = jr_init,
      .destroy            = jr_destroy,
      .attach_output      = jr_attach_output },
};

static const BetlProvider jr_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-json-read",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = jr_components,
    .component_count = sizeof jr_components / sizeof jr_components[0],
};

int betl_register_json_read(BetlRegistry *r) {
    return betl_registry_register(r, &jr_provider, "<builtin:json-read>");
}
