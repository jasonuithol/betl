/* xlsx.write end-to-end test.
 *
 * Drives gen_strings(N) → xlsx.write(path), then:
 *   - confirms the file exists and is non-empty
 *   - confirms the file starts with the ZIP magic (PK\x03\x04)
 *     (xlsx files are zip archives — content validation needs
 *     an xlsx reader, deferred to the xlsx.read test) */

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

static int run_pipeline(const char *yaml, char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-test-xlsx-write-%d.yml",
             (int)getpid());
    if (write_file(path, yaml) != 0) return -1;
    BetlPipeline *p = betl_pipeline_load(path, err, err_cap);
    unlink(path);
    if (!p) return -1;
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK
        && betl_apply_connections(ctx, p, err, err_cap) == BETL_OK)
    {
        rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK) {
            snprintf(err, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    betl_pipeline_free(p);
    betl_registry_destroy(reg);
    betl_context_destroy(ctx);
    return rc;
}

static int file_starts_with_zip_magic(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[4] = {0};
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    return n == 4
        && hdr[0] == 'P' && hdr[1] == 'K'
        && hdr[2] == 0x03 && hdr[3] == 0x04;
}

int main(void) {
    char outpath[80];
    snprintf(outpath, sizeof outpath, "/tmp/betl-xlsx-write-%d.xlsx",
             (int)getpid());
    unlink(outpath);

    /* gen_strings → xlsx.write, mixed int64 + utf8. */
    char yaml[1024];
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: xlsx-write-it\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: betl.gen_strings\n"
        "        row_count: 7\n"
        "      - id: sink\n"
        "        type: xlsx.write\n"
        "        from: source\n"
        "        path: '%s'\n"
        "        sheet: ItemsSheet\n",
        outpath);

    char err[512] = {0};
    int rc = run_pipeline(yaml, err, sizeof err);
    if (rc != 0) fprintf(stderr, "run failed: %s\n", err);
    CHECK(rc == 0);

    struct stat st;
    int ok = (stat(outpath, &st) == 0);
    CHECK(ok);
    if (ok) {
        CHECK(st.st_size > 0);
        CHECK(file_starts_with_zip_magic(outpath));
    }
    unlink(outpath);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: xlsx_write integration test passed\n");
    return 0;
}
