/**
 * @file    font_fallback.c
 * @brief   码点→脚本→字体面的回退链（静态表 + 直接映射缓存）
 *
 * 关键取舍：字体面不逐字登记码点。CJK 面有几千个字，逐字建表会让
 * "多语言"变成"多占几百 KB 启动内存"；按脚本选面 + 面内 lookup 才是
 * 浏览器/终端实际用的做法（fontconfig 的 charset 是给系统级匹配用的，
 * 内核里没有这个预算）。
 *
 * 缓存失效很简单：i18n_font_reset / i18n_font_add_face 都清空缓存，
 * 因为注册表是引导期一次性装配的，运行期不再变。若将来支持热装字体，
 * 必须在装配点清缓存。
 */

#include "i18n/font_fallback.h"

typedef struct {
    uint32_t first;
    uint32_t last;
    uint8_t script;
} script_range_t;

/* 按 first 升序、互不重叠。未列出的码点归 I18N_SCRIPT_LATIN（主字体
 * 兜底）——不是"猜对脚本"，而是保证一定有字体面接手，不出现豆腐块
 * 之外的空白。 */
static const script_range_t script_ranges[] = {
    {0x0000, 0x024F, I18N_SCRIPT_LATIN},
    {0x0250, 0x02FF, I18N_SCRIPT_LATIN},
    {0x0300, 0x036F, I18N_SCRIPT_LATIN},
    {0x0370, 0x03FF, I18N_SCRIPT_GREEK},
    {0x0400, 0x052F, I18N_SCRIPT_CYRILLIC},
    {0x0590, 0x05FF, I18N_SCRIPT_HEBREW},
    {0x0600, 0x06FF, I18N_SCRIPT_ARABIC},
    {0x0750, 0x077F, I18N_SCRIPT_ARABIC},
    {0x08A0, 0x08FF, I18N_SCRIPT_ARABIC},
    {0x0900, 0x097F, I18N_SCRIPT_DEVANAGARI},
    {0x1D00, 0x1DFF, I18N_SCRIPT_LATIN},
    {0x1E00, 0x1EFF, I18N_SCRIPT_LATIN},
    {0x1F00, 0x1FFF, I18N_SCRIPT_GREEK},
    {0x2000, 0x206F, I18N_SCRIPT_SYMBOL},
    {0x2070, 0x209F, I18N_SCRIPT_LATIN},
    {0x20A0, 0x20CF, I18N_SCRIPT_SYMBOL},
    {0x20D0, 0x20FF, I18N_SCRIPT_SYMBOL},
    {0x2100, 0x218F, I18N_SCRIPT_SYMBOL},
    {0x2190, 0x21FF, I18N_SCRIPT_SYMBOL},
    {0x2200, 0x22FF, I18N_SCRIPT_SYMBOL},
    {0x2300, 0x23FF, I18N_SCRIPT_SYMBOL},
    {0x2400, 0x243F, I18N_SCRIPT_SYMBOL},
    {0x2440, 0x245F, I18N_SCRIPT_SYMBOL},
    {0x2460, 0x24FF, I18N_SCRIPT_SYMBOL},
    {0x2500, 0x259F, I18N_SCRIPT_SYMBOL},
    {0x25A0, 0x25FF, I18N_SCRIPT_SYMBOL},
    {0x2600, 0x26FF, I18N_SCRIPT_EMOJI},
    {0x2700, 0x27BF, I18N_SCRIPT_EMOJI},
    {0x27C0, 0x27FF, I18N_SCRIPT_SYMBOL},
    {0x2800, 0x28FF, I18N_SCRIPT_SYMBOL},
    {0x2900, 0x2AFF, I18N_SCRIPT_SYMBOL},
    {0x2B00, 0x2BFF, I18N_SCRIPT_SYMBOL},
    {0x2C60, 0x2C7F, I18N_SCRIPT_LATIN},
    {0x2DE0, 0x2DFF, I18N_SCRIPT_CYRILLIC},
    {0x2E00, 0x2E7F, I18N_SCRIPT_SYMBOL},
    {0x2E80, 0x2EFF, I18N_SCRIPT_CJK},
    {0x2F00, 0x2FDF, I18N_SCRIPT_CJK},
    {0x2FF0, 0x2FFF, I18N_SCRIPT_CJK},
    {0x3000, 0x303F, I18N_SCRIPT_CJK},
    {0x3040, 0x30FF, I18N_SCRIPT_CJK},
    {0x3100, 0x31FF, I18N_SCRIPT_CJK},
    {0x3200, 0x33FF, I18N_SCRIPT_CJK},
    {0x3400, 0x4DBF, I18N_SCRIPT_CJK},
    {0x4DC0, 0x4DFF, I18N_SCRIPT_SYMBOL},
    {0x4E00, 0x9FFF, I18N_SCRIPT_CJK},
    {0xA000, 0xA4CF, I18N_SCRIPT_CJK},
    {0xA640, 0xA69F, I18N_SCRIPT_CYRILLIC},
    {0xA720, 0xA7FF, I18N_SCRIPT_LATIN},
    {0xA800, 0xA82F, I18N_SCRIPT_DEVANAGARI},
    {0xA8E0, 0xA8FF, I18N_SCRIPT_DEVANAGARI},
    {0xAC00, 0xD7AF, I18N_SCRIPT_CJK},
    {0xF900, 0xFAFF, I18N_SCRIPT_CJK},
    {0xFB1D, 0xFB4F, I18N_SCRIPT_HEBREW},
    {0xFB50, 0xFDFF, I18N_SCRIPT_ARABIC},
    {0xFE00, 0xFE0F, I18N_SCRIPT_LATIN},
    {0xFE10, 0xFE1F, I18N_SCRIPT_CJK},
    {0xFE20, 0xFE2F, I18N_SCRIPT_LATIN},
    {0xFE30, 0xFE4F, I18N_SCRIPT_CJK},
    {0xFE50, 0xFE6F, I18N_SCRIPT_CJK},
    {0xFE70, 0xFEFF, I18N_SCRIPT_ARABIC},
    {0xFF00, 0xFFEF, I18N_SCRIPT_CJK},
    {0xFFF0, 0xFFFF, I18N_SCRIPT_SYMBOL},
    {0x1D400, 0x1D7FF, I18N_SCRIPT_SYMBOL},
    {0x1F000, 0x1FAFF, I18N_SCRIPT_EMOJI},
    {0x20000, 0x3FFFD, I18N_SCRIPT_CJK},
    {0xE0000, 0xE007F, I18N_SCRIPT_LATIN},
};

static i18n_font_face_t g_faces[I18N_FONT_MAX_FACES];
static uint32_t g_face_count;
static uint32_t g_probes;

/* cp → 面 的直接映射缓存。0 表示空槽；面下标整体 +1 存储，避免用
 * 0 当"有效下标"时和空槽混淆。 */
typedef struct {
    uint32_t cp;
    int16_t face;
} face_cache_t;
static face_cache_t g_cache[I18N_FONT_CACHE_SIZE];

int i18n_script_of(uint32_t cp)
{
    if (cp > 0x10FFFF) return -1;
    uint32_t lo = 0, hi = sizeof(script_ranges) / sizeof(script_ranges[0]) - 1;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (cp > script_ranges[mid].last) lo = mid + 1;
        else if (cp < script_ranges[mid].first) hi = mid - 1;
        else return script_ranges[mid].script;
    }
    return I18N_SCRIPT_LATIN;
}

const char *i18n_script_name(int script)
{
    switch (script) {
    case I18N_SCRIPT_LATIN:      return "latin";
    case I18N_SCRIPT_GREEK:      return "greek";
    case I18N_SCRIPT_CYRILLIC:   return "cyrillic";
    case I18N_SCRIPT_HEBREW:     return "hebrew";
    case I18N_SCRIPT_ARABIC:     return "arabic";
    case I18N_SCRIPT_DEVANAGARI: return "devanagari";
    case I18N_SCRIPT_CJK:        return "cjk";
    case I18N_SCRIPT_EMOJI:      return "emoji";
    case I18N_SCRIPT_SYMBOL:     return "symbol";
    default:                     return "?";
    }
}

static void cache_clear(void)
{
    for (uint32_t i = 0; i < I18N_FONT_CACHE_SIZE; i++) {
        g_cache[i].cp = 0xFFFFFFFFu;
        g_cache[i].face = -1;
    }
}

void i18n_font_reset(void)
{
    g_face_count = 0;
    g_probes = 0;
    cache_clear();
}

int i18n_font_add_face(const i18n_font_face_t *face)
{
    if (!face || !face->lookup ||
        (unsigned)face->script >= (unsigned)I18N_SCRIPT_COUNT) return -1;
    if (g_face_count >= I18N_FONT_MAX_FACES) return -1;
    g_faces[g_face_count] = *face;
    cache_clear();                            /* 注册表变了，解析结果作废 */
    return (int)g_face_count++;
}

uint32_t i18n_font_face_count(void)
{
    return g_face_count;
}

int i18n_font_chain(uint32_t cp, int *out, uint32_t max)
{
    if (!out || max == 0 || cp > 0x10FFFF) return 0;
    int script = i18n_script_of(cp);
    int n = 0;

    for (uint32_t i = 0; i < g_face_count && (uint32_t)n < max; i++) {
        if ((int)g_faces[i].script == script) out[n++] = (int)i;
    }
    /* 主拉丁面兜底：脚本面无字形时还要有一次机会（CJK 字体缺生僻字、
     * Emoji 面缺符号都很常见）。同脚本候选之间按注册顺序，不去重——
     * 面不同，字形也可能不同。 */
    if (script != I18N_SCRIPT_LATIN) {
        for (uint32_t i = 0; i < g_face_count && (uint32_t)n < max; i++) {
            if ((int)g_faces[i].script == I18N_SCRIPT_LATIN) out[n++] = (int)i;
        }
    }
    return n;
}

int i18n_font_face_for(uint32_t cp)
{
    if (cp > 0x10FFFF) return -1;

    uint32_t slot = cp & (I18N_FONT_CACHE_SIZE - 1);
    if (g_cache[slot].cp == cp) return g_cache[slot].face;

    int chain[I18N_FONT_CHAIN_MAX];
    int n = i18n_font_chain(cp, chain, I18N_FONT_CHAIN_MAX);
    int found = -1;
    for (int i = 0; i < n; i++) {
        const i18n_font_face_t *f = &g_faces[chain[i]];
        g_probes++;
        if (f->lookup(cp, f->ctx)) {
            found = chain[i];
            break;
        }
    }

    g_cache[slot].cp = cp;
    g_cache[slot].face = (int16_t)found;
    return found;
}

uint32_t i18n_font_lookup_probes(void)
{
    return g_probes;
}
