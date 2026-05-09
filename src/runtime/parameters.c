#include "runtime/parameters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "runtime/context.h"

/* ============================================================== *
 *  Sentinels                                                       *
 *                                                                  *
 *  `today` resolves to YYYY-MM-DD using *local* time. Per SPEC §5.2 *
 *  there will eventually be a `--utc` flag to switch to UTC; for    *
 *  v0.1 we just use local time and document it.                     *
 *                                                                  *
 *  `now` resolves to an ISO-8601 timestamp with microsecond         *
 *  precision in local time, e.g. 2026-05-08T14:23:45.123456.        *
 * ============================================================== */

static int resolve_today(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return -1;
    return (size_t)snprintf(out, cap, "%04d-%02d-%02d",
                            tmv.tm_year + 1900,
                            tmv.tm_mon  + 1,
                            tmv.tm_mday) >= cap ? -1 : 0;
}

static int resolve_now(char *out, size_t cap) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    struct tm tmv;
    if (!localtime_r(&ts.tv_sec, &tmv)) return -1;
    long usec = ts.tv_nsec / 1000;
    return (size_t)snprintf(out, cap,
            "%04d-%02d-%02dT%02d:%02d:%02d.%06ld",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
            tmv.tm_hour, tmv.tm_min, tmv.tm_sec, usec) >= cap ? -1 : 0;
}

/* ============================================================== *
 *  Type-format checks for declared parameters                      *
 *                                                                  *
 *  We don't reject anything we can't validate; unknown types just  *
 *  pass through as opaque strings. The supported set is the spec's *
 *  basic types plus aliases.                                        *
 * ============================================================== */

static int looks_like_int(const char *s) {
    if (!*s) return 0;
    if (*s == '-' || *s == '+') ++s;
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        ++s;
    }
    return 1;
}

static int looks_like_bool(const char *s) {
    return strcmp(s, "true") == 0 || strcmp(s, "false") == 0;
}

static int looks_like_date(const char *s) {
    /* YYYY-MM-DD; no calendar validation, just shape. */
    if (strlen(s) != 10) return 0;
    if (s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int looks_like_timestamp(const char *s) {
    /* YYYY-MM-DDTHH:MM:SS  with optional .fraction and tz. We just
     * check the date+T+time prefix. */
    if (strlen(s) < 19) return 0;
    if (s[10] != 'T' && s[10] != ' ') return 0;
    if (s[13] != ':' || s[16] != ':') return 0;
    return 1;
}

/* Returns 0 if `value` is plausibly of `type`; -1 otherwise. */
static int validate_value(const char *type, const char *value) {
    if (!type || !value) return 0;       /* nothing to check */
    if (strcmp(type, "string") == 0) return 0;
    if (strcmp(type, "bool") == 0) return looks_like_bool(value) ? 0 : -1;
    if (strcmp(type, "int32") == 0
     || strcmp(type, "int64") == 0
     || strcmp(type, "int") == 0)        return looks_like_int(value) ? 0 : -1;
    if (strcmp(type, "date") == 0)       return looks_like_date(value) ? 0 : -1;
    if (strcmp(type, "timestamp") == 0)  return looks_like_timestamp(value) ? 0 : -1;
    /* Unknown / unsupported type — accept opaquely. */
    return 0;
}

/* ============================================================== *
 *  CLI override parsing                                            *
 *                                                                  *
 *  Each entry is "NAME=VALUE". Both are scanned on every parameter *
 *  resolution; n is small (typically 1–10), so a linear scan is    *
 *  the simplest correct thing.                                      *
 * ============================================================== */

static const char *find_override(char **overrides, size_t n,
                                 const char *name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i < n; ++i) {
        const char *o = overrides[i];
        if (strncmp(o, name, nlen) == 0 && o[nlen] == '=') {
            return o + nlen + 1;
        }
    }
    return NULL;
}

/* Returns 1 if `name` matches any declared parameter; else 0. */
static int param_declared(const BetlPipeline *p, const char *name) {
    size_t n = betl_pipeline_parameter_count(p);
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(betl_pipeline_parameter(p, i)->name, name) == 0) return 1;
    }
    return 0;
}

/* ============================================================== *
 *  Public API                                                      *
 * ============================================================== */

int betl_apply_parameters(BetlContext *ctx, const BetlPipeline *p,
                          char **cli_overrides, size_t n_overrides,
                          char *err_buf, size_t err_cap) {
    if (err_buf && err_cap > 0) err_buf[0] = '\0';
    if (!ctx || !p) return BETL_ERR_INVALID;

    /* Reject overrides that name an undeclared parameter — typos there
     * silently disappear otherwise. */
    for (size_t i = 0; i < n_overrides; ++i) {
        const char *eq = strchr(cli_overrides[i], '=');
        if (!eq) {
            if (err_buf) snprintf(err_buf, err_cap,
                "--param '%s': expected NAME=VALUE", cli_overrides[i]);
            return BETL_ERR_INVALID;
        }
        size_t nlen = (size_t)(eq - cli_overrides[i]);
        char name[128];
        if (nlen + 1 > sizeof name) {
            if (err_buf) snprintf(err_buf, err_cap,
                "--param: name too long");
            return BETL_ERR_INVALID;
        }
        memcpy(name, cli_overrides[i], nlen);
        name[nlen] = '\0';
        if (!param_declared(p, name)) {
            if (err_buf) snprintf(err_buf, err_cap,
                "--param '%s': not declared in pipeline `parameters:` block",
                name);
            return BETL_ERR_INVALID;
        }
    }

    size_t n = betl_pipeline_parameter_count(p);
    for (size_t i = 0; i < n; ++i) {
        const BetlParameterDecl *pa = betl_pipeline_parameter(p, i);
        const char *value = NULL;
        char        sentinel_buf[64];

        const char *override = find_override(cli_overrides, n_overrides,
                                             pa->name);
        if (override) {
            value = override;
        } else if (pa->has_default) {
            if (pa->is_sentinel) {
                int rc = -1;
                if (strcmp(pa->default_value, "today") == 0) {
                    rc = resolve_today(sentinel_buf, sizeof sentinel_buf);
                } else if (strcmp(pa->default_value, "now") == 0) {
                    rc = resolve_now(sentinel_buf, sizeof sentinel_buf);
                }
                if (rc != 0) {
                    if (err_buf) snprintf(err_buf, err_cap,
                        "parameter '%s': sentinel default '%s' failed to resolve",
                        pa->name, pa->default_value);
                    return BETL_ERR_INTERNAL;
                }
                value = sentinel_buf;
            } else {
                value = pa->default_value;
            }
        } else if (pa->required) {
            if (err_buf) snprintf(err_buf, err_cap,
                "parameter '%s' is required but no value was supplied "
                "(use --param %s=...)", pa->name, pa->name);
            return BETL_ERR_INVALID;
        } else {
            continue;       /* leave unset */
        }

        if (validate_value(pa->type, value) != 0) {
            if (err_buf) snprintf(err_buf, err_cap,
                "parameter '%s': value '%s' is not a valid %s",
                pa->name, value, pa->type);
            return BETL_ERR_INVALID;
        }

        int rc = betl_context_set_param(ctx, pa->name, value);
        if (rc != BETL_OK) {
            if (err_buf) snprintf(err_buf, err_cap,
                "parameter '%s': set_param failed (rc=%d)", pa->name, rc);
            return rc;
        }
    }

    return BETL_OK;
}
