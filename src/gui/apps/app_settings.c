#include "gui_app.h"
#include "gui_draw.h"
#include "../../graphics/gui_font.h"
#include "../../string.h"

/* ── small button helper ─────────────────────────────────── */
#define SB_W  80
#define SB_H  28

static void draw_sb(int x, int y, int w, const char *label, uint32_t color) {
    gui_vgradient(x, y, w, SB_H,
                  gui_rgb(((color >> 16) & 0xff) + 14,
                          ((color >>  8) & 0xff) + 14,
                          (color & 0xff) + 14),
                  color);
    gui_border(x, y, w, SB_H, gui_rgb(20, 30, 42));
    int tw = (int)strlen(label);
    gui_text(x + (w - tw * 6) / 2, y + (SB_H - 10) / 2, label,
             gui_rgb(235, 242, 250), 1);
}

/* ── hit test for row of buttons ─────────────────────────── */
static int hit_row(int mx, int my, int x, int y, int btn_w, int gap, int n) {
    for (int i = 0; i < n; i++) {
        int bx = x + i * (btn_w + gap);
        if (mx >= bx && mx < bx + btn_w && my >= y && my < y + SB_H) return i;
    }
    return -1;
}

/* ── section label ───────────────────────────────────────── */
static void section(int x, int y, const char *s) {
    gui_rect(x, y + 12, 280, 1, gui_rgb(40, 60, 80));
    gui_text(x, y, s, gui_rgb(61, 174, 233), 1);
}

/* ── draw ────────────────────────────────────────────────── */
static void app_settings_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    int x = tx, y = ty;

    /* ── 主题 ── */
    section(x, y, "主题");
    y += 20;
    draw_sb(x,          y, SB_W + 10, st->theme_light ? "● 浅色" : "  浅色",
            st->theme_light ? gui_rgb(61, 174, 233) : gui_rgb(38, 50, 64));
    draw_sb(x + SB_W + 18, y, SB_W + 10, !st->theme_light ? "● 深色" : "  深色",
            !st->theme_light ? gui_rgb(30, 40, 55) : gui_rgb(38, 50, 64));
    y += SB_H + 14;

    /* ── 字体大小 ── */
    section(x, y, "字体大小");
    y += 20;
    int nbase = gui_font_base_count();
    static const char *sz_labels[] = {"小 (16px)", "中 (18px)", "大 (20px)"};
    int active = gui_font_active();
    for (int i = 0; i < nbase && i < 3; i++) {
        uint32_t bc = (i == active) ? gui_rgb(61, 174, 233) : gui_rgb(38, 50, 64);
        draw_sb(x + i * (SB_W + 8), y, SB_W + 4, sz_labels[i], bc);
    }
    y += SB_H + 14;

    /* ── 系统信息 ── */
    section(x, y, "系统信息");
    y += 20;

    char line[64];
    uint32_t pos;

    /* memory - use simple placeholder if no API */
    gui_text(x, y, "HBOS v0.1-beta3", gui_rgb(180, 200, 220), 1);
    y += 18;
    gui_text(x, y, "架构: x86_64 裸机内核", gui_rgb(140, 160, 180), 1);
    y += 18;
    gui_text(x, y, "引导: Limine BIOS", gui_rgb(140, 160, 180), 1);
    y += 18;
    gui_text(x, y, "图形: 线性帧缓冲", gui_rgb(140, 160, 180), 1);
    y += 18;

    line[0] = 0; pos = 0;
    gui_append_str(line, sizeof(line), &pos, "字体槽: ");
    gui_append_int(line, sizeof(line), &pos, active);
    gui_append_str(line, sizeof(line), &pos, " / ");
    gui_append_int(line, sizeof(line), &pos, nbase - 1);
    gui_text(x, y, line, gui_rgb(140, 160, 180), 1);
    y += 18;

    /* ── 快捷键提示 ── */
    y += 6;
    section(x, y, "快捷键");
    y += 20;
    gui_text(x, y,      "F2 / F3    字体缩小 / 放大", gui_rgb(120, 140, 160), 1);
    y += 16;
    gui_text(x, y,      "F4         深色 / 浅色主题",  gui_rgb(120, 140, 160), 1);
    y += 16;
    gui_text(x, y,      "Tab / 空格  切换窗口",         gui_rgb(120, 140, 160), 1);
    y += 16;
    gui_text(x, y,      "Alt+↑      窗口最大化",       gui_rgb(120, 140, 160), 1);
    y += 16;
    gui_text(x, y,      "Alt+↓      窗口最小化",       gui_rgb(120, 140, 160), 1);
}

/* ── on_click ────────────────────────────────────────────── */
static int app_settings_click(gui_state_t *st, int mx, int my,
                              int tx, int ty, int win_w, int win_h) {
    (void)win_w; (void)win_h;
    int x = tx, y = ty;

    /* theme row */
    y += 20;
    int ti = hit_row(mx, my, x, y, SB_W + 10, 8, 2);
    if (ti == 0) { st->theme_light = 1; st->status = "已切换为浅色主题"; return 1; }
    if (ti == 1) { st->theme_light = 0; st->status = "已切换为深色主题"; return 1; }
    y += SB_H + 14;

    /* font size row */
    y += 20;
    int nbase = gui_font_base_count();
    for (int i = 0; i < nbase && i < 3; i++) {
        int bx = x + i * (SB_W + 8);
        if (mx >= bx && mx < bx + SB_W + 4 && my >= y && my < y + SB_H) {
            gui_font_set_active(i);
            st->status = "字体大小已更新";
            return 1;
        }
    }
    return 0;
}

const gui_app_module_t gui_app_settings = {
    .mode     = GUI_APP_SETTINGS,
    .name     = "设置",
    .desc     = "主题、字体、系统信息",
    .draw     = app_settings_draw,
    .on_key   = 0,
    .on_tick  = 0,
    .on_click = app_settings_click,
};
