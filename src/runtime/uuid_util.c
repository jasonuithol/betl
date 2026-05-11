#include "runtime/uuid_util.h"

#include <ctype.h>

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int betl_uuid_parse(const char *s, size_t n, uint8_t out[16]) {
    if (n != 36) return -1;
    /* Dashes at the canonical positions. */
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return -1;
    int byte = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        int h = hex_val(s[i]);
        if (h < 0) return -1;
        int l = hex_val(s[i + 1]);
        if (l < 0) return -1;
        out[byte++] = (uint8_t)((h << 4) | l);
        ++i;       /* skip the second nibble we just consumed */
    }
    return byte == 16 ? 0 : -1;
}

int betl_uuid_format(const uint8_t in[16], char *buf, size_t cap) {
    if (cap < 36) return -1;
    static const char hex[] = "0123456789abcdef";
    int byte = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { buf[i] = '-'; continue; }
        uint8_t b = in[byte++];
        buf[i]     = hex[b >> 4];
        buf[i + 1] = hex[b & 0xF];
        ++i;
    }
    return 36;
}
