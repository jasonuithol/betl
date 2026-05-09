/* Host context test: end-to-end through the loader.
 *
 * argv[1] = path to the ctx-probe provider .so
 *
 * Loads the probe, populates the context with a param + connection,
 * redirects logging into a tmpfile, runs the probe's task, and asserts
 * (a) the task succeeded, (b) the log stream captured the INFO line,
 * (c) the probe's final betl_set_error round-tripped to the host. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/context.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

static char *slurp(FILE *f, size_t *out_len) {
    fflush(f);
    if (fseek(f, 0, SEEK_END) != 0) return NULL;
    long sz = ftell(f);
    if (sz < 0) return NULL;
    if (fseek(f, 0, SEEK_SET) != 0) return NULL;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) return NULL;
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1]
#ifdef BETL_TEST_PLUGIN_PATH
        : BETL_TEST_PLUGIN_PATH;
#else
        : NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-ctx-probe.so>\n", argv[0]);
        return 2;
    }
#endif

    /* --- Set up host context. ---------------------------------------- */
    BetlContext *ctx = betl_context_create();
    CHECK(ctx != NULL);
    if (!ctx) return 1;

    FILE *log = tmpfile();
    CHECK(log != NULL);
    if (!log) { betl_context_destroy(ctx); return 1; }

    betl_context_set_log_stream(ctx, log);
    betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
    betl_context_set_log_tag(ctx, "probe-1");
    int rc = betl_context_set_param(ctx, "greeting", "hello");
    CHECK(rc == BETL_OK);
    rc = betl_context_set_connection(ctx, "warehouse",
                                     "{\"dsn\":\"postgresql://x\"}");
    CHECK(rc == BETL_OK);

    /* --- Defaults sanity check. ------------------------------------- */
    CHECK(betl_should_cancel(ctx) == 0);
    CHECK(betl_get_param(ctx, "no.such") == NULL);
    CHECK(strcmp(betl_get_param(ctx, "greeting"), "hello") == 0);
    CHECK(strstr(betl_get_connection(ctx, "warehouse"), "dsn") != NULL);

    /* --- Round-trip set_param overwrite. ---------------------------- */
    rc = betl_context_set_param(ctx, "greeting", "g'day");
    CHECK(rc == BETL_OK);
    CHECK(strcmp(betl_get_param(ctx, "greeting"), "g'day") == 0);
    /* Restore for the probe's expectation. */
    rc = betl_context_set_param(ctx, "greeting", "hello");
    CHECK(rc == BETL_OK);

    /* --- Cancel flag round-trip. ------------------------------------ */
    betl_context_request_cancel(ctx);
    CHECK(betl_should_cancel(ctx) == 1);
    betl_context_clear_cancel(ctx);
    CHECK(betl_should_cancel(ctx) == 0);

    /* --- Load the probe plugin and exercise it via the registry. ---- */
    BetlRegistry *r = betl_registry_create();
    CHECK(r != NULL);
    if (!r) { betl_context_destroy(ctx); fclose(log); return 1; }

    rc = betl_registry_load(r, plugin_path);
    if (rc != BETL_OK) {
        fprintf(stderr, "load failed: %s\n", betl_registry_last_error(r));
    }
    CHECK(rc == BETL_OK);

    const BetlComponentDef *cd = betl_registry_find(r, "test.ctx_probe");
    CHECK(cd != NULL);
    if (cd) {
        void *state = NULL;
        rc = cd->init(ctx, "{}", &state);
        CHECK(rc == BETL_OK);
        CHECK(state != NULL);

        rc = cd->task_run(state);
        if (rc != BETL_OK) {
            fprintf(stderr, "probe task_run failed: %s\n",
                    betl_context_last_error(ctx));
        }
        CHECK(rc == BETL_OK);

        /* The probe's final action is to set "probe completed". */
        CHECK(strstr(betl_context_last_error(ctx), "probe completed") != NULL);

        cd->destroy(state);
    }

    /* --- Verify the log stream captured the probe's INFO line. ------ */
    size_t log_len = 0;
    char *log_text = slurp(log, &log_len);
    CHECK(log_text != NULL);
    if (log_text) {
        CHECK(strstr(log_text, "INFO")     != NULL);
        CHECK(strstr(log_text, "probe-1")  != NULL);  /* tag */
        CHECK(strstr(log_text, "probe alive") != NULL);
        CHECK(strstr(log_text, "1234")     != NULL);
        free(log_text);
    }

    /* --- Min-log-level filtering: TRACE message under WARN is dropped. */
    rewind(log);
    /* Truncate. ftruncate is POSIX. */
    if (fseek(log, 0, SEEK_SET) == 0) {
        /* fmemopen-style streams support fflush + truncation; tmpfile
         * is a real file. We just rewind and check that a low-level
         * message at INFO is NOT emitted when min_level=ERROR. */
        betl_context_set_min_log_level(ctx, BETL_LOG_ERROR);
        betl_log(ctx, BETL_LOG_INFO, "this should NOT appear");
        betl_log(ctx, BETL_LOG_ERROR, "this SHOULD appear");

        size_t tail_len = 0;
        char *tail = slurp(log, &tail_len);
        CHECK(tail != NULL);
        if (tail) {
            CHECK(strstr(tail, "this SHOULD appear")     != NULL);
            CHECK(strstr(tail, "this should NOT appear") == NULL);
            free(tail);
        }
    }

    betl_registry_destroy(r);
    fclose(log);
    betl_context_destroy(ctx);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("context: all checks passed\n");
    return 0;
}
