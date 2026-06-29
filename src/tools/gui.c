#include <stdbool.h>
#include <stdint.h>

#include "../acpi.h"
#include "../block.h"
#include "../core/pmm.h"
#include "../core/task.h"
#include "../fcntl.h"
#include "../fs.h"
#include "../graphics/font_cjk.h"
#include "../graphics/gui_font.h"
#include "../graphics/gui_wall.h"
#include "../graphics/gui_icons.h"
#include "../graphics/graphics.h"
#include "../input/mouse.h"
#include "../net.h"
#include "../usb_hid.h"
#include "../xhci.h"
#include "../string.h"
#include "../tls.h"
#include "../unistd.h"
#include "../user/app.h"
#include "../vfs.h"
#include "../gui/wm.h"
#include "../gui/gui_state.h"
#include "../gui/gui_dirty.h"
#include "../gui/gui_draw.h"
#include "../gui/gui_app.h"
#include "../shell/shell.h"
#include "tool.h"
#include "cc.h"
#include "python.h"

extern void task_yield(void);
extern int hbos_gcc_run_file_capture(const char *path, char *out, uint32_t out_cap);
extern const char *hbos_gcc_last_error(void);
extern int hbos_gcc_last_error_line(void);
extern int hbos_gcc_last_return(void);

#define PANEL_FILES 0
#define PANEL_DISK  1
#define PANEL_SYS   2
#define PANEL_APPS  3

#define FILE_ACTION_BASE 100
#define APP_ACTION_BASE  200

#define ACTION_W 116
#define ACTION_H 28
#define FILE_ACTION_COUNT 7
#define GUI_MOUSE_POLL_BUDGET 64   /* drain the queue fully so motion doesn't
                                      back up into bursty "忽快忽慢" jumps */
#define TASKBAR_H 50
#define NOTE_EDIT_CAP 512
#define BROWSER_URL_CAP 160
#define BROWSER_PAGE_CAP 2048
#define CODE_EDIT_CAP 4096
#define CODE_OUTPUT_CAP 256
#define CODE_VIEW_ROWS 10
#define SNAKE_W 16
#define SNAKE_H 10
#define GUI_PAGE_SIZE 4096ULL
#define FILE_LIST_ROWS 8
#define FILE_ROW_H 30
#define NOTE_FILE_ROWS 7
#define GUI_PATH_MAX 256
#define CODE_CMD_SAVE    1
#define CODE_CMD_RUN     2
#define CODE_CMD_OPEN    3
#define CODE_CMD_GUI_RUN 4

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);

// Palette — dark branch follows KDE Breeze Dark (cool grays, single blue accent
// #3daee9). The historical cyber_* names are kept so call sites need no churn;
// only the values changed. Light branch retuned toward Breeze Light.
static inline uint32_t cyber_bg_top(int light) { return light ? rgb(239, 240, 241) : rgb(35, 38, 41); }
static inline uint32_t cyber_bg_bot(int light) { return light ? rgb(226, 228, 231) : rgb(27, 30, 33); }
static inline uint32_t cyber_neon_pink(int light) { return light ? rgb(41, 128, 185) : rgb(41, 128, 185); }   // secondary blue
static inline uint32_t cyber_neon_cyan(int light) { return light ? rgb(41, 174, 233) : rgb(61, 174, 233); }   // accent #3daee9
static inline uint32_t cyber_neon_yellow(int light) { return light ? rgb(230, 110, 0) : rgb(246, 116, 0); }   // highlight #f67400
static inline uint32_t cyber_neon_purple(int light) { return light ? rgb(39, 160, 90) : rgb(39, 174, 96); }   // positive green
static inline uint32_t cyber_text(int light) { return light ? rgb(35, 38, 41) : rgb(239, 240, 241); }
static inline uint32_t cyber_text_muted(int light) { return light ? rgb(110, 116, 122) : rgb(160, 167, 173); }
static inline uint32_t cyber_border(int light) { return light ? rgb(188, 192, 196) : rgb(60, 64, 69); }
static inline uint32_t cyber_card_bg_top(int light) { return light ? rgb(252, 252, 252) : rgb(49, 54, 59); }
static inline uint32_t cyber_card_bg_bot(int light) { return light ? rgb(239, 240, 241) : rgb(42, 46, 50); }

static char g_code_buf[CODE_EDIT_CAP];
static char g_code_output[CODE_OUTPUT_CAP];

typedef struct {
    int content_w;
    int side_w;
    int editor_x;
    int editor_y;
    int editor_w;
    int editor_h;
    int bottom_y;
    int bottom_h;
    int row_h;
    int line_no_w;
    int view_rows;
    int file_rows;
} code_layout_t;

typedef struct {
    const char *name;
    const char *description;
    int mode;
} gui_app_meta_t;

typedef struct {
    const char *label;
    int action;
} gui_file_action_t;

static const gui_app_meta_t gui_apps[] = {
    {"记事本", "编辑笔记文件", GUI_APP_NOTES},
    {"计算器", "方向键调整数值", GUI_APP_CALC},
    {"文件统计", "统计选中文件行数和字节", GUI_APP_UWC},
    {"贪吃蛇", "方向键移动", GUI_APP_SNAKE},
    {"浏览器", "打开 HTTP/HTTPS 网页", GUI_APP_BROWSER},
    {"代码工作台", "编辑、保存、运行 C 文件", GUI_APP_CODE},
    {"控制台终端", "运行命令与系统交互", GUI_APP_DIAG},
    {"时钟", "实时时钟与日期", GUI_APP_CLOCK},
};

static const gui_file_action_t gui_file_actions[FILE_ACTION_COUNT] = {
    {"新建",   0},
    {"打开",   1},
    {"上级",   9},
    {"复制",   7},
    {"重命名", 8},
    {"清空",   3},
    {"删除",   4},
};

static uint32_t gui_app_count(void) {
    return (uint32_t)(sizeof(gui_apps) / sizeof(gui_apps[0]));
}

static file_t *selected_file(gui_state_t *st);
static void gui_select_file(gui_state_t *st, int index);
static int gui_select_path(gui_state_t *st, const char *path);
static void draw_desktop(int w, int h, gui_state_t *st);
static void draw_start_menu(gui_state_t *st);
static void draw_window_switcher(int w, int h, gui_state_t *st);
static void gui_sync_focus(gui_state_t *st);

static void clamp_window(gui_state_t *st, int w, int h, int win_w, int win_h) {
    if (st->win_x < 4) st->win_x = 4;
    if (st->win_y < 4) st->win_y = 4;
    if (st->win_x + win_w > w - 4) st->win_x = w - win_w - 4;
    if (st->win_y + win_h > h - TASKBAR_H - 4) st->win_y = h - TASKBAR_H - win_h - 4;
    if (st->win_x < 4) st->win_x = 4;
    if (st->win_y < 4) st->win_y = 4;
}

static wm_window_t *gui_active_window(gui_state_t *st) {
    return wm_get_active(&st->wm);
}

static const char *gui_window_title(const wm_window_t *win) {
    return wm_window_title((wm_window_t *)win);
}

static void gui_window_metrics(gui_state_t *st, int w, int h, const wm_window_t *win, int idx,
                               int *win_x, int *win_y, int *win_w, int *win_h) {
    wm_get_window_rect(&st->wm, idx, win_x, win_y, win_w, win_h);
    (void)w; (void)h; (void)win;
}

static void gui_sync_focus(gui_state_t *st) {
    wm_window_t *win = gui_active_window(st);
    if (!win) {
        st->app_mode = GUI_APP_NONE;
        st->win_x = 0;
        st->win_y = 0;
        return;
    }
    st->win_x = win->x;
    st->win_y = win->y;
    if (win->kind == WM_WIN_PANEL) {
        st->app_mode = GUI_APP_NONE;
        st->active = win->mode;
    } else {
        st->app_mode = win->mode;
        st->active = PANEL_APPS;
    }
}

static void gui_store_focus(gui_state_t *st) {
    wm_window_t *win = gui_active_window(st);
    if (!win) return;
    win->x = st->win_x;
    win->y = st->win_y;
}

static void gui_set_active_window_pos(gui_state_t *st, int x, int y) {
    st->win_x = x;
    st->win_y = y;
    gui_store_focus(st);
}

static void gui_focus_window(gui_state_t *st, int idx) {
    wm_focus_window(&st->wm, idx);
    gui_sync_focus(st);
}

static int gui_open_window(gui_state_t *st, int kind, int mode, int unique) {
    int idx = wm_open_window(&st->wm, kind, mode, unique);
    if (idx >= 0) gui_sync_focus(st);
    else st->status = "窗口数量已满";
    return idx;
}

static void gui_close_window(gui_state_t *st, int idx) {
    wm_close_window(&st->wm, idx);
    gui_sync_focus(st);
    st->status = "窗口已关闭";
}

static void gui_focus_next_window(gui_state_t *st, int dir) {
    wm_focus_next(&st->wm, dir);
    gui_sync_focus(st);
    st->status = "已切换窗口";
}

static void gui_open_panel_window(gui_state_t *st, int panel) {
    (void)gui_open_window(st, WM_WIN_PANEL, panel, 1);
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void rect(int x, int y, int w, int h, uint32_t color);
static void draw_splash_window(int w, int h, int ticks, int light);

static uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint32_t rgb_mix(uint32_t c1, uint32_t c2, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t r = (uint8_t)(r1 + ((int)r2 - (int)r1) * t / 255);
    uint8_t g = (uint8_t)(g1 + ((int)g2 - (int)g1) * t / 255);
    uint8_t b = (uint8_t)(b1 + ((int)b2 - (int)b1) * t / 255);
    return rgb(r, g, b);
}

static uint32_t rgb_lift(uint32_t c, int d) {
    int r = (int)((c >> 16) & 0xFF) + d;
    int g = (int)((c >> 8) & 0xFF) + d;
    int b = (int)(c & 0xFF) + d;
    return rgb(clamp8(r), clamp8(g), clamp8(b));
}

static void vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) {
        rect(x, y + i, w, 1, rgb_mix(top, bottom, (i * 255) / (h > 1 ? h - 1 : 1)));
    }
}

static void hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) {
        rect(x + i, y, 1, h, rgb_mix(left, right, (i * 255) / (w > 1 ? w - 1 : 1)));
    }
}

static void soft_shadow(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    rect(x + 3, y + h, w - 1, 2, rgb(3, 6, 9));
    rect(x + 6, y + h + 2, w - 5, 2, rgb(8, 13, 18));
    rect(x + 10, y + h + 4, w - 12, 1, rgb(14, 20, 25));
    rect(x + w, y + 3, 2, h - 1, rgb(3, 6, 9));
    rect(x + w + 2, y + 6, 2, h - 5, rgb(8, 13, 18));
    rect(x + w + 4, y + 10, 1, h - 12, rgb(14, 20, 25));
}

static uint32_t *g_gui_surface = 0;
static uint32_t g_gui_surface_pitch = 0;
static int g_gui_surface_w = 0;
static int g_gui_surface_h = 0;
static uint8_t g_layer_opacity = 255;

/* ── Script GUI hooks (defined after text()/key_poll()) ─────── */
static const fb_info_t *g_script_fb;

void gui_set_layer_opacity(uint8_t opacity) {
    g_layer_opacity = opacity;
}

uint8_t gui_get_layer_opacity(void) {
    return g_layer_opacity;
}

static uint32_t apply_layer_opacity(uint32_t color) {
    if (g_layer_opacity >= 255) return color;
    if (g_layer_opacity == 0) return color & 0x00FFFFFF;
    uint8_t a = (color >> 24) & 0xFF;
    if (a == 0) a = 255;
    a = (uint8_t)((a * g_layer_opacity) / 255);
    return ((uint32_t)a << 24) | (color & 0x00FFFFFF);
}

void gui_set_surface(uint32_t *surface, int w, int h, uint32_t pitch_px) {
    g_gui_surface = surface;
    g_gui_surface_w = surface ? w : 0;
    g_gui_surface_h = surface ? h : 0;
    g_gui_surface_pitch = surface ? pitch_px : 0;
}

void gui_present_surface(const fb_info_t *fb) {
    if (!fb || !g_gui_surface) return;
    uint32_t fb_pitch = (uint32_t)(fb->pitch / 4);
    for (int y = 0; y < g_gui_surface_h; y++) {
        uint32_t *dst = fb->addr + (uint32_t)y * fb_pitch;
        uint32_t *src = g_gui_surface + (uint32_t)y * g_gui_surface_pitch;
        for (int x = 0; x < g_gui_surface_w; x++) dst[x] = src[x];
    }
}

void gui_present_rect(const fb_info_t *fb, int x, int y, int w, int h) {
    if (!fb || !g_gui_surface || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
    if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;
    if (w <= 0 || h <= 0) return;
    uint32_t fb_pitch = (uint32_t)(fb->pitch / 4);
    for (int yy = 0; yy < h; yy++) {
        uint32_t *dst = fb->addr + (uint32_t)(y + yy) * fb_pitch + (uint32_t)x;
        uint32_t *src = g_gui_surface + (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
        for (int xx = 0; xx < w; xx++) dst[xx] = src[xx];
    }
}

static void rect(int x, int y, int w, int h, uint32_t color);

static void rect_alpha(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (!gui_clip_intersect(&x, &y, &w, &h)) return;
    color = apply_layer_opacity(color);
    uint8_t alpha = (color >> 24) & 0xFF;
    if (alpha == 0) return;
    if (alpha == 0xFF) {
        rect(x, y, w, h, color);
        return;
    }
    if (g_gui_surface) {
        if (x >= g_gui_surface_w || y >= g_gui_surface_h) return;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (w <= 0 || h <= 0) return;
        if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
        if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;

        uint32_t src_r = (color >> 16) & 0xFF;
        uint32_t src_g = (color >> 8) & 0xFF;
        uint32_t src_b = color & 0xFF;
        uint32_t inv_alpha = 255 - alpha;

        for (int yy = 0; yy < h; yy++) {
            uint32_t *row = g_gui_surface + (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
            for (int xx = 0; xx < w; xx++) {
                uint32_t dst = row[xx];
                uint32_t dst_r = (dst >> 16) & 0xFF;
                uint32_t dst_g = (dst >> 8) & 0xFF;
                uint32_t dst_b = dst & 0xFF;

                uint32_t out_r = (src_r * alpha + dst_r * inv_alpha) / 255;
                uint32_t out_g = (src_g * alpha + dst_g * inv_alpha) / 255;
                uint32_t out_b = (src_b * alpha + dst_b * inv_alpha) / 255;

                row[xx] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
        return;
    }
    fb_fill_rect((uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}

static uint32_t rgb_mix_alpha(uint32_t c1, uint32_t c2, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    uint8_t a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t a = (uint8_t)(a1 + ((int)a2 - (int)a1) * t / 255);
    uint8_t r = (uint8_t)(r1 + ((int)r2 - (int)r1) * t / 255);
    uint8_t g = (uint8_t)(g1 + ((int)g2 - (int)g1) * t / 255);
    uint8_t b = (uint8_t)(b1 + ((int)b2 - (int)b1) * t / 255);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void vgradient_alpha(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) {
        rect_alpha(x, y + i, w, 1, rgb_mix_alpha(top, bottom, (i * 255) / (h > 1 ? h - 1 : 1)));
    }
}

static void rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (!gui_clip_intersect(&x, &y, &w, &h)) return;
    color = apply_layer_opacity(color);
    uint8_t layer_a = (color >> 24) & 0xFF;
    if (layer_a > 0 && layer_a < 255) {
        rect_alpha(x, y, w, h, color);
        return;
    }
    if (g_gui_surface) {
        if (x >= g_gui_surface_w || y >= g_gui_surface_h) return;
        if (x < 0) {
            w += x;
            x = 0;
        }
        if (y < 0) {
            h += y;
            y = 0;
        }
        if (w <= 0 || h <= 0) return;
        if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
        if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;
        for (int yy = 0; yy < h; yy++) {
            uint32_t *row = g_gui_surface + (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
            for (int xx = 0; xx < w; xx++) row[xx] = color;
        }
        return;
    }
    fb_fill_rect((uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}

static void border(int x, int y, int w, int h, uint32_t color) {
    rect(x, y, w, 1, color);
    rect(x, y + h - 1, w, 1, color);
    rect(x, y, 1, h, color);
    rect(x + w - 1, y, 1, h, color);
}

enum { RR_TL = 1, RR_TR = 2, RR_BL = 4, RR_BR = 8,
       RR_ALL = 15, RR_TOP = 3, RR_BOT = 12 };

// Fill one r×r corner box, anti-aliasing a quarter-circle (center cxc,cyc) of
// `color` over whatever is already in the surface — so rounded windows blend
// cleanly against the wallpaper/desktop drawn beneath them.
static void round_corner(int bx, int by, int r, int cxc, int cyc, uint32_t color) {
    if (!g_gui_surface) { return; }
    int x = bx, y = by, w = r, h = r;
    if (!gui_clip_intersect(&x, &y, &w, &h)) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= g_gui_surface_w || y >= g_gui_surface_h) return;
    if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
    if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;
    uint32_t op = g_layer_opacity;
    uint32_t base_a = (color >> 24) & 0xFF;
    if (base_a == 0) base_a = 255;
    uint32_t fa = base_a * op / 255;   // corner honors the color's own alpha too
    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    int rin = (r - 1) * (r - 1), rout = r * r;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = g_gui_surface +
                        (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
        for (int xx = 0; xx < w; xx++) {
            int dx = (x + xx) - cxc, dy = (y + yy) - cyc;
            int d2 = dx * dx + dy * dy;
            uint32_t cov;
            if (d2 <= rin) cov = 255;
            else if (d2 >= rout) cov = 0;
            else cov = (uint32_t)((rout - d2) * 255 / (rout - rin));
            if (cov == 0) continue;
            uint32_t a = cov * fa / 255;
            if (a == 0) continue;
            uint32_t d = row[xx];
            uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t inv = 255 - a;
            row[xx] = 0xFF000000 |
                      (((sr * a + dr * inv) / 255) << 16) |
                      (((sg * a + dg * inv) / 255) << 8) |
                      ((sb * a + db * inv) / 255);
        }
    }
}

// Filled rectangle with selectively rounded corners (RR_* mask).
static void fill_round_rect(int x, int y, int w, int h, int r,
                            uint32_t color, int corners) {
    if (w <= 0 || h <= 0) return;
    if (r < 1) { rect(x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    rect(x, y + r, w, h - 2 * r, color);          // middle band, full width
    rect(x + r, y, w - 2 * r, r, color);          // top band between corners
    rect(x + r, y + h - r, w - 2 * r, r, color);  // bottom band between corners
    if (corners & RR_TL) round_corner(x, y, r, x + r, y + r, color);
    else rect(x, y, r, r, color);
    if (corners & RR_TR) round_corner(x + w - r, y, r, x + w - r, y + r, color);
    else rect(x + w - r, y, r, r, color);
    if (corners & RR_BL) round_corner(x, y + h - r, r, x + r, y + h - r, color);
    else rect(x, y + h - r, r, r, color);
    if (corners & RR_BR) round_corner(x + w - r, y + h - r, r, x + w - r, y + h - r, color);
    else rect(x + w - r, y + h - r, r, r, color);
}

static void draw_panel_shell(int x, int y, int w, int h, uint32_t top,
                             uint32_t bottom, uint32_t border_c, uint32_t accent) {
    if (w <= 0 || h <= 0) return;
    // Breeze-flat: near-solid fill with only a whisper of top sheen, a single
    // 1px border, and a slim accent edge (no glossy highlight/shadow runs).
    vgradient(x, y, w, h, rgb_lift(top, 4), bottom);
    if (accent) {
        rect(x, y + 1, 3, h - 2, accent);
    }
    border(x, y, w, h, border_c);
}

static void draw_inset_shell(int x, int y, int w, int h, uint32_t border_c) {
    if (w <= 0 || h <= 0) return;
    vgradient(x, y, w, h, rgb(9, 13, 16), rgb(4, 7, 10));
    rect(x, y, w, 1, rgb_lift(border_c, -28));
    rect(x, y + h - 1, w, 1, rgb_lift(border_c, 12));
    border(x, y, w, h, border_c);
}

enum {
    GUI_CTRL_MIN = 0,
    GUI_CTRL_MAX = 1,
    GUI_CTRL_CLOSE = 2,
};

static void draw_window_control_icon(int x, int y, int kind, int restore, uint32_t color) {
    if (kind == GUI_CTRL_CLOSE) {
        for (int i = 0; i < 8; i++) {
            rect(x + i, y + i, 2, 2, color);
            rect(x + 7 - i, y + i, 2, 2, color);
        }
    } else if (kind == GUI_CTRL_MAX) {
        if (restore) {
            rect(x + 4, y + 1, 9, 1, color);
            rect(x + 12, y + 1, 1, 8, color);
            rect(x + 4, y + 8, 5, 1, color);
            rect(x + 4, y + 1, 1, 4, color);
        }
        rect(x, y + 4, 10, 1, color);
        rect(x, y + 4, 1, 9, color);
        rect(x + 9, y + 4, 1, 9, color);
        rect(x, y + 12, 10, 1, color);
    } else {
        rect(x, y + 10, 11, 2, color);
    }
}

// Bilinearly sample the coverage map at the fractional source position that
// dest offset `d` maps to under `scale` (8.8 fixed point). Used for scale>1 so
// upscaled text (titles, clock) gets smooth anti-aliased edges instead of the
// blocky staircase nearest-neighbour produces.
static inline uint32_t cov_bilinear(const uint8_t *cov, int w, int h,
                                    int dx, int dy, int scale) {
    int fx = (dx * 256) / scale, fy = (dy * 256) / scale;
    int sx0 = fx >> 8, sy0 = fy >> 8;
    if (sx0 < 0) sx0 = 0;
    if (sx0 > w - 1) sx0 = w - 1;
    if (sy0 < 0) sy0 = 0;
    if (sy0 > h - 1) sy0 = h - 1;
    int sx1 = sx0 + 1 < w ? sx0 + 1 : sx0;
    int sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
    int tx = fx & 255, ty = fy & 255;
    const uint8_t *r0 = cov + (uint32_t)sy0 * w;
    const uint8_t *r1 = cov + (uint32_t)sy1 * w;
    int top = r0[sx0] + (((int)r0[sx1] - (int)r0[sx0]) * tx >> 8);
    int bot = r1[sx0] + (((int)r1[sx1] - (int)r1[sx0]) * tx >> 8);
    int v = top + ((bot - top) * ty >> 8);
    return v < 0 ? 0 : (v > 255 ? 255 : (uint32_t)v);
}

// Alpha-blend an 8-bit coverage glyph onto the GUI surface at (x, y) (the
// glyph's top-left). `scale` 1 samples 1:1; scale>1 bilinearly upscales for
// crisp large text. Foreground is `color` (0x00RRGGBB), modulated per pixel by
// the glyph coverage and the active layer opacity.
static void blit_glyph(int x, int y, const gui_glyph_t *g, uint32_t color, int scale) {
    if (!g->coverage || g->width == 0 || g->height == 0) return;
    if (scale < 1) scale = 1;
    int dw = (int)g->width * scale;
    int dh = (int)g->height * scale;
    int cx = x, cy = y, cw = dw, ch = dh;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    int off_x = cx - x, off_y = cy - y;

    uint32_t base_a = (color >> 24) & 0xFF;
    if (base_a == 0) base_a = 255;
    uint32_t fg_a = base_a * g_layer_opacity / 255;
    if (fg_a == 0) return;
    uint32_t src_r = (color >> 16) & 0xFF;
    uint32_t src_g = (color >> 8) & 0xFF;
    uint32_t src_b = color & 0xFF;
    int width = g->width;
    int smooth = scale > 1;

    if (g_gui_surface) {
        if (cx < 0) { cw += cx; off_x -= cx; cx = 0; }
        if (cy < 0) { ch += cy; off_y -= cy; cy = 0; }
        if (cx >= g_gui_surface_w || cy >= g_gui_surface_h) return;
        if (cx + cw > g_gui_surface_w) cw = g_gui_surface_w - cx;
        if (cy + ch > g_gui_surface_h) ch = g_gui_surface_h - cy;
        for (int yy = 0; yy < ch; yy++) {
            int sy = (off_y + yy) / scale;
            const uint8_t *cov_row = g->coverage + (uint32_t)sy * width;
            uint32_t *row = g_gui_surface +
                            (uint32_t)(cy + yy) * g_gui_surface_pitch + (uint32_t)cx;
            for (int xx = 0; xx < cw; xx++) {
                uint32_t cov = smooth
                    ? cov_bilinear(g->coverage, width, g->height,
                                   off_x + xx, off_y + yy, scale)
                    : cov_row[(off_x + xx) / scale];
                if (cov == 0) continue;
                uint32_t a = cov * fg_a / 255;
                if (a == 0) continue;
                uint32_t dst = row[xx];
                uint32_t inv = 255 - a;
                uint32_t dr = (dst >> 16) & 0xFF;
                uint32_t dg = (dst >> 8) & 0xFF;
                uint32_t db = dst & 0xFF;
                uint32_t out_r = (src_r * a + dr * inv) / 255;
                uint32_t out_g = (src_g * a + dg * inv) / 255;
                uint32_t out_b = (src_b * a + db * inv) / 255;
                row[xx] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
        return;
    }
    // Framebuffer fallback (no compositor surface): threshold the coverage.
    for (int yy = 0; yy < ch; yy++) {
        int sy = (off_y + yy) / scale;
        const uint8_t *cov_row = g->coverage + (uint32_t)sy * width;
        for (int xx = 0; xx < cw; xx++) {
            if (cov_row[(off_x + xx) / scale] >= 128)
                fb_put_pixel((uint64_t)(cx + xx), (uint64_t)(cy + yy), color);
        }
    }
}

// Crisp 1-bit monospace glyph from the classic 8x16 VGA console font (the same
// font the TUI uses). No anti-aliasing — solid foreground pixels — so the code
// editor reads like a real terminal instead of the soft proportional GUI font.
extern const uint8_t *fb_console_glyph(uint32_t cp);
#define MONO_GLYPH_W 8
#define MONO_GLYPH_H 16
static void blit_mono_glyph(int x, int y, const uint8_t *glyph, uint32_t color, int scale) {
    if (!glyph) return;
    if (scale < 1) scale = 1;
    int dw = MONO_GLYPH_W * scale, dh = MONO_GLYPH_H * scale;
    int cx = x, cy = y, cw = dw, ch = dh;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    int off_x = cx - x, off_y = cy - y;

    uint32_t base_a = (color >> 24) & 0xFF;
    if (base_a == 0) base_a = 255;
    uint32_t fg_a = base_a * g_layer_opacity / 255;
    if (fg_a == 0) return;
    uint32_t src_r = (color >> 16) & 0xFF;
    uint32_t src_g = (color >> 8) & 0xFF;
    uint32_t src_b = color & 0xFF;

    if (!g_gui_surface) {
        for (int yy = 0; yy < ch; yy++) {
            uint8_t bits = glyph[(off_y + yy) / scale];
            for (int xx = 0; xx < cw; xx++)
                if (bits & (0x80 >> ((off_x + xx) / scale)))
                    fb_put_pixel((uint64_t)(cx + xx), (uint64_t)(cy + yy), color);
        }
        return;
    }
    if (cx < 0) { cw += cx; off_x -= cx; cx = 0; }
    if (cy < 0) { ch += cy; off_y -= cy; cy = 0; }
    if (cx >= g_gui_surface_w || cy >= g_gui_surface_h) return;
    if (cx + cw > g_gui_surface_w) cw = g_gui_surface_w - cx;
    if (cy + ch > g_gui_surface_h) ch = g_gui_surface_h - cy;
    for (int yy = 0; yy < ch; yy++) {
        uint8_t bits = glyph[(off_y + yy) / scale];
        uint32_t *row = g_gui_surface +
                        (uint32_t)(cy + yy) * g_gui_surface_pitch + (uint32_t)cx;
        for (int xx = 0; xx < cw; xx++) {
            if (!(bits & (0x80 >> ((off_x + xx) / scale)))) continue;
            if (fg_a >= 255) { row[xx] = 0xFF000000 | (color & 0xFFFFFF); continue; }
            uint32_t dst = row[xx], inv = 255 - fg_a;
            uint32_t out_r = (src_r * fg_a + ((dst >> 16) & 0xFF) * inv) / 255;
            uint32_t out_g = (src_g * fg_a + ((dst >> 8) & 0xFF) * inv) / 255;
            uint32_t out_b = (src_b * fg_a + (dst & 0xFF) * inv) / 255;
            row[xx] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

// Blit a flat icon tile (RGBA, from the atlas), bilinearly scaled to sz×sz and
// alpha-blended onto the surface. Premultiplied-alpha bilinear keeps the edges
// smooth when downscaling the 64px master to taskbar/launcher sizes. Replaces
// the old hand-drawn rect motifs + their colored backing square.
static void blit_icon(int x, int y, int sz, int id) {
    int tile = 0;
    const uint32_t *src = gui_icon_tile(id, &tile);
    if (!src || tile <= 0 || sz <= 0 || !g_gui_surface) return;
    int cx = x, cy = y, cw = sz, ch = sz;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    int off_x = cx - x, off_y = cy - y;
    if (cx + cw > g_gui_surface_w) cw = g_gui_surface_w - cx;
    if (cy + ch > g_gui_surface_h) ch = g_gui_surface_h - cy;

    // 16.16 source coordinate of each dest pixel center: (d+0.5)*tile/sz - 0.5.
    uint32_t scale = ((uint32_t)tile << 16) / (uint32_t)sz;
    int bias = (int)(scale >> 1) - 32768;          // +0.5 dst, -0.5 src
    int maxc = (tile - 1) << 16;

    for (int yy = 0; yy < ch; yy++) {
        int fy = (off_y + yy) * (int)scale + bias;
        if (fy < 0) fy = 0;
        else if (fy > maxc) fy = maxc;
        int sy0 = fy >> 16, sy1 = sy0 + 1;
        if (sy1 >= tile) sy1 = tile - 1;
        uint32_t wy = (uint32_t)(fy & 0xFFFF);
        const uint32_t *r0 = src + (uint32_t)sy0 * tile;
        const uint32_t *r1 = src + (uint32_t)sy1 * tile;
        uint32_t *drow = g_gui_surface +
                         (uint32_t)(cy + yy) * g_gui_surface_pitch + (uint32_t)cx;
        for (int xx = 0; xx < cw; xx++) {
            int fx = (off_x + xx) * (int)scale + bias;
            if (fx < 0) fx = 0;
            else if (fx > maxc) fx = maxc;
            int sx0 = fx >> 16, sx1 = sx0 + 1;
            if (sx1 >= tile) sx1 = tile - 1;
            uint32_t wx = (uint32_t)(fx & 0xFFFF);

            // Bilerp in premultiplied space. Weights are .14 fixed (each axis
            // weight >>9 = .7, product = .14, sum of the 4 ≈ 16384) — chosen so
            // rgb*a*weight stays inside uint32.
            uint32_t pr = 0, pg = 0, pb = 0, pa = 0;
            const uint32_t *rows[2] = { r0, r1 };
            int sxs[2] = { sx0, sx1 };
            uint32_t wxs[2] = { (65536 - wx) >> 9, wx >> 9 };
            uint32_t wys[2] = { (65536 - wy) >> 9, wy >> 9 };
            for (int j = 0; j < 2; j++) {
                for (int i = 0; i < 2; i++) {
                    uint32_t px = rows[j][sxs[i]];
                    uint32_t a = (px >> 24) & 0xFF;
                    uint32_t wgt = wxs[i] * wys[j];           // .14
                    uint32_t am = a * wgt;                    // <= 255*16384
                    pa += am;
                    pr += ((px >> 16) & 0xFF) * am / 255;
                    pg += ((px >> 8) & 0xFF) * am / 255;
                    pb += (px & 0xFF) * am / 255;
                }
            }
            uint32_t a = pa >> 14;                            // back to 0..255
            a = a * g_layer_opacity / 255;
            if (a == 0) continue;
            if (a > 255) a = 255;
            // premultiplied rgb (already * alpha) for over-opaque compositing:
            uint32_t sr = pr >> 14, sg = pg >> 14, sb = pb >> 14;
            uint32_t dst = drow[xx], inv = 255 - a;
            uint32_t orr = sr + ((dst >> 16) & 0xFF) * inv / 255;
            uint32_t og  = sg + ((dst >> 8) & 0xFF) * inv / 255;
            uint32_t ob  = sb + (dst & 0xFF) * inv / 255;
            if (orr > 255) orr = 255;
            if (og > 255) og = 255;
            if (ob > 255) ob = 255;
            drow[xx] = 0xFF000000 | (orr << 16) | (og << 8) | ob;
        }
    }
}

// Map an app mode / panel id to its atlas icon id.
static int app_icon_id(int mode) {
    switch (mode) {
        case GUI_APP_NOTES:   return ICON_NOTES;
        case GUI_APP_CALC:    return ICON_CALC;
        case GUI_APP_UWC:     return ICON_UWC;
        case GUI_APP_SNAKE:   return ICON_SNAKE;
        case GUI_APP_BROWSER: return ICON_BROWSER;
        case GUI_APP_CODE:    return ICON_CODE;
        case GUI_APP_DIAG:    return ICON_TERM;
        case GUI_APP_CLOCK:   return ICON_CLOCK;
        default:              return ICON_APPS;
    }
}
static int panel_icon_id(int panel) {
    switch (panel) {
        case PANEL_FILES: return ICON_FILES;
        case PANEL_DISK:  return ICON_DISK;
        case PANEL_SYS:   return ICON_SYS;
        case PANEL_APPS:
        default:          return ICON_APPS;
    }
}

// Draw one printable ASCII char in the mono console font; returns the cell width.
static int draw_mono_char(int x, int y, char c, uint32_t color) {
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F)
        blit_mono_glyph(x, y, fb_console_glyph((uint32_t)(unsigned char)c), color, 1);
    return MONO_GLYPH_W;
}

// Draw one codepoint with its top at `y`, returning the pen advance. Latin and
// CJK share a baseline (y + ascent), so mixed text aligns naturally.
// Resolve a codepoint+scale to a concrete glyph. Prefer the native large size
// (idx 1 = 2x base px) for even scales when the glyph exists, so titles and big
// digits render crisply instead of being upscaled from the 16px base. *out_idx
// is the chosen size, *out_sub the residual draw scale. Because the large size
// is exactly 2x the base, baselines/advances line up with the base path, so a
// string mixing both sizes (e.g. a scaled title with a rare uncovered glyph)
// still aligns on one baseline.
static int gui_resolve_glyph(uint32_t cp, int scale, gui_glyph_t *g,
                             int *out_idx, int *out_sub) {
    int base_idx  = gui_font_active_base_idx();
    int large_idx = gui_font_active_large_idx();
    if (scale >= 2 && large_idx >= 0) {
        int base = gui_font_size_px(base_idx), large = gui_font_size_px(large_idx);
        if (base > 0 && large > 0 && (scale * base) % large == 0) {
            int sub = scale * base / large;
            if (sub >= 1 && gui_font_lookup_n(cp, large_idx, g)) {
                *out_idx = large_idx; *out_sub = sub; return 1;
            }
        }
    }
    if (gui_font_lookup_n(cp, base_idx, g)) {
        *out_idx = base_idx; *out_sub = scale; return 1;
    }
    return 0;
}

static int gui_cp_advance(uint32_t cp, int scale) {
    gui_glyph_t g;
    int idx, sub;
    if (!gui_resolve_glyph(cp, scale, &g, &idx, &sub)) return 6 * scale;
    int adv = (int)g.advance * sub;
    if (adv <= 0) adv = (g.width ? (g.width + 1) : 4) * sub;
    return adv;
}

static int draw_text_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale) {
    if (scale < 1) scale = 1;
    gui_glyph_t g;
    int idx, sub;
    if (!gui_resolve_glyph(cp, scale, &g, &idx, &sub)) {
        if (!gui_resolve_glyph('?', scale, &g, &idx, &sub)) return 6 * scale;
    }
    int baseline = y + gui_font_ascent_n(idx) * sub;
    int gx = x + (int)g.bearing_x * sub;
    int gy = baseline - (int)g.bearing_y * sub;
    blit_glyph(gx, gy, &g, color, sub);
    int adv = (int)g.advance * sub;
    if (adv <= 0) adv = (g.width ? (g.width + 1) : 4) * sub;
    return adv;
}

// Total pen advance of a UTF-8 string (no drawing). Used for centering/layout.
static int text_width(const char *s, int scale) {
    if (!s) return 0;
    if (scale < 1) scale = 1;
    int w = 0;
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';
        w += gui_cp_advance(cp, scale);
    }
    return w;
}

static void text(int x, int y, const char *s, uint32_t color, int scale) {
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        x += draw_text_codepoint(x, y, cp, color, scale);
    }
}

// Terminal-style text: ASCII in the crisp 8x16 console bitmap font (fixed 8px
// cells, like the real TUI), any CJK falls back to the proportional GUI font so
// shell output containing Chinese still renders. y is the TOP of the cell.
// Returns the end pen x.
static int text_mono(int x, int y, int max_x, const char *s, uint32_t color) {
    if (!s) return x;
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';
        if (cp < 0x80) {
            if (x + MONO_GLYPH_W > max_x) break;
            draw_mono_char(x, y, (char)cp, color);
            x += MONO_GLYPH_W;
        } else {
            int adv = gui_cp_advance(cp, 1);
            if (x + adv > max_x) break;
            // Align the proportional baseline near the bottom of the mono cell.
            draw_text_codepoint(x, y + MONO_GLYPH_H - gui_font_ascent() - 2, cp, color, 1);
            x += adv;
        }
    }
    return x;
}

static void text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale) {
    if (!s || max_x <= x) return;
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        int advance = gui_cp_advance(cp, scale);
        if (x + advance > max_x) break;
        draw_text_codepoint(x, y, cp, color, scale);
        x += advance;
    }
}

static void append_char(char *buf, uint32_t cap, uint32_t *pos, char c) {
    if (*pos + 1 >= cap) return;
    buf[(*pos)++] = c;
    buf[*pos] = 0;
}

static void append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s) {
    while (s && *s && *pos + 1 < cap) append_char(buf, cap, pos, *s++);
}

static void append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v) {
    char tmp[16];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));
    while (n) append_char(buf, cap, pos, tmp[--n]);
}

static void append_int(char *buf, uint32_t cap, uint32_t *pos, int v) {
    if (v < 0) append_char(buf, cap, pos, '-');
    append_uint(buf, cap, pos, (uint32_t)(v < 0 ? -v : v));
}

static const char *gui_file_path(gui_state_t *st) {
    if (!st->file_path[0]) {
        st->file_path[0] = '/';
        st->file_path[1] = 0;
    }
    return st->file_path;
}

static void gui_set_file_path(gui_state_t *st, const char *path) {
    uint32_t i = 0;
    while (path && path[i] && i + 1 < sizeof(st->file_path)) {
        st->file_path[i] = path[i];
        i++;
    }
    if (i == 0) st->file_path[i++] = '/';
    st->file_path[i] = 0;
    st->selected_file = 0;
    st->last_clicked_file = -1;
    st->delete_confirm_index = -1;
    st->rename_active = 0;
}

static int gui_path_join(const char *base, const char *name, char *out, uint32_t cap) {
    if (!base || !name || !out || cap == 0) return -1;
    return vfs_resolve_path(base, name, out, cap);
}

static uint32_t gui_file_count(gui_state_t *st) {
    char name[VFS_MAX_NAME];
    uint32_t type;
    uint32_t count = 0;
    const char *path = gui_file_path(st);
    while (vfs_readdir_at(path, count, name, &type) == 0) count++;
    return count;
}

static int gui_file_entry(gui_state_t *st, uint32_t index, char *name,
                          uint32_t *type, vfs_node_t **node, char *full,
                          uint32_t full_cap) {
    const char *path = gui_file_path(st);
    uint32_t local_type = 0;
    if (!type) type = &local_type;
    if (!name) return -1;
    if (vfs_readdir_at(path, index, name, type) < 0) return -1;
    if (full && gui_path_join(path, name, full, full_cap) < 0) return -1;
    if (node) *node = full ? vfs_lookup(full) : NULL;
    return 0;
}

static int gui_selected_entry(gui_state_t *st, char *name, uint32_t *type,
                              vfs_node_t **node, char *full, uint32_t full_cap) {
    uint32_t count = gui_file_count(st);
    if (count == 0) {
        st->selected_file = 0;
        return -1;
    }
    if (st->selected_file < 0) st->selected_file = 0;
    if ((uint32_t)st->selected_file >= count) st->selected_file = (int)count - 1;
    return gui_file_entry(st, (uint32_t)st->selected_file, name, type, node, full, full_cap);
}

static file_t *gui_selected_regular_file(gui_state_t *st) {
    char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
    uint32_t type;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, full, sizeof(full)) < 0)
        return 0;
    if (!node || node->type != VFS_NODE_FILE) return 0;
    return fs_find_file(full);
}

static const char *gui_node_type_label(uint32_t type) {
    if (type == VFS_NODE_DIR) return "目录";
    if (type == VFS_NODE_CHARDEV) return "设备";
    return "文件";
}

static void gui_parent_path(const char *path, char *out, uint32_t cap) {
    if (!out || cap == 0) return;
    if (!path || strcmp(path, "/") == 0) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    uint32_t len = (uint32_t)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    while (len > 1 && path[len - 1] != '/') len--;
    if (len <= 1) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    if (len >= cap) len = cap - 1;
    for (uint32_t i = 0; i < len; i++) out[i] = path[i];
    out[len] = 0;
}

static void gui_go_parent(gui_state_t *st) {
    char parent[GUI_PATH_MAX];
    gui_parent_path(gui_file_path(st), parent, sizeof(parent));
    gui_set_file_path(st, parent);
    st->status = "返回上级目录";
}

static void gui_make_note_name(char *out, uint32_t cap, uint32_t index) {
    uint32_t pos = 0;
    out[0] = 0;
    append_str(out, cap, &pos, "gui-note");
    if (index > 0) {
        append_char(out, cap, &pos, '-');
        append_uint(out, cap, &pos, index);
    }
}

static void gui_set_note_name(gui_state_t *st, const char *name) {
    uint32_t i = 0;
    while (name && name[i] && i + 1 < sizeof(st->note_name)) {
        st->note_name[i] = name[i];
        i++;
    }
    st->note_name[i] = 0;
    st->note_loaded = 0;
}

static const char *gui_note_name(gui_state_t *st) {
    if (!st->note_name[0]) gui_set_note_name(st, "gui-note");
    return st->note_name;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0f) + ((v >> 4) * 10));
}

uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t cmos_second(void) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t sec = cmos_read(0x00);
    if ((status_b & 0x04) == 0) sec = bcd_to_bin(sec);
    return sec;
}

static void time_line(char *buf, uint32_t cap) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t hour = cmos_read(0x04);
    uint8_t min = cmos_read(0x02);
    uint8_t sec = cmos_read(0x00);
    if ((status_b & 0x04) == 0) {
        hour = bcd_to_bin(hour);
        min = bcd_to_bin(min);
        sec = bcd_to_bin(sec);
    }
    if ((status_b & 0x02) == 0) {
        uint8_t pm = hour & 0x80;
        hour &= 0x7f;
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }
    uint32_t pos = 0;
    buf[0] = 0;
    if (hour < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, hour);
    append_char(buf, cap, &pos, ':');
    if (min < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, min);
    append_char(buf, cap, &pos, ':');
    if (sec < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, sec);
}

static void line2(char *buf, uint32_t cap, const char *a, const char *b) {
    uint32_t pos = 0;
    buf[0] = 0;
    append_str(buf, cap, &pos, a);
    append_str(buf, cap, &pos, b);
}

static void line_u32(char *buf, uint32_t cap, const char *a, uint32_t v, const char *b) {
    uint32_t pos = 0;
    buf[0] = 0;
    append_str(buf, cap, &pos, a);
    append_uint(buf, cap, &pos, v);
    append_str(buf, cap, &pos, b);
}

// Pure 1:1 pointer mapping with a safety cap — the v0.1-beta3-pre3 feel. Every
// raw mouse count moves exactly one pixel, applied immediately with NO sub-pixel
// carry and NO acceleration. The carry/gain we tried later held back part of a
// small nudge until enough accumulated, which made short movements feel numb
// ("短移动不灵敏"). Direct integer mapping = crisp, responsive short moves.
static int clamp_delta(int v) {
    if (v > 80) return 80;
    if (v < -80) return -80;
    return v;
}

static void draw_button(int x, int y, const char *label, uint32_t color) {
    soft_shadow(x, y, ACTION_W, ACTION_H);
    draw_panel_shell(x, y, ACTION_W, ACTION_H,
                     rgb_lift(color, 34), rgb_lift(color, -24),
                     rgb_lift(color, -46), color);
    rect(x + 7, y + 5, ACTION_W - 14, 1, rgb_lift(color, 72));
    text(x + 13, y + 10, label, rgb(252, 254, 255), 1);
}

static void draw_small_button(int x, int y, int w, const char *label, uint32_t color) {
    // Flat Breeze button: solid surface, 1px border, centered label.
    vgradient(x, y, w, ACTION_H, rgb_lift(color, 8), rgb_lift(color, -8));
    border(x, y, w, ACTION_H, rgb_lift(color, -30));
    int tw = text_width(label, 1);
    int tx = x + (w - tw) / 2;
    if (tx < x + 5) tx = x + 5;
    int ty = y + (ACTION_H - gui_font_line_height()) / 2;
    if (ty < y) ty = y;
    text_clipped(tx, ty, x + w - 4, label, rgb(252, 254, 255), 1);
}

static uint32_t gui_file_action_color(int action) {
    // Breeze button semantics: primary action = accent blue, destructive = red,
    // everything else = neutral surface so the toolbar reads calm and flat.
    if (action == 1) return rgb(61, 174, 233);   // 打开 — primary
    if (action == 3) return rgb(218, 68, 83);     // 清空 — destructive
    if (action == 4) return rgb(218, 68, 83);     // 删除 — destructive
    return rgb(75, 81, 88);                        // neutral surface
}

static int gui_file_action_rect(int content_w, int index, int *x, int *y, int *w) {
    if (index < 0 || index >= FILE_ACTION_COUNT || content_w <= 0) return 0;
    int gap = 8;
    int available = content_w - gap * (FILE_ACTION_COUNT - 1);
    int bw = available / FILE_ACTION_COUNT;
    if (bw < 58) {
        gap = 6;
        available = content_w - gap * (FILE_ACTION_COUNT - 1);
        bw = available / FILE_ACTION_COUNT;
    }
    if (bw < 50) bw = 50;
    if (bw > 78) bw = 78;

    int total = bw * FILE_ACTION_COUNT + gap * (FILE_ACTION_COUNT - 1);
    int left = total < content_w ? (content_w - total) / 2 : 0;
    *x = left + index * (bw + gap);
    *y = 72;
    *w = bw;
    return 1;
}

static void draw_file_action_bar(int tx, int ty, int content_w) {
    for (int i = 0; i < FILE_ACTION_COUNT; i++) {
        int x, y, bw;
        if (!gui_file_action_rect(content_w, i, &x, &y, &bw)) continue;
        int action = gui_file_actions[i].action;
        draw_small_button(tx + x, ty + y, bw, gui_file_actions[i].label,
                          gui_file_action_color(action));
    }
}

static void draw_cursor(int x, int y, int edge) {
    uint32_t c = rgb(238, 246, 255);
    uint32_t d = rgb(20, 27, 34);

    if (edge == WM_EDGE_NONE || edge == WM_EDGE_N || edge == WM_EDGE_S) {
        for (int i = 0; i < 18; i++) rect(x, y + i, 2, 2, c);
        for (int i = 0; i < 12; i++) rect(x + i, y + i, 2, 2, c);
        rect(x + 5, y + 13, 9, 3, d);
        rect(x + 8, y + 15, 4, 7, d);
    } else if (edge == WM_EDGE_W || edge == WM_EDGE_E) {
        for (int i = 0; i < 18; i++) rect(x + 8, y + i, 2, 2, c);
        for (int i = 0; i < 12; i++) rect(x + 8, y + i, 2, 2, c);
        rect(x + 2, y + 4, 12, 2, c);
        rect(x + 2, y + 14, 12, 2, c);
    } else {
        for (int i = 0; i < 18; i++) rect(x, y + i, 2, 2, c);
        for (int i = 0; i < 12; i++) rect(x + i, y + i, 2, 2, c);
        rect(x + 5, y + 13, 9, 3, d);
        rect(x + 8, y + 15, 4, 7, d);
    }
}

static int key_poll(void) {
    int key = kb_poll_key();
    if (key == KB_KEY_UP) return GUI_KEY_UP;
    if (key == KB_KEY_DOWN) return GUI_KEY_DOWN;
    if (key == KB_KEY_LEFT) return GUI_KEY_LEFT;
    if (key == KB_KEY_RIGHT) return GUI_KEY_RIGHT;
    if (key == KB_KEY_HOME) return GUI_KEY_HOME;
    if (key == KB_KEY_END) return GUI_KEY_END;
    if (key == KB_KEY_PGUP) return GUI_KEY_PGUP;
    if (key == KB_KEY_PGDWN) return GUI_KEY_PGDOWN;
    if (key == KB_KEY_DELETE) return GUI_KEY_DELETE;
    if (key == '\b') return GUI_KEY_BACKSPACE;
    return key;
}

/* ── Script GFX hooks (after text/rect/key_poll are defined) ── */
static void sgfx_rect(int x,int y,int w,int h,uint32_t c){rect(x,y,w,h,c);}
static void sgfx_text(int x,int y,const char*s,uint32_t c,int sc){text(x,y,s,c,sc);}
static void sgfx_present(void){ if(g_script_fb) gui_present_surface(g_script_fb); }
static int  sgfx_sw(void){ return g_gui_surface_w; }
static int  sgfx_sh(void){ return g_gui_surface_h; }
static int  sgfx_getkey(void){ return key_poll(); }
static int  sgfx_waitkey(void){ int k; while(!(k=key_poll())) task_yield(); return k; }
static const cc_gfx_t g_sgfx = {
    .rect=sgfx_rect,.text=sgfx_text,.present=sgfx_present,
    .screen_w=sgfx_sw,.screen_h=sgfx_sh,
    .get_key=sgfx_getkey,.wait_key=sgfx_waitkey
};

// Cover-fit the wallpaper across (0,0)-(DW,DH), sampling only the pixels inside
// the current clip rect so cost scales with the damaged area, not the screen.
static void blit_wallpaper(int DW, int DH) {
    int sw = 0, sh = 0;
    const uint32_t *src = gui_wall_pixels(&sw, &sh);
    if (!src || !g_gui_surface || sw <= 0 || sh <= 0) {
        rect(0, 0, DW, DH, rgb(31, 34, 37));
        return;
    }
    int cx = 0, cy = 0, cw = DW, ch = DH;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx >= g_gui_surface_w || cy >= g_gui_surface_h) return;
    if (cx + cw > g_gui_surface_w) cw = g_gui_surface_w - cx;
    if (cy + ch > g_gui_surface_h) ch = g_gui_surface_h - cy;
    if (cw <= 0 || ch <= 0) return;

    // 16.16 fixed-point "cover" mapping (dest -> source), centered crop.
    int64_t inv_x = ((int64_t)sw << 16) / DW;
    int64_t inv_y = ((int64_t)sh << 16) / DH;
    int64_t inv = inv_x < inv_y ? inv_x : inv_y;
    int64_t cropx = (((int64_t)sw << 16) - (int64_t)DW * inv) / 2;
    int64_t cropy = (((int64_t)sh << 16) - (int64_t)DH * inv) / 2;

    for (int yy = 0; yy < ch; yy++) {
        int dy = cy + yy;
        int syi = (int)((cropy + (int64_t)dy * inv) >> 16);
        if (syi < 0) syi = 0;
        if (syi > sh - 1) syi = sh - 1;
        const uint32_t *srow = src + (int64_t)syi * sw;
        uint32_t *drow = g_gui_surface +
                         (uint32_t)dy * g_gui_surface_pitch + (uint32_t)cx;
        for (int xx = 0; xx < cw; xx++) {
            int sxi = (int)((cropx + (int64_t)(cx + xx) * inv) >> 16);
            if (sxi < 0) sxi = 0;
            if (sxi > sw - 1) sxi = sw - 1;
            drow[xx] = 0xFF000000 | (srow[sxi] & 0x00FFFFFF);
        }
    }
}

static void draw_wallpaper(int w, int h, int light) {
    int tb_y = h - TASKBAR_H;

    // Photographic wallpaper across the whole screen; panels float on top.
    blit_wallpaper(w, h);
    // Windows 11-style floating taskbar bar (translucent glass); its centered
    // icon cluster + clock are drawn by draw_taskbar().
    rect_alpha(0, tb_y, w, TASKBAR_H, light ? 0xF0E9EBEE : 0xEE20242A);
    rect(0, tb_y, w, 1, light ? rgb(206, 210, 214) : rgb(58, 63, 70));
}

static void draw_usage_bar(int x, int y, int w, int h, uint32_t used, uint32_t total, uint32_t color) {
    uint32_t filled = total ? (used * (uint32_t)w) / total : 0;
    if (filled > (uint32_t)w) filled = (uint32_t)w;
    draw_inset_shell(x, y, w, h, rgb(48, 62, 66));
    if (filled > 0) {
        hgradient(x + 1, y + 1, (int)filled - 2, h - 2,
                  rgb_lift(color, 20), rgb_lift(color, -20));
        rect(x + 1, y + 1, (int)filled - 2, 1, rgb_lift(color, 62));
    }
}

static const char *task_state_name(task_state_t state) {
    switch (state) {
        case TASK_READY: return "就绪";
        case TASK_RUNNING: return "运行";
        case TASK_BLOCKED: return "阻塞";
        case TASK_TERMINATED: return "结束";
        default: return "未知";
    }
}

static void draw_files_panel(int tx, int ty, int win_w, const gui_state_t *st) {
    char line[96];
    gui_state_t *mst = (gui_state_t *)st;
    int content_w = win_w - 60;
    int side_w = 118;
    int main_x = tx + side_w + 12;
    int detail_w = 184;
    int main_w = content_w - side_w - detail_w - 24;
    if (main_w < 260) {
        detail_w = 152;
        main_w = content_w - side_w - detail_w - 24;
    }
    if (main_w < 220) main_w = 220;
    int detail_x = main_x + main_w + 12;

    uint32_t count = gui_file_count(mst);
    text(tx, ty, "文件管理器", rgb(124, 220, 154), 1);
    line_u32(line, sizeof(line), "当前: ", count, " 项");
    text(tx + 112, ty + 2, line, rgb(168, 188, 202), 1);
    vgradient(tx, ty + 30, content_w, 30, rgb(34, 48, 64), rgb(20, 30, 42));
    rect(tx, ty + 30, content_w, 1, rgb(58, 86, 110));
    rect(tx, ty + 59, content_w, 1, rgb(8, 14, 22));
    border(tx, ty + 30, content_w, 30, rgb(50, 72, 92));
    line2(line, sizeof(line), "位置 ", gui_file_path(mst));
    text_clipped(tx + 12, ty + 42, tx + content_w - 12, line, rgb(238, 246, 252), 1);

    draw_file_action_bar(tx, ty, content_w);

    vgradient(tx, ty + 114, side_w, 206, rgb(22, 30, 40), rgb(14, 20, 28));
    border(tx, ty + 114, side_w, 206, rgb(50, 72, 92));
    rect(tx, ty + 114, side_w, 1, rgb(58, 86, 110));
    text(tx + 14, ty + 128, "位置", rgb(194, 226, 242), 1);
    vgradient(tx + 10, ty + 154, side_w - 20, 28, rgb(38, 76, 104), rgb(22, 50, 70));
    rect(tx + 10, ty + 154, 3, 28, rgb(85, 180, 120));
    text_clipped(tx + 22, ty + 164, tx + side_w - 10, gui_file_path(mst), rgb(244, 250, 255), 1);
    text(tx + 22, ty + 202, "/home", rgb(132, 150, 162), 1);
    text(tx + 22, ty + 232, "/dev  /proc", rgb(132, 150, 162), 1);
    text(tx + 14, ty + 286, "Enter进入  Back上级", rgb(122, 142, 156), 1);

    vgradient(main_x, ty + 114, main_w, 206, rgb(28, 40, 52), rgb(18, 26, 36));
    border(main_x, ty + 114, main_w, 206, rgb(58, 86, 110));
    vgradient(main_x, ty + 114, main_w, 26, rgb(40, 60, 78), rgb(24, 36, 48));
    rect(main_x, ty + 139, main_w, 1, rgb(70, 100, 116));
    text(main_x + 14, ty + 122, "名称", rgb(218, 232, 240), 1);
    text(main_x + main_w - 166, ty + 122, "类型", rgb(218, 232, 240), 1);
    text(main_x + main_w - 82, ty + 122, "大小", rgb(218, 232, 240), 1);

    int list_y = ty + 146;
    if (count == 0) {
        text(main_x + 18, list_y + 18, "目录为空", rgb(190, 208, 218), 1);
        text(main_x + 18, list_y + 42, "点击新建或按 N 创建", rgb(148, 168, 180), 1);
        vgradient(detail_x, ty + 114, detail_w, 206, rgb(22, 30, 40), rgb(14, 20, 28));
        border(detail_x, ty + 114, detail_w, 206, rgb(50, 72, 92));
        text(detail_x + 12, ty + 130, "详细信息", rgb(194, 226, 242), 1);
        text(detail_x + 12, ty + 164, "未选择文件", rgb(148, 162, 174), 1);
        return;
    }

    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count) selected = (int)count - 1;
    uint32_t start = selected >= FILE_LIST_ROWS ? (uint32_t)selected - (FILE_LIST_ROWS - 1) : 0;
    uint32_t max = count - start;
    if (max > FILE_LIST_ROWS) max = FILE_LIST_ROWS;
    for (uint32_t i = 0; i < max; i++) {
        uint32_t file_idx = start + i;
        char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
        uint32_t type = 0;
        vfs_node_t *node = 0;
        if (gui_file_entry(mst, file_idx, name, &type, &node, full, sizeof(full)) < 0) continue;
        int y = list_y + (int)i * FILE_ROW_H;
        if ((i & 1) == 1) rect(main_x + 1, y - 8, main_w - 2, FILE_ROW_H, rgb(42, 46, 50));
        if ((int)file_idx == selected) {
            rect(main_x + 6, y - 7, main_w - 12, 24, rgb(61, 174, 233));
        }
        uint32_t icon = type == VFS_NODE_DIR ? rgb(244, 194, 82) :
                        type == VFS_NODE_CHARDEV ? rgb(102, 214, 255) : rgb(124, 220, 154);
        rect(main_x + 16, y - 2, 18, 14, icon);
        rect(main_x + 20, y - 7, 18, 7, rgb_lift(icon, -16));
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, name);
        uint32_t row_fg = (int)file_idx == selected ? rgb(252, 254, 255) : rgb(218, 232, 240);
        uint32_t meta_fg = (int)file_idx == selected ? rgb(228, 244, 252) : rgb(150, 170, 182);
        text_clipped(main_x + 48, y, main_x + main_w - 130, line, row_fg, 1);
        text(main_x + main_w - 126, y, gui_node_type_label(type), meta_fg, 1);
        line_u32(line, sizeof(line), "", node ? node->size : 0, "B");
        text(main_x + main_w - 64, y, line, meta_fg, 1);
    }

    {
        char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
        uint32_t type = 0;
        vfs_node_t *node = 0;
        if (gui_file_entry(mst, (uint32_t)selected, name, &type, &node, full, sizeof(full)) < 0)
            return;
        vgradient(detail_x, ty + 114, detail_w, 206, rgb(22, 30, 40), rgb(14, 20, 28));
        border(detail_x, ty + 114, detail_w, 206, rgb(50, 72, 92));
        vgradient(detail_x, ty + 114, detail_w, 30, rgb(34, 50, 66), rgb(20, 30, 42));
        rect(detail_x, ty + 143, detail_w, 1, rgb(8, 14, 22));
        text(detail_x + 12, ty + 124, "详细信息", rgb(218, 232, 240), 1);
        uint32_t icon = type == VFS_NODE_DIR ? rgb(244, 194, 82) :
                        type == VFS_NODE_CHARDEV ? rgb(102, 214, 255) : rgb(124, 220, 154);
        rect(detail_x + 12, ty + 156, 30, 24, icon);
        rect(detail_x + 17, ty + 150, 28, 8, rgb_lift(icon, -16));
        text_clipped(detail_x + 52, ty + 160, detail_x + detail_w - 12, name, rgb(238, 246, 255), 1);
        line_u32(line, sizeof(line), "大小: ", node ? node->size : 0, "B");
        text(detail_x + 12, ty + 194, line, rgb(210, 221, 230), 1);
        line2(line, sizeof(line), "类型: ", gui_node_type_label(type));
        text(detail_x + 12, ty + 216, line, rgb(210, 221, 230), 1);
        text(detail_x + 12, ty + 246, "预览", rgb(194, 226, 242), 1);
        char preview[64];
        uint32_t n = 0;
        if (node && node->type == VFS_NODE_FILE) {
            int got = vfs_read(node, 0, preview, sizeof(preview) - 1);
            n = got > 0 ? (uint32_t)got : 0;
            preview[n] = 0;
        } else {
            line2(preview, sizeof(preview), node && node->type == VFS_NODE_DIR ? "目录 " : "设备 ", full);
            n = (uint32_t)strlen(preview);
        }
        for (uint32_t i = 0; i < n; i++) {
            if ((uint8_t)preview[i] < 32 && preview[i] != '\n') preview[i] = '.';
        }
        int px = detail_x + 12;
        int py = ty + 270;
        utf8_state_t utf8;
        utf8_init(&utf8);
        for (uint32_t i = 0; i < n && py < ty + 312; i++) {
            if (preview[i] == '\n') {
                px = detail_x + 12;
                py += 18;
                utf8_init(&utf8);
                continue;
            }
            uint32_t cp = 0;
            int ok = utf8_feed(&utf8, (uint8_t)preview[i], &cp);
            if (ok < 0) continue;
            if (ok == 0) cp = '?';
            px += draw_text_codepoint(px, py, cp, rgb(210, 221, 230), 1);
            if (px > detail_x + detail_w - 20) {
                px = detail_x + 12;
                py += 18;
            }
        }

        vgradient(tx, ty + 330, content_w, 28, rgb(28, 40, 54), rgb(16, 24, 34));
        rect(tx, ty + 330, content_w, 1, rgb(50, 72, 92));
        rect(tx, ty + 357, content_w, 1, rgb(8, 14, 22));
        border(tx, ty + 330, content_w, 28, rgb(46, 66, 86));
        if (st->rename_active) {
            int input_x = tx + 72;
            int hint_w = 132;
            int input_w = content_w - 84 - hint_w;
            if (input_w < 132) {
                hint_w = 0;
                input_w = content_w - 84;
            }
            if (input_w < 80) input_w = 80;
            text(tx + 12, ty + 340, "新名称", rgb(216, 232, 244), 1);
            draw_inset_shell(input_x, ty + 335, input_w, 18, rgb(8, 14, 22));
            text_clipped(input_x + 6, ty + 340, input_x + input_w - 8,
                         st->rename_buf, rgb(236, 246, 252), 1);
            int cursor_x = input_x + 6 + (int)st->rename_len * 6;
            if (cursor_x > input_x + input_w - 8) cursor_x = input_x + input_w - 8;
            rect(cursor_x, ty + 339, 2, 12, rgb(124, 220, 154));
            if (hint_w > 0)
                text(input_x + input_w + 10, ty + 340, "Enter确认 Esc取消",
                     rgb(148, 168, 180), 1);
        } else {
            uint32_t pos = 0;
            line[0] = 0;
            if (st->delete_confirm_index == selected) {
                append_str(line, sizeof(line), &pos, "再次删除确认: ");
                append_str(line, sizeof(line), &pos, name);
                append_str(line, sizeof(line), &pos, "  D/删除确认  Esc取消");
            } else {
                append_str(line, sizeof(line), &pos, "已选择: ");
                append_str(line, sizeof(line), &pos, name);
                append_str(line, sizeof(line), &pos, "  ");
                append_uint(line, sizeof(line), &pos, node ? node->size : 0);
                append_str(line, sizeof(line), &pos, "B  Enter打开/进入  P复制 R重命名");
            }
            text_clipped(tx + 12, ty + 340, tx + content_w - 12, line, rgb(216, 232, 244), 1);
        }
    }
    if (count > FILE_LIST_ROWS) {
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, "显示 ");
        append_uint(line, sizeof(line), &pos, start + 1);
        append_str(line, sizeof(line), &pos, "-");
        append_uint(line, sizeof(line), &pos, start + max);
        append_str(line, sizeof(line), &pos, " / ");
        append_uint(line, sizeof(line), &pos, count);
        text(main_x + main_w - 96, ty + 340, line, rgb(148, 162, 174), 1);
    }
}

static void draw_disk_panel(int tx, int ty, int win_w) {
    char line[96];
    text(tx, ty, "磁盘管理器", rgb(244, 194, 82), 1);
    line2(line, sizeof(line), "块设备: ", block_backend_name());
    text(tx, ty + 38, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "扇区数: ", block_sector_count(), "");
    text(tx, ty + 60, line, rgb(210, 221, 230), 1);
    line2(line, sizeof(line), "文件系统: ", fs_backend_name());
    text(tx, ty + 82, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "起始 LBA: ", fs_disk_start_lba(), "");
    text(tx, ty + 104, line, rgb(210, 221, 230), 1);

    uint32_t used = fs_used_bytes();
    uint32_t cap = fs_capacity_bytes();
    uint32_t pct = cap ? (used * 100) / cap : 0;
    uint32_t pos = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &pos, "已用: ");
    append_uint(line, sizeof(line), &pos, used / 1024);
    append_str(line, sizeof(line), &pos, "K / ");
    append_uint(line, sizeof(line), &pos, cap / 1024);
    append_str(line, sizeof(line), &pos, "K ");
    append_uint(line, sizeof(line), &pos, pct);
    append_str(line, sizeof(line), &pos, "%");
    text(tx, ty + 132, line, rgb(210, 221, 230), 1);
    draw_usage_bar(tx, ty + 154, win_w - 60, 18, used, cap, rgb(244, 194, 82));
    draw_button(tx, ty + 194, "安装 I", rgb(244, 194, 82));
    text(tx + 120, ty + 204, fs_is_disk() ? "HBFS 持久化已就绪" : "当前使用 RAMFS", rgb(132, 196, 232), 1);
}

static void draw_resource_panel(int tx, int ty, int win_w, int w, int h) {
    char line[96];
    uint64_t total = pmm_get_total_mem();
    uint64_t free = pmm_get_free_mem();
    uint64_t used = total > free ? total - free : 0;
    text(tx, ty, "资源管理器", rgb(215, 152, 244), 1);
    line_u32(line, sizeof(line), "内存已用: ", (uint32_t)(used / 1024), "K");
    text(tx, ty + 38, line, rgb(210, 221, 230), 1);
    draw_usage_bar(tx, ty + 60, win_w - 60, 16, (uint32_t)(used / 1024), (uint32_t)(total / 1024), rgb(215, 152, 244));
    line_u32(line, sizeof(line), "内存可用: ", (uint32_t)(free / 1024), "K");
    text(tx, ty + 86, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "任务数: ", (uint32_t)task_get_count(), "");
    text(tx, ty + 110, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "应用数: ", hbos_app_count(), "");
    text(tx, ty + 132, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "文件数: ", fs_get_count(), "");
    text(tx, ty + 154, line, rgb(210, 221, 230), 1);
    line2(line, sizeof(line), "文件系统: ", fs_backend_name());
    text(tx, ty + 176, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "显示: ", (uint32_t)w, "X");
    uint32_t pos = 0;
    while (line[pos]) pos++;
    append_uint(line, sizeof(line), &pos, (uint32_t)h);
    text(tx, ty + 198, line, rgb(210, 221, 230), 1);
    line2(line, sizeof(line), "鼠标: ", mouse_backend_name());
    text(tx, ty + 220, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "USB HID: ", (uint32_t)hid_device_count(), "");
    text(tx, ty + 242, line, rgb(210, 221, 230), 1);

    text(tx, ty + 270, "活动任务", rgb(215, 152, 244), 1);
    uint32_t max = (uint32_t)task_get_count();
    if (max > 3) max = 3;
    for (uint32_t i = 0; i < max; i++) {
        const task_t *task = task_get_active(i);
        if (!task) continue;
        pos = 0;
        line[0] = 0;
        append_uint(line, sizeof(line), &pos, task->id);
        append_str(line, sizeof(line), &pos, " ");
        append_str(line, sizeof(line), &pos, task->name);
        append_str(line, sizeof(line), &pos, " ");
        append_str(line, sizeof(line), &pos, task_state_name(task->state));
        text(tx + 18, ty + 292 + (int)i * 20, line, rgb(220, 230, 238), 1);
    }
}

static uint32_t app_accent(int mode) {
    switch (mode) {
        case GUI_APP_NOTES:   return rgb(46, 204, 113);
        case GUI_APP_CALC:    return rgb(52, 152, 219);
        case GUI_APP_UWC:     return rgb(241, 196, 15);
        case GUI_APP_SNAKE:   return rgb(39, 174, 96);
        case GUI_APP_BROWSER: return rgb(61, 174, 233);
        case GUI_APP_CODE:    return rgb(155, 89, 182);
        case GUI_APP_DIAG:    return rgb(230, 126, 34);
        case GUI_APP_CLOCK:   return rgb(231, 76, 60);
        default:              return rgb(61, 174, 233);
    }
}

static int gui_app_grid_cols(int win_w) {
    int cols = (win_w - 8) / 150;
    if (cols < 2) cols = 2;
    if (cols > 4) cols = 4;
    return cols;
}

// Shared launcher-tile geometry so draw_apps_panel and hit_action stay in sync.
static void gui_app_tile_rect(int tx, int ty, int win_w, uint32_t i,
                              int *x, int *y, int *w, int *h) {
    int gap = 14;
    int cols = gui_app_grid_cols(win_w);
    int tw = (win_w - 8 - gap * (cols - 1)) / cols;
    int th = 112;
    int col = (int)(i % (uint32_t)cols);
    int row = (int)(i / (uint32_t)cols);
    *x = tx + col * (tw + gap);
    *y = ty + 64 + row * (th + gap);
    *w = tw;
    *h = th;
}


static void draw_apps_panel(int tx, int ty, int win_w, const gui_state_t *st) {
    char line[64];
    text(tx, ty, "应用程序", rgb(239, 240, 241), 1);
    line_u32(line, sizeof(line), "共 ", gui_app_count(), " 个应用");
    text(tx + 96, ty + 2, line, rgb(160, 167, 173), 1);

    int cw = win_w - 60;  // content width (panel is inset 30px each side)

    int selected = st->selected_app;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= gui_app_count()) selected = (int)gui_app_count() - 1;

    for (uint32_t i = 0; i < gui_app_count(); i++) {
        const gui_app_meta_t *app = &gui_apps[i];
        int x, y, w, h;
        gui_app_tile_rect(tx, ty, cw, i, &x, &y, &w, &h);
        uint32_t accent = app_accent(app->mode);
        int sel = ((int)i == selected);

        // Translucent glass tile; selected gets an accent wash + ring.
        fill_round_rect(x, y, w, h, 10, sel ? 0x99203A4A : 0x66202428, RR_ALL);
        if (sel) {
            rect(x + 10, y, w - 20, 1, accent);
            rect(x + 10, y + h - 1, w - 20, 1, accent);
            rect(x, y + 10, 1, h - 20, accent);
            rect(x + w - 1, y + 10, 1, h - 20, accent);
        }

        int isz = 52, ix = x + (w - isz) / 2, iy = y + 16;
        blit_icon(ix, iy, isz, app_icon_id(app->mode));

        int tw = text_width(app->name, 1);
        int lx = x + (w - tw) / 2;
        if (lx < x + 4) lx = x + 4;
        text(lx, y + h - 26, app->name,
             sel ? rgb(255, 255, 255) : rgb(228, 232, 236), 1);
    }
    uint32_t cols = (uint32_t)gui_app_grid_cols(cw);
    uint32_t rows = (gui_app_count() + cols - 1) / cols;
    text(tx, ty + 64 + (int)rows * 126 + 6,
         "单击图标启动 · 方向键选择 · Enter 打开",
         rgb(200, 206, 212), 1);
}

static uint32_t count_file_lines(file_t *f) {
    if (!f) return 0;
    uint32_t lines = 0;
    char buf[128];
    uint32_t off = 0;
    while (off < f->size) {
        uint32_t n = fs_read_file_data(f, off, buf, sizeof(buf));
        if (!n) break;
        for (uint32_t i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
        }
        off += n;
    }
    return lines;
}

static void note_load(gui_state_t *st) {
    if (st->note_loaded) return;
    st->note_len = 0;
    st->note_buf[0] = 0;
    file_t *f = fs_find_file(gui_note_name(st));
    if (f) {
        uint32_t n = f->size;
        if (n >= NOTE_EDIT_CAP) n = NOTE_EDIT_CAP - 1;
        st->note_len = fs_read_file_data(f, 0, st->note_buf, n);
        st->note_buf[st->note_len] = 0;
    }
    st->note_cursor = st->note_len;
    st->note_dirty = 0;
    st->note_loaded = 1;
}

static void note_save(gui_state_t *st) {
    const char *name = gui_note_name(st);
    file_t *f = fs_find_file(name);
    if (!f) f = fs_create_file(name);
    if (!f) {
        st->status = "笔记创建失败";
        return;
    }
    if (fs_truncate_file(f) < 0 ||
        fs_write_file_data(f, 0, st->note_buf, st->note_len) < 0) {
        st->status = "笔记保存失败";
        return;
    }
    (void)fs_sync();
    st->note_dirty = 0;
    st->status = "笔记已保存";
}

// 在光标处插入一个字节，仅修改内存缓冲，标记 dirty（Ctrl+S 才落盘）
static void note_insert(gui_state_t *st, char c) {
    if (st->note_len + 1 >= NOTE_EDIT_CAP) {
        st->status = "笔记已满";
        return;
    }
    if (st->note_cursor > st->note_len) st->note_cursor = st->note_len;
    for (uint32_t i = st->note_len; i > st->note_cursor; i--)
        st->note_buf[i] = st->note_buf[i - 1];
    st->note_buf[st->note_cursor] = c;
    st->note_len++;
    st->note_cursor++;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

// 删除光标前的一个 UTF-8 字符
static void note_backspace(gui_state_t *st) {
    if (st->note_cursor == 0) return;
    uint32_t start = st->note_cursor - 1;
    while (start > 0 && ((uint8_t)st->note_buf[start] & 0xC0) == 0x80) start--;
    uint32_t removed = st->note_cursor - start;
    for (uint32_t i = start; i + removed <= st->note_len; i++)
        st->note_buf[i] = st->note_buf[i + removed];
    st->note_len -= removed;
    st->note_cursor = start;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

// 删除光标后的一个 UTF-8 字符
static void note_delete_forward(gui_state_t *st) {
    if (st->note_cursor >= st->note_len) return;
    uint32_t end = st->note_cursor + 1;
    while (end < st->note_len && ((uint8_t)st->note_buf[end] & 0xC0) == 0x80) end++;
    uint32_t removed = end - st->note_cursor;
    for (uint32_t i = st->note_cursor; i + removed <= st->note_len; i++)
        st->note_buf[i] = st->note_buf[i + removed];
    st->note_len -= removed;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

// 光标按 UTF-8 边界左移
static void note_cursor_left(gui_state_t *st) {
    if (st->note_cursor == 0) return;
    st->note_cursor--;
    while (st->note_cursor > 0 &&
           ((uint8_t)st->note_buf[st->note_cursor] & 0xC0) == 0x80)
        st->note_cursor--;
}

// 光标按 UTF-8 边界右移
static void note_cursor_right(gui_state_t *st) {
    if (st->note_cursor >= st->note_len) return;
    st->note_cursor++;
    while (st->note_cursor < st->note_len &&
           ((uint8_t)st->note_buf[st->note_cursor] & 0xC0) == 0x80)
        st->note_cursor++;
}

// 返回光标所在行的起始偏移
static uint32_t note_line_start(gui_state_t *st, uint32_t off) {
    while (off > 0 && st->note_buf[off - 1] != '\n') off--;
    return off;
}

static void note_cursor_home(gui_state_t *st) {
    st->note_cursor = note_line_start(st, st->note_cursor);
}

static void note_cursor_end(gui_state_t *st) {
    while (st->note_cursor < st->note_len && st->note_buf[st->note_cursor] != '\n')
        st->note_cursor++;
}

// 上/下移动光标，尽量保持当前列
static void note_cursor_vertical(gui_state_t *st, int dir) {
    uint32_t ls = note_line_start(st, st->note_cursor);
    uint32_t col = st->note_cursor - ls;
    if (dir < 0) {
        if (ls == 0) { st->note_cursor = 0; return; }
        uint32_t prev = note_line_start(st, ls - 1);
        uint32_t prev_len = (ls - 1) - prev;
        st->note_cursor = prev + (col < prev_len ? col : prev_len);
    } else {
        uint32_t nl = st->note_cursor;
        while (nl < st->note_len && st->note_buf[nl] != '\n') nl++;
        if (nl >= st->note_len) { st->note_cursor = st->note_len; return; }
        uint32_t next = nl + 1;
        uint32_t next_end = next;
        while (next_end < st->note_len && st->note_buf[next_end] != '\n') next_end++;
        uint32_t next_len = next_end - next;
        st->note_cursor = next + (col < next_len ? col : next_len);
    }
}

static void draw_notes_app(int tx, int ty, int win_w, gui_state_t *st) {
    note_load(st);
    int list_w = 150;
    int edit_x = tx + list_w + 18;
    int edit_w = win_w - list_w - 86;
    if (edit_w < 260) edit_w = 260;
    text(tx, ty, "记事本", rgb(124, 220, 154), 1);
    text(tx, ty + 40, "选择左侧文件后编辑", rgb(148, 162, 174), 1);
    char line[96];

    vgradient(tx, ty + 70, list_w, 222, rgb(22, 30, 40), rgb(14, 20, 28));
    border(tx, ty + 70, list_w, 222, rgb(46, 66, 84));
    rect(tx, ty + 70, list_w, 1, rgb(58, 86, 110));
    text(tx + 12, ty + 82, "文件", rgb(194, 226, 242), 1);
    rect(tx + 12, ty + 100, list_w - 24, 1, rgb(50, 72, 92));
    uint32_t count = gui_file_count(st);
    if (count == 0) {
        text(tx + 12, ty + 112, "暂无文件", rgb(148, 162, 174), 1);
        text(tx + 12, ty + 134, "按 N 新建", rgb(148, 162, 174), 1);
    } else {
        int selected = st->selected_file;
        if (selected < 0) selected = 0;
        if ((uint32_t)selected >= count) selected = (int)count - 1;
        uint32_t start = selected >= NOTE_FILE_ROWS ? (uint32_t)selected - (NOTE_FILE_ROWS - 1) : 0;
        uint32_t max = count - start;
        if (max > NOTE_FILE_ROWS) max = NOTE_FILE_ROWS;
        for (uint32_t i = 0; i < max; i++) {
            uint32_t file_idx = start + i;
            char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
            uint32_t type = 0;
            vfs_node_t *node = 0;
            if (gui_file_entry(st, file_idx, name, &type, &node, full, sizeof(full)) < 0)
                continue;
            int y = ty + 112 + (int)i * FILE_ROW_H;
            if ((int)file_idx == selected) {
                vgradient(tx + 8, y - 6, list_w - 16, 24, rgb(28, 80, 116), rgb(16, 50, 78));
                rect(tx + 8, y - 6, 3, 24, rgb(124, 220, 154));
            }
            if (node && node->type == VFS_NODE_DIR) rect(tx + 14, y + 3, 5, 5, rgb(244, 194, 82));
            text_clipped(tx + 24, y, tx + list_w - 12, name,
                         (int)file_idx == selected ? rgb(252, 254, 255) : rgb(210, 222, 234), 1);
        }
    }

    line2(line, sizeof(line), "文件: ", gui_note_name(st));
    text_clipped(edit_x, ty + 70, edit_x + edit_w, line, rgb(210, 221, 230), 1);
    {
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, "大小: ");
        append_uint(line, sizeof(line), &pos, st->note_len);
        append_str(line, sizeof(line), &pos, "B");
        if (st->note_dirty) append_str(line, sizeof(line), &pos, "  ●未保存");
        else append_str(line, sizeof(line), &pos, "  已保存");
    }
    text(edit_x, ty + 92, line, st->note_dirty ? rgb(244, 194, 82) : rgb(150, 200, 160), 1);
    text_clipped(edit_x, ty + 50, edit_x + edit_w,
                 "方向键移动  Ctrl+S 保存  支持中间插入/删除",
                 rgb(120, 150, 168), 1);
    vgradient(edit_x, ty + 118, edit_w, 174, rgb(8, 14, 22), rgb(2, 6, 12));
    rect(edit_x, ty + 118, edit_w, 1, rgb(28, 56, 36));
    rect(edit_x, ty + 291, edit_w, 1, rgb(8, 14, 22));
    border(edit_x, ty + 118, edit_w, 174, rgb(85, 180, 120));
    int x = edit_x + 8;
    int y = ty + 126;
    int cursor_x = x;
    int cursor_y = y;
    int cursor_drawn = 0;
    utf8_state_t utf8;
    utf8_init(&utf8);
    for (uint32_t i = 0; i < st->note_len && y < ty + 280; i++) {
        if (i == st->note_cursor) {
            cursor_x = x;
            cursor_y = y;
            cursor_drawn = 1;
        }
        if (st->note_buf[i] == '\n') {
            x = edit_x + 8;
            y += 18;
            utf8_init(&utf8);
            continue;
        }

        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)st->note_buf[i], &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        int advance = draw_text_codepoint(x, y, cp, rgb(228, 238, 246), 1);
        x += advance;
        if (x > edit_x + edit_w - 16) {
            x = edit_x + 8;
            y += 18;
        }
    }
    if (!cursor_drawn) {
        cursor_x = x;
        cursor_y = y;
    }
    // 在光标实际位置绘制竖线光标
    if (cursor_y < ty + 280) {
        rect(cursor_x, cursor_y, 2, 14, rgb(124, 220, 154));
    }
}

static void draw_uwc_app(int tx, int ty, gui_state_t *st) {
    char line[96];
    file_t *f = selected_file(st);
    text(tx, ty, "文件统计", rgb(244, 194, 82), 1);
    if (!f) {
        text(tx, ty + 42, "未选择文件", rgb(148, 162, 174), 1);
        text(tx, ty + 64, "先在文件管理器中选择或创建文件", rgb(148, 162, 174), 1);
        return;
    }
    line2(line, sizeof(line), "文件: ", f->name);
    text(tx, ty + 42, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "字节: ", f->size, "");
    text(tx, ty + 64, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "行数: ", count_file_lines(f), "");
    text(tx, ty + 86, line, rgb(210, 221, 230), 1);
}

static int gui_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) return 0;
    uint32_t plen = (uint32_t)strlen(path);
    uint32_t slen = (uint32_t)strlen(suffix);
    if (slen == 0 || plen < slen) return 0;
    return strcmp(path + plen - slen, suffix) == 0;
}

static int gui_has_code_suffix(const char *path) {
    return gui_has_suffix(path, ".c") || gui_has_suffix(path, ".h") ||
           gui_has_suffix(path, ".cc") || gui_has_suffix(path, ".cpp") ||
           gui_has_suffix(path, ".hpp") || gui_has_suffix(path, ".asm") ||
           gui_has_suffix(path, ".S");
}

static void code_set_output(const char *msg) {
    uint32_t i = 0;
    while (msg && msg[i] && i + 1 < CODE_OUTPUT_CAP) {
        g_code_output[i] = msg[i];
        i++;
    }
    g_code_output[i] = 0;
}

static void code_append_sanitized(char *buf, uint32_t cap, uint32_t *pos, const char *s) {
    int space = 0;
    while (s && *s && *pos + 1 < cap) {
        char c = *s++;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            if (space) continue;
            space = 1;
        } else {
            space = 0;
        }
        append_char(buf, cap, pos, c);
    }
}

static void code_set_path(gui_state_t *st, const char *path) {
    uint32_t i = 0;
    while (path && path[i] && i + 1 < sizeof(st->code_path)) {
        st->code_path[i] = path[i];
        i++;
    }
    if (i == 0) {
        strcpy(st->code_path, "/home/main.c");
    } else {
        st->code_path[i] = 0;
    }
    st->code_loaded = 0;
    st->code_modified = 0;
    st->code_scroll = 0;
    st->code_cursor = 0;
    st->code_error_line = 0;
    st->code_view_rows = 0;
}

static const char *code_path(gui_state_t *st) {
    if (!st->code_path[0]) code_set_path(st, "/home/main.c");
    return st->code_path;
}

static void code_insert_template(void) {
    const char *tpl =
        "#include <stdio.h>\n"
        "\n"
        "int main() {\n"
        "    puts(\"HBOS Code Workspace\");\n"
        "    printf(\"answer=%d\\n\", 40 + 2);\n"
        "    return 0;\n"
        "}\n";
    uint32_t len = (uint32_t)strlen(tpl);
    if (len >= CODE_EDIT_CAP) len = CODE_EDIT_CAP - 1;
    memcpy(g_code_buf, tpl, len);
    g_code_buf[len] = 0;
}

static int code_save(gui_state_t *st) {
    const char *path = code_path(st);
    vfs_node_t *node = vfs_lookup(path);
    if (!node) node = vfs_create(path);
    if (!node || node->type != VFS_NODE_FILE) {
        code_set_output("Save failed: cannot create file");
        st->status = "代码保存失败";
        return -1;
    }
    if (vfs_truncate(node) < 0 ||
        vfs_write(node, 0, g_code_buf, st->code_len) < 0) {
        code_set_output("Save failed: VFS write error");
        st->status = "代码保存失败";
        return -1;
    }
    (void)fs_sync();
    st->code_modified = 0;
    st->code_error_line = 0;
    code_set_output("Saved");
    st->status = "代码已保存";
    return 0;
}

static void code_load(gui_state_t *st) {
    if (st->code_loaded) return;
    const char *path = code_path(st);
    g_code_buf[0] = 0;
    st->code_len = 0;
    st->code_cursor = 0;
    st->code_scroll = 0;
    st->code_modified = 0;
    st->code_error_line = 0;
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        code_insert_template();
        st->code_len = (uint32_t)strlen(g_code_buf);
        st->code_cursor = st->code_len;
        st->code_modified = 1;
        code_set_output("New C file template");
        (void)code_save(st);
        st->code_loaded = 1;
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        code_set_output("Open failed: selected path is not a file");
        st->code_loaded = 1;
        return;
    }
    uint32_t n = node->size;
    if (n >= CODE_EDIT_CAP) n = CODE_EDIT_CAP - 1;
    int got = vfs_read(node, 0, g_code_buf, n);
    if (got < 0) {
        g_code_buf[0] = 0;
        code_set_output("Open failed: VFS read error");
        st->code_loaded = 1;
        return;
    }
    st->code_len = (uint32_t)got;
    g_code_buf[st->code_len] = 0;
    st->code_cursor = 0;
    code_set_output(node->size >= CODE_EDIT_CAP ? "Opened with truncation" : "Opened");
    st->code_loaded = 1;
}

static void code_open_selected(gui_state_t *st) {
    char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
    uint32_t type = 0;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, full, sizeof(full)) < 0 || !node) {
        code_set_output("Open failed: no selected file");
        st->status = "未选择代码文件";
        return;
    }
    if (node->type == VFS_NODE_DIR) {
        gui_set_file_path(st, full);
        st->status = "已进入目录";
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        code_set_output("Open failed: device node");
        st->status = "设备节点不能打开";
        return;
    }
    code_set_path(st, full);
    code_load(st);
    st->status = "代码文件已打开";
}

static void code_line_col(gui_state_t *st, uint32_t off, uint32_t *line, uint32_t *col) {
    if (off > st->code_len) off = st->code_len;
    uint32_t l = 0, c = 0;
    for (uint32_t i = 0; i < off; i++) {
        if (g_code_buf[i] == '\n') {
            l++;
            c = 0;
        } else {
            c++;
        }
    }
    if (line) *line = l;
    if (col) *col = c;
}

static uint32_t code_line_count(gui_state_t *st) {
    uint32_t lines = 1;
    for (uint32_t i = 0; i < st->code_len; i++) {
        if (g_code_buf[i] == '\n') lines++;
    }
    return lines;
}

static int code_visible_rows(gui_state_t *st) {
    return st->code_view_rows > 0 ? st->code_view_rows : CODE_VIEW_ROWS;
}

static void code_clamp_scroll(gui_state_t *st) {
    int max_scroll = (int)code_line_count(st) - code_visible_rows(st);
    if (max_scroll < 0) max_scroll = 0;
    if (st->code_scroll < 0) st->code_scroll = 0;
    if (st->code_scroll > max_scroll) st->code_scroll = max_scroll;
}

static uint32_t code_find_line_start(gui_state_t *st, uint32_t target_line) {
    uint32_t line = 0;
    for (uint32_t i = 0; i < st->code_len; i++) {
        if (line == target_line) return i;
        if (g_code_buf[i] == '\n') line++;
    }
    return line == target_line ? st->code_len : st->code_len;
}

static uint32_t code_line_len_at(gui_state_t *st, uint32_t start) {
    uint32_t len = 0;
    while (start + len < st->code_len && g_code_buf[start + len] != '\n') len++;
    return len;
}

static uint32_t code_offset_for_line_col(gui_state_t *st, uint32_t line, uint32_t col) {
    uint32_t start = code_find_line_start(st, line);
    uint32_t len = code_line_len_at(st, start);
    if (col > len) col = len;
    return start + col;
}

static void code_jump_to_line(gui_state_t *st, int one_based_line) {
    if (one_based_line <= 0) return;
    uint32_t line = (uint32_t)(one_based_line - 1);
    st->code_cursor = code_offset_for_line_col(st, line, 0);
    if ((int)line < st->code_scroll) st->code_scroll = (int)line;
    if ((int)line >= st->code_scroll + code_visible_rows(st))
        st->code_scroll = (int)line - code_visible_rows(st) + 1;
    code_clamp_scroll(st);
}

static void code_ensure_visible(gui_state_t *st) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    (void)col;
    if ((int)line < st->code_scroll) st->code_scroll = (int)line;
    if ((int)line >= st->code_scroll + code_visible_rows(st))
        st->code_scroll = (int)line - code_visible_rows(st) + 1;
    code_clamp_scroll(st);
}

static void code_move_vertical(gui_state_t *st, int dir) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    if (dir < 0 && line == 0) return;
    uint32_t lines = code_line_count(st);
    if (dir > 0 && line + 1 >= lines) return;
    if (dir > 0) line++;
    else line--;
    st->code_cursor = code_offset_for_line_col(st, line, col);
    code_ensure_visible(st);
}

static void code_move_line_edge(gui_state_t *st, int end) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    (void)col;
    uint32_t start = code_find_line_start(st, line);
    st->code_cursor = start + (end ? code_line_len_at(st, start) : 0);
    code_ensure_visible(st);
}

static void code_move_page(gui_state_t *st, int dir) {
    uint32_t line = 0, col = 0;
    code_line_col(st, st->code_cursor, &line, &col);
    int target = (int)line + dir * code_visible_rows(st);
    int max_line = (int)code_line_count(st) - 1;
    if (target < 0) target = 0;
    if (target > max_line) target = max_line;
    st->code_cursor = code_offset_for_line_col(st, (uint32_t)target, col);
    code_ensure_visible(st);
}

static void code_insert_char(gui_state_t *st, char c) {
    if (st->code_len + 1 >= CODE_EDIT_CAP) {
        code_set_output("Buffer full");
        st->status = "代码缓冲已满";
        return;
    }
    if (st->code_cursor > st->code_len) st->code_cursor = st->code_len;
    memmove(g_code_buf + st->code_cursor + 1,
            g_code_buf + st->code_cursor,
            st->code_len - st->code_cursor + 1);
    g_code_buf[st->code_cursor++] = c;
    st->code_len++;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

static void code_insert_newline(gui_state_t *st) {
    uint32_t start = st->code_cursor;
    char indent_buf[32];
    while (start > 0 && g_code_buf[start - 1] != '\n') start--;
    uint32_t indent = 0;
    while (start + indent < st->code_len &&
           (g_code_buf[start + indent] == ' ' || g_code_buf[start + indent] == '\t') &&
           indent < 32) {
        indent_buf[indent] = g_code_buf[start + indent];
        indent++;
    }
    int block_indent = st->code_cursor > 0 && g_code_buf[st->code_cursor - 1] == '{';
    code_insert_char(st, '\n');
    for (uint32_t i = 0; i < indent; i++) code_insert_char(st, indent_buf[i]);
    if (block_indent) {
        for (int i = 0; i < 4; i++) code_insert_char(st, ' ');
    }
}

static void code_backspace(gui_state_t *st) {
    if (st->code_cursor == 0 || st->code_len == 0) return;
    memmove(g_code_buf + st->code_cursor - 1,
            g_code_buf + st->code_cursor,
            st->code_len - st->code_cursor + 1);
    st->code_cursor--;
    st->code_len--;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

static void code_delete_forward(gui_state_t *st) {
    if (st->code_cursor >= st->code_len || st->code_len == 0) return;
    memmove(g_code_buf + st->code_cursor,
            g_code_buf + st->code_cursor + 1,
            st->code_len - st->code_cursor);
    st->code_len--;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

static void code_run_current(gui_state_t *st) {
    code_load(st);
    if (code_save(st) < 0) return;
    char run_out[CODE_OUTPUT_CAP];
    int rc = hbos_gcc_run_file_capture(code_path(st), run_out, sizeof(run_out));
    if (rc == 0) {
        char line[CODE_OUTPUT_CAP];
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, "Run OK");
        if (run_out[0]) {
            append_str(line, sizeof(line), &pos, ": ");
            code_append_sanitized(line, sizeof(line), &pos, run_out);
        } else {
            append_str(line, sizeof(line), &pos, " return ");
            append_int(line, sizeof(line), &pos, hbos_gcc_last_return());
        }
        st->code_error_line = 0;
        code_set_output(line);
        st->status = "代码运行成功";
    } else {
        int err_line = hbos_gcc_last_error_line();
        const char *err = hbos_gcc_last_error();
        char line[CODE_OUTPUT_CAP];
        uint32_t pos = 0;
        line[0] = 0;
        if (err_line > 0) {
            append_str(line, sizeof(line), &pos, "Line ");
            append_uint(line, sizeof(line), &pos, (uint32_t)err_line);
            append_str(line, sizeof(line), &pos, ": ");
            st->code_error_line = err_line;
            code_jump_to_line(st, err_line);
        } else {
            append_str(line, sizeof(line), &pos, "GCC failed: ");
            st->code_error_line = 0;
        }
        append_str(line, sizeof(line), &pos, err && err[0] ? err : "unknown error");
        code_set_output(line);
        st->status = "代码运行失败";
    }
}

static int code_command_rect(int content_w, int cmd, int *x, int *y, int *bw) {
    int idx = cmd - 1;
    if (idx < 0 || idx > 3 || content_w <= 0) return 0;
    int gap = 6;
    int width = 68;
    int total = width * 4 + gap * 3;
    int left = content_w - total;
    if (left < 0) left = 0;
    if (x) *x = left + idx * (width + gap);
    if (y) *y = 22;
    if (bw) *bw = width;
    return 1;
}

static void code_make_layout(int tx, int ty, int win_w, int win_h, code_layout_t *l) {
    l->content_w = win_w - 60;
    if (l->content_w < 320) l->content_w = 320;
    int body_h = win_h - 82;
    if (body_h < 290) body_h = 290;
    l->row_h = 18;
    l->line_no_w = 42;
    l->side_w = l->content_w / 7;
    if (l->side_w < 154) l->side_w = 154;
    if (l->side_w > 220) l->side_w = 220;
    l->editor_x = tx + l->side_w + 14;
    l->editor_y = ty + 82;
    l->editor_w = l->content_w - l->side_w - 14;
    if (l->editor_w < 300) {
        l->side_w -= 300 - l->editor_w;
        if (l->side_w < 118) l->side_w = 118;
        l->editor_x = tx + l->side_w + 14;
        l->editor_w = l->content_w - l->side_w - 14;
    }

    int output_min_h = 62;
    int editor_bottom = ty + body_h - output_min_h - 12;
    l->editor_h = editor_bottom - l->editor_y;
    if (l->editor_h < 120) l->editor_h = 120;
    l->view_rows = (l->editor_h - 12) / l->row_h;
    if (l->view_rows < 5) l->view_rows = 5;
    l->editor_h = l->view_rows * l->row_h + 12;
    l->bottom_y = l->editor_y + l->editor_h + 12;
    l->bottom_h = ty + body_h - l->bottom_y;
    if (l->bottom_h < output_min_h) l->bottom_h = output_min_h;
    l->file_rows = (l->editor_h - 62) / FILE_ROW_H;
    if (l->file_rows < 3) l->file_rows = 3;
}

static int code_path_is_py(gui_state_t *st) {
    const char *p = code_path(st);
    int n = (int)strlen(p);
    return n >= 3 && p[n-3]=='.' && p[n-2]=='p' && p[n-1]=='y';
}

static void code_gui_run(gui_state_t *st, const fb_info_t *fb) {
    code_load(st);
    if (code_save(st) < 0) return;
    g_script_fb = fb;
    /* clear screen */
    rect(0, 0, g_gui_surface_w, g_gui_surface_h, rgb(10, 13, 18));
    gui_present_surface(fb);
    if (code_path_is_py(st)) {
        py_set_gfx(&g_sgfx);
        int rc = py_run_file(code_path(st));
        py_set_gfx(0);
        if (rc != 0) {
            char line[128]; uint32_t pos = 0; line[0] = 0;
            int el = py_last_error_line();
            if (el > 0) { append_str(line,sizeof(line),&pos,"Line "); append_uint(line,sizeof(line),&pos,(uint32_t)el); append_str(line,sizeof(line),&pos,": "); }
            else append_str(line,sizeof(line),&pos,"Python error: ");
            const char *em = py_last_error();
            append_str(line,sizeof(line),&pos,em&&em[0]?em:"error");
            code_set_output(line);
            st->status = "Python 脚本错误";
        } else {
            code_set_output("Python GUI OK");
            st->status = "Python 脚本运行完成";
        }
    } else {
        cc_set_gfx(&g_sgfx);
        int rc = hbos_gcc_run_file(code_path(st), 0);
        cc_set_gfx(0);
        if (rc != 0) {
            char line[128]; uint32_t pos = 0; line[0] = 0;
            int el = hbos_gcc_last_error_line();
            if (el > 0) { append_str(line,sizeof(line),&pos,"Line "); append_uint(line,sizeof(line),&pos,(uint32_t)el); append_str(line,sizeof(line),&pos,": "); }
            const char *em = hbos_gcc_last_error();
            append_str(line,sizeof(line),&pos,em&&em[0]?em:"error");
            code_set_output(line);
            st->code_error_line = el;
            st->status = "GUI 脚本错误";
        } else {
            code_set_output("GUI OK");
            st->status = "GUI 脚本运行完成";
        }
    }
    g_script_fb = 0;
}

static void handle_code_command(gui_state_t *st, int cmd) {
    if (cmd == CODE_CMD_SAVE) {
        code_load(st);
        (void)code_save(st);
    } else if (cmd == CODE_CMD_RUN) {
        code_run_current(st);
    } else if (cmd == CODE_CMD_OPEN) {
        code_open_selected(st);
    }
    /* CODE_CMD_GUI_RUN is handled separately (needs fb pointer) */
}

static int code_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int code_ident_char(char c) {
    return code_ident_start(c) || (c >= '0' && c <= '9');
}

static int code_word_eq(const char *s, uint32_t len, const char *word) {
    uint32_t i = 0;
    while (word[i]) i++;
    if (i != len) return 0;
    for (i = 0; i < len; i++)
        if (s[i] != word[i]) return 0;
    return 1;
}

static int code_is_keyword(const char *s, uint32_t len) {
    return code_word_eq(s, len, "int") || code_word_eq(s, len, "char") ||
           code_word_eq(s, len, "void") || code_word_eq(s, len, "return") ||
           code_word_eq(s, len, "if") || code_word_eq(s, len, "else") ||
           code_word_eq(s, len, "while") || code_word_eq(s, len, "for") ||
           code_word_eq(s, len, "break") || code_word_eq(s, len, "continue") ||
           code_word_eq(s, len, "class") || code_word_eq(s, len, "public") ||
           code_word_eq(s, len, "private") || code_word_eq(s, len, "new") ||
           code_word_eq(s, len, "delete");
}

// The code editor renders source in the classic 8x16 mono console font (same as
// the TUI), so the cell width is the fixed glyph width. Column-based positioning
// (cursor, click-to-column) all key off this.
static int code_cell_w(void) { return MONO_GLYPH_W; }

// Draw a syntax span in fixed monospace cells using the crisp console bitmap
// font. One byte per cell keeps the column math byte-aligned with the editor's
// offset model (code_offset_for_line_col counts bytes).
static int code_draw_span(int x, int y, int max_x, const char *s,
                          uint32_t start, uint32_t len, uint32_t color) {
    for (uint32_t k = 0; k < len && x < max_x; k++) {
        draw_mono_char(x, y, s[start + k], color);
        x += MONO_GLYPH_W;
    }
    return x;
}

static void code_draw_highlighted_line(int x, int y, int max_x, const char *line, uint32_t len) {
    uint32_t i = 0;
    if (len > 0 && line[0] == '#') {
        (void)code_draw_span(x, y, max_x, line, 0, len, rgb(190, 168, 238));
        return;
    }
    while (i < len && x < max_x) {
        char c = line[i];
        if (c == '/' && i + 1 < len && line[i + 1] == '/') {
            x = code_draw_span(x, y, max_x, line, i, len - i, rgb(116, 170, 130));
            break;
        }
        if (c == '/' && i + 1 < len && line[i + 1] == '*') {
            uint32_t j = i + 2;
            while (j + 1 < len && !(line[j] == '*' && line[j + 1] == '/')) j++;
            if (j + 1 < len) j += 2;
            else j = len;
            x = code_draw_span(x, y, max_x, line, i, j - i, rgb(116, 170, 130));
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            uint32_t j = i + 1;
            while (j < len) {
                if (line[j] == '\\' && j + 1 < len) {
                    j += 2;
                    continue;
                }
                if (line[j++] == quote) break;
            }
            x = code_draw_span(x, y, max_x, line, i, j - i, rgb(236, 192, 116));
            i = j;
            continue;
        }
        if (c >= '0' && c <= '9') {
            uint32_t j = i + 1;
            while (j < len && ((line[j] >= '0' && line[j] <= '9') ||
                   (line[j] >= 'a' && line[j] <= 'f') ||
                   (line[j] >= 'A' && line[j] <= 'F') || line[j] == 'x' || line[j] == 'X'))
                j++;
            x = code_draw_span(x, y, max_x, line, i, j - i, rgb(122, 218, 210));
            i = j;
            continue;
        }
        if (code_ident_start(c)) {
            uint32_t j = i + 1;
            while (j < len && code_ident_char(line[j])) j++;
            uint32_t color = rgb(228, 238, 246);
            if (code_is_keyword(line + i, j - i)) {
                color = rgb(132, 190, 255);
            } else {
                uint32_t k = j;
                while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
                if (k < len && line[k] == '(') color = rgb(150, 220, 182);
            }
            x = code_draw_span(x, y, max_x, line, i, j - i, color);
            i = j;
            continue;
        }
        x = code_draw_span(x, y, max_x, line, i, 1,
                           (c == '{' || c == '}' || c == '(' || c == ')' ||
                            c == '[' || c == ']') ? rgb(250, 224, 142) : rgb(196, 208, 218));
        i++;
    }
}

static void draw_code_app(int tx, int ty, int win_w, int win_h, gui_state_t *st) {
    code_load(st);
    char line[128];
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    st->code_view_rows = l.view_rows;
    code_clamp_scroll(st);

    text(tx, ty, "代码工作台", rgb(102, 214, 255), 1);
    int bx, by, bw;
    if (code_command_rect(l.content_w, CODE_CMD_SAVE, &bx, &by, &bw))
        draw_small_button(tx + bx, ty + by, bw, "保存", rgb(85, 180, 120));
    if (code_command_rect(l.content_w, CODE_CMD_RUN, &bx, &by, &bw))
        draw_small_button(tx + bx, ty + by, bw, "运行", rgb(23, 147, 209));
    if (code_command_rect(l.content_w, CODE_CMD_OPEN, &bx, &by, &bw))
        draw_small_button(tx + bx, ty + by, bw, "打开", rgb(244, 194, 82));
    if (code_command_rect(l.content_w, CODE_CMD_GUI_RUN, &bx, &by, &bw))
        draw_small_button(tx + bx, ty + by, bw, "GUI运行", rgb(215, 100, 244));

    vgradient(tx, ty + 54, l.content_w, 24, rgb(34, 48, 64), rgb(18, 28, 40));
    border(tx, ty + 54, l.content_w, 24, rgb(48, 72, 94));
    line2(line, sizeof(line), "文件 ", code_path(st));
    text_clipped(tx + 10, ty + 62, tx + l.content_w - 90, line,
                 st->code_modified ? rgb(255, 226, 150) : rgb(232, 242, 248), 1);
    text(tx + l.content_w - 78, ty + 62, st->code_modified ? "未保存" : "已保存",
         st->code_modified ? rgb(255, 190, 110) : rgb(124, 220, 154), 1);

    vgradient(tx, l.editor_y, l.side_w, l.editor_h, rgb(22, 30, 40), rgb(14, 20, 28));
    border(tx, l.editor_y, l.side_w, l.editor_h, rgb(46, 66, 84));
    text(tx + 12, l.editor_y + 12, "资源管理器", rgb(194, 226, 242), 1);
    text_clipped(tx + 12, l.editor_y + 34, tx + l.side_w - 10, gui_file_path(st), rgb(132, 196, 232), 1);

    uint32_t count = gui_file_count(st);
    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count && count) selected = (int)count - 1;
    uint32_t start = selected >= l.file_rows ? (uint32_t)selected - (uint32_t)(l.file_rows - 1) : 0;
    uint32_t max = count > start ? count - start : 0;
    if (max > (uint32_t)l.file_rows) max = (uint32_t)l.file_rows;
    for (uint32_t i = 0; i < max; i++) {
        uint32_t file_idx = start + i;
        char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
        uint32_t type = 0;
        vfs_node_t *node = 0;
        if (gui_file_entry(st, file_idx, name, &type, &node, full, sizeof(full)) < 0) continue;
        int y = l.editor_y + 62 + (int)i * FILE_ROW_H;
        if ((int)file_idx == selected) {
            vgradient(tx + 8, y - 6, l.side_w - 16, 24, rgb(28, 80, 116), rgb(16, 50, 78));
            rect(tx + 8, y - 6, 3, 24, rgb(102, 214, 255));
        }
        uint32_t icon = type == VFS_NODE_DIR ? rgb(244, 194, 82) :
                        gui_has_code_suffix(name) ? rgb(102, 214, 255) : rgb(124, 220, 154);
        rect(tx + 14, y + 3, 6, 6, icon);
        text_clipped(tx + 26, y, tx + l.side_w - 10, name,
                     (int)file_idx == selected ? rgb(252, 254, 255) : rgb(210, 222, 234), 1);
    }

    vgradient(l.editor_x, l.editor_y, l.editor_w, l.editor_h, rgb(8, 14, 22), rgb(2, 6, 12));
    border(l.editor_x, l.editor_y, l.editor_w, l.editor_h, rgb(48, 132, 196));
    rect(l.editor_x + l.line_no_w, l.editor_y + 1, 1, l.editor_h - 2, rgb(28, 48, 62));

    uint32_t cursor_line = 0, cursor_col = 0;
    code_line_col(st, st->code_cursor, &cursor_line, &cursor_col);
    uint32_t total_lines = code_line_count(st);
    for (int row = 0; row < l.view_rows; row++) {
        uint32_t line_idx = (uint32_t)(st->code_scroll + row);
        if (line_idx >= total_lines) break;
        uint32_t off = code_find_line_start(st, line_idx);
        uint32_t len = code_line_len_at(st, off);
        if (off > st->code_len) break;
        uint32_t n = len;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, g_code_buf + off, n);
        line[n] = 0;

        char num[16];
        uint32_t pos = 0;
        num[0] = 0;
        append_uint(num, sizeof(num), &pos, line_idx + 1);
        int y = l.editor_y + 10 + row * l.row_h;
        if (st->code_error_line > 0 && (int)(line_idx + 1) == st->code_error_line) {
            rect(l.editor_x + l.line_no_w + 1, y - 3, l.editor_w - l.line_no_w - 4, l.row_h, rgb(70, 24, 30));
            rect(l.editor_x + l.line_no_w + 1, y - 3, 3, l.row_h, rgb(232, 86, 92));
        } else if (line_idx == cursor_line) {
            rect(l.editor_x + l.line_no_w + 1, y - 3, l.editor_w - l.line_no_w - 4, l.row_h, rgb(16, 28, 38));
        }
        text(l.editor_x + 8, y, num, rgb(102, 134, 154), 1);
        code_draw_highlighted_line(l.editor_x + l.line_no_w + 10, y,
                                   l.editor_x + l.editor_w - 10, line, n);
    }
    if ((int)cursor_line >= st->code_scroll && (int)cursor_line < st->code_scroll + l.view_rows) {
        int cx = l.editor_x + l.line_no_w + 10 + (int)cursor_col * code_cell_w();
        int cy = l.editor_y + 10 + ((int)cursor_line - st->code_scroll) * l.row_h;
        if (cx > l.editor_x + l.editor_w - 12) cx = l.editor_x + l.editor_w - 12;
        rect(cx, cy - 2, 2, 14, rgb(102, 214, 255));
    }

    vgradient(tx, l.bottom_y, l.content_w, l.bottom_h, rgb(22, 30, 40), rgb(14, 20, 28));
    border(tx, l.bottom_y, l.content_w, l.bottom_h, st->code_error_line > 0 ? rgb(176, 62, 72) : rgb(46, 66, 84));
    text(tx + 12, l.bottom_y + 12, "输出", rgb(194, 226, 242), 1);
    text_clipped(tx + 62, l.bottom_y + 12, tx + l.content_w - 12,
                 g_code_output[0] ? g_code_output : "Ready",
                 st->code_error_line > 0 ? rgb(255, 188, 190) : rgb(210, 221, 230), 1);
    uint32_t pos = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &pos, "Ln ");
    append_uint(line, sizeof(line), &pos, cursor_line + 1);
    append_str(line, sizeof(line), &pos, ", Col ");
    append_uint(line, sizeof(line), &pos, cursor_col + 1);
    append_str(line, sizeof(line), &pos, "  Bytes ");
    append_uint(line, sizeof(line), &pos, st->code_len);
    text(tx + 12, l.bottom_y + 34, line, rgb(148, 168, 180), 1);
}



#define GUI_CON_ROWS 64
#define GUI_CON_COLS 120

static void console_append_history(gui_state_t *st, const char *line) {
    if (st->console_line_count < GUI_CON_ROWS) {
        strncpy(st->console_history[st->console_line_count], line, GUI_CON_COLS - 1);
        st->console_history[st->console_line_count][GUI_CON_COLS - 1] = 0;
        st->console_line_count++;
    } else {
        for (int i = 0; i < GUI_CON_ROWS - 1; i++) {
            strcpy(st->console_history[i], st->console_history[i + 1]);
        }
        strncpy(st->console_history[GUI_CON_ROWS - 1], line, GUI_CON_COLS - 1);
        st->console_history[GUI_CON_ROWS - 1][GUI_CON_COLS - 1] = 0;
    }
}

// ── Real shell output capture ───────────────────────────────
// The GUI terminal runs the actual shell (cmd_execute) and captures its console
// output here, instead of reimplementing a handful of commands. The sink strips
// ANSI escape sequences and splits on newlines into history lines.
static gui_state_t *g_con_sink_st;
static char         g_con_sink_line[GUI_CON_COLS];
static uint32_t     g_con_sink_pos;
static int          g_con_sink_esc;     // inside an ESC [...] sequence

static void gui_console_flush_line(void) {
    g_con_sink_line[g_con_sink_pos] = 0;
    console_append_history(g_con_sink_st, g_con_sink_line);
    g_con_sink_pos = 0;
}

static void gui_console_sink(char c) {
    if (g_con_sink_esc) {                       // swallow ESC [ ... <final>
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) g_con_sink_esc = 0;
        return;
    }
    if (c == 0x1B) { g_con_sink_esc = 1; return; }
    if (c == '\r') return;
    if (c == '\n') { gui_console_flush_line(); return; }
    if (c == '\t') {
        do {
            if (g_con_sink_pos < GUI_CON_COLS - 1) g_con_sink_line[g_con_sink_pos++] = ' ';
        } while (g_con_sink_pos % 4 && g_con_sink_pos < GUI_CON_COLS - 1);
        return;
    }
    if ((unsigned char)c < 0x20) return;        // drop other control bytes
    g_con_sink_line[g_con_sink_pos++] = c;
    if (g_con_sink_pos >= GUI_CON_COLS - 1) gui_console_flush_line();
}

static void console_exec_cmd(gui_state_t *st) {
    // Echo the prompt + typed command. The "hbos_gui_shell:/# " prefix (18 chars)
    // is also what the up-arrow history search keys off, so keep it exact.
    char cmd_line[GUI_CON_COLS];
    uint32_t cpos = 0;
    cmd_line[0] = 0;
    append_str(cmd_line, sizeof(cmd_line), &cpos, "hbos_gui_shell:/# ");
    append_str(cmd_line, sizeof(cmd_line), &cpos, st->console_input);
    console_append_history(st, cmd_line);

    char *cmd = st->console_input;
    while (*cmd == ' ') cmd++;

    if (*cmd == 0) {
        // empty line — just a fresh prompt
    } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0) {
        st->console_line_count = 0;
    } else if (strcmp(cmd, "gui") == 0 || strcmp(cmd, "startx") == 0 ||
               strcmp(cmd, "exit") == 0 || strcmp(cmd, "reboot") == 0 ||
               strcmp(cmd, "shutdown") == 0 || strcmp(cmd, "poweroff") == 0) {
        // Block commands that would recurse into the GUI or end the session.
        console_append_history(st, "（该命令在图形终端中不可用）");
    } else {
        // Run the REAL shell command and capture its console output into the
        // terminal history — the full command set, no reimplementation.
        g_con_sink_st  = st;
        g_con_sink_pos = 0;
        g_con_sink_esc = 0;
        console_set_sink(gui_console_sink);
        cmd_execute(st->console_input);
        console_set_sink(NULL);
        if (g_con_sink_pos > 0) gui_console_flush_line();  // trailing partial line
    }

    st->console_input_len = 0;
    st->console_input[0] = 0;
    st->console_cursor = 0;
    st->console_history_idx = -1;
}

static void draw_diag_app(int tx, int ty, int win_w, int win_h, gui_state_t *st) {
    int box_x = tx - 20;
    int box_y = ty - 4;
    int box_w = win_w - 20;
    int box_h = win_h - 74;

    // Flat Breeze terminal surface.
    rect(box_x, box_y, box_w, box_h, rgb(27, 30, 33));
    border(box_x, box_y, box_w, box_h, rgb(60, 64, 69));

    int row_h = MONO_GLYPH_H + 2;          // 8x16 console font + 2px leading
    int input_y = box_y + box_h - 24;
    int max_x = box_x + box_w - 12;

    // 自动根据可用高度计算最多绘制的历史记录行数，防止重叠
    int max_lines = (input_y - (box_y + 12)) / row_h;
    if (max_lines < 1) max_lines = 1;
    uint32_t start_idx = 0;
    if (st->console_line_count > (uint32_t)max_lines) {
        start_idx = st->console_line_count - (uint32_t)max_lines;
    }

    // 绘制命令历史（控制台位图字体，与真 TUI 一致）
    int start_y = box_y + 12;
    for (uint32_t i = start_idx; i < st->console_line_count; i++) {
        const char *line = st->console_history[i];
        uint32_t color = cyber_text(0);
        if (strncmp(line, "hbos_gui_shell:", 15) == 0) {
            color = rgb(39, 174, 96); // Breeze 绿 prompt
        } else if (strncmp(line, "hbos_shell:", 11) == 0) {
            color = rgb(218, 68, 83); // Breeze 红 error
        } else if (strncmp(line, "  ", 2) == 0) {
            color = rgb(160, 167, 173); // 次要灰细节
        }
        text_mono(box_x + 12, start_y, max_x, line, color);
        start_y += row_h;
    }

    // 绘制当前输入行（固定 8px 等宽 cell，光标按 cell 对齐）
    const char *prompt = "hbos_gui_shell:/# ";
    int px = text_mono(box_x + 12, input_y, max_x, prompt, rgb(39, 174, 96));
    text_mono(px, input_y, max_x, st->console_input, cyber_text(0));

    // 闪烁光标（细竖线 caret）
    static uint32_t cursor_ticks = 0;
    cursor_ticks++;
    if ((cursor_ticks / 15) % 2) {
        int cursor_x = px + (int)st->console_cursor * MONO_GLYPH_W;
        rect(cursor_x, input_y, 2, MONO_GLYPH_H - 2, rgb(39, 174, 96));
    }
}

static void snake_place_food(gui_state_t *st) {
    for (int step = 0; step < SNAKE_MAX; step++) {
        int x = (st->snake_tx + 5 + step * 3) % SNAKE_W;
        int y = (st->snake_ty + 3 + step * 2) % SNAKE_H;
        int hit = 0;
        for (int i = 0; i < st->snake_len; i++) {
            if (st->snake_body_x[i] == x && st->snake_body_y[i] == y) {
                hit = 1;
                break;
            }
        }
        if (!hit) {
            st->snake_tx = x;
            st->snake_ty = y;
            return;
        }
    }
}

static void snake_reset(gui_state_t *st) {
    st->snake_len = 4;
    st->snake_dx = 1;
    st->snake_dy = 0;
    st->snake_alive = 1;
    st->snake_score = 0;
    st->snake_body_x[0] = 5;
    st->snake_body_y[0] = 4;
    st->snake_body_x[1] = 4;
    st->snake_body_y[1] = 4;
    st->snake_body_x[2] = 3;
    st->snake_body_y[2] = 4;
    st->snake_body_x[3] = 2;
    st->snake_body_y[3] = 4;
    st->snake_x = st->snake_body_x[0];
    st->snake_y = st->snake_body_y[0];
    st->snake_tx = 10;
    st->snake_ty = 4;
    st->snake_last_sec = cmos_second();
    snake_place_food(st);
    st->status = "贪吃蛇已开始";
}

static void snake_turn(gui_state_t *st, int dx, int dy) {
    if (st->snake_len <= 0) snake_reset(st);
    if (!st->snake_alive) return;
    if (st->snake_len <= 1 || dx != -st->snake_dx || dy != -st->snake_dy) {
        st->snake_dx = dx;
        st->snake_dy = dy;
    }
    st->status = "贪吃蛇已转向";
}

static void draw_snake_app(int tx, int ty, gui_state_t *st) {
    if (st->snake_len <= 0) snake_reset(st);
    char line[96];
    text(tx, ty, "贪吃蛇", rgb(85, 180, 120), 1);
    line_u32(line, sizeof(line), "分数: ", (uint32_t)st->snake_score, "");
    text(tx, ty + 34, line, rgb(210, 221, 230), 1);
    text(tx, ty + 56, st->snake_alive ? "方向键移动  吃掉蓝色食物" : "游戏结束  Enter 重新开始", rgb(148, 162, 174), 1);
    int bx = tx;
    int by = ty + 84;
    int cell = 16;
    rect(bx, by, cell * SNAKE_W, cell * SNAKE_H, rgb(5, 10, 16));
    border(bx, by, cell * SNAKE_W, cell * SNAKE_H, rgb(85, 180, 120));
    rect(bx + st->snake_tx * cell + 4, by + st->snake_ty * cell + 4, cell - 8, cell - 8, rgb(23, 147, 209));
    for (int i = st->snake_len - 1; i >= 0; i--) {
        uint32_t color = i == 0 ? rgb(160, 245, 170) : rgb(84, 190, 116);
        rect(bx + st->snake_body_x[i] * cell + 2, by + st->snake_body_y[i] * cell + 2,
             cell - 4, cell - 4, color);
    }
}

static const char *gui_parse_url(const char *url, int *https, char *host, uint32_t host_cap,
                                 uint16_t *port, const char **path) {
    const char *p = url;
    *https = 0;
    *port = 80;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *https = 1;
        *port = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (!strstr(p, "://")) {
        *https = 1;
        *port = 443;
    } else {
        return "仅支持 http:// 和 https://";
    }
    uint32_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 >= host_cap) return "主机名太长";
        if (*p == ':') {
            uint32_t v = 0;
            p++;
            if (*p < '0' || *p > '9') return "端口错误";
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (uint32_t)(*p++ - '0');
                if (v == 0 || v > 65535) return "端口错误";
            }
            *port = (uint16_t)v;
            break;
        }
        host[n++] = *p++;
    }
    while (*p && *p != '/') return "URL 格式错误";
    host[n] = 0;
    if (!n) return "缺少主机名";
    *path = *p ? p : "/";
    return 0;
}

static const char *http_body_ptr(const char *buf) {
    const char *p = strstr(buf, "\r\n\r\n");
    return p ? p + 4 : buf;
}

static void browser_text_from_html(const char *html, char *out, uint32_t cap, uint32_t *out_len) {
    uint32_t pos = 0;
    int tag = 0;
    int space = 1;
    for (uint32_t i = 0; html[i] && pos + 1 < cap; i++) {
        char c = html[i];
        if (c == '<') {
            tag = 1;
            if (!space && pos + 1 < cap) {
                out[pos++] = '\n';
                space = 1;
            }
            continue;
        }
        if (tag) {
            if (c == '>') tag = 0;
            continue;
        }
        if (c == '&') {
            if (strncmp(html + i, "&amp;", 5) == 0) { c = '&'; i += 4; }
            else if (strncmp(html + i, "&lt;", 4) == 0) { c = '<'; i += 3; }
            else if (strncmp(html + i, "&gt;", 4) == 0) { c = '>'; i += 3; }
        }
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') c = ' ';
        if (c == ' ') {
            if (space) continue;
            space = 1;
        } else {
            space = 0;
        }
        out[pos++] = c;
    }
    out[pos] = 0;
    if (out_len) *out_len = pos;
}

static void browser_init(gui_state_t *st) {
    if (st->browser_loaded) return;
    strcpy(st->browser_url, "https://example.com/");
    strcpy(st->browser_page, "输入网址后按 Enter 加载。当前 HTTPS 支持 TLS 1.3 + ChaCha20-Poly1305；部分网站会自动尝试 HTTP。");
    st->browser_page_len = (uint32_t)strlen(st->browser_page);
    st->browser_scroll = 0;
    st->browser_loaded = 1;
}

static void browser_load(gui_state_t *st) {
    browser_init(st);
    char host[96];
    const char *path = "/";
    uint16_t port = 80;
    int https = 0;
    const char *err = gui_parse_url(st->browser_url, &https, host, sizeof(host), &port, &path);
    if (err) {
        strcpy(st->browser_page, err);
        st->browser_page_len = (uint32_t)strlen(st->browser_page);
        st->status = "浏览器 URL 错误";
        return;
    }
    st->status = "浏览器加载中";
    if (!net_primary()->dhcp_ok && net_dhcp() < 0) {
        line2(st->browser_page, BROWSER_PAGE_CAP, "网络未配置: ", net_last_error());
        st->browser_page_len = (uint32_t)strlen(st->browser_page);
        st->status = "浏览器网络失败";
        return;
    }
    uint32_t ip = 0;
    if (net_dns_resolve(host, &ip) < 0) {
        line2(st->browser_page, BROWSER_PAGE_CAP, "DNS 失败: ", net_last_error());
        st->browser_page_len = (uint32_t)strlen(st->browser_page);
        st->status = "浏览器 DNS 失败";
        return;
    }
    static char response[8192];
    uint32_t len = 0;
    int ok = https ? tls_https_get(host, ip, port, path, response, sizeof(response), &len)
                   : net_http_request("GET", host, ip, port, path, response, sizeof(response), &len);
    const char *tls_error = https ? tls_last_error() : "";
    if (ok < 0 && https) {
        port = 80;
        ok = net_http_request("GET", host, ip, port, path, response, sizeof(response), &len);
        if (ok == 0) st->status = "HTTPS 失败，已用 HTTP 回退";
    }
    if (ok < 0) {
        line2(st->browser_page, BROWSER_PAGE_CAP, "加载失败: ", https ? tls_error : net_last_error());
        st->browser_page_len = (uint32_t)strlen(st->browser_page);
        st->status = "浏览器加载失败";
        return;
    }
    browser_text_from_html(http_body_ptr(response), st->browser_page, BROWSER_PAGE_CAP, &st->browser_page_len);
    st->browser_scroll = 0;
    if (!https || strcmp(st->status, "HTTPS 失败，已用 HTTP 回退") != 0)
        st->status = "浏览器加载完成";
}

static void browser_save_page(gui_state_t *st) {
    browser_init(st);
    char path[GUI_PATH_MAX];
    if (gui_path_join(gui_file_path(st), "browser-page.txt", path, sizeof(path)) < 0) {
        st->status = "保存路径过长";
        return;
    }
    vfs_node_t *node = vfs_lookup(path);
    if (!node) node = vfs_create(path);
    if (!node || node->type != VFS_NODE_FILE ||
        vfs_truncate(node) < 0 ||
        vfs_write(node, 0, st->browser_page, st->browser_page_len) < 0) {
        st->status = "网页保存失败";
        return;
    }
    (void)fs_sync();
    (void)gui_select_path(st, path);
    st->status = "网页已保存到文件";
}

static void draw_wrapped_text(int x, int y, int w, int h, const char *body, int scroll) {
    char line[96];
    uint32_t line_pos = 0;
    int max_cols = w / 8;
    if (max_cols < 16) max_cols = 16;
    if (max_cols > 90) max_cols = 90;
    int current_line = 0;
    int drawn = 0;
    int max_lines = h / 18;
    for (uint32_t i = 0;; i++) {
        char c = body[i];
        int flush = c == 0 || c == '\n' || line_pos >= (uint32_t)max_cols;
        if (!flush && c) line[line_pos++] = c;
        if (flush) {
            line[line_pos] = 0;
            if (current_line >= scroll && drawn < max_lines) {
                text(x, y + drawn * 18, line, rgb(218, 230, 238), 1);
                drawn++;
            }
            current_line++;
            line_pos = 0;
            if (c == 0 || drawn >= max_lines) break;
            if (c != '\n') i--;
        }
    }
}

static void draw_browser_app(int tx, int ty, int win_w, gui_state_t *st) {
    browser_init(st);
    int view_w = win_w - 72;
    if (view_w < 320) view_w = 320;
    const net_device_t *dev = net_primary();
    char ipbuf[16];
    net_ipv4_to_str(dev->ip, ipbuf);
    text(tx, ty, "浏览器", rgb(78, 192, 236), 1);
    text(tx, ty + 30, "Enter加载  Ctrl+S保存  方向键滚动", rgb(148, 162, 174), 1);
    rect(tx, ty + 58, view_w, 32, rgb(6, 14, 22));
    border(tx, ty + 58, view_w, 32, rgb(78, 192, 236));
    text(tx + 10, ty + 68, st->browser_url, rgb(232, 242, 248), 1);
    rect(tx + view_w - 12, ty + 68, 6, 14, rgb(78, 192, 236));
    draw_small_button(tx, ty + 104, 96, "Enter 加载", rgb(78, 192, 236));
    draw_small_button(tx + 108, ty + 104, 68, "保存", rgb(85, 180, 120));
    char line[128];
    uint32_t pos = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &pos, dev->dhcp_ok ? "网络 " : "未配置 ");
    append_str(line, sizeof(line), &pos, net_driver_name(dev->driver));
    append_str(line, sizeof(line), &pos, " ");
    append_str(line, sizeof(line), &pos, ipbuf);
    append_str(line, sizeof(line), &pos, "  ");
    append_str(line, sizeof(line), &pos, st->status ? st->status : "浏览器就绪");
    text_clipped(tx + 190, ty + 112, tx + view_w - 8, line, rgb(168, 190, 204), 1);
    rect(tx, ty + 146, view_w, 196, rgb(4, 9, 14));
    border(tx, ty + 146, view_w, 196, rgb(50, 74, 90));
    draw_wrapped_text(tx + 12, ty + 158, view_w - 24, 172, st->browser_page, st->browser_scroll);
}

static void draw_window_frame(int x, int y, int win_w, int win_h, const char *title, int active, int state, int light) {
    int R = (state == WM_STATE_MAXIMIZED) ? 0 : 8;
    if (state != WM_STATE_MAXIMIZED) {
        soft_shadow(x, y, win_w, win_h);
        soft_shadow(x + 2, y + 3, win_w - 4, win_h - 4);  // deeper drop shadow
    }

    uint32_t body_bg  = active ? (light ? rgb(239, 240, 241) : rgb(42, 46, 50))
                               : (light ? rgb(239, 240, 241) : rgb(35, 38, 41));
    uint32_t title_bg = active ? (light ? rgb(214, 217, 221) : rgb(49, 54, 59))
                               : (light ? rgb(231, 233, 235) : rgb(38, 41, 44));
    uint32_t border_c = active ? rgb(61, 174, 233)
                               : (light ? rgb(188, 192, 196) : rgb(60, 64, 69));

    // Rounded Breeze frame: rounded body, rounded-top titlebar, 1px separator,
    // focus border on the straight edges (corners carried by the AA fill).
    fill_round_rect(x, y, win_w, win_h, R, body_bg, RR_ALL);
    fill_round_rect(x, y, win_w, WM_TITLE_H, R, title_bg, RR_TOP);
    rect(x + 1, y + WM_TITLE_H, win_w - 2, 1, light ? rgb(196, 200, 204) : rgb(60, 64, 69));
    rect(x + R, y, win_w - 2 * R, 1, border_c);
    rect(x + R, y + win_h - 1, win_w - 2 * R, 1, border_c);
    rect(x, y + R, 1, win_h - 2 * R, border_c);
    rect(x + win_w - 1, y + R, 1, win_h - 2 * R, border_c);

    // Window controls: flat, close=red, min/max neutral (no accent strip).
    uint32_t nbtn = active ? (light ? rgb(206, 209, 213) : rgb(66, 71, 77))
                           : (light ? rgb(222, 224, 227) : rgb(54, 58, 63));
    int btn_x = x + win_w - WM_BTN_W - 8;
    int btn_y = y + 7;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20,
                     rgb(218, 68, 83), rgb(218, 68, 83), rgb(160, 46, 58), 0);
    draw_window_control_icon(btn_x + 7, btn_y + 6, GUI_CTRL_CLOSE, 0,
                             rgb(252, 238, 236));

    btn_x -= WM_BTN_W + WM_BTN_GAP;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20, nbtn, nbtn,
                     light ? rgb(176, 180, 184) : rgb(60, 64, 69), 0);
    draw_window_control_icon(btn_x + 6, btn_y + 4, GUI_CTRL_MAX,
                             state == WM_STATE_MAXIMIZED,
                             light ? rgb(60, 64, 69) : rgb(236, 242, 240));

    btn_x -= WM_BTN_W + WM_BTN_GAP;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20, nbtn, nbtn,
                     light ? rgb(176, 180, 184) : rgb(60, 64, 69), 0);
    draw_window_control_icon(btn_x + 6, btn_y + 4, GUI_CTRL_MIN, 0,
                             light ? rgb(60, 64, 69) : rgb(236, 242, 240));

    // Titlebar app icon (rounded accent square) + vertically-centered title.
    int ico = 14, icy = y + (WM_TITLE_H - ico) / 2;
    uint32_t icon_c = active ? rgb(61, 174, 233)
                             : (light ? rgb(150, 156, 162) : rgb(110, 118, 126));
    fill_round_rect(x + 12, icy, ico, ico, 3, icon_c, RR_ALL);
    rect(x + 12 + 4, icy + 5, ico - 8, ico - 9, rgb(255, 255, 255));
    int tty = y + (WM_TITLE_H - gui_font_line_height()) / 2;
    text_clipped(x + 34, tty, x + win_w - WM_BTN_W * 3 - WM_BTN_GAP * 2 - 16,
                 title, active ? rgb(255, 255, 255) : (light ? rgb(90, 96, 102) : rgb(190, 196, 202)), 1);
}

static void draw_panel_window(int tx, int ty, int win_w, int w, int h, gui_state_t *st, int panel) {
    if (panel == PANEL_FILES) draw_files_panel(tx, ty, win_w, st);
    else if (panel == PANEL_DISK) draw_disk_panel(tx, ty, win_w);
    else if (panel == PANEL_SYS) draw_resource_panel(tx, ty, win_w, w, h);
    else draw_apps_panel(tx, ty, win_w, st);
}

static void draw_app_window_body(int tx, int ty, int win_w, int win_h, gui_state_t *st, int mode) {
    if (gui_app_draw(st, mode, tx, ty, win_w, win_h)) return;
    if (mode == GUI_APP_NOTES) draw_notes_app(tx, ty, win_w, st);
    else if (mode == GUI_APP_UWC) draw_uwc_app(tx, ty, st);
    else if (mode == GUI_APP_SNAKE) draw_snake_app(tx, ty, st);
    else if (mode == GUI_APP_BROWSER) draw_browser_app(tx, ty, win_w, st);
    else if (mode == GUI_APP_CODE) draw_code_app(tx, ty, win_w, win_h, st);
    else if (mode == GUI_APP_DIAG) draw_diag_app(tx, ty, win_w, win_h, st);
}

static void draw_one_window(int w, int h, gui_state_t *st, int idx) {
    wm_window_t *win = wm_get_window(&st->wm, idx);
    if (!win) return;
    if (win->state == WM_STATE_MINIMIZED && win->anim_type != WM_ANIM_MINIMIZE) return;
    int old_active = st->active;
    int old_app = st->app_mode;
    int old_x = st->win_x;
    int old_y = st->win_y;
    int win_x, win_y, win_w, win_h;
    gui_window_metrics(st, w, h, win, idx, &win_x, &win_y, &win_w, &win_h);
    uint8_t saved_opacity = gui_get_layer_opacity();
    if (win->opacity < 255) gui_set_layer_opacity(win->opacity);
    draw_window_frame(win_x, win_y, win_w, win_h, gui_window_title(win),
                      idx == st->wm.active_window, win->state, st->theme_light);

    st->win_x = win_x;
    st->win_y = win_y;
    if (idx == st->wm.active_window) gui_store_focus(st);

    int tx = win_x + 30;
    int ty = win_y + 42;
    if (win->kind == WM_WIN_PANEL) {
        st->active = win->mode;
        st->app_mode = GUI_APP_NONE;
        draw_panel_window(tx, ty, win_w, w, h, st, win->mode);
    } else {
        st->active = PANEL_APPS;
        st->app_mode = win->mode;
        draw_app_window_body(tx, ty, win_w, win_h, st, win->mode);
    }
    vgradient_alpha(win_x + 1, win_y + win_h - 31, win_w - 2, 30,
                    0xD0000000 | (st->theme_light ? rgb(220, 225, 230) : rgb(28, 34, 37)),
                    0xD0000000 | (st->theme_light ? rgb(200, 205, 210) : rgb(13, 16, 19)));
    rect(win_x + 1, win_y + win_h - 32, win_w - 2, 1, st->theme_light ? rgb(180, 185, 190) : rgb(75, 96, 92));
    rect(win_x + 1, win_y + win_h - 31, win_w - 2, 1, idx == st->wm.active_window ? cyber_neon_pink(st->theme_light) : (st->theme_light ? rgb(190, 195, 200) : rgb(44, 54, 58)));
    rect(win_x + 1, win_y + win_h - 1, win_w - 2, 1, st->theme_light ? rgb(180, 185, 190) : rgb(5, 8, 10));
    text(win_x + 24, win_y + win_h - 22,
         idx == st->wm.active_window ? st->status : "单击任务栏切换",
         cyber_text_muted(st->theme_light), 1);

    st->active = old_active;
    st->app_mode = old_app;
    st->win_x = old_x;
    st->win_y = old_y;
    gui_set_layer_opacity(saved_opacity);
}

// ---- Windows 11-style centered taskbar ----
#define TB_BTN 46
#define TB_GAP 8
#define TBHIT_NONE  -1
#define TBHIT_START -2
#define TBHIT_WIN_BASE 100

static const struct { const char *label; int panel; } g_taskbar_pins[] = {
    {"文件", PANEL_FILES},
    {"磁盘", PANEL_DISK},
    {"资源", PANEL_SYS},
    {"应用", PANEL_APPS},
};
#define TB_PIN_COUNT ((int)(sizeof(g_taskbar_pins) / sizeof(g_taskbar_pins[0])))

static int taskbar_windows(const gui_state_t *st, int *out, int max) {
    int n = 0;
    for (int i = 0; i < st->wm.window_count && n < max; i++) {
        wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, i);
        if (win && win->used) out[n++] = i;
    }
    return n;
}

static void taskbar_item_xy(int idx, int item_count, int w, int h, int *rx, int *ry) {
    int total = item_count * TB_BTN + (item_count - 1) * TB_GAP;
    int sx = (w - total) / 2;
    if (sx < 8) sx = 8;
    *rx = sx + idx * (TB_BTN + TB_GAP);
    *ry = h - TASKBAR_H + (TASKBAR_H - TB_BTN) / 2;
}



// Small white motif for an app window icon, tuned for a ~28px icon box. Every
// app mode gets a distinct, recognizable shape — without this the taskbar drew

static void draw_taskbar(int w, int h, const gui_state_t *st) {
    int light = st->theme_light;
    uint32_t accent = rgb(61, 174, 233);
    int wins[WM_MAX_WINDOWS];
    int n = taskbar_windows(st, wins, WM_MAX_WINDOWS);
    int item_count = 1 + TB_PIN_COUNT + n;

    for (int i = 0; i < item_count; i++) {
        int rx, ry;
        taskbar_item_xy(i, item_count, w, h, &rx, &ry);
        if (i == 0) {
            if (st->wm.start_menu_open)
                fill_round_rect(rx, ry, TB_BTN, TB_BTN, 8, 0x44FFFFFF, RR_ALL);
            int s = 7, gx = rx + TB_BTN / 2 - 8, gy = ry + TB_BTN / 2 - 8;
            rect(gx, gy, s, s, accent);
            rect(gx + s + 2, gy, s, s, accent);
            rect(gx, gy + s + 2, s, s, accent);
            rect(gx + s + 2, gy + s + 2, s, s, accent);
        } else if (i <= TB_PIN_COUNT) {
            int panel = g_taskbar_pins[i - 1].panel;
            int isz = 30, ix = rx + (TB_BTN - isz) / 2, iy = ry + (TB_BTN - isz) / 2;
            blit_icon(ix, iy, isz, panel_icon_id(panel));
        } else {
            int slot = wins[i - 1 - TB_PIN_COUNT];
            wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, slot);
            int act = (slot == st->wm.active_window);
            int isz = 30, ix = rx + (TB_BTN - isz) / 2, iy = ry + (TB_BTN - isz) / 2 - 2;
            if (win->kind == WM_WIN_PANEL)
                blit_icon(ix, iy, isz, panel_icon_id(win->mode));
            else
                blit_icon(ix, iy, isz, app_icon_id(win->mode));
            rect(rx + TB_BTN / 2 - 7, ry + TB_BTN - 1, 14, 3,
                 act ? accent : (light ? rgb(150, 154, 158) : rgb(150, 155, 160)));
        }
    }

    char line[32];
    time_line(line, sizeof(line));
    int tw = text_width(line, 1);
    text(w - 16 - tw, h - TASKBAR_H + (TASKBAR_H - gui_font_line_height()) / 2,
         line, light ? cyber_text(1) : rgb(235, 238, 242), 1);
}

static int taskbar_hit(int w, int h, const gui_state_t *st, int mx, int my) {
    int wins[WM_MAX_WINDOWS];
    int n = taskbar_windows(st, wins, WM_MAX_WINDOWS);
    int item_count = 1 + TB_PIN_COUNT + n;
    int ry = h - TASKBAR_H + (TASKBAR_H - TB_BTN) / 2;
    if (my < ry || my >= ry + TB_BTN) return TBHIT_NONE;
    for (int i = 0; i < item_count; i++) {
        int rx, ryy;
        taskbar_item_xy(i, item_count, w, h, &rx, &ryy);
        if (mx >= rx && mx < rx + TB_BTN) {
            if (i == 0) return TBHIT_START;
            if (i <= TB_PIN_COUNT) return i - 1;            // pin index
            return TBHIT_WIN_BASE + wins[i - 1 - TB_PIN_COUNT];
        }
    }
    return TBHIT_NONE;
}

static void draw_gui_screen(int w, int h, gui_state_t *st) {
    gui_sync_focus(st);
    draw_desktop(w, h, st);
    draw_start_menu(st);
    draw_window_switcher(w, h, st);
    if (st->splash_ticks > 0)
        draw_splash_window(w, h, st->splash_ticks, st->theme_light);
}

static void draw_gui_screen(int w, int h, gui_state_t *st);

static void gui_damage_region(int x, int y, int rw, int rh, int scr_w, int scr_h) {
    gui_dirty_expand(&x, &y, &rw, &rh, GUI_DIRTY_PAD, scr_w, scr_h);
    if (rw > 0 && rh > 0) gui_dirty_add(x, y, rw, rh);
}

static void gui_damage_window(gui_state_t *st, int scr_w, int scr_h, int idx) {
    int wx, wy, ww, wh;
    gui_window_metrics(st, scr_w, scr_h, NULL, idx, &wx, &wy, &ww, &wh);
    gui_damage_region(wx, wy, ww, wh, scr_w, scr_h);
}

static void gui_damage_cursor(int omx, int omy, int nmx, int nmy) {
    // The cursor glyph spans x..x+14, y..y+22; the damage box must fully cover
    // it (incl. the -2 top margin) or stray bottom pixels leave a dotted trail.
    gui_dirty_add(omx - 2, omy - 2, 22, 27);
    gui_dirty_add(nmx - 2, nmy - 2, 22, 27);
}

static int cursor_overlaps_rect(int mx, int my, int rx, int ry, int rw, int rh) {
    return mx + 22 > rx && mx - 2 < rx + rw && my + 25 > ry && my - 2 < ry + rh;
}

static void draw_gui_frame(const fb_info_t *fb, int w, int h, gui_state_t *st, int mx, int my, int edge) {
    if (gui_dirty_is_full() || gui_dirty_count() == 0) {
        if (gui_dirty_count() == 0) gui_dirty_mark_full();
        draw_gui_screen(w, h, st);
        draw_cursor(mx, my, edge);
        gui_present_surface(fb);
        gui_dirty_reset();
        return;
    }

    int n = gui_dirty_count();
    for (int i = 0; i < n; i++) {
        int rx, ry, rw, rh;
        if (gui_dirty_get(i, &rx, &ry, &rw, &rh) < 0) continue;
        gui_clip_set(rx, ry, rw, rh);
        draw_gui_screen(w, h, st);
        if (cursor_overlaps_rect(mx, my, rx, ry, rw, rh))
            draw_cursor(mx, my, edge);
        gui_present_rect(fb, rx, ry, rw, rh);
    }
    gui_clip_clear();
    gui_dirty_reset();
}

// Desktop shortcut icons over the wallpaper. Single click opens the target.
typedef struct { const char *label; int kind; int mode; } desktop_icon_t;
static const desktop_icon_t g_desktop_icons[] = {
    {"文件管理器", WM_WIN_PANEL, PANEL_FILES},
    {"应用",       WM_WIN_PANEL, PANEL_APPS},
    {"控制台",     WM_WIN_APP,   GUI_APP_DIAG},
    {"计算器",     WM_WIN_APP,   GUI_APP_CALC},
};
#define DESKTOP_ICON_COUNT ((int)(sizeof(g_desktop_icons) / sizeof(g_desktop_icons[0])))

static void desktop_icon_rect(int i, int *x, int *y, int *w, int *h) {
    *w = 96; *h = 100;
    *x = 42;
    *y = 46 + i * 112;
}


static void draw_desktop_icons(void) {
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        const desktop_icon_t *d = &g_desktop_icons[i];
        int x, y, w, h;
        desktop_icon_rect(i, &x, &y, &w, &h);
        int isz = 58, ix = x + (w - isz) / 2, iy = y;
        if (d->kind == WM_WIN_PANEL)
            blit_icon(ix, iy, isz, panel_icon_id(d->mode));
        else
            blit_icon(ix, iy, isz, app_icon_id(d->mode));
        int tw = text_width(d->label, 1);
        int lx = x + (w - tw) / 2;
        text(lx + 1, iy + isz + 6, d->label, rgb(0, 0, 0), 1);          // shadow
        text(lx, iy + isz + 5, d->label, rgb(245, 247, 250), 1);
    }
}

static int hit_desktop_icon(int mx, int my) {
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        int x, y, w, h;
        desktop_icon_rect(i, &x, &y, &w, &h);
        if (mx >= x && mx < x + w && my >= y && my < y + h + 16) return i;
    }
    return -1;
}

static void draw_desktop(int w, int h, gui_state_t *st) {
    draw_wallpaper(w, h, st->theme_light);

    // Clean desktop: just shortcut icons over the wallpaper (launchers live on
    // the Windows 11-style bottom taskbar now, not a left dock).
    draw_desktop_icons();

    for (int i = 0; i < st->wm.window_count; i++) {
        draw_one_window(w, h, st, st->wm.z_order[i]);
    }

    if (st->snap_preview != WM_SNAP_NONE) {
        int px = 0, py = 0, pw = w, ph = h - TASKBAR_H;
        if (st->snap_preview == WM_SNAP_LEFT) { pw = w / 2; }
        else if (st->snap_preview == WM_SNAP_RIGHT) { px = w / 2; pw = w - w / 2; }
        uint32_t accent = cyber_neon_cyan(st->theme_light);
        for (int yy = py + 4; yy < py + ph - 4; yy += 3)
            rect(px + 6, yy, pw - 12, 1, st->theme_light ? rgb(200, 215, 220) : rgb(40, 70, 92));
        rect(px + 4, py + 4, pw - 8, 3, accent);
        rect(px + 4, py + ph - 7, pw - 8, 3, accent);
        rect(px + 4, py + 4, 3, ph - 8, accent);
        rect(px + pw - 7, py + 4, 3, ph - 8, accent);
    }

    draw_taskbar(w, h, st);
}

static file_t *selected_file(gui_state_t *st) {
    return gui_selected_regular_file(st);
}

static int gui_select_path(gui_state_t *st, const char *path) {
    uint32_t count = gui_file_count(st);
    for (uint32_t i = 0; i < count; i++) {
        char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
        uint32_t type;
        if (gui_file_entry(st, i, name, &type, 0, full, sizeof(full)) < 0) continue;
        (void)type;
        if (strcmp(full, path) == 0) {
            gui_select_file(st, (int)i);
            return 1;
        }
    }
    return 0;
}

static void gui_select_file(gui_state_t *st, int index) {
    uint32_t count = gui_file_count(st);
    if (count == 0) {
        st->selected_file = 0;
        st->last_clicked_file = -1;
        st->delete_confirm_index = -1;
        return;
    }
    if (index < 0) index = 0;
    if ((uint32_t)index >= count) index = (int)count - 1;
    if (st->selected_file != index) {
        st->rename_active = 0;
        st->delete_confirm_index = -1;
    }
    st->selected_file = index;

    file_t *f = gui_selected_regular_file(st);
    if (f) gui_set_note_name(st, f->name);
}

static void gui_create_note(gui_state_t *st) {
    char base[MAX_FILENAME];
    char full[GUI_PATH_MAX];
    file_t *f = 0;
    uint32_t index = 0;
    for (; index < MAX_FILES; index++) {
        gui_make_note_name(base, sizeof(base), index);
        if (gui_path_join(gui_file_path(st), base, full, sizeof(full)) < 0) continue;
        if (fs_find_file(full)) continue;
        f = fs_create_file(full);
        break;
    }
    if (!f) {
        st->status = "创建失败";
        return;
    }
    const char msg[] = "来自 HBOS 图形桌面的笔记\n";
    if (fs_write_file_data(f, 0, msg, sizeof(msg) - 1) < 0) st->status = "写入失败";
    else {
        st->status = "已创建新笔记";
        uint32_t len = sizeof(msg) - 1;
        if (len >= NOTE_EDIT_CAP) len = NOTE_EDIT_CAP - 1;
        for (uint32_t i = 0; i < len; i++) st->note_buf[i] = msg[i];
        st->note_buf[len] = 0;
        st->note_len = len;
        st->note_cursor = len;
        st->note_dirty = 0;
        st->note_loaded = 1;
        gui_set_note_name(st, f->name);
        st->note_loaded = 1;
    }
    (void)fs_sync();
    (void)gui_select_path(st, f->name);
}

static void gui_append_note(gui_state_t *st) {
    st->delete_confirm_index = -1;
    file_t *f = selected_file(st);
    if (!f) {
        gui_create_note(st);
        return;
    }
    const char msg[] = "从图形桌面追加一行\n";
    if (fs_write_file_data(f, f->size, msg, sizeof(msg) - 1) < 0) st->status = "追加失败";
    else {
        st->status = "已追加内容";
        st->note_loaded = 0;
    }
    (void)fs_sync();
}

static void gui_delete_selected(gui_state_t *st) {
    char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
    uint32_t type = 0;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, full, sizeof(full)) < 0 || !node) {
        st->status = "未选择文件";
        return;
    }
    if (st->delete_confirm_index != st->selected_file) {
        st->delete_confirm_index = st->selected_file;
        st->rename_active = 0;
        st->status = "再次删除确认";
        return;
    }
    if (node->type == VFS_NODE_DIR) {
        if (rmdir(full) < 0) st->status = "删除目录失败";
        else st->status = "目录已删除";
    } else if (node->type == VFS_NODE_FILE) {
        if (fs_delete_file(full) < 0) st->status = "删除失败";
        else st->status = "文件已删除";
    } else {
        st->status = "设备不能删除";
    }
    if (strcmp(full, gui_note_name(st)) == 0) {
        st->note_len = 0;
        st->note_buf[0] = 0;
        st->note_name[0] = 0;
        st->note_loaded = 1;
    }
    if (gui_file_count(st) > 0) gui_select_file(st, st->selected_file > 0 ? st->selected_file - 1 : 0);
    else st->selected_file = 0;
    st->delete_confirm_index = -1;
    (void)fs_sync();
}

static void gui_truncate_selected(gui_state_t *st) {
    st->delete_confirm_index = -1;
    file_t *f = selected_file(st);
    if (!f) {
        st->status = "未选择普通文件";
        return;
    }
    if (fs_truncate_file(f) < 0) {
        st->status = "清空失败";
        return;
    }
    st->note_loaded = 0;
    (void)fs_sync();
    st->status = "文件已清空";
}

static int gui_make_file_variant(char *out, uint32_t cap, const char *base,
                                 const char *suffix) {
    for (uint32_t idx = 0; idx < 100; idx++) {
        uint32_t pos = 0;
        out[0] = 0;
        uint32_t suffix_len = (uint32_t)strlen(suffix);
        uint32_t digits = idx == 0 ? 0 : 1;
        uint32_t v = idx;
        while (v >= 10) { digits++; v /= 10; }
        uint32_t keep = cap > suffix_len + digits + 2 ? cap - suffix_len - digits - 2 : 0;
        for (uint32_t i = 0; base[i] && i < keep && pos + 1 < cap; i++)
            append_char(out, cap, &pos, base[i]);
        append_str(out, cap, &pos, suffix);
        if (idx > 0) {
            append_char(out, cap, &pos, '-');
            append_uint(out, cap, &pos, idx);
        }
        if (!fs_find_file(out)) return 1;
    }
    return 0;
}

static void gui_copy_selected(gui_state_t *st) {
    st->delete_confirm_index = -1;
    char name[VFS_MAX_NAME], src[GUI_PATH_MAX], dst_base[MAX_FILENAME], dst[GUI_PATH_MAX];
    uint32_t type = 0;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, src, sizeof(src)) < 0 || !node) {
        st->status = "未选择文件";
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        st->status = "暂不支持复制目录";
        return;
    }
    if (!gui_make_file_variant(dst_base, sizeof(dst_base), name, "-copy") ||
        gui_path_join(gui_file_path(st), dst_base, dst, sizeof(dst)) < 0) {
        st->status = "无法生成副本名";
        return;
    }
    if (fs_copy_file(src, dst) < 0) {
        st->status = fs_last_error();
        return;
    }
    (void)gui_select_path(st, dst);
    st->status = "文件已复制";
}

static void gui_begin_rename_selected(gui_state_t *st) {
    st->delete_confirm_index = -1;
    char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
    uint32_t type = 0;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, full, sizeof(full)) < 0 || !node) {
        st->status = "未选择文件";
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        st->status = "暂不支持重命名目录";
        return;
    }
    st->rename_len = 0;
    while (name[st->rename_len] && st->rename_len + 1 < sizeof(st->rename_buf)) {
        st->rename_buf[st->rename_len] = name[st->rename_len];
        st->rename_len++;
    }
    st->rename_buf[st->rename_len] = 0;
    st->rename_active = 1;
    st->status = "输入新文件名";
}

static void gui_cancel_rename(gui_state_t *st) {
    st->rename_active = 0;
    st->rename_len = 0;
    st->rename_buf[0] = 0;
    st->status = "已取消重命名";
}

static void gui_cancel_delete_confirm(gui_state_t *st) {
    st->delete_confirm_index = -1;
    st->status = "已取消删除";
}

static void gui_commit_rename(gui_state_t *st) {
    char name[VFS_MAX_NAME], old_full[GUI_PATH_MAX], new_full[GUI_PATH_MAX];
    uint32_t type = 0;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, old_full, sizeof(old_full)) < 0 ||
        !node || !st->rename_buf[0]) {
        gui_cancel_rename(st);
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        st->status = "暂不支持重命名目录";
        st->rename_active = 0;
        return;
    }
    if (gui_path_join(gui_file_path(st), st->rename_buf, new_full, sizeof(new_full)) < 0) {
        st->status = "路径过长";
        return;
    }
    if (fs_rename_file(old_full, new_full) < 0) {
        st->status = fs_last_error();
        return;
    }
    if (strcmp(old_full, gui_note_name(st)) == 0) {
        gui_set_note_name(st, new_full);
        st->note_loaded = 0;
    }
    (void)gui_select_path(st, new_full);
    st->rename_active = 0;
    st->delete_confirm_index = -1;
    st->status = "文件已重命名";
}

static int gui_handle_rename_key(gui_state_t *st, int key) {
    if (!st->rename_active) return 0;
    if (key == 27) {
        gui_cancel_rename(st);
        return 1;
    }
    if (key == '\n') {
        gui_commit_rename(st);
        return 1;
    }
    if (key == GUI_KEY_BACKSPACE) {
        if (st->rename_len > 0) {
            st->rename_len--;
            st->rename_buf[st->rename_len] = 0;
        }
        return 1;
    }
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
        (key >= '0' && key <= '9') || key == '-' || key == '_' || key == '.') {
        if (st->rename_len + 1 < sizeof(st->rename_buf)) {
            st->rename_buf[st->rename_len++] = (char)key;
            st->rename_buf[st->rename_len] = 0;
        }
        return 1;
    }
    st->status = "文件名仅支持字母数字 . _ -";
    return 1;
}

static void gui_install(gui_state_t *st) {
    if (fs_install_disk() < 0) st->status = fs_last_error();
    else st->status = "HBFS 已安装";
}

static const gui_app_meta_t *selected_gui_app(gui_state_t *st) {
    uint32_t count = gui_app_count();
    if (count == 0) {
        st->selected_app = 0;
        return 0;
    }
    if (st->selected_app < 0) st->selected_app = 0;
    if ((uint32_t)st->selected_app >= count) st->selected_app = (int)count - 1;
    return &gui_apps[(uint32_t)st->selected_app];
}

static void gui_open_selected_app(gui_state_t *st) {
    const gui_app_meta_t *app = selected_gui_app(st);
    if (!app) {
        st->status = "未选择应用";
        return;
    }
    if (gui_open_window(st, WM_WIN_APP, app->mode, 0) < 0) return;
    if (app->mode == GUI_APP_SNAKE) {
        snake_reset(st);
    }
    st->status = "应用已打开";
}

static void gui_open_selected_file(gui_state_t *st) {
    char name[VFS_MAX_NAME], full[GUI_PATH_MAX];
    uint32_t type = 0;
    vfs_node_t *node = 0;
    if (gui_selected_entry(st, name, &type, &node, full, sizeof(full)) < 0 || !node) {
        st->status = "未选择文件";
        return;
    }
    if (node->type == VFS_NODE_DIR) {
        gui_set_file_path(st, full);
        st->status = "已进入目录";
        return;
    }
    if (node->type != VFS_NODE_FILE) {
        st->status = "设备节点不能编辑";
        return;
    }
    if (gui_has_code_suffix(full)) {
        code_set_path(st, full);
        if (gui_open_window(st, WM_WIN_APP, GUI_APP_CODE, 0) >= 0)
            st->status = "已用代码工作台打开";
        return;
    }
    gui_set_note_name(st, full);
    if (gui_open_window(st, WM_WIN_APP, GUI_APP_NOTES, 0) >= 0)
        st->status = "已用记事本打开";
}

static int gui_step_selection(int current, uint32_t count, int steps) {
    if (count == 0) return 0;
    current += steps;
    if (current < 0) current = 0;
    if ((uint32_t)current >= count) current = (int)count - 1;
    return current;
}

static void handle_wheel(gui_state_t *st, int dz) {
    if (dz == 0) return;

    int steps = dz > 0 ? -1 : 1;
    if (dz > 1 || dz < -1) steps *= dz > 0 ? dz : -dz;

    gui_sync_focus(st);
    if (st->app_mode == GUI_APP_UWC || (st->app_mode == GUI_APP_NONE && st->active == PANEL_FILES)) {
        gui_select_file(st, gui_step_selection(st->selected_file, gui_file_count(st), steps));
        st->status = "滚轮选择文件";
    } else if (st->app_mode == GUI_APP_NONE && st->active == PANEL_APPS) {
        st->selected_app = gui_step_selection(st->selected_app, gui_app_count(), steps);
        st->status = "滚轮选择应用";
    } else if (st->app_mode == GUI_APP_CODE) {
        code_load(st);
        st->code_scroll += steps;
        code_clamp_scroll(st);
        st->status = "滚动代码";
    }
}

static void handle_action(gui_state_t *st, int action) {
    if (action == 0) gui_create_note(st);
    else if (action == 1) gui_open_selected_file(st);
    else if (action == 2) gui_append_note(st);
    else if (action == 3) gui_truncate_selected(st);
    else if (action == 4) gui_delete_selected(st);
    else if (action == 5) gui_install(st);
    else if (action == 6) gui_open_selected_app(st);
    else if (action == 7) gui_copy_selected(st);
    else if (action == 8) gui_begin_rename_selected(st);
    else if (action == 9) gui_go_parent(st);
}

static int hit_window_titlebar(int w, int h, const gui_state_t *st, int mx, int my) {
    (void)w; (void)h;
    return wm_hit_titlebar((wm_state_t *)&st->wm, mx, my);
}

static int hit_window_close(int w, int h, const gui_state_t *st, int mx, int my) {
    (void)w; (void)h;
    return wm_hit_close((wm_state_t *)&st->wm, mx, my);
}

static int hit_window_minimize(int w, int h, const gui_state_t *st, int mx, int my) {
    (void)w; (void)h;
    return wm_hit_minimize((wm_state_t *)&st->wm, mx, my);
}

static int hit_window_maximize(int w, int h, const gui_state_t *st, int mx, int my) {
    (void)w; (void)h;
    return wm_hit_maximize((wm_state_t *)&st->wm, mx, my);
}

static int hit_action(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return -1;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_PANEL) return -1;
    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    (void)win_h;
    int panel = win->mode;
    if (panel == PANEL_FILES) {
        int content_w = win_w - 60;
        for (int i = 0; i < FILE_ACTION_COUNT; i++) {
            int x, y, bw;
            if (!gui_file_action_rect(content_w, i, &x, &y, &bw)) continue;
            if (mx >= tx + x && mx < tx + x + bw && my >= ty + y && my < ty + y + ACTION_H)
                return gui_file_actions[i].action;
        }
        int side_w = 118;
        int main_x = tx + side_w + 12;
        int detail_w = 184;
        int main_w = content_w - side_w - detail_w - 24;
        if (main_w < 260) {
            detail_w = 152;
            main_w = content_w - side_w - detail_w - 24;
        }
        if (main_w < 220) main_w = 220;
        int list_y = ty + 146;
        if (mx >= main_x && mx < main_x + main_w && my >= list_y - 8 && my < list_y - 8 + FILE_LIST_ROWS * FILE_ROW_H) {
            int selected = st->selected_file;
            if (selected < 0) selected = 0;
            uint32_t start = selected >= FILE_LIST_ROWS ? (uint32_t)selected - (FILE_LIST_ROWS - 1) : 0;
            int idx = (my - (list_y - 8)) / FILE_ROW_H;
            uint32_t file_idx = start + (uint32_t)idx;
            if (idx >= 0 && idx < FILE_LIST_ROWS && file_idx < gui_file_count((gui_state_t *)st))
                return FILE_ACTION_BASE + (int)file_idx;
        }
    } else if (panel == PANEL_DISK) {
        int y = ty + 194;
        if (mx >= tx && mx < tx + ACTION_W && my >= y && my < y + ACTION_H) return 5;
    } else if (panel == PANEL_APPS) {
        int cw = win_w - 60;
        for (uint32_t i = 0; i < gui_app_count(); i++) {
            int x, y, tw, th;
            gui_app_tile_rect(tx, ty, cw, i, &x, &y, &tw, &th);
            if (mx >= x && mx < x + tw && my >= y && my < y + th)
                return APP_ACTION_BASE + (int)i;
        }
    }
    return -1;
}

static int hit_note_file(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return -1;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_NOTES) return -1;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_w;
    (void)win_h;
    int tx = win_x + 30;
    int ty = win_y + 42;
    int list_w = 150;
    int list_y = ty + 112;
    if (mx < tx || mx >= tx + list_w || my < list_y - 8) return -1;

    uint32_t count = gui_file_count((gui_state_t *)st);
    if (count == 0) return -1;
    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count) selected = (int)count - 1;
    uint32_t start = selected >= NOTE_FILE_ROWS ? (uint32_t)selected - (NOTE_FILE_ROWS - 1) : 0;
    int idx = (my - (list_y - 8)) / FILE_ROW_H;
    uint32_t file_idx = start + (uint32_t)idx;
    if (idx >= 0 && idx < NOTE_FILE_ROWS && file_idx < count) return (int)file_idx;
    return -1;
}

static int hit_code_command(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_CODE) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    for (int cmd = CODE_CMD_SAVE; cmd <= CODE_CMD_GUI_RUN; cmd++) {
        int x, y, bw;
        if (!code_command_rect(l.content_w, cmd, &x, &y, &bw)) continue;
        if (mx >= tx + x && mx < tx + x + bw &&
            my >= ty + y && my < ty + y + ACTION_H)
            return cmd;
    }
    return 0;
}

static int hit_code_editor(int w, int h, gui_state_t *st, int mx, int my, uint32_t *off) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window(&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_CODE) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics(st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    if (mx < l.editor_x + l.line_no_w || mx >= l.editor_x + l.editor_w ||
        my < l.editor_y || my >= l.editor_y + l.editor_h)
        return 0;

    int row = (my - (l.editor_y + 10)) / l.row_h;
    if (row < 0) row = 0;
    if (row >= l.view_rows) row = l.view_rows - 1;
    uint32_t line = (uint32_t)(st->code_scroll + row);
    uint32_t total = code_line_count(st);
    if (line >= total) line = total ? total - 1 : 0;
    int col = (mx - (l.editor_x + l.line_no_w + 10)) / code_cell_w();
    if (col < 0) col = 0;
    if (off) *off = code_offset_for_line_col(st, line, (uint32_t)col);
    return 1;
}

static int hit_code_file(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return -1;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_CODE) return -1;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 42;
    code_layout_t l;
    code_make_layout(tx, ty, win_w, win_h, &l);
    int list_y = l.editor_y + 62;
    if (mx < tx || mx >= tx + l.side_w || my < list_y - 8) return -1;

    uint32_t count = gui_file_count((gui_state_t *)st);
    if (count == 0) return -1;
    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count) selected = (int)count - 1;
    uint32_t start = selected >= l.file_rows ? (uint32_t)selected - (uint32_t)(l.file_rows - 1) : 0;
    int idx = (my - (list_y - 8)) / FILE_ROW_H;
    uint32_t file_idx = start + (uint32_t)idx;
    if (idx >= 0 && idx < l.file_rows && file_idx < count) return (int)file_idx;
    return -1;
}

static void snake_move(gui_state_t *st, int dx, int dy) {
    if (st->snake_len <= 0) snake_reset(st);
    if (!st->snake_alive) return;

    if (dx != 0 || dy != 0) {
        if (st->snake_len <= 1 || dx != -st->snake_dx || dy != -st->snake_dy) {
            st->snake_dx = dx;
            st->snake_dy = dy;
        }
    }

    int nx = st->snake_body_x[0] + st->snake_dx;
    int ny = st->snake_body_y[0] + st->snake_dy;
    if (nx < 0 || nx >= SNAKE_W || ny < 0 || ny >= SNAKE_H) {
        st->snake_alive = 0;
        st->status = "贪吃蛇撞墙";
        return;
    }

    int grow = (nx == st->snake_tx && ny == st->snake_ty);
    int check_len = grow ? st->snake_len : st->snake_len - 1;
    for (int i = 0; i < check_len; i++) {
        if (st->snake_body_x[i] == nx && st->snake_body_y[i] == ny) {
            st->snake_alive = 0;
            st->status = "贪吃蛇撞到自己";
            return;
        }
    }

    int new_len = st->snake_len + (grow && st->snake_len < SNAKE_MAX ? 1 : 0);
    for (int i = new_len - 1; i > 0; i--) {
        st->snake_body_x[i] = st->snake_body_x[i - 1];
        st->snake_body_y[i] = st->snake_body_y[i - 1];
    }
    st->snake_body_x[0] = nx;
    st->snake_body_y[0] = ny;
    st->snake_len = new_len;
    st->snake_x = nx;
    st->snake_y = ny;

    if (grow) {
        st->snake_score++;
        snake_place_food(st);
        st->status = "吃到食物";
    } else {
        st->status = "贪吃蛇移动";
    }
}

static int snake_auto_tick(gui_state_t *st) {
    gui_sync_focus(st);
    if (st->app_mode != GUI_APP_SNAKE) return 0;
    if (st->snake_len <= 0) snake_reset(st);
    uint8_t sec = cmos_second();
    if (sec == st->snake_last_sec) return 0;
    st->snake_last_sec = sec;
    snake_move(st, 0, 0);
    return 1;
}

static void handle_app_key(gui_state_t *st, int key) {
    if (key == '\t' && st->app_mode != GUI_APP_CODE) {
        gui_focus_next_window(st, 1);
        if (st->wm.window_count > 1) st->switcher_ticks = 40;
        return;
    }
    gui_sync_focus(st);
    if (gui_app_handle_key(st, key)) return;
    if (st->app_mode == GUI_APP_NOTES) {
        note_load(st);
        if (key == 19) {  // Ctrl+S
            note_save(st);
        } else if (key == GUI_KEY_BACKSPACE) {
            note_backspace(st);
        } else if (key == GUI_KEY_DELETE) {
            note_delete_forward(st);
        } else if (key == GUI_KEY_LEFT) {
            note_cursor_left(st);
        } else if (key == GUI_KEY_RIGHT) {
            note_cursor_right(st);
        } else if (key == GUI_KEY_UP) {
            note_cursor_vertical(st, -1);
        } else if (key == GUI_KEY_DOWN) {
            note_cursor_vertical(st, 1);
        } else if (key == GUI_KEY_HOME) {
            note_cursor_home(st);
        } else if (key == GUI_KEY_END) {
            note_cursor_end(st);
        } else if (key == '\n') {
            note_insert(st, '\n');
        } else if (key == '\t') {
            for (int i = 0; i < 4; i++) note_insert(st, ' ');
        } else if (key >= 32 && key <= 126) {
            note_insert(st, (char)key);
        }
    } else if (st->app_mode == GUI_APP_UWC) {
        if (key == 'n') gui_create_note(st);
        else if (key == GUI_KEY_UP && st->selected_file > 0) gui_select_file(st, st->selected_file - 1);
        else if (key == GUI_KEY_DOWN && (uint32_t)(st->selected_file + 1) < gui_file_count(st)) gui_select_file(st, st->selected_file + 1);
    } else if (st->app_mode == GUI_APP_SNAKE) {
        if (key == '\n') snake_reset(st);
        else if (key == GUI_KEY_LEFT) snake_turn(st, -1, 0);
        else if (key == GUI_KEY_RIGHT) snake_turn(st, 1, 0);
        else if (key == GUI_KEY_UP) snake_turn(st, 0, -1);
        else if (key == GUI_KEY_DOWN) snake_turn(st, 0, 1);
    } else if (st->app_mode == GUI_APP_BROWSER) {
        browser_init(st);
        if (key == '\n') browser_load(st);
        else if (key == 19) browser_save_page(st);
        else if (key == GUI_KEY_BACKSPACE) {
            uint32_t n = (uint32_t)strlen(st->browser_url);
            if (n) st->browser_url[n - 1] = 0;
        } else if (key == GUI_KEY_UP) {
            if (st->browser_scroll > 0) st->browser_scroll--;
        } else if (key == GUI_KEY_DOWN) {
            st->browser_scroll++;
        } else if (key >= 32 && key <= 126) {
            uint32_t n = (uint32_t)strlen(st->browser_url);
            if (n + 1 < BROWSER_URL_CAP) {
                st->browser_url[n] = (char)key;
                st->browser_url[n + 1] = 0;
            }
        }
    } else if (st->app_mode == GUI_APP_CODE) {
        code_load(st);
        if (key == 19) {
            (void)code_save(st);
        } else if (key == 18) {
            code_run_current(st);
        } else if (key == 15) {
            code_open_selected(st);
        } else if (key == GUI_KEY_LEFT) {
            if (st->code_cursor > 0) st->code_cursor--;
            code_ensure_visible(st);
        } else if (key == GUI_KEY_RIGHT) {
            if (st->code_cursor < st->code_len) st->code_cursor++;
            code_ensure_visible(st);
        } else if (key == GUI_KEY_UP) {
            code_move_vertical(st, -1);
        } else if (key == GUI_KEY_DOWN) {
            code_move_vertical(st, 1);
        } else if (key == GUI_KEY_HOME) {
            code_move_line_edge(st, 0);
        } else if (key == GUI_KEY_END) {
            code_move_line_edge(st, 1);
        } else if (key == GUI_KEY_PGUP) {
            code_move_page(st, -1);
        } else if (key == GUI_KEY_PGDOWN) {
            code_move_page(st, 1);
        } else if (key == GUI_KEY_BACKSPACE) {
            code_backspace(st);
        } else if (key == GUI_KEY_DELETE) {
            code_delete_forward(st);
        } else if (key == 3) {
            code_set_output("Use Esc/window close to leave Code Workspace");
            st->status = "代码工作台保持打开";
        } else if (key == '\t') {
            for (int i = 0; i < 4; i++) code_insert_char(st, ' ');
        } else if (key == '\n') {
            code_insert_newline(st);
        } else if (key >= 32 && key <= 126) {
            code_insert_char(st, (char)key);
        }
    } else if (st->app_mode == GUI_APP_DIAG) {
        if (key == GUI_KEY_BACKSPACE) {
            if (st->console_cursor > 0) {
                for (uint32_t j = st->console_cursor - 1; j < st->console_input_len; j++) {
                    st->console_input[j] = st->console_input[j + 1];
                }
                st->console_cursor--;
                st->console_input_len--;
            }
        } else if (key == '\n' || key == '\r') {
            console_exec_cmd(st);
        } else if (key == GUI_KEY_LEFT) {
            if (st->console_cursor > 0) {
                st->console_cursor--;
            }
        } else if (key == GUI_KEY_RIGHT) {
            if (st->console_cursor < st->console_input_len) {
                st->console_cursor++;
            }
        } else if (key == GUI_KEY_UP) {
            int curr = (st->console_history_idx == -1) ? 15 : st->console_history_idx - 1;
            for (int i = curr; i >= 0; i--) {
                if (strncmp(st->console_history[i], "hbos_gui_shell:/# ", 18) == 0) {
                    const char *cmd_val = st->console_history[i] + 18;
                    strncpy(st->console_input, cmd_val, 79);
                    st->console_input[79] = 0;
                    st->console_input_len = (uint32_t)strlen(st->console_input);
                    st->console_cursor = st->console_input_len;
                    st->console_history_idx = i;
                    break;
                }
            }
        } else if (key == GUI_KEY_DOWN) {
            if (st->console_history_idx != -1) {
                int found = 0;
                for (int i = st->console_history_idx + 1; i <= 15; i++) {
                    if (strncmp(st->console_history[i], "hbos_gui_shell:/# ", 18) == 0) {
                        const char *cmd_val = st->console_history[i] + 18;
                        strncpy(st->console_input, cmd_val, 79);
                        st->console_input[79] = 0;
                        st->console_input_len = (uint32_t)strlen(st->console_input);
                        st->console_cursor = st->console_input_len;
                        st->console_history_idx = i;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    st->console_input[0] = 0;
                    st->console_input_len = 0;
                    st->console_cursor = 0;
                    st->console_history_idx = -1;
                }
            }
        } else if (key >= 32 && key <= 126) {
            if (st->console_input_len + 1 < 80) {
                for (uint32_t j = st->console_input_len; j > st->console_cursor; j--) {
                    st->console_input[j] = st->console_input[j - 1];
                }
                st->console_input[st->console_cursor] = (char)key;
                st->console_cursor++;
                st->console_input_len++;
                st->console_input[st->console_input_len] = 0;
            }
        }
    }
}

static void handle_key(gui_state_t *st, int key) {
    gui_sync_focus(st);
    if (gui_handle_rename_key(st, key)) return;
    if (key == '\t' || key == ' ') {
        gui_focus_next_window(st, 1);
        if (st->wm.window_count > 1) st->switcher_ticks = 40;
    } else if (key == GUI_KEY_RIGHT && st->wm.window_count > 0 && st->app_mode == GUI_APP_NONE) {
        wm_window_t *win = gui_active_window(st);
        st->active = (st->active + 1) & 3;
        if (win) win->mode = st->active;
        st->status = "已切换面板";
    } else if (key == GUI_KEY_LEFT && st->wm.window_count > 0 && st->app_mode == GUI_APP_NONE) {
        wm_window_t *win = gui_active_window(st);
        st->active = (st->active + 3) & 3;
        if (win) win->mode = st->active;
        st->status = "已切换面板";
    } else if (key == GUI_KEY_UP && st->active == PANEL_FILES) {
        if (st->selected_file > 0) gui_select_file(st, st->selected_file - 1);
        st->status = "已选择文件";
    } else if (key == GUI_KEY_DOWN && st->active == PANEL_FILES) {
        if ((uint32_t)(st->selected_file + 1) < gui_file_count(st)) gui_select_file(st, st->selected_file + 1);
        st->status = "已选择文件";
    } else if (key == GUI_KEY_HOME && st->active == PANEL_FILES) {
        gui_select_file(st, 0);
        st->status = "已选择文件";
    } else if (key == GUI_KEY_END && st->active == PANEL_FILES) {
        uint32_t count = gui_file_count(st);
        if (count) gui_select_file(st, (int)count - 1);
        st->status = "已选择文件";
    } else if (key == GUI_KEY_PGUP && st->active == PANEL_FILES) {
        gui_select_file(st, gui_step_selection(st->selected_file, gui_file_count(st), -FILE_LIST_ROWS));
        st->status = "已选择文件";
    } else if (key == GUI_KEY_PGDOWN && st->active == PANEL_FILES) {
        gui_select_file(st, gui_step_selection(st->selected_file, gui_file_count(st), FILE_LIST_ROWS));
        st->status = "已选择文件";
    } else if (key == GUI_KEY_UP && st->active == PANEL_APPS) {
        if (st->selected_app > 0) st->selected_app--;
        st->status = "已选择应用";
    } else if (key == GUI_KEY_DOWN && st->active == PANEL_APPS) {
        if ((uint32_t)(st->selected_app + 1) < gui_app_count()) st->selected_app++;
        st->status = "已选择应用";
    } else if (key == GUI_KEY_HOME && st->active == PANEL_APPS) {
        st->selected_app = 0;
        st->status = "已选择应用";
    } else if (key == GUI_KEY_END && st->active == PANEL_APPS) {
        uint32_t count = gui_app_count();
        if (count) st->selected_app = (int)count - 1;
        st->status = "已选择应用";
    } else if (key == GUI_KEY_PGUP && st->active == PANEL_APPS) {
        st->selected_app = gui_step_selection(st->selected_app, gui_app_count(), -4);
        st->status = "已选择应用";
    } else if (key == GUI_KEY_PGDOWN && st->active == PANEL_APPS) {
        st->selected_app = gui_step_selection(st->selected_app, gui_app_count(), 4);
        st->status = "已选择应用";
    } else if (key == 'n') {
        gui_create_note(st);
        gui_open_panel_window(st, PANEL_FILES);
    } else if (key == 'a') {
        gui_append_note(st);
        gui_open_panel_window(st, PANEL_FILES);
    } else if (key == 'p' && st->active == PANEL_FILES) {
        gui_copy_selected(st);
    } else if (key == 'r' && st->active == PANEL_FILES) {
        gui_begin_rename_selected(st);
    } else if (key == 'c') {
        if (st->active == PANEL_FILES) gui_truncate_selected(st);
    } else if (key == 'd') {
        gui_delete_selected(st);
        gui_open_panel_window(st, PANEL_FILES);
    } else if (key == 'i') {
        gui_install(st);
        gui_open_panel_window(st, PANEL_DISK);
    } else if (key == 'r') {
        gui_open_selected_app(st);
    } else if (key == 'o') {
        if (st->active == PANEL_FILES) gui_open_selected_file(st);
    } else if (key == GUI_KEY_BACKSPACE && st->active == PANEL_FILES) {
        gui_go_parent(st);
    } else if (key == '\n') {
        if (st->active == PANEL_FILES) gui_open_selected_file(st);
        else if (st->active == PANEL_DISK) gui_install(st);
        else if (st->active == PANEL_APPS) gui_open_selected_app(st);
    }
}

// Tab 窗口切换器浮层：列出全部窗口并高亮当前焦点
static void draw_window_switcher(int w, int h, gui_state_t *st) {
    if (st->switcher_ticks <= 0) return;
    int n = st->wm.window_count;
    if (n <= 0) return;

    int row_h = 30;
    int pad = 14;
    int ow = 360;
    int oh = pad * 2 + 28 + n * row_h;
    int ox = (w - ow) / 2;
    int oy = (h - oh) / 2;

    soft_shadow(ox, oy, ow, oh);
    draw_panel_shell(ox, oy, ow, oh, cyber_card_bg_top(st->theme_light), cyber_card_bg_bot(st->theme_light),
                     cyber_border(st->theme_light), cyber_neon_cyan(st->theme_light));
    text(ox + pad, oy + pad, "切换窗口  (Tab 循环)", cyber_text(st->theme_light), 1);
    rect(ox + pad, oy + pad + 22, ow - pad * 2, 1, cyber_neon_pink(st->theme_light));

    int ry = oy + pad + 30;
    for (int i = 0; i < n; i++) {
        wm_window_t *win = wm_get_window(&st->wm, i);
        if (!win) continue;
        int active = (i == st->wm.active_window);
        uint32_t accent = win->kind == WM_WIN_PANEL ? rgb(85, 180, 120) : cyber_neon_cyan(st->theme_light);
        if (active) {
            vgradient(ox + pad, ry, ow - pad * 2, row_h - 4,
                      st->theme_light ? rgb(230, 180, 210) : rgb(50, 0, 80),
                      st->theme_light ? rgb(210, 150, 180) : rgb(30, 0, 50));
            rect(ox + pad, ry, 3, row_h - 4, cyber_neon_pink(st->theme_light));
        }
        rect(ox + pad + 12, ry + 9, 10, 10, accent);
        const char *label = gui_window_title(win);
        text_clipped(ox + pad + 32, ry + 7, ox + ow - pad - 60, label,
                     active ? cyber_text(st->theme_light) : cyber_text_muted(st->theme_light), 1);
        if (win->state == WM_STATE_MINIMIZED)
            text(ox + ow - pad - 54, ry + 7, "最小化", cyber_text_muted(st->theme_light), 1);
        ry += row_h;
    }
}

static void draw_start_menu(gui_state_t *st) {
    wm_state_t *wm = &st->wm;
    if (!wm->start_menu_open) return;
    int mx = wm->menu_x, my = wm->menu_y, mw = wm->menu_w, mh = wm->menu_h;

    int light = st->theme_light;
    soft_shadow(mx, my, mw, mh);
    soft_shadow(mx + 2, my + 3, mw - 4, mh - 4);

    // Flat Breeze glass panel + straight border (rounded corners via AA fill).
    fill_round_rect(mx, my, mw, mh, 10, light ? 0xF4F4F5F7 : 0xF41B1F24, RR_ALL);
    uint32_t bd = light ? rgb(206, 210, 214) : rgb(58, 63, 70);
    rect(mx + 10, my, mw - 20, 1, bd);
    rect(mx + 10, my + mh - 1, mw - 20, 1, bd);
    rect(mx, my + 10, 1, mh - 20, bd);
    rect(mx + mw - 1, my + 10, 1, mh - 20, bd);

    // Header.
    uint32_t accent = rgb(61, 174, 233);
    int hy = my + (44 - gui_font_line_height()) / 2;
    text(mx + 16, hy, "HBOS", accent, 1);
    text(mx + 16 + text_width("HBOS ", 1), hy, "工作站",
         light ? rgb(120, 126, 132) : rgb(150, 156, 162), 1);
    rect(mx + 12, my + 43, mw - 24, 1, bd);

    static const char *menu_items[] = {
        "文件管理器", "磁盘管理器", "资源管理器", "应用程序",
        "记事本", "计算器", "贪吃蛇", "浏览器", "代码工作台",
        "控制台终端", "时钟", "返回 Shell", "关机"
    };
    static const uint32_t menu_colors[13] = {
        0xF1C40F, 0xE67E22, 0x9B59B6, 0x3498DB, 0x2ECC71, 0x3498DB,
        0x27AE60, 0x3DAEE9, 0x9B59B6, 0xE67E22, 0xE74C3C, 0x95A5A6,
        0xDA4453
    };
    int count = sizeof(menu_items) / sizeof(menu_items[0]);
    for (int i = 0; i < count; i++) {
        int iy = my + 44 + i * 32;
        if (iy + 32 > my + mh) break;
        // small rounded app-colored icon
        int isz = 22, ix = mx + 12, icy = iy + (32 - isz) / 2;
        fill_round_rect(ix, icy, isz, isz, 6, menu_colors[i] | 0xFF000000, RR_ALL);
        rect(ix + isz / 2 - 3, icy + isz / 2 - 3, 6, 6, rgb(255, 255, 255));
        uint32_t tc = (i == 12) ? rgb(218, 68, 83)                      // 关机 red
                    : (i == 11) ? (light ? rgb(120, 126, 132) : rgb(170, 176, 182))
                                : (light ? rgb(40, 44, 48) : rgb(228, 232, 236));
        text(mx + 44, iy + (32 - gui_font_line_height()) / 2, menu_items[i], tc, 1);
    }
}

static void draw_splash_window(int w, int h, int ticks, int light) {
    int sw = 440, sh = 200;
    int sx = (w - sw) / 2, sy = (h - sh) / 2;
    int title_h = WM_TITLE_H;

    soft_shadow(sx, sy, sw, sh);

    vgradient(sx + 1, sy + 1, sw - 2, title_h, cyber_neon_pink(light), light ? rgb(160, 0, 70) : rgb(180, 0, 90));
    rect(sx, sy, sw, 1, light ? rgb(255, 120, 200) : rgb(255, 100, 180));
    rect(sx, sy + title_h, sw, 1, light ? rgb(180, 185, 192) : rgb(10, 8, 16));
    rect(sx + 1, sy + title_h + 1, sw - 2, sh - title_h - 2, cyber_bg_top(light));
    rect(sx, sy + sh - 1, sw, 1, light ? rgb(180, 185, 190) : rgb(8, 5, 12));
    rect(sx, sy, 1, sh, cyber_neon_pink(light));
    rect(sx + sw - 1, sy, 1, sh, cyber_neon_cyan(light));
    border(sx, sy, sw, sh, cyber_neon_cyan(light));

    text_clipped(sx + 14, sy + 10, sx + sw - 20, "HBOS  v0.1 beta3-pre3", rgb(255, 255, 255), 1);

    int bx = sx + 16, by = sy + title_h + 18;
    vgradient(bx, by, 48, 48, cyber_neon_pink(light), light ? rgb(160, 0, 70) : rgb(180, 0, 90));
    border(bx, by, 48, 48, cyber_neon_cyan(light));
    rect(bx + 6, by + 18, 36, 5, cyber_neon_yellow(light));
    rect(bx + 6, by + 26, 22, 5, cyber_neon_cyan(light));
    rect(bx + 6, by + 34, 30, 5, cyber_neon_pink(light));

    text(sx + 80, sy + title_h + 22, "欢迎使用 HBOS！", cyber_neon_yellow(light), 1);
    text(sx + 80, sy + title_h + 44, "64 位 x86_64 操作系统", cyber_neon_cyan(light), 1);
    text(sx + 80, sy + title_h + 64, "BIOS / UEFI 双启动  协作式多任务", cyber_text(light), 1);

    rect(sx + 16, sy + title_h + 94, sw - 32, 1, cyber_neon_purple(light));
    text(sx + 20, sy + title_h + 106, "help  命令列表      gui  图形界面", cyber_neon_cyan(light), 1);
    text(sx + 20, sy + title_h + 124, "ls / cat  文件      net / http  网络", cyber_neon_cyan(light), 1);

    int bar_w = sw - 32;
    int filled = ticks > 0 ? bar_w * ticks / 90 : 0;
    rect(sx + 16, sy + sh - 22, bar_w, 8, light ? rgb(220, 225, 230) : rgb(10, 8, 16));
    if (filled > 0) {
        hgradient(sx + 16, sy + sh - 22, filled, 8, cyber_neon_cyan(light), light ? rgb(0, 120, 160) : rgb(0, 120, 200));
    }
    border(sx + 16, sy + sh - 22, bar_w, 8, cyber_neon_pink(light));
    text(sx + 20, sy + sh - 12, "点击任意位置关闭", cyber_neon_pink(light), 1);
}

static void cmd_gui(int argc, char **argv) {
    (void)argc;
    (void)argv;

    fb_info_t fb;
    if (fb_get_info(&fb) < 0) {
        console_puts("gui: 需要 framebuffer 模式\n");
        return;
    }

    if (!gui_font_init()) {
        console_puts("gui: GUI 字体加载失败\n");
        return;
    }
    gui_wall_init();   // 壁纸可选，失败则回退纯色背景
    gui_icons_init();  // 扁平图标图集；失败则图标不绘制（不影响其余 UI）

    int w = (int)fb.width;
    int h = (int)fb.height;
    int mx = w / 2;
    int my = h / 2;
    uint8_t last_buttons = 0;
    int dragging_window = -1;
    int drag_off_x = 0;
    int drag_off_y = 0;
    int drag_last_draw_x = 0;
    int drag_last_draw_y = 0;
    int drag_pending = 0;
    int resizing_window = -1;
    int resize_edge = WM_EDGE_NONE;
    int resize_orig_x = 0, resize_orig_y = 0, resize_orig_w = 0, resize_orig_h = 0;
    int resize_orig_win_x = 0, resize_orig_win_y = 0;
    int cursor_edge = WM_EDGE_NONE;
    uint32_t frame_tick = 0;          // 自增帧计数，用于双击计时
    uint32_t last_title_click = 0;    // 上次点击标题栏的帧
    int last_title_idx = -1;          // 上次点击的窗口
    int snap_hint = WM_SNAP_NONE;     // 拖动吸附预览：WM_SNAP_*
    int drag_bounds_valid = 0;
    int drag_last_x = 0, drag_last_y = 0, drag_last_w = 0, drag_last_h = 0;
    int resize_bounds_valid = 0;
    int resize_last_x = 0, resize_last_y = 0, resize_last_w = 0, resize_last_h = 0;
    // Static storage (not a stack local): gui_state_t is large (~14KB with the
    // terminal scrollback) and the GUI is a single non-reentrant instance, so
    // keeping it off the stack avoids overflow — especially now the terminal
    // nests cmd_execute(). memset gives a fresh state on every entry.
    static gui_state_t st;
    memset(&st, 0, sizeof(st));
    st.active = PANEL_FILES;
    st.app_mode = GUI_APP_NONE;
    st.snake_x = 5;
    st.snake_y = 3;
    st.snake_tx = 9;
    st.snake_ty = 5;
    st.last_clicked_file = -1;
    st.delete_confirm_index = -1;
    st.status = "就绪";
    wm_init(&st.wm, w, h);
    st.console_input[0] = 0;
    st.console_input_len = 0;
    st.console_line_count = 0;
    st.console_cursor = 0;
    st.console_history_idx = -1;
    console_append_history(&st, "Welcome to HBOS Cyber Console!");
    console_append_history(&st, "Type 'help' to view available commands.");
    wm_set_panel_title(PANEL_FILES, "文件管理器");
    wm_set_panel_title(PANEL_DISK, "磁盘管理器");
    wm_set_panel_title(PANEL_SYS, "资源管理器");
    wm_set_panel_title(PANEL_APPS, "应用程序");
    wm_set_app_title(GUI_APP_NOTES, "记事本");
    wm_set_app_title(GUI_APP_CALC, "计算器");
    wm_set_app_title(GUI_APP_UWC, "文件统计");
    wm_set_app_title(GUI_APP_SNAKE, "贪吃蛇");
    wm_set_app_title(GUI_APP_BROWSER, "浏览器");
    wm_set_app_title(GUI_APP_CODE, "代码工作台");
    wm_set_app_title(GUI_APP_DIAG, "控制台终端");
    wm_set_app_title(GUI_APP_CLOCK, "时钟");

    (void)block_init();
    if (mouse_init() < 0) st.status = "未检测到鼠标";
    else st.status = mouse_backend_name();
    // Boot to a clean desktop (wallpaper + icons); user opens apps from there.

    uint64_t surface_bytes = (uint64_t)w * (uint64_t)h * sizeof(uint32_t);
    size_t surface_pages = (size_t)((surface_bytes + GUI_PAGE_SIZE - 1) / GUI_PAGE_SIZE);
    uint64_t surface_phys = pmm_alloc_blocks(surface_pages);
    if (surface_phys) {
        gui_set_surface((uint32_t *)(uintptr_t)surface_phys, w, h, (uint32_t)w);
    } else {
        st.status = "图形缓冲分配失败";
    }

    st.splash_ticks = 90;
    gui_dirty_mark_full();
    draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
    while (1) {
        wm_update_animations(&st.wm);

        int key = key_poll();
        /* F2 / F3: shrink / grow the GUI font (global, works in any app). */
        if (key == KB_KEY_F2 || key == KB_KEY_F3) {
            int slot = gui_font_active() + (key == KB_KEY_F3 ? 1 : -1);
            gui_font_set_active(slot);
            st.status = (key == KB_KEY_F3) ? "字体已放大" : "字体已缩小";
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (st.splash_ticks > 0 && key) {
            st.splash_ticks = 0;
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.rename_active) {
            gui_cancel_rename(&st);
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.delete_confirm_index >= 0) {
            gui_cancel_delete_confirm(&st);
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.wm.window_count > 0) {
            gui_close_window(&st, st.wm.active_window);
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 || (key == 'q' && st.app_mode == GUI_APP_NONE)) break;
        if (key) {
            gui_sync_focus(&st);
            if (st.app_mode == GUI_APP_NONE) handle_key(&st, key);
            else handle_app_key(&st, key);
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }

        mouse_event_t ev;
        int acc_dx = 0;
        int acc_dy = 0;
        int acc_dz = 0;
        int saw_mouse = 0;
        int mouse_budget = GUI_MOUSE_POLL_BUDGET;
        while (mouse_budget-- > 0 && mouse_poll(&ev)) {
            saw_mouse = 1;
            acc_dx += ev.dx;
            acc_dy += ev.dy;
            acc_dz += ev.dz;
            st.buttons = ev.buttons;
        }

        if (saw_mouse) {
            /* Direct 1:1 mapping (v0.1-beta3-pre3 feel) — apply the raw per-frame
             * delta immediately, capped. No carry, no gain. */
            acc_dx = clamp_delta(acc_dx);
            acc_dy = clamp_delta(acc_dy);
            int old_mx = mx;
            int old_my = my;
            mx += acc_dx;
            my += acc_dy;
            if (mx < 0) mx = 0;
            if (my < 0) my = 0;
            if (mx > w - 1) mx = w - 1;
            if (my > h - 1) my = h - 1;

            int redraw = 0;
            if (acc_dz != 0) {
                handle_wheel(&st, acc_dz);
                redraw = 1;
            }

            int left_down = (st.buttons & 1) != 0;

            if (st.splash_ticks > 0 && left_down) {
                st.splash_ticks = 0;
                redraw = 1;
            }

            if (st.splash_ticks > 0) goto skip_input;

            if (resizing_window >= 0 && left_down) {
                wm_window_t *rw = wm_get_window(&st.wm, resizing_window);
                if (rw) {
                    int dx = mx - resize_orig_x;
                    int dy = my - resize_orig_y;
                    int new_x = resize_orig_win_x;
                    int new_y = resize_orig_win_y;
                    int new_w = resize_orig_w;
                    int new_h = resize_orig_h;

                    if (resize_edge == WM_EDGE_E || resize_edge == WM_EDGE_NE || resize_edge == WM_EDGE_SE)
                        new_w = resize_orig_w + dx;
                    if (resize_edge == WM_EDGE_W || resize_edge == WM_EDGE_NW || resize_edge == WM_EDGE_SW)
                        new_w = resize_orig_w - dx;
                    if (new_w < 200) new_w = 200;

                    if (resize_edge == WM_EDGE_W || resize_edge == WM_EDGE_NW || resize_edge == WM_EDGE_SW)
                        new_x = (resize_orig_win_x + resize_orig_w) - new_w;

                    if (resize_edge == WM_EDGE_S || resize_edge == WM_EDGE_SE || resize_edge == WM_EDGE_SW)
                        new_h = resize_orig_h + dy;
                    if (resize_edge == WM_EDGE_N || resize_edge == WM_EDGE_NE || resize_edge == WM_EDGE_NW)
                        new_h = resize_orig_h - dy;
                    if (new_h < 120) new_h = 120;

                    if (resize_edge == WM_EDGE_N || resize_edge == WM_EDGE_NE || resize_edge == WM_EDGE_NW)
                        new_y = (resize_orig_win_y + resize_orig_h) - new_h;

                    rw->w = new_w;
                    rw->h = new_h;
                    rw->x = new_x;
                    rw->y = new_y;
                    redraw = 1;
                    st.status = "调整窗口大小";
                }
            } else if (!left_down) {
                resizing_window = -1;
                resize_bounds_valid = 0;
            }

            if (dragging_window >= 0 && left_down && resizing_window < 0) {
                int win_x, win_y, win_w, win_h;
                gui_focus_window(&st, dragging_window);
                gui_window_metrics(&st, w, h, NULL, dragging_window, &win_x, &win_y, &win_w, &win_h);
                gui_set_active_window_pos(&st, mx - drag_off_x, my - drag_off_y);
                clamp_window(&st, w, h, win_w, win_h);
                gui_store_focus(&st);
                // 根据指针靠近的屏幕边缘预测吸附目标
                int prev_hint = snap_hint;
                if (my <= 4) snap_hint = WM_SNAP_TOP;
                else if (mx <= 4) snap_hint = WM_SNAP_LEFT;
                else if (mx >= w - 5) snap_hint = WM_SNAP_RIGHT;
                else snap_hint = WM_SNAP_NONE;
                st.snap_preview = snap_hint;
                st.status = snap_hint != WM_SNAP_NONE ? "松开吸附窗口" : "窗口已移动";
                drag_pending = 1;
                int moved = mx - drag_last_draw_x;
                if (moved < 0) moved = -moved;
                int moved_y = my - drag_last_draw_y;
                if (moved_y < 0) moved_y = -moved_y;
                if (moved + moved_y >= 6 || snap_hint != prev_hint) {
                    drag_last_draw_x = mx;
                    drag_last_draw_y = my;
                    redraw = 1;
                    drag_pending = 0;
                }
            } else if (!left_down) {
                if (dragging_window >= 0) {
                    if (snap_hint != WM_SNAP_NONE) {
                        wm_snap_window(&st.wm, dragging_window, snap_hint);
                        gui_sync_focus(&st);
                        st.status = "窗口已吸附";
                    }
                    if (drag_pending || snap_hint != WM_SNAP_NONE) redraw = 1;
                }
                snap_hint = WM_SNAP_NONE;
                st.snap_preview = WM_SNAP_NONE;
                dragging_window = -1;
                drag_pending = 0;
                drag_bounds_valid = 0;
            }

            int edge = WM_EDGE_NONE;
            wm_hit_border(&st.wm, mx, my, &edge);
            cursor_edge = edge;

            if ((st.buttons & 1) && !(last_buttons & 1)) {
                st.clicks++;
                if (st.wm.start_menu_open) {
                    int item = wm_hit_start_menu(&st.wm, mx, my);
                    if (item >= 0) {
                        wm_close_start_menu(&st.wm);
                        if (item == 0) gui_open_panel_window(&st, PANEL_FILES);
                        else if (item == 1) gui_open_panel_window(&st, PANEL_DISK);
                        else if (item == 2) gui_open_panel_window(&st, PANEL_SYS);
                        else if (item == 3) gui_open_panel_window(&st, PANEL_APPS);
                        else if (item == 4) gui_open_window(&st, WM_WIN_APP, GUI_APP_NOTES, 0);
                        else if (item == 5) gui_open_window(&st, WM_WIN_APP, GUI_APP_CALC, 0);
                        else if (item == 6) gui_open_window(&st, WM_WIN_APP, GUI_APP_SNAKE, 0);
                        else if (item == 7) gui_open_window(&st, WM_WIN_APP, GUI_APP_BROWSER, 0);
                        else if (item == 8) gui_open_window(&st, WM_WIN_APP, GUI_APP_CODE, 0);
                        else if (item == 9) gui_open_window(&st, WM_WIN_APP, GUI_APP_DIAG, 0);
                        else if (item == 10) gui_open_window(&st, WM_WIN_APP, GUI_APP_CLOCK, 0);
                        else if (item == 11) break;
                        else if (item == 12) { acpi_poweroff(); break; }
                        redraw = 1;
                    } else {
                        wm_close_start_menu(&st.wm);
                        redraw = 1;
                    }
                } else {
                    int close_idx = hit_window_close(w, h, &st, mx, my);
                    int min_idx = hit_window_minimize(w, h, &st, mx, my);
                    int max_idx = hit_window_maximize(w, h, &st, mx, my);

                    if (close_idx >= 0) {
                        gui_close_window(&st, close_idx);
                    } else if (min_idx >= 0) {
                        wm_minimize_window(&st.wm, min_idx);
                        gui_sync_focus(&st);
                        st.status = "窗口已最小化";
                    } else if (max_idx >= 0) {
                        wm_window_t *mw = wm_get_window(&st.wm, max_idx);
                        if (mw && mw->state == WM_STATE_MAXIMIZED)
                            wm_restore_window(&st.wm, max_idx);
                        else
                            wm_maximize_window(&st.wm, max_idx);
                        gui_sync_focus(&st);
                        st.status = "窗口已最大化/还原";
                    } else if (edge != WM_EDGE_NONE) {
                        int bidx = wm_hit_border(&st.wm, mx, my, &resize_edge);
                        if (bidx >= 0) {
                            wm_focus_window(&st.wm, bidx);
                            wm_window_t *bw = wm_get_window(&st.wm, bidx);
                            if (bw) {
                                resizing_window = bidx;
                                resize_orig_x = mx;
                                resize_orig_y = my;
                                resize_orig_w = bw->w > 0 ? bw->w : (w > 920 ? 790 : w - 120);
                                resize_orig_h = bw->h > 0 ? bw->h : (h > 620 ? 430 : h - 140);
                                resize_orig_win_x = bw->x;
                                resize_orig_win_y = bw->y;
                            }
                        }
                    } else {
                        int tb = taskbar_hit(w, h, &st, mx, my);
                        if (tb == TBHIT_START) {
                            wm_toggle_start_menu(&st.wm);
                            if (st.wm.start_menu_open) {
                                // Anchor the menu above the centered Start button.
                                int wins[WM_MAX_WINDOWS];
                                int n = taskbar_windows(&st, wins, WM_MAX_WINDOWS);
                                int bx, by;
                                taskbar_item_xy(0, 1 + TB_PIN_COUNT + n, w, h, &bx, &by);
                                st.wm.menu_x = bx;
                                if (st.wm.menu_x + st.wm.menu_w > w - 8)
                                    st.wm.menu_x = w - 8 - st.wm.menu_w;
                                if (st.wm.menu_x < 8) st.wm.menu_x = 8;
                                st.wm.menu_y = h - TASKBAR_H - st.wm.menu_h - 8;
                                if (st.wm.menu_y < 8) st.wm.menu_y = 8;
                            }
                            st.status = "开始菜单";
                        } else if (tb >= TBHIT_WIN_BASE) {
                            int slot = tb - TBHIT_WIN_BASE;
                            wm_window_t *tw = wm_get_window(&st.wm, slot);
                            if (tw && tw->state == WM_STATE_MINIMIZED) {
                                wm_restore_window(&st.wm, slot);
                            }
                            gui_focus_window(&st, slot);
                            st.status = "已切换窗口";
                        } else if (tb >= 0 && tb < TB_PIN_COUNT) {
                            gui_open_panel_window(&st, g_taskbar_pins[tb].panel);
                        } else {
                            int title_idx = hit_window_titlebar(w, h, &st, mx, my);
                            if (title_idx >= 0) {
                                gui_focus_window(&st, title_idx);
                                // 双击标题栏：最大化/还原（同一窗口、约 30 帧内）
                                if (last_title_idx == title_idx &&
                                    frame_tick - last_title_click < 40) {
                                    wm_toggle_maximize(&st.wm, title_idx);
                                    gui_sync_focus(&st);
                                    last_title_idx = -1;
                                    st.status = "窗口已最大化/还原";
                                } else {
                                    int win_x, win_y, win_w, win_h;
                                    gui_window_metrics(&st, w, h, NULL, title_idx, &win_x, &win_y, &win_w, &win_h);
                                    dragging_window = title_idx;
                                    drag_off_x = mx - win_x;
                                    drag_off_y = my - win_y;
                                    drag_last_draw_x = mx;
                                    drag_last_draw_y = my;
                                    drag_pending = 0;
                                    snap_hint = WM_SNAP_NONE;
                                    last_title_idx = title_idx;
                                    last_title_click = frame_tick;
                                    st.status = "拖动窗口";
                                }
                            } else if (st.app_mode != GUI_APP_NONE) {
                                int code_cmd = hit_code_command(w, h, &st, mx, my);
                                if (code_cmd == CODE_CMD_GUI_RUN) {
                                    code_gui_run(&st, &fb);
                                } else if (code_cmd) {
                                    handle_code_command(&st, code_cmd);
                                } else {
                                    int note_file = hit_note_file(w, h, &st, mx, my);
                                    if (note_file >= 0) {
                                        gui_select_file(&st, note_file);
                                        st.status = "已切换编辑文件";
                                    } else {
                                        int code_file = hit_code_file(w, h, &st, mx, my);
                                        if (code_file >= 0) {
                                            if (st.last_clicked_file == code_file && st.selected_file == code_file) {
                                                code_open_selected(&st);
                                                st.last_clicked_file = -1;
                                            } else {
                                                gui_select_file(&st, code_file);
                                                st.last_clicked_file = code_file;
                                                st.status = "已选择代码文件";
                                            }
                                        } else {
                                            uint32_t code_off = 0;
                                            if (hit_code_editor(w, h, &st, mx, my, &code_off)) {
                                                st.code_cursor = code_off;
                                                code_ensure_visible(&st);
                                                st.status = "已移动代码光标";
                                            } else {
                                                st.status = "应用已聚焦";
                                            }
                                        }
                                    }
                                }
                            } else {
                                int dico = hit_desktop_icon(mx, my);
                                if (dico >= 0) {
                                    const desktop_icon_t *d = &g_desktop_icons[dico];
                                    if (d->kind == WM_WIN_PANEL)
                                        gui_open_panel_window(&st, d->mode);
                                    else
                                        gui_open_window(&st, WM_WIN_APP, d->mode, 0);
                                } else {
                                    int action = hit_action(w, h, &st, mx, my);
                                    if (action >= FILE_ACTION_BASE) {
                                        if (action >= APP_ACTION_BASE) {
                                            st.selected_app = action - APP_ACTION_BASE;
                                            gui_open_selected_app(&st);
                                        } else {
                                            int file_index = action - FILE_ACTION_BASE;
                                            if (st.last_clicked_file == file_index && st.selected_file == file_index) {
                                                gui_open_selected_file(&st);
                                                st.last_clicked_file = -1;
                                            } else {
                                                gui_select_file(&st, file_index);
                                                st.last_clicked_file = file_index;
                                                st.status = "已选择文件";
                                            }
                                        }
                                    } else if (action >= 0) {
                                        handle_action(&st, action);
                                    }
                                }
                            }
                        }
                    }
                }
                redraw = 1;
            }
            last_buttons = st.buttons;
            skip_input:;

            if (redraw) {
                gui_dirty_reset();
                if (dragging_window >= 0) {
                    if (drag_bounds_valid)
                        gui_damage_region(drag_last_x, drag_last_y, drag_last_w, drag_last_h, w, h);
                    gui_damage_window(&st, w, h, dragging_window);
                    if (st.snap_preview != WM_SNAP_NONE) {
                        int px = 0, py = 0, pw = w, ph = h - TASKBAR_H;
                        if (st.snap_preview == WM_SNAP_LEFT) pw = w / 2;
                        else if (st.snap_preview == WM_SNAP_RIGHT) { px = w / 2; pw = w - w / 2; }
                        gui_damage_region(px, py, pw, ph, w, h);
                    }
                    gui_window_metrics(&st, w, h, NULL, dragging_window,
                                       &drag_last_x, &drag_last_y, &drag_last_w, &drag_last_h);
                    gui_dirty_expand(&drag_last_x, &drag_last_y, &drag_last_w, &drag_last_h,
                                     GUI_DIRTY_PAD, w, h);
                    drag_bounds_valid = 1;
                } else if (resizing_window >= 0) {
                    if (resize_bounds_valid)
                        gui_damage_region(resize_last_x, resize_last_y, resize_last_w, resize_last_h, w, h);
                    gui_damage_window(&st, w, h, resizing_window);
                    gui_window_metrics(&st, w, h, NULL, resizing_window,
                                       &resize_last_x, &resize_last_y, &resize_last_w, &resize_last_h);
                    gui_dirty_expand(&resize_last_x, &resize_last_y, &resize_last_w, &resize_last_h,
                                     GUI_DIRTY_PAD, w, h);
                    resize_bounds_valid = 1;
                } else {
                    drag_bounds_valid = 0;
                    resize_bounds_valid = 0;
                    gui_dirty_mark_full();
                }
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            } else if (mx != old_mx || my != old_my) {
                gui_dirty_reset();
                gui_damage_cursor(old_mx, old_my, mx, my);
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            }
        }
        if (snake_auto_tick(&st)) {
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (gui_app_tick(&st)) {
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.app_mode == GUI_APP_DIAG && (frame_tick % 20 == 0)) {
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.switcher_ticks > 0) {
            st.switcher_ticks--;
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.splash_ticks > 0) {
            st.splash_ticks--;
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (wm_has_active_animations(&st.wm)) {
            gui_dirty_reset();
            for (int i = 0; i < st.wm.window_count; i++) {
                wm_window_t *win = wm_get_window(&st.wm, i);
                if (win && win->anim_type != WM_ANIM_NONE)
                    gui_damage_window(&st, w, h, i);
            }
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        frame_tick++;
        task_yield();
        usleep(15000); // Throttle GUI loop to ~60 FPS to prevent 100% CPU busy-looping
    }

    gui_set_surface(0, 0, 0, 0);
    if (surface_phys) pmm_free_blocks(surface_phys, surface_pages);
    mouse_shutdown();
    console_reset_terminal();
    console_puts("gui: 已返回 shell\n");
}

void tool_gui_init(void) {
    static const command_t cmds[] = {
        {"gui", CMD_GROUP_GRAPHICS, "Start graphical control panel", "gui", cmd_gui},
        {"startx", CMD_GROUP_GRAPHICS, "Start graphical control panel", "startx", cmd_gui},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}

uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b) { return rgb(r, g, b); }
void gui_rect(int x, int y, int w, int h, uint32_t color) { rect(x, y, w, h, color); }
void gui_rect_alpha(int x, int y, int w, int h, uint32_t color) { rect_alpha(x, y, w, h, color); }
void gui_border(int x, int y, int w, int h, uint32_t color) { border(x, y, w, h, color); }
void gui_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) { vgradient(x, y, w, h, top, bottom); }
void gui_soft_shadow(int x, int y, int w, int h) { soft_shadow(x, y, w, h); }
void gui_draw_panel_shell(int x, int y, int w, int h, uint32_t top, uint32_t bottom,
                          uint32_t border_c, uint32_t accent) {
    draw_panel_shell(x, y, w, h, top, bottom, border_c, accent);
}
void gui_text(int x, int y, const char *s, uint32_t color, int scale) { text(x, y, s, color, scale); }
void gui_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale) {
    text_clipped(x, y, max_x, s, color, scale);
}
void gui_append_char(char *buf, uint32_t cap, uint32_t *pos, char c) { append_char(buf, cap, pos, c); }
void gui_append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s) { append_str(buf, cap, pos, s); }
void gui_append_int(char *buf, uint32_t cap, uint32_t *pos, int v) { append_int(buf, cap, pos, v); }
void gui_append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v) { append_uint(buf, cap, pos, v); }
