/**
 * @file    utf8.c
 * @brief   严格 + 容错 UTF-8 解码、编码与字素簇迭代（无 libc 依赖）
 *
 * 设计要点：
 *   · 严格解码的合法性判据就是"最短形式 + 不自代理 + 不越界"三条，
 *     用 first-byte 推 need/min，再逐字节校验续字节。
 *   · 容错解码按 Unicode maximal subpart 消费，保证每次至少吃掉 1 字节，
 *     渲染循环不会因为一个坏字节卡死（这是旧 flanterm 实现的问题之一：
 *     坏续字节被丢掉后，后面的字节可能被误当成首字节）。
 *   · 簇切分只需要"前一个码点是不是 Extend"这一个判据，复杂度集中在
 *     wcwidth 的零宽表里；本文件不重复维护 Unicode 表。
 *
 * 性能：cluster_prev 从 off 向左最多回退 3 个续字节，然后按簇内成员
 * 逐个回退；一个簇通常 1..8 个码点，退格路径 O(簇长)。
 */

#include "i18n/utf8.h"
#include "i18n/wcwidth.h"

static int lead_len(uint8_t b)
{
    if (b >= 0xC2 && b <= 0xDF) return 2;   /* C0/C1 是超长编码，直接非法 */
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;   /* F5..FF 必然越界 */
    return 0;
}

/* 第二字节的取值范围同时编码了两条规则：E0 的续字节 < A0 即超长，
 * ED 的续字节 >= A0 即代理区，F0/F4 同理。在这里挡住，后面的通用循环
 * 就只需要检查 10xxxxxx。 */
static int second_ok(uint8_t b0, uint8_t b1)
{
    if (b0 == 0xE0) return b1 >= 0xA0 && b1 <= 0xBF;
    if (b0 == 0xED) return b1 >= 0x80 && b1 <= 0x9F;
    if (b0 == 0xF0) return b1 >= 0x90 && b1 <= 0xBF;
    if (b0 == 0xF4) return b1 >= 0x80 && b1 <= 0x8F;
    return (b1 & 0xC0) == 0x80;
}

int utf8_decode(const char *s, uint32_t len, uint32_t *cp)
{
    if (!s || len == 0) return 0;
    const uint8_t *p = (const uint8_t *)s;
    uint8_t b0 = p[0];

    if (b0 < 0x80) {
        if (cp) *cp = b0;
        return 1;
    }

    int L = lead_len(b0);
    if (L == 0) return 0;                    /* 孤立续字节或非法首字节 */
    if (len < (uint32_t)L) return 0;         /* 截断 */

    if (!second_ok(b0, p[1])) return 0;

    uint32_t v;
    if (L == 2) v = b0 & 0x1F;
    else if (L == 3) v = b0 & 0x0F;
    else v = b0 & 0x07;

    for (int i = 1; i < L; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (uint32_t)(p[i] & 0x3F);
    }

    if (v > 0x10FFFF) return 0;
    if (v >= 0xD800 && v <= 0xDFFF) return 0;
    if (cp) *cp = v;
    return L;
}

int utf8_decode_fffd(const char *s, uint32_t len, uint32_t *cp)
{
    if (!s || len == 0) return 0;

    int n = utf8_decode(s, len, cp);
    if (n > 0) return n;

    const uint8_t *p = (const uint8_t *)s;
    uint32_t take = 1;
    int L = lead_len(p[0]);
    if (L >= 2 && len >= 2 && second_ok(p[0], p[1])) {
        take = 2;
        for (int k = 2; k < L && (uint32_t)k < len; k++) {
            if ((p[k] & 0xC0) != 0x80) break;
            take++;
        }
    }
    if (cp) *cp = UTF8_REPLACEMENT;
    return (int)take;
}

int utf8_encode(uint32_t cp, char *out)
{
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

int utf8_valid(const char *s, uint32_t len)
{
    uint32_t i = 0;
    while (i < len) {
        int n = utf8_decode(s + i, len - i, 0);
        if (!n) return 0;
        i += (uint32_t)n;
    }
    return 1;
}

uint32_t utf8_length(const char *s, uint32_t len)
{
    uint32_t i = 0, count = 0;
    while (i < len) {
        int n = utf8_decode(s + i, len - i, 0);
        i += (uint32_t)(n ? n : 1);          /* 非法序列按一个替换字符计 */
        count++;
    }
    return count;
}

uint32_t utf8_next(const char *s, uint32_t len, uint32_t off, uint32_t *cp)
{
    if (!s || off >= len) {
        if (cp) *cp = UTF8_REPLACEMENT;
        return len;
    }
    uint32_t c = 0;
    int n = utf8_decode_fffd(s + off, len - off, &c);
    if (n <= 0) n = 1;
    if (cp) *cp = c;
    return off + (uint32_t)n;
}

/* 向左找一个码点起点：先跨过最多 3 个续字节，再退到首字节。
 * 遇到 4 个以上续字节的损坏串时不猜，直接把 off 当作边界。 */
static uint32_t cp_start_before(const char *s, uint32_t off)
{
    uint32_t i = off;
    uint32_t steps = 0;
    while (i > 0 && steps < 3 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) {
        i--;
        steps++;
    }
    if (i == 0) return 0;
    if (((uint8_t)s[i - 1] & 0xC0) == 0x80) return i;
    return i - 1;
}

uint32_t utf8_prev(const char *s, uint32_t len, uint32_t off)
{
    if (!s || off == 0) return 0;
    if (off > len) off = len;
    return cp_start_before(s, off);
}

uint32_t utf8_cluster_next(const char *s, uint32_t len, uint32_t off)
{
    if (!s || off >= len) return len;

    uint32_t i = off, cp = 0;
    int n = utf8_decode_fffd(s + i, len - i, &cp);
    if (n <= 0) n = 1;
    i += (uint32_t)n;

    int simple = 1;                          /* 还没吃任何 Extend */
    int ri_pending = i18n_cp_is_regional_indicator(cp);
    int joined = 0;                          /* 上一个码点是 ZWJ */

    while (i < len) {
        uint32_t next = 0;
        int m = utf8_decode_fffd(s + i, len - i, &next);
        if (m <= 0) break;

        if (cp == '\r' && next == '\n' && simple) {
            i += (uint32_t)m;                /* CRLF 是一个簇 */
            break;
        }
        if (i18n_cp_is_regional_indicator(next)) {
            if (ri_pending) {                /* 区域指示符成对才算一个簇 */
                i += (uint32_t)m;
                return i;
            }
            break;
        }
        if (i18n_cp_is_extend(next) || next == 0x200D) {
            i += (uint32_t)m;
            simple = 0;
            ri_pending = 0;
            joined = (next == 0x200D);
            continue;
        }
        if (joined) {                        /* ZWJ 后面的基字符属于同一簇 */
            i += (uint32_t)m;
            joined = 0;
            simple = 0;
            ri_pending = 0;
            continue;
        }
        break;
    }
    return i;
}

uint32_t utf8_cluster_prev(const char *s, uint32_t len, uint32_t off)
{
    if (!s || off == 0) return 0;
    if (off > len) off = len;

    /* 从最后一个码点向左：扩展符（含 ZWJ）都属于当前簇；基字符只有在
     * "前一个码点是 ZWJ" 时才继续向左（Emoji ZWJ 序列），否则它就是
     * 簇首。国旗对是两个基字符，单独判一次。 */
    uint32_t j = cp_start_before(s, off);
    for (;;) {
        if (j == 0) break;

        uint32_t cur = 0;
        if (utf8_decode_fffd(s + j, len - j, &cur) <= 0) break;

        if (i18n_cp_is_extend(cur)) {
            j = cp_start_before(s, j);
            continue;
        }

        uint32_t p = cp_start_before(s, j);
        uint32_t prev = 0;
        if (utf8_decode_fffd(s + p, j - p, &prev) <= 0) break;

        if (prev == 0x200D) {
            j = p;
            continue;
        }
        if (i18n_cp_is_regional_indicator(prev) &&
            i18n_cp_is_regional_indicator(cur)) {
            j = p;
        }
        break;
    }

    if (j > 0 && s[j] == '\n' && s[j - 1] == '\r') j--;   /* CRLF */
    return j;
}
