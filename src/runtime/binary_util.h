#ifndef BETL_RUNTIME_BINARY_UTIL_H
#define BETL_RUNTIME_BINARY_UTIL_H

/* Helpers for Arrow binary columns (Arrow leaf format `z`).
 *
 * Storage shape mirrors utf8: validity bitmap + int32 offsets +
 * concatenated byte blob. These helpers handle the text encodings we
 * trade in across providers:
 *
 *   - CSV / generic transport: bare lower-case hex, e.g. "deadbeef".
 *   - Postgres BYTEA (text mode): "\xHEX" prefix form (the default
 *     `bytea_output = hex` since pg 9.0).
 */

#include <stdint.h>
#include <stddef.h>

/* Parse `n` hex digits into `cap` bytes of `out`. Returns the byte
 * count on success, -1 if `n` is odd, contains a non-hex char, or
 * would exceed `cap`. */
int betl_hex_decode(const char *s, size_t n, uint8_t *out, size_t cap);

/* Format `n` bytes of `in` into 2*n lower-case hex chars at `out`.
 * `out` is NOT NUL-terminated. Returns 2*n on success, -1 if
 * `cap < 2*n`. */
int betl_hex_encode(const uint8_t *in, size_t n, char *out, size_t cap);

#endif
