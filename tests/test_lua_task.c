/* betl-lua provider end-to-end test.
 *
 * argv[1] = path to betl-lua.so
 *
 * Loads the provider via the registry, then exercises lua.task:
 *   1. happy path — a script that calls log.info, reads params.X,
 *      returns normally; check log captured the message and the
 *      task returned BETL_OK.
 *   2. compile-time syntax error → init returns INVALID, ctx error
 *      mentions the syntax problem, no leak.
 *   3. runtime error (Lua `error("boom")`) → init OK, task_run
 *      returns non-zero, ctx error contains "boom".
 *   4. missing 'script' key → init returns INVALID, error explains.
 *   5. params.X for an unset param → returns nil (not a hard error).
 *   6. params is read-only → assignment raises a runtime error.
 *   7. connection("name") → returns the JSON we registered.
 */

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

static char *slurp(FILE *f) {
    fflush(f);
    if (fseek(f, 0, SEEK_END) != 0) return NULL;
    long sz = ftell(f);
    if (sz < 0) return NULL;
    if (fseek(f, 0, SEEK_SET) != 0) return NULL;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) return NULL;
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    return buf;
}

/* Run lua.task with the given JSON config; return rc from task_run
 * (or from init if init failed; in that case task_run is not called). */
static int run_task(const BetlComponentDef *cd, BetlContext *ctx,
                    const char *cfg_json, int *init_rc_out) {
    void *state = NULL;
    int init_rc = cd->init(ctx, cfg_json, &state);
    if (init_rc_out) *init_rc_out = init_rc;
    if (init_rc != BETL_OK) return init_rc;
    int run_rc = cd->task_run(state);
    cd->destroy(state);
    return run_rc;
}

int main(int argc, char **argv) {
    const char *plugin_path = (argc >= 2) ? argv[1]
#ifdef BETL_TEST_PLUGIN_PATH
        : BETL_TEST_PLUGIN_PATH;
#else
        : NULL;
    if (!plugin_path) {
        fprintf(stderr, "usage: %s <path-to-betl-lua.so>\n", argv[0]);
        return 2;
    }
#endif

    BetlRegistry *r = betl_registry_create();
    CHECK(r != NULL);
    if (!r) return 1;

    int rc = betl_registry_load(r, plugin_path);
    if (rc != BETL_OK) {
        fprintf(stderr, "load failed: %s\n", betl_registry_last_error(r));
    }
    CHECK(rc == BETL_OK);

    const BetlComponentDef *cd = betl_registry_find(r, "lua.task");
    CHECK(cd != NULL);
    /* Bail hard if the vtable is incomplete: the rest of the test calls
     * cd->init / task_run / destroy unconditionally, and CHECK only
     * counts failures, so we'd null-deref on a degraded component. */
    if (!cd || !cd->init || !cd->destroy || !cd->task_run) {
        fprintf(stderr, "lua.task missing or has incomplete vtable\n");
        betl_registry_destroy(r);
        return 1;
    }
    CHECK(cd->kind == BETL_KIND_TASK);

    /* --- 1: happy path -------------------------------------------------- */
    {
        BetlContext *ctx = betl_context_create();
        CHECK(ctx != NULL);
        FILE *log = tmpfile();
        CHECK(log != NULL);
        betl_context_set_log_stream(ctx, log);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
        betl_context_set_param(ctx, "greeting", "g'day");

        int init_rc = -1;
        int run_rc = run_task(cd, ctx,
            "{\"script\":\"log.info('hello from lua: ' .. params.greeting)\"}",
            &init_rc);
        CHECK(init_rc == BETL_OK);
        CHECK(run_rc  == BETL_OK);

        char *log_text = slurp(log);
        CHECK(log_text != NULL);
        if (log_text) {
            CHECK(strstr(log_text, "hello from lua: g'day") != NULL);
            free(log_text);
        }
        fclose(log);
        betl_context_destroy(ctx);
    }

    /* --- 2: compile-time error ------------------------------------------ */
    {
        BetlContext *ctx = betl_context_create();
        int init_rc = -1;
        run_task(cd, ctx,
            "{\"script\":\"this is :: not lua\"}",
            &init_rc);
        CHECK(init_rc == BETL_ERR_INVALID);
        const char *err = betl_context_last_error(ctx);
        CHECK(strstr(err, "compile error") != NULL);
        betl_context_destroy(ctx);
    }

    /* --- 3: runtime error ----------------------------------------------- */
    {
        BetlContext *ctx = betl_context_create();
        int init_rc = -1;
        int run_rc = run_task(cd, ctx,
            "{\"script\":\"error('boom on purpose')\"}",
            &init_rc);
        CHECK(init_rc == BETL_OK);
        CHECK(run_rc  != BETL_OK);
        const char *err = betl_context_last_error(ctx);
        CHECK(strstr(err, "boom on purpose") != NULL);
        betl_context_destroy(ctx);
    }

    /* --- 4: missing script ---------------------------------------------- */
    {
        BetlContext *ctx = betl_context_create();
        int init_rc = -1;
        run_task(cd, ctx, "{}", &init_rc);
        CHECK(init_rc == BETL_ERR_INVALID);
        const char *err = betl_context_last_error(ctx);
        CHECK(strstr(err, "script") != NULL);
        betl_context_destroy(ctx);
    }

    /* --- 5: unset param returns nil, no error --------------------------- */
    {
        BetlContext *ctx = betl_context_create();
        int init_rc = -1;
        int run_rc = run_task(cd, ctx,
            "{\"script\":\"if params.unset_xyz ~= nil then error('expected nil') end\"}",
            &init_rc);
        CHECK(init_rc == BETL_OK);
        CHECK(run_rc  == BETL_OK);
        betl_context_destroy(ctx);
    }

    /* --- 6: params is read-only ----------------------------------------- */
    {
        BetlContext *ctx = betl_context_create();
        int init_rc = -1;
        int run_rc = run_task(cd, ctx,
            "{\"script\":\"params.foo = 'bar'\"}",
            &init_rc);
        CHECK(init_rc == BETL_OK);
        CHECK(run_rc  != BETL_OK);
        const char *err = betl_context_last_error(ctx);
        CHECK(strstr(err, "read-only") != NULL);
        betl_context_destroy(ctx);
    }

    /* --- 7: connection() bridge ----------------------------------------- */
    {
        BetlContext *ctx = betl_context_create();
        FILE *log = tmpfile();
        betl_context_set_log_stream(ctx, log);
        betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
        betl_context_set_connection(ctx, "warehouse",
            "{\"dsn\":\"postgresql://x\"}");
        int init_rc = -1;
        int run_rc = run_task(cd, ctx,
            "{\"script\":\"local c = connection('warehouse')\\n"
            "if c == nil then error('warehouse missing') end\\n"
            "log.info('conn=' .. c)\"}",
            &init_rc);
        CHECK(init_rc == BETL_OK);
        CHECK(run_rc  == BETL_OK);
        char *log_text = slurp(log);
        if (log_text) {
            CHECK(strstr(log_text, "postgresql://x") != NULL);
            free(log_text);
        }
        fclose(log);
        betl_context_destroy(ctx);
    }

    betl_registry_destroy(r);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("lua_task: all checks passed\n");
    return 0;
}
