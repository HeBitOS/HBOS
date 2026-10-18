/**
 * @file    utf8.c
 * @brief   严格 UTF-8 编解码（无 libc 依赖，宿主机可直接单测）
 */

#include "utf8.h"

int utf8_decode(const char *s, uint32_t len, uint32_t *cp) {
    if (!s || len == 0) return 0;
    const uint8_t *p = (const uint8_t *)s;
    uint8_t b0 = p[0];

    if (b0 < 0x80) {
        if (cp) *cp = b0;
        return 1;
    }

    int need;
    uint32_t v, min;
    if ((b0 & 0xE0) == 0xC0) { need = 1; v = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { need = 2; v = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { need = 3; v = b0 & 0x07; min = 0x10000; }
    else return 0;   /* 0x80..0xBF 孤立续字节，或 F8..FF */

    if (len < (uint32_t)(1 + need)) return 0;
    for (int i = 1; i <= need; i++) {
        uint8_t b = p[i];
        if ((b & 0xC0) != 0x80) return 0;
        v = (v << 6) | (uint32_t)(b & 0x3F);
    }

    if (v < min) return 0;                       /* overlong */
    if (v > 0x10FFFF) return 0;                  /* 超出 Unicode 范围 */
    if (v >= 0xD800 && v <= 0xDFFF) return 0;    /* 代理区 */
    if (cp) *cp = v;
    return 1 + need;
}

int utf8_encode(uint32_t cp, char *out) {
    if (!out) return 0;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;

    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_valid(const char *s, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        int n = utf8_decode(s + i, len - i, 0);
        if (!n) return 0;
        i += (uint32_t)n;
    }
    return 1;
}

uint32_t utf8_length(const char *s, uint32_t len) {
    uint32_t i = 0, count = 0;
    while (i < len) {
        int n = utf8_decode(s + i, len - i, 0);
        i += (uint32_t)(n ? n : 1);   /* 非法序列按 1 字节跳过 */
        count++;
    }
    return count;
}
