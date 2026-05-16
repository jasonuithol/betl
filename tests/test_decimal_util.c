/* Boundary unit tests for decimal_util.
 *
 * Covers parse/format round-trips, negative values, zero, exact-fraction
 * preservation at multiple scales, parser rejection of malformed input,
 * and the to_double lossy conversion. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/decimal_util.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

int main(void) {
    /* ---- "123.45" parses as 12345 at scale=2 ------------------------ */
    {
        betl_dec128 v = 0;
        CHECK(betl_dec128_parse("123.45", 6, 2, &v) == 0);
        CHECK(v == 12345);
        char buf[44];
        int n = betl_dec128_format(v, 2, buf, sizeof buf);
        CHECK(n > 0);
        buf[n] = '\0';
        CHECK(strcmp(buf, "123.45") == 0);
    }

    /* ---- negative round-trip ---------------------------------------- */
    {
        betl_dec128 v = 0;
        CHECK(betl_dec128_parse("-7.50", 5, 2, &v) == 0);
        CHECK(v == -750);
        char buf[44];
        int n = betl_dec128_format(v, 2, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(strcmp(buf, "-7.50") == 0);
    }

    /* ---- "0" round-trips at any scale ------------------------------- */
    {
        betl_dec128 v = 0;
        CHECK(betl_dec128_parse("0", 1, 4, &v) == 0);
        CHECK(v == 0);
        char buf[44];
        int n = betl_dec128_format(v, 4, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(strcmp(buf, "0.0000") == 0);
    }

    /* ---- integer parses with zero-padding to scale ------------------ */
    {
        betl_dec128 v = 0;
        CHECK(betl_dec128_parse("42", 2, 3, &v) == 0);
        CHECK(v == 42000);
        char buf[44];
        int n = betl_dec128_format(v, 3, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(strcmp(buf, "42.000") == 0);
    }

    /* ---- format always zero-pads to the requested scale ------------- */
    {
        char buf[44];
        int n = betl_dec128_format(5, 4, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(strcmp(buf, "0.0005") == 0);
    }

    /* ---- malformed input rejected ----------------------------------- */
    {
        betl_dec128 v = 0;
        const char *bad[] = {
            "",                  /* empty */
            "abc",               /* non-numeric */
            "1.2.3",             /* two dots */
            "1e2",               /* exponent (docs say rejected) */
            " 1.2",              /* leading space */
            "1.2 ",              /* trailing space */
            "+",                 /* sign only */
            "-",                 /* sign only */
            ".",                 /* dot only */
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            CHECK(betl_dec128_parse(bad[i], strlen(bad[i]), 2, &v) == -1);
        }
    }

    /* ---- buffer too small returns -1 -------------------------------- */
    {
        char tiny[3];   /* enough only for "0" + NUL, not for "0.00" */
        int n = betl_dec128_format(0, 2, tiny, sizeof tiny);
        CHECK(n == -1);
    }

    /* ---- to_double on canonical values ------------------------------ */
    {
        /* 12345 at scale=2 should give exactly 123.45 (double precision
         * comfortable for this magnitude). */
        double d = betl_dec128_to_double(12345, 2);
        CHECK(d > 123.44 && d < 123.46);
        double d_neg = betl_dec128_to_double(-12345, 2);
        CHECK(d_neg < -123.44 && d_neg > -123.46);
        CHECK(betl_dec128_to_double(0, 0) == 0.0);
    }

    /* ---- large precision works (decimal(38, 4) shape) --------------- */
    {
        /* 999999999999999999999999999999.1234 — 30-digit integer part
         * with 4 fractional digits.  Stored as
         * 9999999999999999999999999999991234 (34 digits, fits in int128). */
        const char *src = "999999999999999999999999999999.1234";
        betl_dec128 v = 0;
        CHECK(betl_dec128_parse(src, strlen(src), 4, &v) == 0);
        char buf[44];
        int n = betl_dec128_format(v, 4, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(strcmp(buf, src) == 0);
    }

    /* ---- value at scale 0 has no decimal point ---------------------- */
    {
        char buf[44];
        int n = betl_dec128_format(42, 0, buf, sizeof buf);
        buf[n] = '\0';
        CHECK(strcmp(buf, "42") == 0);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: decimal_util unit tests passed\n");
    return 0;
}
