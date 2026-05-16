/* Boundary unit tests for date_util.
 *
 * These exercise the helpers directly rather than through a pipeline,
 * to catch corner cases (leap years, far-past / far-future dates,
 * negative civil years, malformed ISO strings, timezone-offset
 * normalisation) that end-to-end tests would only hit if a recipe
 * happened to use those inputs. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "runtime/date_util.h"

static int failures = 0;

#define CHECK(cond) do {                                        \
    if (!(cond)) {                                              \
        fprintf(stderr, "FAIL %s:%d: %s\n",                     \
                __FILE__, __LINE__, #cond);                     \
        failures++;                                             \
    }                                                           \
} while (0)

int main(void) {
    /* ---- civil ↔ days round-trip on key anchor dates ----------------- */
    {
        struct { int y; unsigned m; unsigned d; int32_t expected_days; } cases[] = {
            { 1970,  1,  1,        0 },     /* epoch */
            { 1969, 12, 31,       -1 },     /* day before epoch */
            { 2000,  2, 29,    11016 },     /* leap day */
            { 2024,  2, 29,    19782 },     /* recent leap day */
            { 1900,  2, 28,   -25509 },     /* 1900 was NOT a leap year */
            {    1,  1,  1,  -719162 },     /* proleptic year 1 */
            { 9999, 12, 31,  2932896 },     /* far future */
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            int32_t got = betl_days_from_civil(cases[i].y, cases[i].m, cases[i].d);
            CHECK(got == cases[i].expected_days);
            int yy = 0; unsigned mm = 0, dd = 0;
            betl_civil_from_days(got, &yy, &mm, &dd);
            CHECK(yy == cases[i].y && mm == cases[i].m && dd == cases[i].d);
        }
    }

    /* ---- 1900 is NOT a leap year (divisible by 100 but not 400) ------ */
    {
        /* March 1, 1900 should be exactly 1 day after Feb 28, 1900. */
        int32_t feb28 = betl_days_from_civil(1900, 2, 28);
        int32_t mar01 = betl_days_from_civil(1900, 3, 1);
        CHECK(mar01 - feb28 == 1);
    }

    /* ---- 2000 IS a leap year (divisible by 400) ---------------------- */
    {
        int32_t feb28 = betl_days_from_civil(2000, 2, 28);
        int32_t mar01 = betl_days_from_civil(2000, 3, 1);
        CHECK(mar01 - feb28 == 2);   /* Feb 29 sits between */
    }

    /* ---- ISO date parser: well-formed input -------------------------- */
    {
        int32_t days = 0;
        const char *s = "2026-05-16";
        CHECK(betl_parse_iso_date(s, strlen(s), &days) == 0);
        int y; unsigned m, d;
        betl_civil_from_days(days, &y, &m, &d);
        CHECK(y == 2026 && m == 5 && d == 16);
    }

    /* ---- ISO date parser: malformed input is rejected ---------------- */
    {
        int32_t days = 0;
        /* Parser is strict about format/separators but lenient about
         * day-of-month validity (2026-02-30 is accepted as March 2 via
         * civil overflow). So we test only structural rejection here. */
        const char *bad[] = {
            "",                  /* empty */
            "not-a-date",        /* garbage */
            "2026-13-01",        /* invalid month */
            "2026-5-1",          /* missing zero padding (wrong length) */
            "2026/05/16",        /* wrong separator */
            "20260516",          /* no separators (wrong length) */
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
            int rc = betl_parse_iso_date(bad[i], strlen(bad[i]), &days);
            CHECK(rc == -1);
        }
    }

    /* ---- ISO timestamp parser: well-formed input --------------------- */
    {
        int64_t us = 0;
        const char *s = "2026-05-16 12:34:56.789012";
        CHECK(betl_parse_iso_ts(s, strlen(s), &us) == 0);
        int32_t days = 0; int64_t us_of_day = 0;
        betl_split_ts(us, &days, &us_of_day);
        CHECK(us_of_day == ((int64_t)12 * 3600 + 34 * 60 + 56) * 1000000
              + 789012);
    }

    /* ---- ISO timestamp with 'T' separator works ---------------------- */
    {
        int64_t us = 0;
        const char *s = "2026-05-16T00:00:00";
        CHECK(betl_parse_iso_ts(s, strlen(s), &us) == 0);
        int32_t days = 0; int64_t us_of_day = 0;
        betl_split_ts(us, &days, &us_of_day);
        CHECK(us_of_day == 0);
    }

    /* ---- ISO timestamp with sub-microsecond fractions truncate ------ */
    {
        int64_t us = 0;
        const char *s = "2026-05-16 00:00:00.1";        /* 1 digit fraction */
        CHECK(betl_parse_iso_ts(s, strlen(s), &us) == 0);
        int32_t days = 0; int64_t us_of_day = 0;
        betl_split_ts(us, &days, &us_of_day);
        /* ".1" means 100000 micros, not 1 micro. */
        CHECK(us_of_day == 100000);
    }

    /* ---- tstz: offset normalises to UTC ------------------------------ */
    {
        int64_t utc_micro = 0, plus2_micro = 0;
        const char *u = "2026-05-16 12:00:00Z";
        const char *p = "2026-05-16 14:00:00+02:00";
        CHECK(betl_parse_iso_tstz(u, strlen(u), &utc_micro)  == 0);
        CHECK(betl_parse_iso_tstz(p, strlen(p), &plus2_micro) == 0);
        /* +02:00 at 14:00 should map to the same UTC micro as 12:00Z. */
        CHECK(utc_micro == plus2_micro);
    }

    /* ---- tstz: HHMM, +HH, -HH all accepted --------------------------- */
    {
        int64_t a, b, c;
        const char *s1 = "2026-05-16 12:00:00+0000";
        const char *s2 = "2026-05-16 12:00:00+00";
        const char *s3 = "2026-05-16 07:00:00-05:00";
        CHECK(betl_parse_iso_tstz(s1, strlen(s1), &a) == 0);
        CHECK(betl_parse_iso_tstz(s2, strlen(s2), &b) == 0);
        CHECK(betl_parse_iso_tstz(s3, strlen(s3), &c) == 0);
        CHECK(a == b);
        CHECK(a == c);     /* 12:00 UTC == 07:00 -05:00 */
    }

    /* ---- time of day: parse round-trips through format --------------- */
    {
        int64_t us = 0;
        const char *s = "23:59:59.999999";
        CHECK(betl_parse_iso_time(s, strlen(s), &us) == 0);
        char buf[32];
        int n = betl_format_iso_time(us, buf, sizeof buf);
        CHECK(n > 0);
        CHECK(strncmp(buf, "23:59:59.999999", (size_t)n) == 0);
    }

    /* ---- time of day: 24:00:00 is rejected --------------------------- */
    {
        int64_t us = 0;
        CHECK(betl_parse_iso_time("24:00:00", 8, &us) == -1);
    }

    /* ---- format_iso_time: tiny buffer returns -1 --------------------- */
    {
        char tiny[4];
        int n = betl_format_iso_time(0, tiny, sizeof tiny);
        CHECK(n == -1);
    }

    /* ---- split_ts: floor-division for negative microseconds ---------- */
    {
        /* 1969-12-31 23:59:59.999999 is one microsecond before epoch.
         * split_ts must give (days=-1, us_of_day=86399999999), not
         * (days=0, us_of_day=-1). */
        int32_t days = 0; int64_t us_of_day = 0;
        betl_split_ts((int64_t)-1, &days, &us_of_day);
        CHECK(days == -1);
        CHECK(us_of_day == 86400000000LL - 1);
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ok: date_util unit tests passed\n");
    return 0;
}
