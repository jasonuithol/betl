/* Control-flow features end-to-end:
 *
 *   1. foreach over a literal list, with `${vars.NAME}` bound per
 *      iteration into a `file.copy` task's `dst:`.
 *   2. on_failure: continue — a failing /bin/false stage doesn't
 *      stop a later sibling.
 *   3. condition: "false" — the stage is skipped entirely.
 *   4. condition: "${vars.X}" — truthy under a foreach binding.
 *
 * The test drives full pipelines through betl_run (so the executor's
 * stage runner — topo + condition + on_failure + foreach — is the
 * thing under test, not the parser alone).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/context.h"
#include "runtime/exec.h"

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

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Run one YAML pipeline. Returns rc from betl_run; on failure, the
 * caller can inspect err_out (populated by the executor's
 * betl_set_error). */
static int run_yaml(const char *yaml, char *err_out, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path,
             "/tmp/betl-test-controlflow-%d.yml", (int)getpid());
    if (write_file(path, yaml) != 0) return BETL_ERR_IO;
    char perr[512] = {0};
    BetlPipeline *p = betl_pipeline_load(path, perr, sizeof perr);
    if (!p) {
        if (err_out) snprintf(err_out, err_cap, "pipeline_load: %s", perr);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK && err_out) {
            snprintf(err_out, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

int main(void) {
    char src[64];
    snprintf(src, sizeof src, "/tmp/betl-cf-src-%d.txt", (int)getpid());
    const char *payload = "control-flow fixture\n";
    if (write_file(src, payload) != 0) {
        fprintf(stderr, "setup: write_file failed\n");
        return 1;
    }

    /* --- 1: foreach over ["a","b","c"] copies src to 3 dsts. -------- */
    {
        char dst_a[64], dst_b[64], dst_c[64];
        snprintf(dst_a, sizeof dst_a, "/tmp/betl-cf-foreach-a-%d.txt", (int)getpid());
        snprintf(dst_b, sizeof dst_b, "/tmp/betl-cf-foreach-b-%d.txt", (int)getpid());
        snprintf(dst_c, sizeof dst_c, "/tmp/betl-cf-foreach-c-%d.txt", (int)getpid());
        unlink(dst_a); unlink(dst_b); unlink(dst_c);

        char yaml[2048];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: cf-foreach\n"
            "pipeline:\n"
            "  - id: per_file\n"
            "    type: foreach\n"
            "    over: [\"%s\", \"%s\", \"%s\"]\n"
            "    as: dst\n"
            "    body:\n"
            "      - id: copy_one\n"
            "        type: file.copy\n"
            "        src: \"%s\"\n"
            "        dst: \"${vars.dst}\"\n",
            dst_a, dst_b, dst_c, src);

        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 1: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(file_exists(dst_a));
        CHECK(file_exists(dst_b));
        CHECK(file_exists(dst_c));
        unlink(dst_a); unlink(dst_b); unlink(dst_c);
    }

    /* --- 2: on_failure: stop (default) aborts the second stage. ----- */
    {
        char marker[64];
        snprintf(marker, sizeof marker, "/tmp/betl-cf-marker-stop-%d.txt",
                 (int)getpid());
        unlink(marker);
        char yaml[1024];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: cf-onfail-stop\n"
            "pipeline:\n"
            "  - id: must_fail\n"
            "    type: shell\n"
            "    argv: [\"/bin/false\"]\n"
            "  - id: after_marker\n"
            "    type: shell\n"
            "    after: [must_fail]\n"
            "    argv: [\"/bin/sh\", \"-c\", \"echo x > %s\"]\n",
            marker);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(!file_exists(marker));
        unlink(marker);
    }

    /* --- 3: on_failure: continue lets the second stage run. --------- */
    {
        char marker[64];
        snprintf(marker, sizeof marker, "/tmp/betl-cf-marker-cont-%d.txt",
                 (int)getpid());
        unlink(marker);
        char yaml[1024];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: cf-onfail-continue\n"
            "pipeline:\n"
            "  - id: must_fail\n"
            "    type: shell\n"
            "    argv: [\"/bin/false\"]\n"
            "    on_failure: continue\n"
            "  - id: after_marker\n"
            "    type: shell\n"
            "    after: [must_fail]\n"
            "    argv: [\"/bin/sh\", \"-c\", \"echo x > %s\"]\n",
            marker);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 3: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(file_exists(marker));
        unlink(marker);
    }

    /* --- 4: condition: "false" skips a stage that would otherwise fail. */
    {
        char yaml[512] =
            "betl: 1\n"
            "name: cf-cond-false\n"
            "pipeline:\n"
            "  - id: skipped_failure\n"
            "    type: shell\n"
            "    argv: [\"/bin/false\"]\n"
            "    condition: \"false\"\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 4: %s\n", err);
        CHECK(rc == BETL_OK);   /* skipped → pipeline succeeds */
    }

    /* --- 5: condition: "true" still runs (the failing stage fails). - */
    {
        char yaml[512] =
            "betl: 1\n"
            "name: cf-cond-true\n"
            "pipeline:\n"
            "  - id: ran_failure\n"
            "    type: shell\n"
            "    argv: [\"/bin/false\"]\n"
            "    condition: \"true\"\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
    }

    /* --- 6: condition via ${vars.X} inside foreach: only one of three
     *        iterations actually runs the body. ----------------------- */
    {
        char dst_skip[64], dst_run[64];
        snprintf(dst_skip, sizeof dst_skip,
                 "/tmp/betl-cf-foreach-skip-%d.txt", (int)getpid());
        snprintf(dst_run, sizeof dst_run,
                 "/tmp/betl-cf-foreach-run-%d.txt", (int)getpid());
        unlink(dst_skip); unlink(dst_run);

        char yaml[2048];
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: cf-foreach-cond\n"
            "pipeline:\n"
            "  - id: per_flag\n"
            "    type: foreach\n"
            "    over: [\"false\", \"true\", \"false\"]\n"
            "    as: flag\n"
            "    body:\n"
            "      - id: maybe_copy\n"
            "        type: file.copy\n"
            "        condition: \"${vars.flag}\"\n"
            "        src: \"%s\"\n"
            "        dst: \"%s\"\n",
            src, dst_run);
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "case 6: %s\n", err);
        CHECK(rc == BETL_OK);
        CHECK(file_exists(dst_run));    /* the middle iter ran */
        unlink(dst_run);
    }

    /* --- 7: bad condition value → clear error. --------------------- */
    {
        char yaml[512] =
            "betl: 1\n"
            "name: cf-cond-bad\n"
            "pipeline:\n"
            "  - id: bogus\n"
            "    type: shell\n"
            "    argv: [\"/bin/true\"]\n"
            "    condition: \"maybe\"\n";
        char err[512] = {0};
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "non-boolean") != NULL);
    }

    unlink(src);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: control_flow integration test passed\n");
    return 0;
}
