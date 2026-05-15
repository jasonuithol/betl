/* http.get / http.post — TASK components built on libcurl's easy
 * interface. Response body lands at a local file path so the next
 * stage can pick it up with json.read / csv.read / xml.read.
 *
 * http.get   { url:, save_to:, headers: [...], timeout: "30s" }
 * http.post  { url:, save_to:, body:, headers: [...], timeout: "30s" }
 *
 * `headers:` is an optional list of "Name: value" strings.
 * `body:` is a literal request body; `body_file:` (alternative)
 * reads the body from a local path. `timeout:` parses as seconds
 * (e.g. "30" / "30s" / "2m"), capped at 1h.
 *
 * Failure mode: any HTTP status outside 2xx fails the task with the
 * status code and the first 200 bytes of the response body in the
 * error message.
 */

#include "runtime/http_ops.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "betl/provider.h"
#include "runtime/transforms_internal.h"

typedef struct {
    BetlContext *ctx;
    char        *url;
    char        *save_to;
    char        *body;            /* may be NULL (http.get or empty body) */
    char        *body_file;       /* alternative to body (POST only) */
    char       **headers;
    size_t       n_headers;
    long         timeout_s;       /* 0 => no client-side timeout */
    int          is_post;
} HoState;

/* ---- Config parsing ---- */

typedef struct { HoState *s; int err; } HdrCtx;

static int header_visit(const char *value, size_t value_len, void *user) {
    HdrCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        betl_set_error(c->s->ctx,
            "%s: `headers:` entries must be \"Name: value\" strings",
            c->s->is_post ? "http.post" : "http.get");
        c->err = 1; return -1;
    }
    char *vbuf = malloc(value_len + 1);
    if (!vbuf) { c->err = 1; return -1; }
    memcpy(vbuf, value, value_len);
    vbuf[value_len] = '\0';
    char *decoded = NULL;
    if (betl_tx_json_decode_str(vbuf, &decoded) != 0 || !decoded) {
        free(vbuf); c->err = 1; return -1;
    }
    free(vbuf);
    char **grow = realloc(c->s->headers,
                          (c->s->n_headers + 1) * sizeof *grow);
    if (!grow) { free(decoded); c->err = 1; return -1; }
    c->s->headers = grow;
    c->s->headers[c->s->n_headers++] = decoded;
    return 0;
}

/* Same shape as shell.c's parse_timeout: bare number is seconds, "Ns"
 * is seconds, "Nm" is minutes. Cap at 1h. */
static int parse_timeout(const char *raw, long *out_s) {
    char *end = NULL;
    long v = strtol(raw, &end, 10);
    if (end == raw || v < 0) return -1;
    int mul = 1;
    if (*end == 's' || *end == 'S')      { mul = 1;  ++end; }
    else if (*end == 'm' || *end == 'M') { mul = 60; ++end; }
    else if (*end != '\0')               { return -1; }
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return -1;
    long long secs = (long long)v * mul;
    if (secs > 3600) return -1;
    *out_s = (long)secs;
    return 0;
}

static int ho_init_common(BetlContext *ctx, const char *cfg,
                          int is_post, const char *tag, void **state) {
    HoState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx     = ctx;
    s->is_post = is_post;
    cfg = cfg ? cfg : "{}";

    if (betl_tx_json_string_at(cfg, "url", &s->url) != 0 || !s->url) {
        betl_set_error(ctx, "%s: missing required `url`", tag);
        free(s); return BETL_ERR_INVALID;
    }
    if (betl_tx_json_string_at(cfg, "save_to", &s->save_to) != 0
        || !s->save_to) {
        betl_set_error(ctx, "%s: missing required `save_to` (path to write the "
                            "response body)", tag);
        free(s->url); free(s);
        return BETL_ERR_INVALID;
    }

    if (is_post) {
        char *body_lit = NULL, *body_path = NULL;
        (void)betl_tx_json_string_at(cfg, "body", &body_lit);
        (void)betl_tx_json_string_at(cfg, "body_file", &body_path);
        if (body_lit && body_path) {
            betl_set_error(ctx, "%s: specify either `body:` or `body_file:`, "
                                "not both", tag);
            free(body_lit); free(body_path);
            free(s->url); free(s->save_to); free(s);
            return BETL_ERR_INVALID;
        }
        s->body      = body_lit;
        s->body_file = body_path;
    }

    const char *hp = betl_tx_json_value_after(cfg, "headers");
    if (hp && *hp == '[') {
        HdrCtx hc = { .s = s, .err = 0 };
        if (betl_tx_json_walk_array(hp, header_visit, &hc) != 0 || hc.err) {
            for (size_t i = 0; i < s->n_headers; ++i) free(s->headers[i]);
            free(s->headers);
            free(s->body); free(s->body_file);
            free(s->url); free(s->save_to); free(s);
            return BETL_ERR_INVALID;
        }
    }

    char *tout = NULL;
    if (betl_tx_json_string_at(cfg, "timeout", &tout) == 0 && tout) {
        if (parse_timeout(tout, &s->timeout_s) != 0) {
            betl_set_error(ctx,
                "%s: invalid `timeout: %s` (expected e.g. \"30s\", \"2m\")",
                tag, tout);
            free(tout);
            for (size_t i = 0; i < s->n_headers; ++i) free(s->headers[i]);
            free(s->headers);
            free(s->body); free(s->body_file);
            free(s->url); free(s->save_to); free(s);
            return BETL_ERR_INVALID;
        }
        free(tout);
    }

    *state = s;
    return BETL_OK;
}

static int hg_init(BetlContext *ctx, const char *cfg, void **state) {
    return ho_init_common(ctx, cfg, 0, "http.get", state);
}
static int hp_init(BetlContext *ctx, const char *cfg, void **state) {
    return ho_init_common(ctx, cfg, 1, "http.post", state);
}

static void ho_destroy(void *state) {
    if (!state) return;
    HoState *s = state;
    free(s->url);
    free(s->save_to);
    free(s->body);
    free(s->body_file);
    for (size_t i = 0; i < s->n_headers; ++i) free(s->headers[i]);
    free(s->headers);
    free(s);
}

/* ---- libcurl glue ---- */

typedef struct {
    FILE  *fp;
    char  *peek;       /* first ~200 bytes for error messages */
    size_t peek_len;
} WriteCtx;

#define HTTP_PEEK_CAP 200

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    WriteCtx *w = ud;
    size_t n = size * nmemb;
    if (w->fp && n) {
        size_t wn = fwrite(ptr, 1, n, w->fp);
        if (wn != n) return wn;
    }
    if (w->peek && w->peek_len < HTTP_PEEK_CAP) {
        size_t take = HTTP_PEEK_CAP - w->peek_len;
        if (take > n) take = n;
        memcpy(w->peek + w->peek_len, ptr, take);
        w->peek_len += take;
    }
    return n;
}

static char *slurp_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static int ho_run(void *state) {
    HoState *s = state;
    CURL *c = curl_easy_init();
    if (!c) {
        betl_set_error(s->ctx, "%s: curl_easy_init failed",
                       s->is_post ? "http.post" : "http.get");
        return BETL_ERR_INTERNAL;
    }

    FILE *out_fp = fopen(s->save_to, "wb");
    if (!out_fp) {
        betl_set_error(s->ctx, "%s: open(%s): %s",
                       s->is_post ? "http.post" : "http.get",
                       s->save_to, strerror(errno));
        curl_easy_cleanup(c);
        return BETL_ERR_IO;
    }
    char peek[HTTP_PEEK_CAP + 1] = {0};
    WriteCtx wctx = { .fp = out_fp, .peek = peek, .peek_len = 0 };

    char *post_body = NULL;
    size_t post_len = 0;
    struct curl_slist *hdr_list = NULL;
    int rc_out = BETL_OK;

    curl_easy_setopt(c, CURLOPT_URL, s->url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &wctx);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 0L);  /* we check status manually */
    if (s->timeout_s > 0) {
        curl_easy_setopt(c, CURLOPT_TIMEOUT, s->timeout_s);
    }

    for (size_t i = 0; i < s->n_headers; ++i) {
        struct curl_slist *grow = curl_slist_append(hdr_list, s->headers[i]);
        if (!grow) {
            betl_set_error(s->ctx, "%s: curl_slist_append failed",
                           s->is_post ? "http.post" : "http.get");
            rc_out = BETL_ERR_INTERNAL;
            goto cleanup;
        }
        hdr_list = grow;
    }
    if (hdr_list) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr_list);

    if (s->is_post) {
        if (s->body_file) {
            post_body = slurp_file(s->body_file, &post_len);
            if (!post_body) {
                betl_set_error(s->ctx,
                    "http.post: failed to read body_file '%s': %s",
                    s->body_file, strerror(errno));
                rc_out = BETL_ERR_IO;
                goto cleanup;
            }
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_body);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)post_len);
        } else if (s->body) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, s->body);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(s->body));
        } else {
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
        }
    }

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        betl_set_error(s->ctx, "%s: %s: %s",
                       s->is_post ? "http.post" : "http.get",
                       s->url, curl_easy_strerror(rc));
        rc_out = BETL_ERR_IO;
        goto cleanup;
    }

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status < 200 || status >= 300) {
        peek[wctx.peek_len] = '\0';
        /* Strip non-printables for a clean error message. */
        for (size_t i = 0; i < wctx.peek_len; ++i) {
            if (peek[i] < 0x20 && peek[i] != '\n' && peek[i] != '\t')
                peek[i] = '?';
        }
        betl_set_error(s->ctx,
            "%s: %s: HTTP %ld%s%s",
            s->is_post ? "http.post" : "http.get", s->url, status,
            wctx.peek_len ? ": " : "",
            wctx.peek_len ? peek : "");
        rc_out = BETL_ERR_IO;
        goto cleanup;
    }

cleanup:
    if (out_fp) {
        fclose(out_fp);
        if (rc_out != BETL_OK) unlink(s->save_to);
    }
    if (hdr_list) curl_slist_free_all(hdr_list);
    free(post_body);
    curl_easy_cleanup(c);
    return rc_out;
}

/* ---- Component table ---- */

static const BetlComponentDef ho_components[] = {
    { .name               = "http.get",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = hg_init,
      .destroy            = ho_destroy,
      .task_run           = ho_run },
    { .name               = "http.post",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = hp_init,
      .destroy            = ho_destroy,
      .task_run           = ho_run },
};

static const BetlProvider ho_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-http-ops",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = ho_components,
    .component_count = sizeof ho_components / sizeof ho_components[0],
};

int betl_register_http_ops(BetlRegistry *r) {
    /* curl_global_init is fine to call repeatedly per curl docs (it's
     * refcounted, but we don't call curl_global_cleanup since the host
     * process exit handles teardown). */
    static int inited = 0;
    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        inited = 1;
    }
    return betl_registry_register(r, &ho_provider, "<builtin:http-ops>");
}
