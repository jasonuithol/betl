#include "runtime/date_util.h"

#include <stdio.h>     /* snprintf */

int32_t betl_days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)(153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + (int)doe - 719468);
}

void betl_civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = year + (*m <= 2);
}

void betl_split_ts(int64_t us, int32_t *out_days, int64_t *out_us_of_day) {
    int64_t day_us = 86400000000LL;
    int64_t day = us / day_us;
    int64_t r = us % day_us;
    if (r < 0) { r += day_us; --day; }
    *out_days = (int32_t)day;
    *out_us_of_day = r;
}

int betl_parse_iso_date(const char *s, size_t n, int32_t *out_days) {
    if (n != 10) return -1;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        int expect_digit = (i != 4 && i != 7);
        if (expect_digit && !(c >= '0' && c <= '9')) return -1;
        if (!expect_digit && c != '-') return -1;
    }
    int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    unsigned mo = (unsigned)((s[5]-'0')*10 + (s[6]-'0'));
    unsigned dy = (unsigned)((s[8]-'0')*10 + (s[9]-'0'));
    if (mo < 1 || mo > 12 || dy < 1 || dy > 31) return -1;
    *out_days = betl_days_from_civil(y, mo, dy);
    return 0;
}

/* Find the boundary between the seconds (possibly fractional) and an
 * optional tz suffix. Returns the length of the no-tz prefix, plus the
 * offset-from-UTC in micros via *out_offset_us (0 if no tz / UTC). */
static int split_tz(const char *s, size_t n, size_t *out_prefix,
                    int64_t *out_offset_us) {
    /* Walk from the end looking for `Z`, `+` or `-`. The earlier
     * `YYYY-MM-DD` has `-`s but those are within the first 10 chars
     * which we won't touch. */
    *out_offset_us = 0;
    if (n == 0) { *out_prefix = 0; return -1; }
    if (s[n - 1] == 'Z') {
        *out_prefix = n - 1;
        return 0;
    }
    /* Search past position 19 (the seconds index) for + or -. */
    size_t i = (n > 19) ? 19 : n;
    while (i < n) {
        char c = s[i];
        if (c == '+' || c == '-') {
            *out_prefix = i;
            int sign = (c == '+') ? 1 : -1;
            size_t r = n - i - 1;
            int hh = 0, mm = 0;
            if (r == 2) {                                /* +HH */
                if (s[i+1] < '0' || s[i+1] > '9' || s[i+2] < '0' || s[i+2] > '9')
                    return -1;
                hh = (s[i+1]-'0')*10 + (s[i+2]-'0');
            } else if (r == 4) {                          /* +HHMM */
                for (int k = 0; k < 4; ++k)
                    if (s[i+1+k] < '0' || s[i+1+k] > '9') return -1;
                hh = (s[i+1]-'0')*10 + (s[i+2]-'0');
                mm = (s[i+3]-'0')*10 + (s[i+4]-'0');
            } else if (r == 5 && s[i+3] == ':') {        /* +HH:MM */
                if (s[i+1] < '0' || s[i+1] > '9' || s[i+2] < '0' || s[i+2] > '9'
                 || s[i+4] < '0' || s[i+4] > '9' || s[i+5] < '0' || s[i+5] > '9')
                    return -1;
                hh = (s[i+1]-'0')*10 + (s[i+2]-'0');
                mm = (s[i+4]-'0')*10 + (s[i+5]-'0');
            } else {
                return -1;
            }
            if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
            *out_offset_us = (int64_t)sign *
                ((int64_t)hh * 3600000000LL + (int64_t)mm * 60000000LL);
            return 0;
        }
        ++i;
    }
    /* No tz suffix found — treat input as already UTC. */
    *out_prefix = n;
    return 0;
}

int betl_parse_iso_time(const char *s, size_t n, int64_t *out_us) {
    if (n < 8) return -1;
    for (size_t i = 0; i < 8; ++i) {
        char c = s[i];
        int digit = (i != 2 && i != 5);
        if (digit && !(c >= '0' && c <= '9')) return -1;
        if (!digit && c != ':') return -1;
    }
    int hh = (s[0]-'0')*10 + (s[1]-'0');
    int mm = (s[3]-'0')*10 + (s[4]-'0');
    int ss = (s[6]-'0')*10 + (s[7]-'0');
    if (hh > 23 || mm > 59 || ss > 59) return -1;
    int64_t us = (int64_t)hh * 3600000000LL
               + (int64_t)mm *   60000000LL
               + (int64_t)ss *    1000000LL;
    if (n > 8) {
        if (s[8] != '.') return -1;
        if (n > 15) return -1;
        int frac = 0, mult = 100000;
        for (size_t i = 9; i < n; ++i) {
            char c = s[i];
            if (c < '0' || c > '9') return -1;
            frac += (c - '0') * mult;
            mult /= 10;
        }
        us += frac;
    }
    *out_us = us;
    return 0;
}

int betl_format_iso_time(int64_t us_of_day, char *buf, size_t cap) {
    int hh = (int)(us_of_day / 3600000000LL);
    int mm = (int)((us_of_day / 60000000LL) % 60);
    int ss = (int)((us_of_day / 1000000LL) % 60);
    int frac = (int)(us_of_day % 1000000LL);
    int n;
    if (frac == 0) {
        n = snprintf(buf, cap, "%02d:%02d:%02d", hh, mm, ss);
    } else {
        n = snprintf(buf, cap, "%02d:%02d:%02d.%06d", hh, mm, ss, frac);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

int betl_parse_iso_tstz(const char *s, size_t n, int64_t *out_us) {
    size_t prefix = 0;
    int64_t offset_us = 0;
    if (split_tz(s, n, &prefix, &offset_us) != 0) return -1;
    int64_t local_us;
    if (betl_parse_iso_ts(s, prefix, &local_us) != 0) return -1;
    /* Local-with-offset → UTC: subtract the offset. */
    *out_us = local_us - offset_us;
    return 0;
}

int betl_parse_iso_ts(const char *s, size_t n, int64_t *out_us) {
    if (n < 19) return -1;
    int32_t days;
    if (betl_parse_iso_date(s, 10, &days) != 0) return -1;
    if (s[10] != ' ' && s[10] != 'T') return -1;
    for (size_t i = 11; i < 19; ++i) {
        char c = s[i];
        int expect_digit = (i != 13 && i != 16);
        if (expect_digit && !(c >= '0' && c <= '9')) return -1;
        if (!expect_digit && c != ':') return -1;
    }
    int hh = (s[11]-'0')*10 + (s[12]-'0');
    int mm = (s[14]-'0')*10 + (s[15]-'0');
    int ss = (s[17]-'0')*10 + (s[18]-'0');
    if (hh > 23 || mm > 59 || ss > 59) return -1;
    int64_t us_of_day = (int64_t)hh * 3600000000LL
                      + (int64_t)mm * 60000000LL
                      + (int64_t)ss * 1000000LL;
    if (n > 19) {
        if (s[19] != '.') return -1;
        if (n > 26) return -1;
        int frac = 0; int mult = 100000;
        for (size_t i = 20; i < n; ++i) {
            char c = s[i];
            if (c < '0' || c > '9') return -1;
            frac += (c - '0') * mult;
            mult /= 10;
        }
        us_of_day += frac;
    }
    *out_us = (int64_t)days * 86400000000LL + us_of_day;
    return 0;
}
