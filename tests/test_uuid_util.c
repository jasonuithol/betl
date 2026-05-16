/* Boundary unit tests for uuid_util.
 *
 * Covers: canonical 36-char form, lower/upper hex, missing dashes,
 * wrong length, non-hex chars, the all-zeros and all-FF edge values,
 * and tight cap overflow on format. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "runtime/uuid_util.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

int main(void) {
    /* ---- canonical round-trip (lower-case) --------------------------- */
    {
        const char *src = "550e8400-e29b-41d4-a716-446655440000";
        uint8_t bytes[16] = {0};
        CHECK(betl_uuid_parse(src, 36, bytes) == 0);
        /* Verify byte 0 (first hex pair) decodes to 0x55. */
        CHECK(bytes[0] == 0x55);
        CHECK(bytes[15] == 0x00);
        char back[36];
        CHECK(betl_uuid_format(bytes, back, sizeof back) == 36);
        CHECK(memcmp(back, src, 36) == 0);
    }

    /* ---- upper-case hex accepted on parse, lower-case on format ----- */
    {
        const char *src = "AABBCCDD-EEFF-0011-2233-445566778899";
        uint8_t bytes[16] = {0};
        CHECK(betl_uuid_parse(src, 36, bytes) == 0);
        char back[36];
        CHECK(betl_uuid_format(bytes, back, sizeof back) == 36);
        /* Format always returns lower-case. */
        CHECK(memcmp(back,
                     "aabbccdd-eeff-0011-2233-445566778899", 36) == 0);
    }

    /* ---- all-zero is valid ------------------------------------------ */
    {
        const char *src = "00000000-0000-0000-0000-000000000000";
        uint8_t bytes[16] = {0xff};   /* poison */
        CHECK(betl_uuid_parse(src, 36, bytes) == 0);
        for (int i = 0; i < 16; ++i) CHECK(bytes[i] == 0);
    }

    /* ---- all-FF round-trips ----------------------------------------- */
    {
        uint8_t bytes[16];
        memset(bytes, 0xff, 16);
        char back[36];
        CHECK(betl_uuid_format(bytes, back, sizeof back) == 36);
        CHECK(memcmp(back, "ffffffff-ffff-ffff-ffff-ffffffffffff", 36) == 0);
        uint8_t roundtrip[16] = {0};
        CHECK(betl_uuid_parse(back, 36, roundtrip) == 0);
        CHECK(memcmp(bytes, roundtrip, 16) == 0);
    }

    /* ---- wrong length rejected -------------------------------------- */
    {
        uint8_t bytes[16];
        CHECK(betl_uuid_parse("12345", 5, bytes) == -1);
        CHECK(betl_uuid_parse("550e8400-e29b-41d4-a716-446655440000-extra",
                              41, bytes) == -1);
    }

    /* ---- missing dashes rejected ------------------------------------ */
    {
        uint8_t bytes[16];
        /* 32 hex digits, no dashes, padded with garbage to make len 36. */
        CHECK(betl_uuid_parse("550e8400e29b41d4a716446655440000aaaa",
                              36, bytes) == -1);
    }

    /* ---- dashes in wrong positions rejected ------------------------- */
    {
        uint8_t bytes[16];
        /* Dashes at the wrong indices. */
        CHECK(betl_uuid_parse("5-50e8400e29b-41d4-a716-446655440000",
                              36, bytes) == -1);
    }

    /* ---- non-hex chars rejected ------------------------------------- */
    {
        uint8_t bytes[16];
        CHECK(betl_uuid_parse("zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz",
                              36, bytes) == -1);
    }

    /* ---- format cap < 36 rejected ----------------------------------- */
    {
        uint8_t bytes[16] = {0};
        char tiny[35];
        CHECK(betl_uuid_format(bytes, tiny, sizeof tiny) == -1);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: uuid_util unit tests passed\n");
    return 0;
}
