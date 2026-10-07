/**
 * @file    json.c
 * @brief   极简只读 JSON 解析器实现（见 json.h 的设计说明）。
 *
 * 关键性质：不做任何内存分配，不依赖堆。解析在调用方给的 json_doc_t
 * 定长槽位上完成，因此即使堆耗尽也能解析主题文件并安全降级。
 */
#include "json.h"

#include "../string.h"

/* ── 字符辅助 ─────────────────────────────────────────────── */

static bool j_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/** 复制到定长缓冲，超长则截断（返回实际写入长度，不含 NUL） */
static uint32_t j_copy(char *dst, uint32_t cap, const char *src, uint32_t n) {
    if (cap == 0) return 0;
    uint32_t i = 0;
    for (; i < n && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
    return i;
}

/** 跳空白；返回新位置 */
static uint32_t j_skip(const char *t, uint32_t p, uint32_t len) {
    while (p < len && j_is_space(t[p])) p++;
    return p;
}

/* ── 值解析 ───────────────────────────────────────────────── */

/**
 * 解析一个值，写入 pair。
 * @return 新位置；无法识别时返回 (uint32_t)-1
 */
static uint32_t j_value(const char *t, uint32_t p, uint32_t len, json_pair_t *pair) {
    if (p >= len) return (uint32_t)-1;

    if (t[p] == '"') {
        /* 字符串：不支持转义（主题里不需要；#RRGGBB 与路径都是普通字符） */
        uint32_t s = ++p;
        while (p < len && t[p] != '"') p++;
        if (p >= len) return (uint32_t)-1;      /* 未闭合 */
        j_copy(pair->val, JSON_VAL_MAX, t + s, p - s);
        pair->type = JSON_STR;
        return p + 1;
    }

    if (t[p] == 't' && p + 4 <= len && memcmp(t + p, "true", 4) == 0) {
        pair->type = JSON_BOOL; pair->boolean = true;
        j_copy(pair->val, JSON_VAL_MAX, "true", 4);
        return p + 4;
    }
    if (t[p] == 'f' && p + 5 <= len && memcmp(t + p, "false", 5) == 0) {
        pair->type = JSON_BOOL; pair->boolean = false;
        j_copy(pair->val, JSON_VAL_MAX, "false", 5);
        return p + 5;
    }
    if (t[p] == 'n' && p + 4 <= len && memcmp(t + p, "null", 4) == 0) {
        pair->type = JSON_NULL;
        return p + 4;
    }

    /* 数字（含负号、小数与科学计数法）。科学计数法看起来可以省掉，但主题
     * 文件是人写的，radius 写成 1e1 这类写法不该让整个文件解析失败。 */
    if (t[p] == '-' || (t[p] >= '0' && t[p] <= '9')) {
        uint32_t s = p;
        if (t[p] == '-') p++;
        bool any = false;
        while (p < len && t[p] >= '0' && t[p] <= '9') { p++; any = true; }
        if (p < len && t[p] == '.') {
            p++;
            while (p < len && t[p] >= '0' && t[p] <= '9') { p++; any = true; }
        }
        if (!any) return (uint32_t)-1;

        /* 指数部分：先记下指数值，稍后并进定点结果 */
        int32_t exp10 = 0;
        if (p < len && (t[p] == 'e' || t[p] == 'E')) {
            uint32_t save = p;
            uint32_t q = p + 1;
            int esign = 1;
            if (q < len && (t[q] == '+' || t[q] == '-')) {
                if (t[q] == '-') esign = -1;
                q++;
            }
            int32_t ev = 0; bool ed = false;
            while (q < len && t[q] >= '0' && t[q] <= '9') {
                ev = ev * 10 + (t[q] - '0');
                if (ev > 9) { ev = 9; }          /* 钳制，防溢出；主题用不到大指数 */
                q++; ed = true;
            }
            if (ed) { exp10 = esign * ev; p = q; }
            else p = save;
        }

        uint32_t n = j_copy(pair->val, JSON_VAL_MAX, t + s, p - s);
        pair->type = JSON_NUM;

        /* 定点解析：整数部分 + 小数部分各累计到 ×1000 表示。
         * 全程 int32，不碰浮点（内核禁 SSE）。 */
        {
            int32_t iv = 0;      /* 整数部分 */
            int32_t frac = 0;    /* 小数部分，已按位对齐到千分位 */
            int     fdigits = 0;
            bool    neg = false;
            uint32_t i = 0;
            if (i < n && pair->val[i] == '-') { neg = true; i++; }
            for (; i < n && pair->val[i] >= '0' && pair->val[i] <= '9'; i++) {
                iv = iv * 10 + (pair->val[i] - '0');
                if (iv > 2000000) iv = 2000000;   /* 钳制，防溢出 */
            }
            if (i < n && pair->val[i] == '.') {
                for (i++; i < n && pair->val[i] >= '0' && pair->val[i] <= '9'; i++) {
                    if (fdigits < 3) { frac = frac * 10 + (pair->val[i] - '0'); fdigits++; }
                }
            }
            while (fdigits < 3) { frac *= 10; fdigits++; }   /* 1.5 → .500 */
            int32_t v = iv * 1000 + frac;

            /* 指数：×10 或 ÷10，注意除法要按千分位取舍 */
            for (int32_t k = 0; k < (exp10 > 0 ? exp10 : -exp10); k++) {
                if (exp10 > 0) { if (v < 200000000) v *= 10; else v = 200000000; }
                else           { v /= 10; }
            }
            pair->num1000 = neg ? -v : v;
        }
        return p;
    }

    return (uint32_t)-1;
}

/* ── 顶层解析 ─────────────────────────────────────────────── */

/**
 * 内部实现。失败时统一把 count 归零——调用方若只检查返回值却忘了看 ok
 * （或反之），也不会读到"解析到一半的半个对象"。这是安全降级的前提。
 */
static bool j_parse_impl(const char *text, uint32_t len, json_doc_t *out) {
    uint32_t p = j_skip(text, 0, len);
    if (p >= len || text[p] != '{') return false;   /* 必须是对象 */
    p++;

    for (;;) {
        p = j_skip(text, p, len);
        if (p >= len) return false;                 /* 没有收尾 } */

        if (text[p] == '}') {                       /* 空对象或正常结束 */
            return true;
        }

        /* 键：必须是字符串。非字符串说明结构坏了，整体失败 */
        if (text[p] != '"') return false;
        uint32_t ks = ++p;
        while (p < len && text[p] != '"') p++;
        if (p >= len) return false;

        json_pair_t *pair = NULL;
        if (out->count < JSON_MAX_PAIRS) {
            pair = &out->pairs[out->count];
            memset(pair, 0, sizeof(*pair));
            j_copy(pair->key, JSON_KEY_MAX, text + ks, p - ks);
        }
        p++;                                        /* 跳过闭引号 */

        p = j_skip(text, p, len);
        if (p >= len || text[p] != ':') return false;
        p++;
        p = j_skip(text, p, len);

        /* 值：即使槽位已满也要继续解析以定位到 ',' 或 '}'（保持结构校验） */
        json_pair_t scratch;
        json_pair_t *dst = pair ? pair : &scratch;
        if (!pair) memset(&scratch, 0, sizeof(scratch));
        uint32_t np = j_value(text, p, len, dst);
        if (np == (uint32_t)-1) return false;
        if (pair) out->count++;
        p = j_skip(text, np, len);

        if (p < len && text[p] == ',') {
            p++;
            /* 容忍尾逗号：{"a":1,} */
            uint32_t q = j_skip(text, p, len);
            if (q < len && text[q] == '}') return true;
            continue;
        }
        if (p < len && text[p] == '}') return true;

        return false;                               /* 既不是 , 也不是 } */
    }
}

bool json_parse_object(const char *text, uint32_t len, json_doc_t *out) {
    if (!out) return false;
    out->count = 0;
    out->ok = false;
    if (!text || len == 0) return false;

    bool ok = j_parse_impl(text, len, out);
    out->ok = ok;
    if (!ok) out->count = 0;   /* 失败即归零，避免调用方读到半截数据 */
    return ok;
}

/* ── 取值 ─────────────────────────────────────────────────── */

static const json_pair_t *j_find(const json_doc_t *doc, const char *key) {
    if (!doc || !doc->ok || !key) return NULL;
    for (int i = 0; i < doc->count; i++) {
        if (strcmp(doc->pairs[i].key, key) == 0) return &doc->pairs[i];
    }
    return NULL;
}

const char *json_get_str(const json_doc_t *doc, const char *key) {
    const json_pair_t *p = j_find(doc, key);
    if (!p || p->type != JSON_STR) return NULL;
    return p->val;
}

int32_t json_get_num1000(const json_doc_t *doc, const char *key, int32_t fallback) {
    const json_pair_t *p = j_find(doc, key);
    if (!p || p->type != JSON_NUM) return fallback;
    return p->num1000;
}

bool json_get_bool(const json_doc_t *doc, const char *key, bool fallback) {
    const json_pair_t *p = j_find(doc, key);
    if (!p || p->type != JSON_BOOL) return fallback;
    return p->boolean;
}

int json_get_int(const json_doc_t *doc, const char *key, int fallback) {
    const json_pair_t *p = j_find(doc, key);
    if (!p || p->type != JSON_NUM) return fallback;
    /* 先钳制定点值再转整数，避免 (v+500) 溢出；对齐头文件承诺的"范围钳制"——
     * 主题里写 radius = 9999 应被钳到上限，而不是当作没写而退回默认值。 */
    int32_t v = p->num1000;
    const int32_t lim = 1000000 * 1000;          /* ±1e9，留足余量不溢出 */
    if (v >  lim) v =  lim;
    if (v < -lim) v = -lim;
    int32_t r = (v >= 0) ? (v + 500) / 1000 : -((-v + 500) / 1000);
    return (int)r;
}

/* ── 颜色 ─────────────────────────────────────────────────── */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool json_parse_color(const char *s, uint32_t *out) {
    if (!s || !out) return false;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    else if (s[0] == '#') s++;
    else return false;

    /* 统计十六进制位数 */
    uint32_t n = 0, v = 0;
    while (s[n] && n < 8) {
        int d = hex_val(s[n]);
        if (d < 0) break;
        v = (v << 4) | (uint32_t)d;
        n++;
    }
    if (s[n] != '\0') return false;      /* 有非法字符 */
    if (n == 6) { *out = 0xFF000000u | v; return true; }   /* RRGGBB */
    if (n == 8) { *out = v; return true; }                 /* AARRGGBB */
    if (n == 3) {                                            /* RGB 简写 */
        uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
        r = r * 17; g = g * 17; b = b * 17;
        *out = 0xFF000000u | (r << 16) | (g << 8) | b;
        return true;
    }
    return false;
}
