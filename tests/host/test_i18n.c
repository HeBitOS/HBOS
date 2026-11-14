/* 多语言核心宿主单测：
 *   (a) UTF-8 严格/容错解码与替换行为（U+FFFD、maximal subpart）
 *   (b) wcwidth / 字素簇（组合符、Emoji、国旗对、ZWJ、CRLF）与退格不变式
 *   (c) 字体回退链的数据结构与查表成本（探测计数/缓存）
 *   (d) locale 契约（LC_ALL > LC_CTYPE > LANG，UTF-8 判定）
 */
#include <stdio.h>
#include <string.h>
#include "i18n/utf8.h"
#include "i18n/wcwidth.h"
#include "i18n/font_fallback.h"
#include "i18n/locale.h"

static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

static uint32_t dec(const char *s) {
    uint32_t cp = 0;
    int n = utf8_decode(s, (uint32_t)strlen(s), &cp);
    return n ? cp : UTF8_INVALID;
}

static void test_decode(void) {
    printf("[I18N] UTF-8 严格解码\n");
    CHECK(dec("A") == 'A');
    CHECK(dec("\xC2\xA2") == 0xA2);
    CHECK(dec("\xE4\xB8\xAD") == 0x4E2D);
    CHECK(dec("\xF0\x9F\x98\x80") == 0x1F600);
    CHECK(utf8_decode("\xE4\xB8\xAD", 3, 0) == 3);

    CHECK(dec("\xC0\x80") == UTF8_INVALID);          /* overlong NUL */
    CHECK(dec("\xC1\xBF") == UTF8_INVALID);
    CHECK(dec("\xE0\x80\x80") == UTF8_INVALID);
    CHECK(dec("\xF0\x80\x80\x80") == UTF8_INVALID);
    CHECK(dec("\xED\xA0\x80") == UTF8_INVALID);      /* surrogate */
    CHECK(dec("\xED\xBF\xBF") == UTF8_INVALID);
    CHECK(dec("\xF4\x90\x80\x80") == UTF8_INVALID);  /* > U+10FFFF */
    CHECK(dec("\xF5\x80\x80\x80") == UTF8_INVALID);
    CHECK(dec("\xE4\xB8") == UTF8_INVALID);
    CHECK(dec("\x80") == UTF8_INVALID);
    CHECK(dec("\xC2") == UTF8_INVALID);
    CHECK(!utf8_valid("ab\xE4\xB8", 4));
    CHECK(utf8_valid("中文abc", (uint32_t)strlen("中文abc")));
    CHECK(utf8_length("\x80\x80", 2) == 2);
}

static void test_replacement(void) {
    printf("[I18N] U+FFFD 确定性替换\n");
    char buf[8] = {0};
    uint32_t cp = 0;

    /* 截断的 3 字节序列：按 maximal subpart 一次消费 2 字节，只产生一个 FFFD */
    int n = utf8_decode_fffd("\xE4\xB8", 2, &cp);
    CHECK(n == 2 && cp == UTF8_REPLACEMENT);
    /* 孤立续字节：每次 1 字节 */
    n = utf8_decode_fffd("\x80\x80", 2, &cp);
    CHECK(n == 1 && cp == UTF8_REPLACEMENT);
    /* E0 的第二字节越界（overlong）：只消费首字节，后面两个 80 各自 1 个 FFFD */
    n = utf8_decode_fffd("\xE0\x80\x80", 3, &cp);
    CHECK(n == 1 && cp == UTF8_REPLACEMENT);
    /* 合法序列不替换 */
    n = utf8_decode_fffd("\xE4\xB8\xAD", 3, &cp);
    CHECK(n == 3 && cp == 0x4E2D);
    /* 容错迭代必然前进并终止 */
    uint32_t off = 0, count = 0;
    const char *bad = "\xE4\xB8" "A\x80" "\xF0\x9F\x98\x80";
    while (off < (uint32_t)strlen(bad)) {
        off = utf8_next(bad, (uint32_t)strlen(bad), off, &cp);
        count++;
        CHECK(count <= 16);
    }
    CHECK(count == 4);   /* FFFD + 'A' + FFFD + 😀 */
    (void)buf;
}

static void test_width(void) {
    printf("[I18N] wcwidth\n");
    CHECK(utf8_wcwidth('A') == 1);
    CHECK(utf8_wcwidth(0x0301) == 0);        /* combining acute */
    CHECK(utf8_wcwidth(0x200D) == 0);        /* ZWJ */
    CHECK(utf8_wcwidth(0xFE0F) == 0);        /* VS16 */
    CHECK(utf8_wcwidth(0x4E2D) == 2);        /* 中 */
    CHECK(utf8_wcwidth(0x1F600) == 2);       /* 😀 */
    CHECK(utf8_wcwidth(0x1160) == 0);        /* Hangul Jamo medial */
    CHECK(utf8_wcwidth(0x00AD) == 1);        /* 软连字符按 Kuhn 约定占 1 列 */
    CHECK(utf8_wcwidth(0x01) == -1);         /* 控制字符 */
    CHECK(utf8_wcwidth(0x110000) == -1);

    CHECK(utf8_strwidth("abc", 3) == 3);
    CHECK(utf8_strwidth("中文abc", (uint32_t)strlen("中文abc")) == 7);
    /* e + U+0301：组合附加符不占独立格 */
    CHECK(utf8_strwidth("e\xCC\x81", 3) == 1);
    /* ❤️ = U+2764 + VS16：文本呈现 1 列，Emoji 呈现 2 列 */
    CHECK(utf8_wcwidth(0x2764) == 1);
    CHECK(utf8_strwidth("\xE2\x9D\xA4\xEF\xB8\x8F", 6) == 2);
    /* 1️⃣ = '1' + VS16 + U+20E3 */
    CHECK(utf8_strwidth("1\xEF\xB8\x8F\xE2\x83\xA3", 7) == 2);
}

static void test_clusters(void) {
    printf("[I18N] 字素簇\n");
    const char *comb = "e\xCC\x81";                   /* e + U+0301 */
    CHECK(utf8_cluster_next(comb, 3, 0) == 3);
    CHECK(utf8_cluster_prev(comb, 3, 3) == 0);
    CHECK(utf8_cluster_width(comb, 3) == 1);

    const char *family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    uint32_t fl = (uint32_t)strlen(family);
    CHECK(utf8_cluster_next(family, fl, 0) == fl);     /* 👨‍👩‍👧 一个簇 */
    CHECK(utf8_cluster_prev(family, fl, fl) == 0);
    CHECK(utf8_cluster_width(family, fl) == 2);

    const char *flag = "\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3";  /* 🇨🇳 */
    CHECK(utf8_cluster_next(flag, 8, 0) == 8);
    CHECK(utf8_cluster_width(flag, 8) == 2);

    CHECK(utf8_cluster_next("\r\n", 2, 0) == 2);       /* CRLF 一个簇 */
    CHECK(utf8_cluster_prev("\r\n", 2, 2) == 0);

    /* 前进边界与后退边界必须互逆（退格不变式） */
    const char *mix = "a" "\xE4\xB8\xAD" "e\xCC\x81" "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9" "z";
    uint32_t len = (uint32_t)strlen(mix);
    uint32_t prev = 0, off = 0;
    while (off < len) {
        off = utf8_cluster_next(mix, len, off);
        CHECK(off > prev);
        if (off > 0) CHECK(utf8_cluster_prev(mix, len, off) == prev);
        prev = off;
    }
    CHECK(off == len);
}

/* ── 字体回退：两个假字体面 ───────────────────────────────────── */
static const uint8_t fake_latin = 1, fake_cjk = 2, fake_emoji = 3;
static const uint8_t *latin_lookup(uint32_t cp, void *ctx) {
    (void)ctx;
    return (cp >= 0x20 && cp < 0x7F) || cp == 0x4E2E ? &fake_latin : NULL;
}
static const uint8_t *cjk_lookup(uint32_t cp, void *ctx) {
    (void)ctx;
    return cp == 0x4E2D ? &fake_cjk : NULL;
}
static const uint8_t *emoji_lookup(uint32_t cp, void *ctx) {
    (void)ctx;
    return cp >= 0x1F000 && cp <= 0x1FAFF ? &fake_emoji : NULL;
}

static void test_font_fallback(void) {
    printf("[I18N] 字体回退链\n");
    i18n_font_reset();
    CHECK(i18n_font_add_face(&(i18n_font_face_t){
        .name = "latin", .script = I18N_SCRIPT_LATIN, .lookup = latin_lookup }) == 0);
    CHECK(i18n_font_add_face(&(i18n_font_face_t){
        .name = "cjk", .script = I18N_SCRIPT_CJK, .lookup = cjk_lookup }) == 1);
    CHECK(i18n_font_add_face(&(i18n_font_face_t){
        .name = "emoji", .script = I18N_SCRIPT_EMOJI, .lookup = emoji_lookup }) == 2);
    CHECK(i18n_font_add_face(&(i18n_font_face_t){
        .name = "bad", .script = I18N_SCRIPT_LATIN, .lookup = NULL }) == -1);

    CHECK(i18n_script_of(0x4E2D) == I18N_SCRIPT_CJK);
    CHECK(i18n_script_of(0x0627) == I18N_SCRIPT_ARABIC);
    CHECK(i18n_script_of(0x05D0) == I18N_SCRIPT_HEBREW);
    CHECK(i18n_script_of(0x1F600) == I18N_SCRIPT_EMOJI);
    CHECK(i18n_script_of(0x0041) == I18N_SCRIPT_LATIN);

    int chain[I18N_FONT_CHAIN_MAX];
    int n = i18n_font_chain(0x4E2D, chain, I18N_FONT_CHAIN_MAX);
    CHECK(n >= 2);
    CHECK(chain[0] == 1);          /* CJK 面优先 */
    CHECK(chain[n - 1] == 0);      /* 拉丁面兜底 */

    uint32_t probes = i18n_font_lookup_probes();
    CHECK(i18n_font_face_for(0x4E2D) == 1);
    CHECK(i18n_font_lookup_probes() == probes + 1);   /* 第一个面就命中 */
    probes = i18n_font_lookup_probes();
    CHECK(i18n_font_face_for(0x4E2D) == 1);
    CHECK(i18n_font_lookup_probes() == probes);       /* 缓存命中不探测 */

    /* CJK 字体缺字 → 回退到拉丁面（不是直接报缺字） */
    CHECK(i18n_font_face_for(0x4E2E) == 0);
    CHECK(i18n_font_face_for(0x1F600) == 2);
    CHECK(i18n_font_face_for(0x0301) == -1);          /* 所有面都没有 */
}

/* ── locale：用假环境变量验证优先级 ───────────────────────────── */
static const char *fake_env(const char *name) {
    if (strcmp(name, "LC_CTYPE") == 0) return "zh_CN.UTF-8";
    if (strcmp(name, "LANG") == 0) return "C";
    return NULL;
}
static const char *utf8_all_env(const char *name) {
    if (strcmp(name, "LC_ALL") == 0) return "C.UTF-8";
    if (strcmp(name, "LANG") == 0) return "C";
    return NULL;
}

static void test_locale(void) {
    printf("[I18N] locale 契约\n");
    setlocale(LC_ALL, "C");
    CHECK(!i18n_locale_is_utf8(LC_ALL));

    i18n_locale_init(fake_env);
    CHECK(strcmp(i18n_locale_name(LC_CTYPE), "zh_CN.UTF-8") == 0);  /* LC_CTYPE 优先于 LANG */
    CHECK(i18n_locale_is_utf8(LC_CTYPE));
    CHECK(strcmp(i18n_locale_name(LC_TIME), "C") == 0);            /* 无 LC_TIME → LANG */
    CHECK(!i18n_locale_is_utf8(LC_ALL));                            /* 混用 → 非全 UTF-8 */

    i18n_locale_init(utf8_all_env);
    CHECK(i18n_locale_is_utf8(LC_ALL));
    CHECK(strcmp(i18n_locale_name(LC_ALL), "C.UTF-8") == 0);

    CHECK(setlocale(LC_ALL, "en_US.UTF-8") != NULL);
    CHECK(i18n_locale_is_utf8(LC_ALL));
    CHECK(setlocale(LC_ALL, "en_US.ISO-8859-1") == NULL);            /* 不支持就失败 */
    CHECK(i18n_locale_is_utf8(LC_ALL));                              /* 且状态不变 */
    CHECK(setlocale(LC_CTYPE, "POSIX") != NULL);
    CHECK(!i18n_locale_is_utf8(LC_CTYPE));
    CHECK(setlocale(LC_ALL, "C.UTF-8") != NULL);
    CHECK(setlocale(LC_CTYPE, "utf8") != NULL);                      /* 无点的 utf8 别名 */

    i18n_locale_reset();
    CHECK(strcmp(i18n_locale_name(LC_ALL), "C") == 0);
    CHECK(!i18n_locale_is_utf8(LC_ALL));
    CHECK(setlocale(99, "C") == NULL);
}

int main(void) {
    test_decode();
    test_replacement();
    test_width();
    test_clusters();
    test_font_fallback();
    test_locale();

    if (failures) {
        printf("[I18N] FAILED: %d\n", failures);
        return 1;
    }
    printf("[I18N] all tests passed\n");
    return 0;
}
