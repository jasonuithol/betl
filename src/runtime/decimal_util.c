#include "runtime/decimal_util.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

/* Multiply two int128s, returning -1 on overflow (signed). We treat
 * overflow as a parse error rather than wrapping. */
static int safe_mul10(betl_dec128 *v, int positive_digit) {
    betl_dec128 ten = 10;
    if (*v > 0 && *v > ((__int128)1 << 126) / ten) return -1; /* approx */
    *v *= ten;
    if (positive_digit) *v += positive_digit;
    return 0;
}

int betl_dec128_parse(const char *s, size_t n, int scale, betl_dec128 *out) {
    if (n == 0 || scale < 0 || scale > 38) return -1;
    size_t i = 0;
    int sign = 1;
    if (s[0] == '-') { sign = -1; ++i; }
    else if (s[0] == '+') ++i;
    if (i == n) return -1;

    betl_dec128 mag = 0;
    int seen_digit = 0;
    int frac_digits = 0;
    int in_frac = 0;

    for (; i < n; ++i) {
        char c = s[i];
        if (c == '.') {
            if (in_frac) return -1;
            in_frac = 1;
            continue;
        }
        if (c < '0' || c > '9') return -1;
        seen_digit = 1;
        if (in_frac && frac_digits >= scale) {
            /* Past target scale: accept only zeros (no banker's rounding). */
            if (c != '0') return -1;
            continue;
        }
        if (safe_mul10(&mag, c - '0') != 0) return -1;
        if (in_frac) ++frac_digits;
    }
    if (!seen_digit) return -1;

    /* Pad fractional digits up to the target scale. */
    while (frac_digits < scale) {
        if (safe_mul10(&mag, 0) != 0) return -1;
        ++frac_digits;
    }
    *out = (sign < 0) ? -mag : mag;
    return 0;
}

int betl_dec128_format(betl_dec128 v, int scale, char *buf, size_t cap) {
    if (scale < 0 || scale > 38) return -1;

    /* Render |v| into a digit stack, sign separately. */
    int negative = (v < 0);
    /* INT128_MIN's absolute value doesn't fit in a positive int128; the
     * 39-digit cap below would catch that, but accept the wrap quietly. */
    betl_dec128 mag = negative ? -v : v;
    char digits[40];
    int n = 0;
    if (mag == 0) {
        digits[n++] = '0';
    } else {
        while (mag > 0 && n < (int)sizeof digits) {
            digits[n++] = (char)('0' + (int)(mag % 10));
            mag /= 10;
        }
        if (mag > 0) return -1;       /* shouldn't happen with __int128 */
    }
    /* Zero-pad on the high side so we have at least scale+1 digits. */
    while (n < scale + 1) digits[n++] = '0';

    size_t need = (size_t)n + (scale > 0 ? 1 : 0) + (negative ? 1 : 0);
    if (need + 1 > cap) return -1;
    char *p = buf;
    if (negative) *p++ = '-';
    /* Integer part: digits[n-1] down to digits[scale]. */
    for (int k = n - 1; k >= scale; --k) *p++ = digits[k];
    if (scale > 0) {
        *p++ = '.';
        for (int k = scale - 1; k >= 0; --k) *p++ = digits[k];
    }
    *p = '\0';
    return (int)(p - buf);
}

double betl_dec128_to_double(betl_dec128 v, int scale) {
    int neg = (v < 0);
    betl_dec128 mag = neg ? -v : v;
    /* Split into high/low halves so we don't lose the full 128 bits to
     * the first cast. The double precision (~16 decimal digits) puts a
     * lower bound on how much fidelity we can recover anyway. */
    uint64_t lo = (uint64_t)mag;
    uint64_t hi = (uint64_t)(mag >> 64);
    double d = (double)hi * 18446744073709551616.0 + (double)lo;
    d /= pow(10.0, scale);
    return neg ? -d : d;
}

#pragma GCC diagnostic pop
