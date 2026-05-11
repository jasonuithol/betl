#ifndef BETL_RUNTIME_DECIMAL_UTIL_H
#define BETL_RUNTIME_DECIMAL_UTIL_H

/* Helpers for Arrow decimal128(precision, scale) columns.
 *
 * Storage: int128 value with an associated `scale` (the integer is the
 * cents-style scaled integer; e.g. 12345 with scale=2 is 123.45).
 * Precision is metadata for I/O — the value field itself doesn't
 * track it.
 *
 * We use GCC's __int128 extension, which works on every C compiler
 * betl currently builds with (gcc 13, clang). Windows MSVC will need
 * an `_int128` shim if we ever ship there. */

#include <stdint.h>
#include <stddef.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
typedef __int128 betl_dec128;
#pragma GCC diagnostic pop

/* Parse a decimal text representation ("123.45", "-7", "12345e-2") into
 * a fixed-scale int128. The output is scaled so that the integer is
 * value × 10^scale.
 *
 * Returns 0 on success, -1 on parse error or overflow. Trailing whitespace
 * and exponents (`e`/`E`) are rejected — we expect well-formed numeric
 * text from libpq / ODBC / CSV. */
int betl_dec128_parse(const char *s, size_t n, int scale, betl_dec128 *out);

/* Format a (value, scale) decimal as text into `buf`. The result never
 * contains an exponent and always has exactly `scale` fractional digits
 * (zero-padded if needed). Returns the number of bytes written (without
 * NUL), or -1 on overflow.
 *
 * The buffer must be at least 44 bytes to hold any int128 with up to
 * 38 digits + sign + decimal point + NUL. */
int betl_dec128_format(betl_dec128 v, int scale, char *buf, size_t cap);

/* Lossy conversion for casts to DT_R8 / display. */
double betl_dec128_to_double(betl_dec128 v, int scale);

#endif
