/* file.copy / file.move / file.delete control-flow task tests.
 *
 * Each case wires the task through the registry and runs the vtable
 * directly (init → task_run → destroy). No pipeline / dataflow needed
 * since these are pure side-effect tasks.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int write_file(const char *path, const char *contents) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(contents);
    int rc = fwrite(contents, 1, len, f) == len ? 0 : -1;
    fclose(f);
    return rc;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int run_task(BetlRegistry *r, BetlContext *ctx,
                    const char *type, const char *cfg) {
    const BetlComponentDef *cd = betl_registry_find(r, type);
    if (!cd) { fprintf(stderr, "no component '%s'\n", type); return -1; }
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

    char src[64], dst[64], dst2[64];
    snprintf(src,  sizeof src,  "/tmp/betl-file-ops-src-%d.txt",  (int)getpid());
    snprintf(dst,  sizeof dst,  "/tmp/betl-file-ops-dst-%d.txt",  (int)getpid());
    snprintf(dst2, sizeof dst2, "/tmp/betl-file-ops-dst2-%d.txt", (int)getpid());
    unlink(src); unlink(dst); unlink(dst2);
    const char *payload = "hello betl file.* tasks\n";
    CHECK(write_file(src, payload) == 0);

    /* --- 1: file.copy succeeds and content matches -------------------- */
    {
        char cfg[256];
        snprintf(cfg, sizeof cfg, "{\"src\":\"%s\",\"dst\":\"%s\"}", src, dst);
        CHECK(run_task(reg, ctx, "file.copy", cfg) == BETL_OK);
        CHECK(file_exists(dst));
        char *got = read_file(dst);
        CHECK(got && strcmp(got, payload) == 0);
        free(got);
        CHECK(file_exists(src));    /* source still intact */
    }

    /* --- 2: file.copy overwrites existing destination ----------------- */
    {
        CHECK(write_file(dst, "stale\n") == 0);
        char cfg[256];
        snprintf(cfg, sizeof cfg, "{\"src\":\"%s\",\"dst\":\"%s\"}", src, dst);
        CHECK(run_task(reg, ctx, "file.copy", cfg) == BETL_OK);
        char *got = read_file(dst);
        CHECK(got && strcmp(got, payload) == 0);
        free(got);
    }

    /* --- 3: file.copy fails when source is missing -------------------- */
    {
        char cfg[256];
        snprintf(cfg, sizeof cfg, "{\"src\":\"/tmp/betl-no-such-%d\","
                                  "\"dst\":\"%s\"}", (int)getpid(), dst);
        CHECK(run_task(reg, ctx, "file.copy", cfg) != BETL_OK);
        const char *e = betl_context_last_error(ctx);
        CHECK(e && strstr(e, "open") != NULL);
    }

    /* --- 4: file.move renames + source disappears --------------------- */
    {
        unlink(dst2);
        char cfg[256];
        snprintf(cfg, sizeof cfg, "{\"src\":\"%s\",\"dst\":\"%s\"}", dst, dst2);
        CHECK(run_task(reg, ctx, "file.move", cfg) == BETL_OK);
        CHECK(file_exists(dst2));
        CHECK(!file_exists(dst));
        char *got = read_file(dst2);
        CHECK(got && strcmp(got, payload) == 0);
        free(got);
    }

    /* --- 5: file.delete removes path ---------------------------------- */
    {
        char cfg[256];
        snprintf(cfg, sizeof cfg, "{\"path\":\"%s\"}", dst2);
        CHECK(run_task(reg, ctx, "file.delete", cfg) == BETL_OK);
        CHECK(!file_exists(dst2));
    }

    /* --- 6: file.delete on missing path errors ------------------------ */
    {
        char cfg[256];
        snprintf(cfg, sizeof cfg, "{\"path\":\"%s\"}", dst2);  /* already gone */
        CHECK(run_task(reg, ctx, "file.delete", cfg) != BETL_OK);
    }

    /* --- 7: missing required keys --------------------------------- */
    {
        CHECK(run_task(reg, ctx, "file.copy",  "{}") == BETL_ERR_INVALID);
        CHECK(run_task(reg, ctx, "file.move",  "{\"src\":\"x\"}")
              == BETL_ERR_INVALID);
        CHECK(run_task(reg, ctx, "file.delete","{}") == BETL_ERR_INVALID);
    }

    unlink(src); unlink(dst); unlink(dst2);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: file_ops integration test passed\n");
    return 0;
}
