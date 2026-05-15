/* xml.read — SOURCE that walks an XML file via libxml2 + XPath. SSIS
 * XML Source parity.
 *
 * Config:
 *   path        string, required
 *   row_xpath   string, required — XPath selecting the row nodes,
 *                                  e.g. /catalog/book
 *   columns:    map<string,string>, required — output column name →
 *                                              XPath evaluated relative
 *                                              to each row node.
 *                                              Use 'title' for an
 *                                              element's content,
 *                                              '@isbn' for an attribute,
 *                                              'price/text()' to be
 *                                              explicit.
 *   batch_size  int, optional, default 1024
 *
 * All output columns are Arrow utf8. Coerce to int/decimal/date via
 * `map` downstream when needed.
 *
 * v0.1 loads the whole document in memory (libxml2 tree API). For
 * very large XML (10s of MB+) the SAX path would be better; deferred. */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/transforms_internal.h"
#include "runtime/xml_read.h"

typedef struct {
    char *name;       /* output column name */
    char *xpath;      /* relative XPath */
    xmlXPathCompExprPtr compiled;
} XmlBinding;

typedef struct {
    BetlContext *ctx;

    char        *path;
    char        *row_xpath;
    size_t       batch_size;

    XmlBinding  *cols;
    size_t       n_cols;

    xmlDocPtr           doc;
    xmlXPathContextPtr  xpctx;
    xmlNodeSetPtr       row_nodes;
    xmlXPathObjectPtr   row_xobj;        /* owns row_nodes */
    int                 opened;
    size_t              cursor;          /* next row index to emit */

    char         last_err[400];
} XmState;

static void xmset_err(XmState *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_err, sizeof s->last_err, fmt, ap);
    va_end(ap);
    betl_set_error(s->ctx, "%s", s->last_err);
}

/* ============================================================== *
 *  Config: parse the `columns:` map                                *
 * ============================================================== */

typedef struct { XmState *s; int err; } ColCtx;

static int col_visit(const char *key, const char *value, size_t value_len,
                     void *user) {
    ColCtx *c = user;
    XmState *s = c->s;
    if (value_len == 0 || value[0] != '"') {
        xmset_err(s, "xml.read: column '%s' value must be an XPath string", key);
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';
    char *xpath = NULL;
    if (betl_tx_json_decode_str(vbuf, &xpath) != 0 || !xpath) {
        free(vbuf);
        xmset_err(s, "xml.read: column '%s' XPath must be a JSON string", key);
        c->err = 1; return -1;
    }
    free(vbuf);

    XmlBinding *grow = realloc(s->cols, (s->n_cols + 1) * sizeof *grow);
    if (!grow) { free(xpath); c->err = 1; return -1; }
    s->cols = grow;
    XmlBinding *b = &s->cols[s->n_cols++];
    memset(b, 0, sizeof *b);
    b->name  = strdup(key);
    b->xpath = xpath;
    if (!b->name) { c->err = 1; return -1; }
    return 0;
}

static int parse_columns(XmState *s, const char *cfg) {
    const char *p = betl_tx_json_value_after(cfg, "columns");
    if (!p || *p != '{') {
        xmset_err(s, "xml.read: missing required `columns:` mapping");
        return -1;
    }
    ColCtx c = { .s = s, .err = 0 };
    if (betl_tx_json_walk_object(p, col_visit, &c) != 0 || c.err) return -1;
    if (s->n_cols == 0) {
        xmset_err(s, "xml.read: `columns:` is empty");
        return -1;
    }
    return 0;
}

/* ============================================================== *
 *  Lifecycle                                                       *
 * ============================================================== */

static int xm_init(BetlContext *ctx, const char *cfg, void **state) {
    XmState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    s->batch_size = 1024;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "path", &s->path) != 0 || !s->path) {
        xmset_err(s, "xml.read: missing required `path`");
        free(s); return BETL_ERR_INVALID;
    }
    if (betl_tx_json_string_at(cfg, "row_xpath", &s->row_xpath) != 0
        || !s->row_xpath) {
        xmset_err(s, "xml.read: missing required `row_xpath`");
        free(s->path); free(s);
        return BETL_ERR_INVALID;
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
    if (parse_columns(s, cfg) != 0) {
        free(s->path); free(s->row_xpath); free(s->cols); free(s);
        return BETL_ERR_INVALID;
    }

    *state = s;
    return BETL_OK;
}

static void xm_destroy(void *state) {
    if (!state) return;
    XmState *s = state;
    for (size_t i = 0; i < s->n_cols; ++i) {
        free(s->cols[i].name);
        free(s->cols[i].xpath);
        if (s->cols[i].compiled) xmlXPathFreeCompExpr(s->cols[i].compiled);
    }
    free(s->cols);
    if (s->row_xobj) xmlXPathFreeObject(s->row_xobj);
    if (s->xpctx)    xmlXPathFreeContext(s->xpctx);
    if (s->doc)      xmlFreeDoc(s->doc);
    free(s->row_xpath);
    free(s->path);
    free(s);
}

static int xm_open(XmState *s) {
    if (s->opened) return BETL_OK;
    s->doc = xmlReadFile(s->path, NULL, XML_PARSE_NOBLANKS | XML_PARSE_NOCDATA);
    if (!s->doc) {
        xmset_err(s, "xml.read: failed to parse '%s'", s->path);
        return BETL_ERR_IO;
    }
    s->xpctx = xmlXPathNewContext(s->doc);
    if (!s->xpctx) {
        xmset_err(s, "xml.read: xmlXPathNewContext failed");
        return BETL_ERR_INTERNAL;
    }
    s->row_xobj = xmlXPathEvalExpression((const xmlChar *)s->row_xpath,
                                         s->xpctx);
    if (!s->row_xobj
        || s->row_xobj->type != XPATH_NODESET
        || !s->row_xobj->nodesetval)
    {
        xmset_err(s, "xml.read: row_xpath '%s' did not produce a node-set",
                  s->row_xpath);
        return BETL_ERR_INVALID;
    }
    s->row_nodes = s->row_xobj->nodesetval;

    /* Pre-compile per-column XPaths so each cell evaluation is fast. */
    for (size_t i = 0; i < s->n_cols; ++i) {
        s->cols[i].compiled = xmlXPathCompile((const xmlChar *)s->cols[i].xpath);
        if (!s->cols[i].compiled) {
            xmset_err(s, "xml.read: failed to compile XPath '%s' for column '%s'",
                      s->cols[i].xpath, s->cols[i].name);
            return BETL_ERR_INVALID;
        }
    }
    s->opened = 1;
    return BETL_OK;
}

/* ============================================================== *
 *  Per-cell XPath evaluation → string                              *
 * ============================================================== */

/* Returns a malloc'd UTF-8 string with the cell value, or NULL if the
 * cell is null/missing. *err_out is set if encode fails. */
static char *eval_cell(XmState *s, xmlNodePtr ctx_node,
                       XmlBinding *b, int *err_out) {
    *err_out = 0;
    s->xpctx->node = ctx_node;
    xmlXPathObjectPtr o = xmlXPathCompiledEval(b->compiled, s->xpctx);
    if (!o) {
        *err_out = 1;
        return NULL;
    }
    char *result = NULL;
    switch (o->type) {
    case XPATH_NODESET: {
        xmlNodeSetPtr ns = o->nodesetval;
        if (ns && ns->nodeNr > 0) {
            xmlChar *t = xmlNodeListGetString(s->doc,
                                              ns->nodeTab[0]->children, 1);
            if (!t) {
                /* No text children — could be an attribute or empty
                 * element. Try xmlNodeGetContent which handles both. */
                t = xmlNodeGetContent(ns->nodeTab[0]);
            }
            if (t) {
                result = strdup((const char *)t);
                xmlFree(t);
                if (!result) *err_out = 1;
            }
        }
        break;
    }
    case XPATH_STRING:
        if (o->stringval) {
            result = strdup((const char *)o->stringval);
            if (!result) *err_out = 1;
        }
        break;
    case XPATH_NUMBER: {
        char buf[40];
        int n = snprintf(buf, sizeof buf, "%.17g", o->floatval);
        if (n > 0 && (size_t)n < sizeof buf) {
            result = malloc((size_t)n + 1);
            if (result) memcpy(result, buf, (size_t)n + 1);
            else *err_out = 1;
        } else *err_out = 1;
        break;
    }
    case XPATH_BOOLEAN:
        result = strdup(o->boolval ? "true" : "false");
        if (!result) *err_out = 1;
        break;
    default:
        break;
    }
    xmlXPathFreeObject(o);
    return result;
}

/* Same utf8-leaf builder as xlsx.read uses — kept local for clarity. */
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

/* ============================================================== *
 *  Stream                                                          *
 * ============================================================== */

static int xm_get_schema(struct ArrowArrayStream *st, struct ArrowSchema *out) {
    XmState *s = st->private_data;
    if (!s) return EINVAL;
    if (xm_open(s) != BETL_OK) return EIO;
    memset(out, 0, sizeof *out);

    struct ArrowSchema **kids = calloc(s->n_cols, sizeof *kids);
    if (!kids) return ENOMEM;
    for (size_t i = 0; i < s->n_cols; ++i) {
        kids[i] = betl_tx_new_leaf_schema(s->cols[i].name, "u");
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

static int xm_get_next(struct ArrowArrayStream *st, struct ArrowArray *out) {
    XmState *s = st->private_data;
    memset(out, 0, sizeof *out);
    if (!s) return EINVAL;
    if (xm_open(s) != BETL_OK) return EIO;

    int total_rows = s->row_nodes ? s->row_nodes->nodeNr : 0;
    if ((int)s->cursor >= total_rows) return 0;

    size_t take = s->batch_size;
    if ((int)(s->cursor + take) > total_rows) {
        take = (size_t)total_rows - s->cursor;
    }

    char ***rows = calloc(s->n_cols, sizeof *rows);
    if (!rows) { xmset_err(s, "xml.read: out of memory"); return EIO; }
    for (size_t c = 0; c < s->n_cols; ++c) {
        rows[c] = calloc(take, sizeof **rows);
        if (!rows[c]) {
            for (size_t k = 0; k < c; ++k) free(rows[k]);
            free(rows);
            xmset_err(s, "xml.read: out of memory");
            return EIO;
        }
    }

    for (size_t r = 0; r < take; ++r) {
        if (betl_should_cancel(s->ctx)) {
            for (size_t c = 0; c < s->n_cols; ++c) {
                for (size_t k = 0; k < r; ++k) free(rows[c][k]);
                free(rows[c]);
            }
            free(rows);
            xmset_err(s, "xml.read: cancelled");
            return EIO;
        }
        xmlNodePtr row_node = s->row_nodes->nodeTab[s->cursor + r];
        for (size_t c = 0; c < s->n_cols; ++c) {
            int enc_err = 0;
            rows[c][r] = eval_cell(s, row_node, &s->cols[c], &enc_err);
            if (enc_err) {
                xmset_err(s, "xml.read: XPath eval failed for column '%s' at row %zu",
                          s->cols[c].name, s->cursor + r);
                for (size_t cc = 0; cc < s->n_cols; ++cc) {
                    for (size_t k = 0; k <= r; ++k) free(rows[cc][k]);
                    free(rows[cc]);
                }
                free(rows);
                return EIO;
            }
        }
    }
    s->cursor += take;

    struct ArrowArray **kids = calloc(s->n_cols, sizeof *kids);
    if (!kids) {
        for (size_t c = 0; c < s->n_cols; ++c) {
            for (size_t r = 0; r < take; ++r) free(rows[c][r]);
            free(rows[c]);
        }
        free(rows);
        xmset_err(s, "xml.read: out of memory");
        return EIO;
    }
    for (size_t c = 0; c < s->n_cols; ++c) {
        kids[c] = calloc(1, sizeof **kids);
        if (!kids[c] || build_utf8_leaf(kids[c], rows[c], take) != 0) {
            free(kids[c]);
            for (size_t k = 0; k < c; ++k) {
                if (kids[k]->release) kids[k]->release(kids[k]);
                free(kids[k]);
            }
            free(kids);
            for (size_t cc = 0; cc < s->n_cols; ++cc) {
                for (size_t r = 0; r < take; ++r) free(rows[cc][r]);
                free(rows[cc]);
            }
            free(rows);
            xmset_err(s, "xml.read: out of memory");
            return EIO;
        }
    }
    for (size_t c = 0; c < s->n_cols; ++c) {
        for (size_t r = 0; r < take; ++r) free(rows[c][r]);
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
        xmset_err(s, "xml.read: out of memory");
        return EIO;
    }
    outer[0] = NULL;
    out->length     = (int64_t)take;
    out->null_count = 0;
    out->offset     = 0;
    out->n_buffers  = 1;
    out->n_children = (int64_t)s->n_cols;
    out->buffers    = outer;
    out->children   = kids;
    out->release    = betl_tx_release_struct;
    return 0;
}

static const char *xm_get_last_error(struct ArrowArrayStream *st) {
    XmState *s = st->private_data;
    return (s && s->last_err[0]) ? s->last_err : NULL;
}

static void xm_release(struct ArrowArrayStream *st) {
    st->private_data = NULL;
    st->release      = NULL;
}

static int xm_attach_output(void *state, int port,
                            struct ArrowArrayStream *out) {
    (void)port;
    out->get_schema     = xm_get_schema;
    out->get_next       = xm_get_next;
    out->get_last_error = xm_get_last_error;
    out->release        = xm_release;
    out->private_data   = state;
    return BETL_OK;
}

/* ============================================================== *
 *  Component definition                                            *
 * ============================================================== */

static const BetlPortDef xm_outputs[] = {
    { .name = "out", .schema_mode = BETL_SCHEMA_DYNAMIC,
      .doc = "rows from XML" },
};

static const BetlComponentDef xm_components[] = {
    { .name               = "xml.read",
      .kind               = BETL_KIND_SOURCE,
      .config_schema_json = "{}",
      .flags              = 0,
      .outputs            = xm_outputs,
      .output_count       = 1,
      .init               = xm_init,
      .destroy            = xm_destroy,
      .attach_output      = xm_attach_output },
};

static const BetlProvider xm_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-xml-read",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = xm_components,
    .component_count = sizeof xm_components / sizeof xm_components[0],
};

int betl_register_xml_read(BetlRegistry *r) {
    return betl_registry_register(r, &xm_provider, "<builtin:xml-read>");
}
