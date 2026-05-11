#include "runtime/binary_util.h"

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int betl_hex_decode(const char *s, size_t n, uint8_t *out, size_t cap) {
    if ((n & 1u) != 0) return -1;
    size_t bytes = n / 2;
    if (bytes > cap) return -1;
    for (size_t i = 0; i < bytes; ++i) {
        int h = hex_val(s[i * 2]);
        int l = hex_val(s[i * 2 + 1]);
        if (h < 0 || l < 0) return -1;
        out[i] = (uint8_t)((h << 4) | l);
    }
    return (int)bytes;
}

int betl_hex_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
    if (cap < n * 2) return -1;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    return (int)(n * 2);
}
