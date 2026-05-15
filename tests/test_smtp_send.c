/* smtp.send config-validation tests.
 *
 * Actually sending mail requires a reachable SMTP server, which the
 * test harness doesn't provide. These cases exercise the init path
 * (required field checks, mutually-exclusive body/body_file, address
 * list parsing) and a failing send against an unreachable host
 * (confirms the curl error surfaces with the expected error code).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "runtime/builtins.h"
#include "runtime/context.h"

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
                          __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

/* Init the task with the given JSON config; return the rc from init.
 * Does NOT call task_run. */
static int init_only(BetlRegistry *r, BetlContext *ctx, const char *cfg) {
    const BetlComponentDef *cd = betl_registry_find(r, "smtp.send");
    if (!cd) return -1;
    void *state = NULL;
    int rc = cd->init(ctx, cfg, &state);
    if (rc == BETL_OK) cd->destroy(state);
    return rc;
}

/* Init + task_run with the given config; returns task_run rc (or init
 * rc if init failed). */
static int init_and_run(BetlRegistry *r, BetlContext *ctx, const char *cfg) {
    const BetlComponentDef *cd = betl_registry_find(r, "smtp.send");
    if (!cd) return -1;
    void *state = NULL;
    int rc = cd->init(ctx, cfg, &state);
    if (rc != BETL_OK) return rc;
    rc = cd->task_run(state);
    cd->destroy(state);
    return rc;
}

int main(void) {
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    CHECK(ctx && reg);
    CHECK(betl_register_builtins(reg) == BETL_OK);

    /* --- 1: missing required fields -------------------------------- */
    CHECK(init_only(reg, ctx, "{}") == BETL_ERR_INVALID);

    CHECK(init_only(reg, ctx,
        "{\"url\":\"smtp://localhost:25\"}") == BETL_ERR_INVALID);

    CHECK(init_only(reg, ctx,
        "{\"url\":\"smtp://localhost:25\","
         "\"from\":\"a@x\"}") == BETL_ERR_INVALID);

    CHECK(init_only(reg, ctx,
        "{\"url\":\"smtp://localhost:25\","
         "\"from\":\"a@x\","
         "\"subject\":\"hi\"}") == BETL_ERR_INVALID);  /* no to: */

    /* --- 2: body XOR body_file ------------------------------------- */
    CHECK(init_only(reg, ctx,
        "{\"url\":\"smtp://localhost:25\","
         "\"from\":\"a@x\","
         "\"subject\":\"hi\","
         "\"to\":[\"b@x\"],"
         "\"body\":\"hello\","
         "\"body_file\":\"/tmp/no\"}") == BETL_ERR_INVALID);

    CHECK(init_only(reg, ctx,
        "{\"url\":\"smtp://localhost:25\","
         "\"from\":\"a@x\","
         "\"subject\":\"hi\","
         "\"to\":[\"b@x\"]}") == BETL_ERR_INVALID);   /* neither */

    /* --- 3: valid init (no network call yet) ---------------------- */
    CHECK(init_only(reg, ctx,
        "{\"url\":\"smtp://127.0.0.1:1\","
         "\"from\":\"alice@x.test\","
         "\"subject\":\"hi\","
         "\"to\":[\"bob@x.test\", \"carol@x.test\"],"
         "\"cc\":[\"dave@x.test\"],"
         "\"body\":\"hello\\nworld\\n\"}") == BETL_OK);

    /* --- 4: send to an unreachable host fails with BETL_ERR_IO ----- *
     * Port 1 is privileged + never accepting; libcurl reports a clean
     * connect failure. Keep the smoke test self-contained. */
    {
        int rc = init_and_run(reg, ctx,
            "{\"url\":\"smtp://127.0.0.1:1\","
             "\"from\":\"alice@x.test\","
             "\"subject\":\"hi\","
             "\"to\":[\"bob@x.test\"],"
             "\"body\":\"unreachable\"}");
        CHECK(rc == BETL_ERR_IO);
        const char *e = betl_context_last_error(ctx);
        CHECK(e && strstr(e, "smtp.send") != NULL);
    }

    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: smtp_send integration test passed\n");
    return 0;
}
