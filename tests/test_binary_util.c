/* Boundary unit tests for binary_util (hex encode/decode).
 *
 * Covers: empty input, odd-length input, non-hex chars, buffer cap
 * overflow, lower/upper case digits, full byte range round-trip. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "runtime/binary_util.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

int main(void) {
    /* ---- empty input round-trips to empty output --------------------- */
    {
        uint8_t out[1];
        CHECK(betl_hex_decode("", 0, out, sizeof out) == 0);
        char hex[1];
        CHECK(betl_hex_encode((const uint8_t*)"", 0, hex, sizeof hex) == 0);
    }

    /* ---- canonical "deadbeef" round-trip ----------------------------- */
    {
        uint8_t bytes[4] = {0};
        int n = betl_hex_decode("deadbeef", 8, bytes, sizeof bytes);
        CHECK(n == 4);
        CHECK(bytes[0] == 0xde && bytes[1] == 0xad
              && bytes[2] == 0xbe && bytes[3] == 0xef);
        char hex[8];
        CHECK(betl_hex_encode(bytes, 4, hex, sizeof hex) == 8);
        CHECK(memcmp(hex, "deadbeef", 8) == 0);
    }

    /* ---- uppercase hex digits accepted on decode --------------------- */
    {
        uint8_t bytes[2] = {0};
        int n = betl_hex_decode("CAFE", 4, bytes, sizeof bytes);
        CHECK(n == 2);
        CHECK(bytes[0] == 0xca && bytes[1] == 0xfe);
    }

    /* ---- odd-length input rejected ----------------------------------- */
    {
        uint8_t bytes[4] = {0};
        CHECK(betl_hex_decode("abc", 3, bytes, sizeof bytes) == -1);
    }

    /* ---- non-hex chars rejected -------------------------------------- */
    {
        uint8_t bytes[2] = {0};
        CHECK(betl_hex_decode("xx", 2, bytes, sizeof bytes) == -1);
        CHECK(betl_hex_decode("0g", 2, bytes, sizeof bytes) == -1);
        CHECK(betl_hex_decode("0 ", 2, bytes, sizeof bytes) == -1);
    }

    /* ---- output capacity overflow rejected --------------------------- */
    {
        uint8_t tiny[1];
        CHECK(betl_hex_decode("abcd", 4, tiny, sizeof tiny) == -1);
        char hex_tiny[3];
        CHECK(betl_hex_encode((const uint8_t*)"\xab\xcd", 2,
                              hex_tiny, sizeof hex_tiny) == -1);
    }

    /* ---- full-byte-range round-trip ---------------------------------- */
    {
        uint8_t bytes[256];
        for (int i = 0; i < 256; ++i) bytes[i] = (uint8_t)i;
        char hex[512];
        CHECK(betl_hex_encode(bytes, 256, hex, sizeof hex) == 512);
        uint8_t roundtrip[256] = {0};
        CHECK(betl_hex_decode(hex, 512, roundtrip, sizeof roundtrip) == 256);
        CHECK(memcmp(bytes, roundtrip, 256) == 0);
    }

    /* ---- encoder emits lower-case --------------------------------- */
    {
        char hex[2];
        CHECK(betl_hex_encode((const uint8_t*)"\xab", 1, hex, sizeof hex) == 2);
        CHECK(hex[0] == 'a' && hex[1] == 'b');
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: binary_util unit tests passed\n");
    return 0;
}
