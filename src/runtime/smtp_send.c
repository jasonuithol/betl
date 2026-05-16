/* smtp.send — TASK that delivers a plain-text email via SMTP. Built
 * on libcurl's SMTP support so the wire protocol, AUTH, and STARTTLS
 * negotiation are libcurl's problem.
 *
 *   url:        "smtps://smtp.example.com:465"  (TLS) or
 *               "smtp://smtp.example.com:25"     (cleartext / STARTTLS)
 *   username:   optional, used for AUTH PLAIN / LOGIN
 *   password:   optional (typically `${env.SMTP_PASS}`)
 *   from:       sender address (bare or "Name <addr@host>")
 *   to:         list of recipient addresses, required
 *   cc:         optional list
 *   subject:    subject line
 *   body:       message body (plain text). XOR with body_file.
 *   body_file:  local path; alternative to body
 *
 * v0.1 sends plain text only. MIME multipart / attachments / HTML
 * are deferred — they need a real MIME library or hand-rolled
 * boundary handling, both of which expand scope significantly.
 * Users who need attachments can shell out to mailx via the `shell`
 * task as a stopgap. */

#include "runtime/smtp_send.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

#include "betl/provider.h"
#include "runtime/transforms_internal.h"

typedef struct {
    BetlContext *ctx;
    char        *url;
    char        *username;
    char        *password;
    char        *from;
    char       **to;
    size_t       n_to;
    char       **cc;
    size_t       n_cc;
    char        *subject;
    char        *body;            /* literal */
    char        *body_file;       /* alternative */
} SmState;

/* ---- string-list parsing (mirrors http_ops.c) ---------------------- */

typedef struct { SmState *s; char ***out; size_t *n_out; int err; } AddrCtx;

static int addr_visit(const char *value, size_t value_len, void *user) {
    AddrCtx *c = user;
    if (value_len == 0 || value[0] != '"') {
        betl_set_error(c->s->ctx,
            "smtp.send: address list entries must be strings");
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
    char **grow = realloc(*c->out, (*c->n_out + 1) * sizeof *grow);
    if (!grow) { free(decoded); c->err = 1; return -1; }
    *c->out = grow;
    (*c->out)[(*c->n_out)++] = decoded;
    return 0;
}

static int parse_addr_list(SmState *s, const char *cfg, const char *key,
                           char ***out, size_t *n_out) {
    *out = NULL; *n_out = 0;
    const char *pos = betl_tx_json_value_after(cfg, key);
    if (!pos) return 0;
    if (*pos != '[') {
        betl_set_error(s->ctx, "smtp.send: `%s:` must be a list of strings", key);
        return -1;
    }
    AddrCtx c = { .s = s, .out = out, .n_out = n_out, .err = 0 };
    if (betl_tx_json_walk_array(pos, addr_visit, &c) != 0 || c.err) return -1;
    return 0;
}

/* ---- Lifecycle ---------------------------------------------------- */

static int sm_init(BetlContext *ctx, const char *cfg, void **state) {
    SmState *s = calloc(1, sizeof *s);
    if (!s) return BETL_ERR_INTERNAL;
    s->ctx = ctx;
    cfg = cfg ? cfg : "{}";

#define REQUIRE_STR(k, dst) do { \
        if (betl_tx_json_string_at(cfg, k, &s->dst) != 0 || !s->dst) { \
            betl_set_error(ctx, "smtp.send: missing required `%s`", k); \
            goto bail; \
        } \
    } while (0)
    REQUIRE_STR("url",     url);
    REQUIRE_STR("from",    from);
    REQUIRE_STR("subject", subject);

    if (parse_addr_list(s, cfg, "to", &s->to, &s->n_to) != 0) goto bail;
    if (s->n_to == 0) {
        betl_set_error(ctx, "smtp.send: `to:` is required and must list at "
                            "least one recipient");
        goto bail;
    }
    if (parse_addr_list(s, cfg, "cc", &s->cc, &s->n_cc) != 0) goto bail;

    /* username / password are optional but mutually-implied; if one is
     * set, the other should be too. We just pass through what's there
     * and let curl complain if the server requires AUTH and we didn't
     * provide credentials. */
    (void)betl_tx_json_string_at(cfg, "username", &s->username);
    (void)betl_tx_json_string_at(cfg, "password", &s->password);

    char *body_lit = NULL, *body_path = NULL;
    (void)betl_tx_json_string_at(cfg, "body",      &body_lit);
    (void)betl_tx_json_string_at(cfg, "body_file", &body_path);
    if (body_lit && body_path) {
        betl_set_error(ctx, "smtp.send: specify either `body:` or "
                            "`body_file:`, not both");
        free(body_lit); free(body_path);
        goto bail;
    }
    if (!body_lit && !body_path) {
        betl_set_error(ctx, "smtp.send: one of `body:` or `body_file:` "
                            "is required");
        goto bail;
    }
    s->body      = body_lit;
    s->body_file = body_path;

#undef REQUIRE_STR

    *state = s;
    return BETL_OK;

bail:
    free(s->url);
    free(s->from);
    free(s->subject);
    free(s->username);
    free(s->password);
    free(s->body);
    free(s->body_file);
    for (size_t i = 0; i < s->n_to; ++i) free(s->to[i]);
    for (size_t i = 0; i < s->n_cc; ++i) free(s->cc[i]);
    free(s->to); free(s->cc);
    free(s);
    return BETL_ERR_INVALID;
}

static void sm_destroy(void *state) {
    if (!state) return;
    SmState *s = state;
    free(s->url);
    free(s->from);
    free(s->subject);
    free(s->username);
    free(s->password);
    free(s->body);
    free(s->body_file);
    for (size_t i = 0; i < s->n_to; ++i) free(s->to[i]);
    for (size_t i = 0; i < s->n_cc; ++i) free(s->cc[i]);
    free(s->to); free(s->cc);
    free(s);
}

/* ---- Build the RFC 5322 message ----------------------------------- *
 *
 * For plain text we just concatenate headers + blank line + body. Curl
 * speaks SMTP DATA on its own. We add Date and a Message-ID since some
 * servers reject messages that lack them.
 *
 * The result is malloc'd; caller frees. */

static char *slurp_file_(const char *path, size_t *out_len) {
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

/* snprintf into a growable buffer, doubling on truncation. Returns 0 on
 * success, -1 on OOM. Replaces the `pos += snprintf(buf+pos, cap-pos,
 * ...)` chain which silently corrupts the buffer if any single call
 * truncates: the chain underflows `cap - pos` to a huge size_t on the
 * next call once pos > cap. The conservative initial cap below makes
 * truncation unlikely in practice, but this guards against future
 * additions to the header set that escape the budget. */
static int append_fmt(char **buf, size_t *cap, size_t *pos,
                      const char *fmt, ...) {
    for (;;) {
        size_t remaining = (*pos < *cap) ? (*cap - *pos) : 0;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *pos, remaining, fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        if ((size_t)n < remaining) {
            *pos += (size_t)n;
            return 0;
        }
        size_t new_cap = *cap ? *cap * 2 : 256;
        while (new_cap < *pos + (size_t)n + 1) new_cap *= 2;
        char *grow = realloc(*buf, new_cap);
        if (!grow) return -1;
        *buf = grow;
        *cap = new_cap;
    }
}

static int build_message(SmState *s, char **out_msg, size_t *out_len) {
    /* Headers. */
    char date_buf[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date_buf, sizeof date_buf,
             "%a, %d %b %Y %H:%M:%S +0000", &tm);

    /* Message-ID: use timestamp + pid + the URL host portion. Best-
     * effort uniqueness; not cryptographically random. */
    const char *host = strstr(s->url, "://");
    host = host ? host + 3 : "betl.local";

    /* Body: load from file or use literal. */
    char  *body     = NULL;
    size_t body_len = 0;
    int    body_owned = 0;
    if (s->body) {
        body = s->body;
        body_len = strlen(body);
    } else {
        body = slurp_file_(s->body_file, &body_len);
        if (!body) {
            betl_set_error(s->ctx, "smtp.send: failed to read body_file '%s': %s",
                           s->body_file, strerror(errno));
            return -1;
        }
        body_owned = 1;
    }

    /* Conservative size estimate: headers + 1KB padding + body. */
    size_t to_join_len = 0;
    for (size_t i = 0; i < s->n_to; ++i) to_join_len += strlen(s->to[i]) + 2;
    size_t cc_join_len = 0;
    for (size_t i = 0; i < s->n_cc; ++i) cc_join_len += strlen(s->cc[i]) + 2;
    size_t cap = strlen(s->from) + strlen(s->subject) + to_join_len
               + cc_join_len + strlen(date_buf) + strlen(host) + body_len + 1024;
    char *buf = malloc(cap);
    if (!buf) {
        if (body_owned) free(body);
        return -1;
    }
    size_t pos = 0;
#define AF(...) do { if (append_fmt(&buf, &cap, &pos, __VA_ARGS__) != 0) { \
        free(buf); if (body_owned) free(body); return -1; } } while (0)
    AF("Date: %s\r\n", date_buf);
    AF("Message-ID: <betl-%lu-%d@%s>\r\n",
       (unsigned long)now, (int)getpid(), host);
    AF("From: %s\r\n", s->from);
    AF("To: ");
    for (size_t i = 0; i < s->n_to; ++i) {
        AF("%s%s", i ? ", " : "", s->to[i]);
    }
    AF("\r\n");
    if (s->n_cc) {
        AF("Cc: ");
        for (size_t i = 0; i < s->n_cc; ++i) {
            AF("%s%s", i ? ", " : "", s->cc[i]);
        }
        AF("\r\n");
    }
    AF("Subject: %s\r\n", s->subject);
    AF("MIME-Version: 1.0\r\n"
       "Content-Type: text/plain; charset=utf-8\r\n"
       "\r\n");
#undef AF
    /* Body: copy as-is, but rewrite bare LF to CRLF for safety on the
     * wire. (Curl uses CRLF.LF as the end-of-DATA marker; bare LFs in
     * the body are tolerated by most servers but we normalize.) */
    for (size_t i = 0; i < body_len; ++i) {
        if (body[i] == '\n' && (i == 0 || body[i-1] != '\r')) {
            if (pos + 2 >= cap) goto realloc_;
            buf[pos++] = '\r';
            buf[pos++] = '\n';
        } else {
            if (pos + 1 >= cap) goto realloc_;
            buf[pos++] = body[i];
        }
        continue;
realloc_:
        cap *= 2;
        char *grow = realloc(buf, cap);
        if (!grow) { free(buf); if (body_owned) free(body); return -1; }
        buf = grow;
        --i;                      /* retry this byte */
    }
    if (body_owned) free(body);

    *out_msg = buf;
    *out_len = pos;
    return 0;
}

/* ---- Read callback for curl --------------------------------------- */

typedef struct {
    const char *msg;
    size_t      len;
    size_t      pos;
} ReadCtx;

static size_t msg_read_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    ReadCtx *r = ud;
    size_t want = size * nmemb;
    size_t left = r->len - r->pos;
    size_t take = left < want ? left : want;
    if (take) {
        memcpy(ptr, r->msg + r->pos, take);
        r->pos += take;
    }
    return take;
}

/* ---- Runner ------------------------------------------------------- */

static int sm_run(void *state) {
    SmState *s = state;

    char  *msg     = NULL;
    size_t msg_len = 0;
    if (build_message(s, &msg, &msg_len) != 0) {
        return BETL_ERR_INTERNAL;
    }

    CURL *c = curl_easy_init();
    if (!c) {
        free(msg);
        betl_set_error(s->ctx, "smtp.send: curl_easy_init failed");
        return BETL_ERR_INTERNAL;
    }

    struct curl_slist *rcpts = NULL;
    int rc_out = BETL_OK;

    curl_easy_setopt(c, CURLOPT_URL, s->url);
    if (s->username) curl_easy_setopt(c, CURLOPT_USERNAME, s->username);
    if (s->password) curl_easy_setopt(c, CURLOPT_PASSWORD, s->password);
    /* USE_SSL=TRY upgrades to STARTTLS on smtp:// when the server
     * advertises it; smtps:// is already TLS by URL scheme. */
    curl_easy_setopt(c, CURLOPT_USE_SSL, (long)CURLUSESSL_TRY);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

    /* MAIL FROM must be an angle-bracketed address. If the user gave
     * "Alice <alice@x>", extract the inner address; if they gave a bare
     * address, wrap it. */
    char from_env[256] = {0};
    {
        const char *p = strchr(s->from, '<');
        const char *q = strchr(s->from, '>');
        if (p && q && q > p) {
            size_t L = (size_t)(q - p - 1);
            if (L >= sizeof from_env) L = sizeof from_env - 1;
            from_env[0] = '<';
            memcpy(from_env + 1, p + 1, L);
            from_env[1 + L] = '>';
            from_env[2 + L] = '\0';
        } else {
            snprintf(from_env, sizeof from_env, "<%s>", s->from);
        }
    }
    curl_easy_setopt(c, CURLOPT_MAIL_FROM, from_env);

    for (size_t i = 0; i < s->n_to; ++i) {
        char buf[256];
        snprintf(buf, sizeof buf, "<%s>", s->to[i]);
        struct curl_slist *grow = curl_slist_append(rcpts, buf);
        if (!grow) { rc_out = BETL_ERR_INTERNAL; goto cleanup; }
        rcpts = grow;
    }
    for (size_t i = 0; i < s->n_cc; ++i) {
        char buf[256];
        snprintf(buf, sizeof buf, "<%s>", s->cc[i]);
        struct curl_slist *grow = curl_slist_append(rcpts, buf);
        if (!grow) { rc_out = BETL_ERR_INTERNAL; goto cleanup; }
        rcpts = grow;
    }
    curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpts);

    ReadCtx rc = { .msg = msg, .len = msg_len, .pos = 0 };
    curl_easy_setopt(c, CURLOPT_READFUNCTION, msg_read_cb);
    curl_easy_setopt(c, CURLOPT_READDATA, &rc);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);

    CURLcode ce = curl_easy_perform(c);
    if (ce != CURLE_OK) {
        betl_set_error(s->ctx, "smtp.send: %s: %s",
                       s->url, curl_easy_strerror(ce));
        rc_out = BETL_ERR_IO;
        goto cleanup;
    }

cleanup:
    if (rcpts) curl_slist_free_all(rcpts);
    curl_easy_cleanup(c);
    free(msg);
    return rc_out;
}

static const BetlComponentDef sm_components[] = {
    { .name               = "smtp.send",
      .kind               = BETL_KIND_TASK,
      .config_schema_json = "{}",
      .flags              = 0,
      .init               = sm_init,
      .destroy            = sm_destroy,
      .task_run           = sm_run },
};

static const BetlProvider sm_provider = {
    .abi_version     = BETL_ABI_VERSION,
    .name            = "betl-builtins-smtp-send",
    .version         = "0.1.0",
    .license         = "Apache-2.0",
    .components      = sm_components,
    .component_count = sizeof sm_components / sizeof sm_components[0],
};

int betl_register_smtp_send(BetlRegistry *r) {
    /* curl_global_init is refcounted; http_ops also calls it. Calling
     * twice is harmless. */
    static int inited = 0;
    if (!inited) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        inited = 1;
    }
    return betl_registry_register(r, &sm_provider, "<builtin:smtp-send>");
}
