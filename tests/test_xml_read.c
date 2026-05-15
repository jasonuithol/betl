/* xml.read end-to-end test.
 *
 * Writes a small <catalog><book/></catalog> XML doc, then drives
 *   xml.read(row_xpath=/catalog/book, columns={isbn, title, price})
 *     → lua.map (logs each row)
 *     → count_rows(expect: 3)
 *
 * Verifies that the rows we declared appear at the downstream sink. */

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
    snprintf(path, sizeof path, "/tmp/betl-test-xml-read-%d.yml",
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

    char xml_path[80];
    snprintf(xml_path, sizeof xml_path, "/tmp/betl-xml-rt-%d.xml",
             (int)getpid());

    const char *XML =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<catalog>\n"
        "  <book isbn=\"111\"><title>Foo</title><price>9.99</price></book>\n"
        "  <book isbn=\"222\"><title>Bar</title><price>19.50</price></book>\n"
        "  <book isbn=\"333\"><title>Baz</title><price>4.25</price></book>\n"
        "</catalog>\n";
    CHECK(write_file(xml_path, XML) == 0);

    char yaml[1024];
    char err[512] = {0};
    snprintf(yaml, sizeof yaml,
        "betl: 1\n"
        "name: xml-read-it\n"
        "pipeline:\n"
        "  - id: stage_one\n"
        "    type: dataflow\n"
        "    steps:\n"
        "      - id: source\n"
        "        type: xml.read\n"
        "        path: '%s'\n"
        "        row_xpath: /catalog/book\n"
        "        columns:\n"
        "          isbn:  '@isbn'\n"
        "          title: 'title'\n"
        "          price: 'price'\n"
        "      - id: probe\n"
        "        type: lua.map\n"
        "        from: source\n"
        "        script: |\n"
        "          log.info('xml isbn=' .. tostring(row.isbn) "
        ".. ' title=' .. tostring(row.title) "
        ".. ' price=' .. tostring(row.price))\n"
        "          return row\n"
        "      - id: sink\n"
        "        type: betl.count_rows\n"
        "        from: probe\n"
        "        expect: 3\n",
        xml_path);

    FILE *log = tmpfile();
    int rc = run_pipeline(plugin_path, yaml, err, sizeof err, log);
    if (rc != 0) fprintf(stderr, "xml run failed: %s\n", err);
    CHECK(rc == 0);

    char *txt = slurp_file(log);
    if (txt) {
        CHECK(strstr(txt, "xml isbn=111 title=Foo price=9.99")  != NULL);
        CHECK(strstr(txt, "xml isbn=222 title=Bar price=19.50") != NULL);
        CHECK(strstr(txt, "xml isbn=333 title=Baz price=4.25")  != NULL);
        free(txt);
    }
    fclose(log);
    unlink(xml_path);

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: xml_read integration test passed\n");
    return 0;
}
