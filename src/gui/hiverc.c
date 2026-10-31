#include "hiverc.h"
#include "gui_state.h"
#include "../vfs.h"
#include "../string.h"

#define HC_USER_MAX  32
static char g_hc_user[HC_USER_MAX] = "linpinf";
static int  g_hc_user_resolved;

/* 当前用户：/etc/cur-user 单行文本；没有就默认 linpinf。
 * 登录管理（未来的 gdm 类组件）只需写这个文件即可切换用户，
 * 所有 per-user 数据（含 ~/.hiverc）随之隔离。 */
const char *hiverc_current_user(void) {
    if (g_hc_user_resolved) return g_hc_user;
    g_hc_user_resolved = 1;
    vfs_node_t *n = vfs_lookup("/etc/cur-user");
    if (n) {
        char buf[HC_USER_MAX];
        int rn = vfs_read(n, 0, buf, sizeof(buf) - 1);
        if (rn > 0) {
            buf[rn] = 0;
            int p = 0;
            for (int i = 0; buf[i] && p + 1 < HC_USER_MAX; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r' || c == ' ') break;
                if (c == '/' || c == '.') continue;   /* 防路径穿越 */
                g_hc_user[p++] = c;
            }
            g_hc_user[p] = 0;
            if (p == 0) memcpy(g_hc_user, "linpinf", 8);
        }
    }
    return g_hc_user;
}

void hiverc_user_home(char *out, int cap) {
    const char *u = hiverc_current_user();
    const char *pre = "/home/";
    int p = 0;
    for (int i = 0; pre[i] && p + 1 < cap; i++) out[p++] = pre[i];
    for (int i = 0; u[i] && p + 1 < cap; i++) out[p++] = u[i];
    out[p] = 0;
}

static void hiverc_path(char *out, int cap) {
    hiverc_user_home(out, cap - 12);
    int p = (int)strlen(out);
    const char *s = "/.hiverc";
    for (int i = 0; s[i] && p + 1 < cap; i++) out[p++] = s[i];
    out[p] = 0;
}

/* ── 键表 ─────────────────────────────────────────────────── */
typedef struct { const char *key; int *val; int def; } hc_bind_t;

void hiverc_load(struct gui_state *st) {
    int theme_light        = st->theme_light;
    int desktop_style      = st->desktop_style;
    int dock_pos           = st->dock_pos;
    int glass              = st->glass_enabled;
    int glass_lv           = st->glass_strength;
    int topbar_ah          = st->topbar_autohide;
    int seconds            = st->taskbar_show_seconds;
    int brightness         = st->brightness;
    int drag               = st->drag_enabled;
    int drag3d             = st->drag3d_enabled;
    int knock              = st->knock_down_enabled;
    int fx                 = st->effects_enabled;
    int dpi                = st->dpi_scale_override;

    hc_bind_t binds[] = {
        { "theme_light",   &theme_light,      theme_light },
        { "desktop_style", &desktop_style,    desktop_style },
        { "dock_pos",      &dock_pos,         dock_pos },
        { "glass",         &glass,            glass },
        { "glass_lv",      &glass_lv,         glass_lv },
        { "topbar_ah",     &topbar_ah,        topbar_ah },
        { "seconds",       &seconds,          seconds },
        { "brightness",    &brightness,       brightness },
        { "drag",          &drag,             drag },
        { "drag3d",        &drag3d,           drag3d },
        { "knock",         &knock,            knock },
        { "fx",            &fx,               fx },
        { "dpi",           &dpi,              dpi },
    };

    char path[96];
    hiverc_path(path, sizeof(path));
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return;                      /* 首次登录：用默认值 */

    static char text[1200];
    int rn = vfs_read(n, 0, text, sizeof(text) - 1);
    if (rn <= 0) return;
    text[rn] = 0;

    /* 逐行 key=value；坏行直接跳过（手改坏了不会炸） */
    char *p = text;
    while (*p) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        *eol = 0;
        if (p[0] != '#' && p[0] != 0) {
            char *eq = p;
            while (*eq && *eq != '=') eq++;
            if (*eq == '=') {
                *eq = 0;
                int v = 0, neg = 0;
                const char *vs = eq + 1;
                if (*vs == '-') { neg = 1; vs++; }
                while (*vs >= '0' && *vs <= '9') v = v * 10 + (*vs++ - '0');
                if (neg) v = -v;
                for (uint32_t i = 0; i < sizeof(binds) / sizeof(binds[0]); i++) {
                    if (strcmp(p, binds[i].key) == 0) { *binds[i].val = v; break; }
                }
            }
        }
        p = eol + 1;
    }

    /* 约束 */
    if (glass_lv < 0 || glass_lv > 2) glass_lv = 1;
    if (brightness < 20) brightness = 20;
    if (brightness > 100) brightness = 100;
    if (desktop_style != 0 && desktop_style != 1) desktop_style = 0;
    if (dock_pos != 0 && dock_pos != 1) dock_pos = 0;
    if (dpi < 0) dpi = 0;
    if (dpi > 220) dpi = 220;

    st->theme_light          = theme_light;
    st->desktop_style        = desktop_style;
    st->dock_pos             = dock_pos;
    st->glass_enabled        = glass;
    st->glass_strength       = glass_lv;
    st->topbar_autohide      = topbar_ah;
    st->taskbar_show_seconds = seconds;
    st->brightness           = brightness;
    st->drag_enabled         = drag;
    st->drag3d_enabled       = drag3d;
    st->knock_down_enabled   = knock;
    st->effects_enabled      = fx;
    st->dpi_scale_override   = dpi;
}

void hiverc_save(struct gui_state *st) {
    char path[96];
    hiverc_path(path, sizeof(path));

    /* 格式固定，手工拼接（内核侧没有 snprintf 头） */
    static char out[900];
    uint32_t o = 0;
    struct { const char *k; int v; } rows[] = {
        { "theme_light",   st->theme_light },
        { "desktop_style", st->desktop_style },
        { "dock_pos",      st->dock_pos },
        { "glass",         st->glass_enabled },
        { "glass_lv",      st->glass_strength },
        { "topbar_ah",     st->topbar_autohide },
        { "seconds",       st->taskbar_show_seconds },
        { "brightness",    st->brightness },
        { "drag",          st->drag_enabled },
        { "drag3d",        st->drag3d_enabled },
        { "knock",         st->knock_down_enabled },
        { "fx",            st->effects_enabled },
        { "dpi",           st->dpi_scale_override },
    };
    /* 文件头 */
    {
        const char *h1 = "# HBOS HIVE user config (";
        const char *h2 = ")\n";
        const char *u = hiverc_current_user();
        for (int i = 0; h1[i]; i++) out[o++] = h1[i];
        for (int i = 0; u[i]; i++) out[o++] = u[i];
        for (int i = 0; h2[i]; i++) out[o++] = h2[i];
    }
    for (uint32_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        const char *k = rows[r].k;
        for (int i = 0; k[i]; i++) out[o++] = k[i];
        out[o++] = '=';
        int v = rows[r].v;
        if (v < 0) { out[o++] = '-'; v = -v; }
        char num[12];
        int n = 0;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) out[o++] = num[--n];
        out[o++] = '\n';
    }
    out[o] = 0;

    vfs_node_t *node = vfs_lookup_nofollow(path);
    if (!node) node = vfs_create(path);
    if (!node) return;
    (void)vfs_truncate(node, 0);
    (void)vfs_write(node, 0, out, o);
}

/* ── 显示名 ────────────────────────────────────────────────── */
void hive_display_name(char *out, int cap) {
    const char *def = "user";
    int p = 0;
    vfs_node_t *n = vfs_lookup("/etc/display-user");
    if (n) {
        static char buf[64];
        int rn = vfs_read(n, 0, buf, sizeof(buf) - 1);
        if (rn > 0) {
            buf[rn] = 0;
            int start = 0, end = rn;
            /* 去首尾空白 */
            while (start < end && (buf[start] == ' ' || buf[start] == '\n' ||
                   buf[start] == '\r' || buf[start] == '\t')) start++;
            while (end > start && (buf[end - 1] == ' ' || buf[end - 1] == '\n' ||
                   buf[end - 1] == '\r' || buf[end - 1] == '\t')) end--;
            if (end > start) {
                for (int i = start; i < end && p + 1 < cap; i++) out[p++] = buf[i];
            }
        }
    }
    if (p == 0) for (int i = 0; def[i] && p + 1 < cap; i++) out[p++] = def[i];
    out[p] = 0;
}

int hive_set_display_name(const char *name) {
    if (!name) return -1;
    /* 去首尾空白 + 长度限制 */
    const char *s = name;
    while (*s == ' ') s++;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\n')) e--;
    if (e - s <= 0 || e - s > 31) return -1;

    static char data[36];
    uint32_t o = 0;
    for (const char *p = s; p < e && o + 1 < sizeof(data); p++) data[o++] = *p;
    data[o++] = '\n';

    vfs_node_t *node = vfs_lookup_nofollow("/etc/display-user");
    if (!node) node = vfs_create("/etc/display-user");
    if (!node) return -1;
    (void)vfs_truncate(node, 0);
    return vfs_write(node, 0, data, o) == (int)o ? 0 : -1;
}
