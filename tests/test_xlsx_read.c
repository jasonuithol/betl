/* xlsx.read end-to-end test.
 *
 * Round-trip:
 *   gen_strings(N) → xlsx.write(file)
 *   xlsx.read(file) → lua.map(log row.id, row.name) → count_rows
 *
 * Verifies that the rows we wrote come back with the right column
 * names and values. (Values arrive as utf8; gen_strings produces
 * id=int64 + name=utf8, so the read-back `id` is the decimal string
 * representation of the original int.) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/connections.h"
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

static char *slurp_file(FILE *f) {
    fflush(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) return NULL;
    char *buf = malloc((size_t)sz + 1);
    if (!buf) return NULL;
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    return buf;
}

static int run_pipeline(const char *plugin_path, const char *yaml,
                        char *err, size_t err_cap, FILE *log) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-xlsx-read-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) return -1;
    BetlPipeline *p = betl_pipeline_load(path, err, err_cap);
    unlink(path);
    if (!p) return -1;
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        if (plugin_path) {
            rc = betl_registry_load(reg, plugin_path);
            if (rc != BETL_OK) {
                snprintf(err, err_cap, "%s", betl_registry_last_error(reg));
                goto cleanup;
            }
        }
        if (log) {
            betl_context_set_log_stream(ctx, log);
            betl_context_set_min_log_level(ctx, BETL_LOG_TRACE);
        }
        if (betl_apply_connections(ctx, p, err, err_cap) == BETL_OK) {
            rc = betl_run(ctx, reg, p);
            if (rc != BETL_OK) {
                snprintf(err, err_cap, "%s", betl_context_last_error(ctx));
            }
        }
    }
cleanup:
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    return rc;
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

    char outpath[80];
    snprintf(outpath, sizeof outpath, "/tmp/betl-xlsx-rt-%d.xlsx",
             (int)getpid());
    unlink(outpath);

    /* --- 1. Write a small file with gen_strings → xlsx.write. --- */
    char yaml[1024];
    char err[512] = {0};
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: xlsx-rt-write\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_strings\n"
        "        row_count: 5\n"
        "      - id: sink\n"
        "        type: xlsx.write\n"
        "        from: source\n"
        "        path: '%s'\n",
        outpath);
    int rc = run_pipeline(plugin_path, yaml, err, sizeof err, NULL);
    if (rc != 0) fprintf(stderr, "write run failed: %s\n", err);
    CHECK(rc == 0);

    struct stat st;
    CHECK(stat(outpath, &st) == 0 && st.st_size > 0);

    /* --- 2. Read the file back, log id+name via lua.map, count. --- */
    err[0] = 0;
    FILE *log = tmpfile();
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: xlsx-rt-read\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: xlsx.read\n"
        "        path: '%s'\n"
        "      - id: probe\n"
        "        type: lua.map\n"
        "        from: source\n"
        "        script: |\n"
        "          log.info('rt id=' .. tostring(row.id) "
        ".. ' name=' .. tostring(row.name))\n"
        "          return row\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: probe\n"
        "        expect: 5\n",
        outpath);
    rc = run_pipeline(plugin_path, yaml, err, sizeof err, log);
    if (rc != 0) fprintf(stderr, "read run failed: %s\n", err);
    CHECK(rc == 0);

    char *txt = slurp_file(log);
    if (txt) {
        /* gen_strings emits id=0..N-1 (int64) + name='row_0'..'row_N-1'.
         * xlsx.read returns everything as utf8 → ids come back as
         * decimal strings. */
        CHECK(strstr(txt, "rt id=0 name=row_0") != NULL);
        CHECK(strstr(txt, "rt id=4 name=row_4") != NULL);
        free(txt);
    }
    fclose(log);

    unlink(outpath);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: xlsx_read integration test passed\n");
    return 0;
}
