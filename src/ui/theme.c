/**
 * @file    theme.c
 * @brief   主题令牌层实现（见 theme.h）。
 *
 * 职责边界：
 *  - 内置 dark/light 是**权威兜底**，永不依赖外部文件，保证系统总能启动。
 *  - 外部 /themes 下的 .theme（JSON）只做"覆盖"：基于某个内置主题，改它声明的键。
 *  - 文件系统不可用时（纯 ramfs、无盘），退化为「只有内置主题」，一切照常。
 */
#include "theme.h"

#include "../fs.h"
#include "../vfs.h"
#include "../lib/json.h"
#include "../string.h"

/* ── 内置主题：定案 1.1「Breeze Card」─────────────────────────
 * 骨架沿用 Breeze（蓝色强调、语义色克制，迁移成本最低），观感取 Soft Card
 * （中圆角 + 分层柔影 + 卡片面略提亮）。数值与 docs/ui-concepts/6-final.html
 * 的设计令牌一一对应，改这里等于改那份设计稿。
 */

static const gui_theme_t g_dark = {
    .bg         = 0xFF191C1F,
    .surface    = 0xFF24282C,
    .surface2   = 0xFF2B3034,
    .surface3   = 0xFF33393F,
    .line       = 0x663A4045,   /* 半透明，压在任意面上都自然 */
    .line2      = 0xFF4A5158,
    .fg         = 0xFFEFF0F1,
    .fg2        = 0xFFBDC3C7,
    .dim        = 0xFF7F8C8D,
    .accent       = 0xFF3DAEE9,
    .accent_fg    = 0xFFFFFFFF,
    .accent_hover = 0xFF4DB8F0,
    .accent_active= 0xFF2E9BD6,
    .ok   = 0xFF27AE60,
    .warn = 0xFFF67400,
    .err  = 0xFFDA4453,
    .info = 0xFF3DAEE9,
    .sel  = 0x3D3DAEE9,         /* accent @ 24% */
    .radius = 10, .radius_small = 7, .radius_large = 14,
    .border_w = 1, .shadow = 2, .is_dark = 1,
    .name = "Breeze Card Dark",
};

static const gui_theme_t g_light = {
    .bg         = 0xFFEFF0F1,
    .surface    = 0xFFFCFCFC,
    .surface2   = 0xFFEFF0F1,
    .surface3   = 0xFFE3E5E7,
    .line       = 0x66D5D8DB,
    .line2      = 0xFFBFC3C7,
    .fg         = 0xFF232629,
    .fg2        = 0xFF4D5358,
    .dim        = 0xFF7A8288,
    .accent       = 0xFF2980B9,
    .accent_fg    = 0xFFFFFFFF,
    .accent_hover = 0xFF1F6DA0,
    .accent_active= 0xFF195C8A,
    .ok   = 0xFF1E8E4E,
    .warn = 0xFFC25E00,
    .err  = 0xFFC0392B,
    .info = 0xFF2980B9,
    .sel  = 0x292980B9,         /* accent @ 16% */
    .radius = 10, .radius_small = 7, .radius_large = 14,
    .border_w = 1, .shadow = 1, .is_dark = 0,
    .name = "Breeze Card Light",
};

/* ── 主题槽位表 ───────────────────────────────────────────── */

typedef struct {
    bool     used;
    bool     builtin;      /**< 内置主题：不可被覆盖，且始终排在前面 */
    gui_theme_t t;
} theme_slot_t;

static theme_slot_t g_slots[UI_THEME_MAX_SLOTS];
static int  g_count = 0;
static int  g_current = 0;
static bool g_inited = false;

/* 前向声明：ui_theme_init 需要它，定义在文件后半（持久化那节） */
static bool ui_theme_load_choice(char *out, uint32_t cap);

const gui_theme_t *ui_theme_builtin_dark(void)  { return &g_dark; }
const gui_theme_t *ui_theme_builtin_light(void) { return &g_light; }

/** 加入一个主题槽（重名则替换非内置项） */
static int slot_add(const gui_theme_t *t, bool builtin) {
    if (g_count < UI_THEME_MAX_SLOTS) {
        theme_slot_t *s = &g_slots[g_count];
        s->used = true;
        s->builtin = builtin;
        s->t = *t;
        return g_count++;
    }
    return -1;
}

static int slot_find(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < g_count; i++) {
        if (g_slots[i].used && strcmp(g_slots[i].t.name, name) == 0) return i;
    }
    /* 允许用 "dark"/"light" 指代内置主题（startup.cfg 里存短名更稳） */
    if (strcmp(name, "dark") == 0) {
        for (int i = 0; i < g_count; i++)
            if (g_slots[i].used && g_slots[i].t.is_dark) return i;
    }
    if (strcmp(name, "light") == 0) {
        for (int i = 0; i < g_count; i++)
            if (g_slots[i].used && !g_slots[i].t.is_dark) return i;
    }
    return -1;
}

/* ── 外部主题文件 ─────────────────────────────────────────── */

#define THEME_DIR   "/themes"
#define THEME_MAX_BYTES 8192u   /**< 单个主题文件读取上限，防超大文件拖垮启动 */

/**
 * 尝试加载一个主题文件。
 * @return true 成功加入槽位
 *
 * 任何失败都只是"这个主题不可用"，绝不影响其他主题与内置兜底。
 */
static bool load_theme_file(const char *path) {
    file_t *f = fs_find_file(path);
    if (!f || f->size == 0) return false;

    uint32_t len = f->size;
    if (len > THEME_MAX_BYTES) len = THEME_MAX_BYTES;   /* 截断而非拒绝 */

    static char buf[THEME_MAX_BYTES + 1];
    int got = fs_read_file_data(f, 0, buf, len);
    if (got <= 0) return false;
    buf[got] = '\0';

    json_doc_t doc;
    if (!json_parse_object(buf, (uint32_t)got, &doc) || !doc.ok) return false;

    /* 基主题：based_on 指 dark/light；缺省时按文件里 is_dark 线索猜，
     * 都没有就用深色（多数人做的是深色主题）。 */
    const gui_theme_t *base = &g_dark;
    const char *bo = json_get_str(&doc, "based_on");
    if (bo && strcmp(bo, "light") == 0) base = &g_light;
    else if (bo && strcmp(bo, "dark") == 0) base = &g_dark;
    else {
        bool d = json_get_bool(&doc, "dark", true);
        base = d ? &g_dark : &g_light;
    }

    gui_theme_t t;
    int applied = ui_theme_parse_json(buf, (uint32_t)got, base, &t);
    if (applied <= 0) return false;      /* 一个键都没覆盖：视为无效主题 */
    return slot_add(&t, false) >= 0;
}

/** 扫描 /themes 目录，加载其中的主题文件 */
static void scan_theme_dir(void) {
    /* 目录不存在（纯 ramfs 或用户还没放主题）时静默跳过：只有内置主题
     * 也完全可用，这是设计上允许的降级状态。 */
    if (!vfs_lookup(THEME_DIR)) return;

    /* vfs_readdir_at 按索引取目录项，遍历到下界即止。主题目录很小，
     * 设个上限避免目录异常时无限循环。 */
    for (uint32_t i = 0; i < 64; i++) {
        char name[UI_THEME_NAME_MAX * 2];
        uint32_t type = 0;
        if (vfs_readdir_at(THEME_DIR, i, name, &type) < 0) break;

        uint32_t nl = (uint32_t)strlen(name);
        if (nl < 7 || strcmp(name + nl - 6, ".theme") != 0) continue;

        char path[128];
        uint32_t p = 0;
        const char *pre = THEME_DIR "/";
        for (const char *s = pre; *s && p + 1 < sizeof(path); s++) path[p++] = *s;
        for (const char *s = name; *s && p + 1 < sizeof(path); s++) path[p++] = *s;
        path[p] = '\0';
        (void)load_theme_file(path);
    }
}

/* ── 公开 API ─────────────────────────────────────────────── */

const gui_theme_t *ui_theme(void) {
    if (!g_inited) ui_theme_init();
    if (g_current < 0 || g_current >= g_count || !g_slots[g_current].used) {
        /* 兜底：任何异常都回退到内置深色，绝不返回 NULL */
        return &g_dark;
    }
    return &g_slots[g_current].t;
}

int ui_theme_init(void) {
    if (g_inited) return 0;
    g_inited = true;
    g_count = 0;
    g_current = 0;

    /* 内置排最前，保证索引 0 永远是可用主题 */
    slot_add(&g_dark, true);
    slot_add(&g_light, true);

    scan_theme_dir();

    /* 应用上次保存的选择（无持久化后端时读不到，用内置深色） */
    char saved[UI_THEME_NAME_MAX] = {0};
    if (ui_theme_load_choice(saved, sizeof(saved))) {
        int idx = slot_find(saved);
        if (idx >= 0) g_current = idx;
    }
    return 0;
}

int ui_theme_set(const char *name) {
    int idx = slot_find(name);
    if (idx < 0) return -1;
    g_current = idx;
    return 0;
}

int ui_theme_set_index(int index) {
    if (index < 0 || index >= g_count || !g_slots[index].used) return -1;
    g_current = index;
    return 0;
}

int ui_theme_current_index(void) {
    if (g_current < 0 || g_current >= g_count || !g_slots[g_current].used) return -1;
    return g_current;
}

int ui_theme_reload(void) {
    int keep = g_current;
    char cur[UI_THEME_NAME_MAX] = {0};
    if (keep >= 0 && keep < g_count) {
        uint32_t i = 0;
        for (; g_slots[keep].t.name[i] && i + 1 < sizeof(cur); i++)
            cur[i] = g_slots[keep].t.name[i];
        cur[i] = '\0';
    }

    g_count = 0;
    slot_add(&g_dark, true);
    slot_add(&g_light, true);
    scan_theme_dir();

    int idx = slot_find(cur);
    g_current = (idx >= 0) ? idx : 0;
    return g_count;
}

int ui_theme_scan(char names[][UI_THEME_NAME_MAX], int max) {
    if (!g_inited) ui_theme_init();
    int n = 0;
    for (int i = 0; i < g_count && n < max; i++) {
        if (!g_slots[i].used) continue;
        const char *s = g_slots[i].t.name;
        uint32_t k = 0;
        for (; s[k] && k + 1 < UI_THEME_NAME_MAX; k++) names[n][k] = s[k];
        names[n][k] = '\0';
        n++;
    }
    return n;
}

int ui_theme_count(void) {
    if (!g_inited) ui_theme_init();
    return g_count;
}

/* ── JSON 覆盖 ────────────────────────────────────────────── */

/**
 * 取颜色键：缺失或非法都保持原值（返回 0 表示未覆盖）。
 * "非法就跳过"是刻意的——一个写错的颜色不该让整个主题不可用。
 */
static int ov_color(const json_doc_t *d, const char *key, uint32_t *dst) {
    const char *s = json_get_str(d, key);
    if (!s) return 0;
    uint32_t c;
    if (!json_parse_color(s, &c)) return 0;
    *dst = c;
    return 1;
}

static int ov_u8(const json_doc_t *d, const char *key, uint8_t *dst, int lo, int hi) {
    if (!d->ok) return 0;
    const json_pair_t *p = NULL;
    for (int i = 0; i < d->count; i++)
        if (strcmp(d->pairs[i].key, key) == 0) { p = &d->pairs[i]; break; }
    if (!p || p->type != JSON_NUM) return 0;
    /* 用整数四舍五入（内核禁 SSE，不能用 double 参与运算） */
    int32_t f = p->num1000;
    int v = (int)((f >= 0) ? (f + 500) / 1000 : -((-f + 500) / 1000));
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    *dst = (uint8_t)v;
    return 1;
}

int ui_theme_parse_json(const char *json, uint32_t len,
                        const gui_theme_t *base, gui_theme_t *out) {
    if (!json || !base || !out) return -1;
    json_doc_t doc;
    if (!json_parse_object(json, len, &doc) || !doc.ok) return -1;

    *out = *base;
    int n = 0;

    n += ov_color(&doc, "bg",        &out->bg);
    n += ov_color(&doc, "surface",   &out->surface);
    n += ov_color(&doc, "surface2",  &out->surface2);
    n += ov_color(&doc, "surface3",  &out->surface3);
    n += ov_color(&doc, "line",      &out->line);
    n += ov_color(&doc, "line2",     &out->line2);
    n += ov_color(&doc, "fg",        &out->fg);
    n += ov_color(&doc, "fg2",       &out->fg2);
    n += ov_color(&doc, "dim",       &out->dim);
    n += ov_color(&doc, "accent",        &out->accent);
    n += ov_color(&doc, "accent_fg",     &out->accent_fg);
    n += ov_color(&doc, "accent_hover",  &out->accent_hover);
    n += ov_color(&doc, "accent_active", &out->accent_active);
    n += ov_color(&doc, "ok",   &out->ok);
    n += ov_color(&doc, "warn", &out->warn);
    n += ov_color(&doc, "err",  &out->err);
    n += ov_color(&doc, "info", &out->info);
    n += ov_color(&doc, "sel",  &out->sel);

    n += ov_u8(&doc, "radius",       &out->radius,       0, 40);
    n += ov_u8(&doc, "radius_small", &out->radius_small, 0, 40);
    n += ov_u8(&doc, "radius_large", &out->radius_large, 0, 40);
    n += ov_u8(&doc, "border_w",     &out->border_w,     0, 4);
    n += ov_u8(&doc, "shadow",       &out->shadow,       0, 3);

    /* 主题名：允许覆盖，便于外部主题显示自己的名字 */
    const char *nm = json_get_str(&doc, "name");
    if (nm && nm[0]) {
        uint32_t i = 0;
        for (; nm[i] && i + 1 < UI_THEME_NAME_MAX; i++) out->name[i] = nm[i];
        out->name[i] = '\0';
        n++;
    }
    /* 明暗属性可显式声明 */
    if (doc.ok) {
        for (int i = 0; i < doc.count; i++) {
            if (strcmp(doc.pairs[i].key, "dark") == 0 && doc.pairs[i].type == JSON_BOOL) {
                out->is_dark = doc.pairs[i].boolean ? 1 : 0;
                n++;
                break;
            }
        }
    }
    return n;
}

/* ── 选择持久化 ───────────────────────────────────────────── */

/* 复用已验证的磁盘链路。主题选择存独立文件，不与 shell 的启动偏好
 * (/startup.cfg，首位是 'g'/'t') 混在一起，避免两套格式互相干扰；
 * 无持久化后端时写失败属预期（仅本次开机生效）。 */
#define THEME_CHOICE_FILE "/theme.cfg"

int ui_theme_save_choice(const char *name) {
    if (!name || !name[0]) return -1;
    file_t *f = fs_create_file(THEME_CHOICE_FILE);
    if (!f) return -1;                     /* 无盘：接受，仅本次生效 */
    uint32_t len = (uint32_t)strlen(name);
    if (fs_write_file_data(f, 0, name, len) < 0) return -1;
    (void)fs_sync();
    return 0;
}

static bool ui_theme_load_choice(char *out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    file_t *f = fs_find_file("/theme.cfg");
    if (!f || f->size == 0) return false;
    uint32_t len = f->size;
    if (len > cap - 1) len = cap - 1;
    if (fs_read_file_data(f, 0, out, len) <= 0) return false;
    out[len] = '\0';
    /* 去掉可能的换行/回车与尾部空白 */
    for (uint32_t i = 0; i < len; i++) {
        if (out[i] == '\n' || out[i] == '\r') { out[i] = '\0'; break; }
    }
    return out[0] != '\0';
}

/* ── 颜色工具 ─────────────────────────────────────────────── */

uint32_t ui_blend(uint32_t dst, uint32_t src) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 0)   return dst;
    if (sa == 255) return src;

    /* source-over（非预乘）：先求合成后的 alpha，再逐通道加权 */
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t oa = sa + da * (255u - sa) / 255u;
    if (oa == 0) return 0;

    uint32_t out = oa << 24;
    for (int shift = 16; shift >= 0; shift -= 8) {
        uint32_t sc = (src >> shift) & 0xFF;
        uint32_t dc = (dst >> shift) & 0xFF;
        uint32_t c = (sc * sa + dc * da * (255u - sa) / 255u) / oa;
        if (c > 255) c = 255;
        out |= c << shift;
    }
    return out;
}

uint32_t ui_shade(uint32_t color, int amount) {
    int a = (int)((color >> 24) & 0xFF);
    int r = (int)((color >> 16) & 0xFF);
    int g = (int)((color >> 8) & 0xFF);
    int b = (int)(color & 0xFF);
    r += amount; g += amount; b += amount;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
