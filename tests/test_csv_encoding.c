/* csv.read / csv.write with non-UTF-8 source encodings.
 *
 * Covers the SSIS-parity codepage path: CSVs produced by Windows tools
 * (Excel, Notepad with default ANSI, legacy POS exports) often use
 * cp1252 / cp1250 / utf-16 instead of UTF-8. betl's runtime is canonical
 * UTF-8 everywhere, so the only place encoding lives is at csv.read /
 * csv.write boundaries via the `encoding:` parameter.
 *
 * Cases:
 *   1. csv.read encoding=cp1252 — diacritics decode to UTF-8 internally,
 *      and a downstream csv.write produces a UTF-8 file with the right
 *      multi-byte sequences.
 *   2. csv.read with a UTF-8 BOM at the start of the file — BOM is
 *      consumed silently, downstream sees clean UTF-8.
 *   3. csv.read encoding=utf-16 — UTF-16-LE-with-BOM input lands as
 *      UTF-8 inside the pipeline.
 *   4. csv.write encoding=cp1252 — UTF-8 batch produces a cp1252 file
 *      whose bytes match the SSIS-expected encoding.
 *   5. csv.write encoding=cp1252 with a character outside cp1252's
 *      repertoire (e.g. an em dash isn't representable in 1252 but is)
 *      — verify the substitution fallback produces a '?'.
 *   6. Unknown encoding rejected with a clear error message.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "betl/provider.h"
#include "loader/registry.h"
#include "pipeline/pipeline.h"
#include "runtime/builtins.h"
#include "runtime/context.h"
#include "runtime/exec.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

static int write_bytes(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rc = (fwrite(data, 1, n, f) == n) ? 0 : -1;
    fclose(f);
    return rc;
}

static int slurp_bytes(const char *path, unsigned char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    unsigned char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    *out = buf;
    *out_len = n;
    return 0;
}

static int run_yaml(const char *yaml, char *err, size_t err_cap) {
    char path[64];
    snprintf(path, sizeof path, "/tmp/betl-csvenc-%d.yml", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (!f) return BETL_ERR_IO;
    fputs(yaml, f);
    fclose(f);
    char p_err[1024] = {0};
    BetlPipeline *p = betl_pipeline_load(path, p_err, sizeof p_err);
    if (!p) {
        if (err) snprintf(err, err_cap, "%s", p_err);
        unlink(path);
        return BETL_ERR_INVALID;
    }
    BetlContext  *ctx = betl_context_create();
    BetlRegistry *reg = betl_registry_create();
    int rc = BETL_ERR_INTERNAL;
    if (ctx && reg && betl_register_builtins(reg) == BETL_OK) {
        rc = betl_run(ctx, reg, p);
        if (rc != BETL_OK && err) {
            snprintf(err, err_cap, "%s", betl_context_last_error(ctx));
        }
    }
    if (p)   betl_pipeline_free(p);
    if (reg) betl_registry_destroy(reg);
    if (ctx) betl_context_destroy(ctx);
    unlink(path);
    return rc;
}

static void unique_path(char *buf, size_t cap, const char *tag) {
    snprintf(buf, cap, "/tmp/betl-csvenc-%d-%s.csv", (int)getpid(), tag);
}

int main(void) {
    /* --- 1. cp1252 source: read a file with diacritics, write UTF-8.
     * The cp1252 bytes 0xE9 0xFC translate to U+00E9 (é) U+00FC (ü);
     * UTF-8 encodes those as C3 A9 / C3 BC. ----------------------------- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "cp1252-in");
        unique_path(out_path, sizeof out_path, "cp1252-out");
        /* Header row + one data row, all in cp1252. "name" is ASCII; the
         * data column has "café" and "Müller". */
        const unsigned char input[] = {
            'i','d',',','n','a','m','e','\n',
            '1',',','c','a','f',0xE9,'\n',
            '2',',','M',0xFC,'l','l','e','r','\n',
        };
        CHECK(write_bytes(in_path, input, sizeof input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvenc-cp1252\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        encoding: cp1252\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: name, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "cp1252 case: %s\n", err);
        CHECK(rc == BETL_OK);
        unsigned char *got = NULL; size_t got_len = 0;
        CHECK(slurp_bytes(out_path, &got, &got_len) == 0);
        if (got) {
            const unsigned char expected[] = {
                'i','d',',','n','a','m','e','\n',
                '1',',','c','a','f',0xC3,0xA9,'\n',           /* "café" UTF-8 */
                '2',',','M',0xC3,0xBC,'l','l','e','r','\n',   /* "Müller" UTF-8 */
            };
            CHECK(got_len == sizeof expected);
            CHECK(got_len == sizeof expected
                  && memcmp(got, expected, sizeof expected) == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 2. UTF-8 BOM at the start of the file is consumed silently
     * (Excel-style export). No `encoding:` set — default UTF-8 path. ---- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "bom-in");
        unique_path(out_path, sizeof out_path, "bom-out");
        const unsigned char input[] = {
            0xEF,0xBB,0xBF,            /* UTF-8 BOM */
            'i','d',',','n','a','m','e','\n',
            '1',',','c','a','f',0xC3,0xA9,'\n',
        };
        CHECK(write_bytes(in_path, input, sizeof input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvenc-bom\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: name, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "bom case: %s\n", err);
        CHECK(rc == BETL_OK);
        unsigned char *got = NULL; size_t got_len = 0;
        CHECK(slurp_bytes(out_path, &got, &got_len) == 0);
        if (got) {
            /* BOM stripped; header column name is "id" not "﻿id". */
            const unsigned char expected[] = {
                'i','d',',','n','a','m','e','\n',
                '1',',','c','a','f',0xC3,0xA9,'\n',
            };
            CHECK(got_len == sizeof expected
                  && memcmp(got, expected, sizeof expected) == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 3. UTF-16-LE source: iconv "UTF-16" auto-detects the BOM and
     * picks endianness. The file has FF FE BOM + UTF-16-LE bytes. ------- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "utf16-in");
        unique_path(out_path, sizeof out_path, "utf16-out");
        /* Build "id,name\n1,café\n" as UTF-16-LE with BOM. */
        const unsigned char input[] = {
            0xFF,0xFE,                                   /* UTF-16-LE BOM */
            'i',0x00, 'd',0x00, ',',0x00,
            'n',0x00, 'a',0x00, 'm',0x00, 'e',0x00,
            '\n',0x00,
            '1',0x00, ',',0x00,
            'c',0x00, 'a',0x00, 'f',0x00, 0xE9,0x00,     /* é = U+00E9 */
            '\n',0x00,
        };
        CHECK(write_bytes(in_path, input, sizeof input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvenc-utf16\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        encoding: utf-16\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: name, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "utf16 case: %s\n", err);
        CHECK(rc == BETL_OK);
        unsigned char *got = NULL; size_t got_len = 0;
        CHECK(slurp_bytes(out_path, &got, &got_len) == 0);
        if (got) {
            const unsigned char expected[] = {
                'i','d',',','n','a','m','e','\n',
                '1',',','c','a','f',0xC3,0xA9,'\n',
            };
            CHECK(got_len == sizeof expected
                  && memcmp(got, expected, sizeof expected) == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 4. csv.write encoding=cp1252: write a UTF-8 batch and verify
     * the on-disk bytes are in cp1252 (single-byte 0xE9 for é). --------- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "cp1252-w-in");
        unique_path(out_path, sizeof out_path, "cp1252-w-out");
        /* Source file is already UTF-8. */
        const unsigned char input[] = {
            'i','d',',','n','a','m','e','\n',
            '1',',','c','a','f',0xC3,0xA9,'\n',
        };
        CHECK(write_bytes(in_path, input, sizeof input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvenc-cp1252-write\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: name, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n"
            "        encoding: cp1252\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "cp1252-write case: %s\n", err);
        CHECK(rc == BETL_OK);
        unsigned char *got = NULL; size_t got_len = 0;
        CHECK(slurp_bytes(out_path, &got, &got_len) == 0);
        if (got) {
            const unsigned char expected[] = {
                'i','d',',','n','a','m','e','\n',
                '1',',','c','a','f',0xE9,'\n',           /* cp1252 single-byte */
            };
            CHECK(got_len == sizeof expected
                  && memcmp(got, expected, sizeof expected) == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 5. csv.write encoding=cp1252 with an unrepresentable char.
     * U+20AC (€) and U+2014 (em dash) are in cp1252 (0x80 and 0x97
     * respectively), but U+2603 (snowman ☃) is NOT. The writer should
     * substitute '?' for it. ------------------------------------------- */
    {
        char in_path[64], out_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "cp1252-sub-in");
        unique_path(out_path, sizeof out_path, "cp1252-sub-out");
        /* Input is UTF-8: "id,note\n1,sun☃rise\n". ☃ = E2 98 83. */
        const unsigned char input[] = {
            'i','d',',','n','o','t','e','\n',
            '1',',','s','u','n',0xE2,0x98,0x83,'r','i','s','e','\n',
        };
        CHECK(write_bytes(in_path, input, sizeof input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvenc-cp1252-sub\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id,   type: int64 }\n"
            "            - { name: note, type: utf8  }\n"
            "      - id: sink\n"
            "        type: csv.write\n"
            "        from: source\n"
            "        path: %s\n"
            "        encoding: cp1252\n", in_path, out_path);
        int rc = run_yaml(yaml, err, sizeof err);
        if (rc != BETL_OK) fprintf(stderr, "cp1252-sub case: %s\n", err);
        CHECK(rc == BETL_OK);
        unsigned char *got = NULL; size_t got_len = 0;
        CHECK(slurp_bytes(out_path, &got, &got_len) == 0);
        if (got) {
            const unsigned char expected[] = {
                'i','d',',','n','o','t','e','\n',
                '1',',','s','u','n','?','r','i','s','e','\n',
            };
            CHECK(got_len == sizeof expected
                  && memcmp(got, expected, sizeof expected) == 0);
            free(got);
        }
        unlink(in_path);
        unlink(out_path);
    }

    /* --- 6. Unknown encoding rejected with a clear error message. ---- */
    {
        char in_path[64], yaml[1024], err[512] = {0};
        unique_path(in_path,  sizeof in_path,  "badenc-in");
        const unsigned char input[] = { 'i','d','\n','1','\n' };
        CHECK(write_bytes(in_path, input, sizeof input) == 0);
        snprintf(yaml, sizeof yaml,
            "betl: 1\n"
            "name: csvenc-bad\n"
            "pipeline:\n"
            "  - id: stage_one\n"
            "    type: dataflow\n"
            "    steps:\n"
            "      - id: source\n"
            "        type: csv.read\n"
            "        path: %s\n"
            "        encoding: not-a-real-encoding\n"
            "        schema:\n"
            "          columns:\n"
            "            - { name: id, type: int64 }\n"
            "      - id: sink\n"
            "        type: betl.count_rows\n"
            "        from: source\n"
            "        expect: 1\n", in_path);
        int rc = run_yaml(yaml, err, sizeof err);
        CHECK(rc != BETL_OK);
        CHECK(strstr(err, "encoding") != NULL
              || strstr(err, "not-a-real-encoding") != NULL);
        unlink(in_path);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: csv encoding tests passed\n");
    return 0;
}
