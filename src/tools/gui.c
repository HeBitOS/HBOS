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
#include "../user/hax_app.h"
#include "../version.h"
#include "../vfs.h"
#include "../gui/wm.h"
#include "../gui/winsrv.h"
#include "../gui/gui_state.h"
#include "../gui/gui_dirty.h"
#include "../gui/gui_draw.h"
#include "../gui/gui_app.h"

/* app_imgview.c / app_hexview.c 各自路径 setter，供 gui_open_selected_file()
 * 打开对应查看器前写入要打开的文件路径 */
void app_imgview_set_path(gui_state_t *st, const char *path);
void app_hexview_set_path(gui_state_t *st, const char *path);
#include "../gui/rtc_tz.h"
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

/* ── DPI 缩放（g_ui_scale 在 GUI 启动时按分辨率设置；声明在 wm.h） ── */
int g_ui_scale = 100;
int ui_s(int v) { return v * g_ui_scale / 100; }

#define ACTION_W ui_s(116)
#define ACTION_H ui_s(28)
#define FILE_ACTION_COUNT 7
#define GUI_MOUSE_POLL_BUDGET 64   /* drain the queue fully so motion doesn't
                                      back up into bursty "忽快忽慢" jumps */
#define TASKBAR_H ui_s(50)
/* NOTE_EDIT_CAP/BROWSER_URL_CAP/BROWSER_PAGE_CAP/CODE_EDIT_CAP 都在
 * gui_state.h（上面已 include）——这里以前有一份重复定义，用的是旧的
 * 更小的值，会把 gui_state.h 的实际值静默盖掉（宏后定义生效），不要
 * 再在这里重复定义。 */
#define CODE_OUTPUT_CAP 256
#define CODE_VIEW_ROWS 10
#define SNAKE_W 16
#define SNAKE_H 10
#define GUI_PAGE_SIZE 4096ULL
#define FILE_LIST_ROWS 8
#define FILE_ROW_H 30
#define NOTE_FILE_ROWS 7
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
    {"设置", "主题与字体", GUI_APP_SETTINGS},
    {"任务管理器", "查看运行中的任务，结束任务", GUI_APP_TASKMGR},
    {"快捷键", "查看全局和各应用的键盘快捷键", GUI_APP_SHORTCUTS},
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
static void draw_context_menu(gui_state_t *st);
static void draw_calendar_popup(int w, int h, gui_state_t *st);
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

#define TOAST_TICKS 130   /* toast 显示帧数（约 2 秒 @ ~60fps 上限） */

/* 弹出一条 toast 通知（msg = a + b 拼接，无外部 helper 依赖） */
static void gui_toast(gui_state_t *st, const char *a, const char *b) {
    uint32_t n = 0;
    while (a && *a && n < sizeof(st->toast_msg) - 1) st->toast_msg[n++] = *a++;
    while (b && *b && n < sizeof(st->toast_msg) - 1) st->toast_msg[n++] = *b++;
    st->toast_msg[n] = 0;
    st->toast_ticks = TOAST_TICKS;
}

static int gui_open_window(gui_state_t *st, int kind, int mode, int unique) {
    int idx = wm_open_window(&st->wm, kind, mode, unique);
    if (idx >= 0) {
        gui_sync_focus(st);
        const char *t = wm_window_title(wm_get_window(&st->wm, idx));
        gui_toast(st, "已打开 ", t ? t : "窗口");
    } else {
        st->status = "窗口数量已满";
        gui_toast(st, "窗口数量已满", "");
    }
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
static void draw_toast(int w, int h, gui_state_t *st);

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
/* 假斜体：没有真正的斜体字形数据，靠 blit_glyph 逐行加一点递减的水平位移
 * 模拟倾斜（顶部偏右、底部不动），跟粗体的"描边两次"是同一类"没有字形
 * 数据就用像素技巧凑"的思路。只在 <em>/<i> 或 font-style:italic 命中时
 * 临时置 1，画完立刻复位，不影响其余所有 text() 调用者。 */
static int g_text_italic = 0;

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

// 屏幕亮度：没有真实背光可调，用"呈现到硬件帧缓冲时按比例压暗每个像素的
// RGB 通道"模拟。20..100，floor 在 20 避免完全变黑（那样用户没法再把它调回来）。
static int g_brightness = 100;

void gui_set_brightness(int pct) {
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    g_brightness = pct;
}

int gui_get_brightness(void) { return g_brightness; }

static inline uint32_t apply_brightness_px(uint32_t px) {
    if (g_brightness >= 100) return px;
    uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
    r = r * (uint32_t)g_brightness / 100;
    g = g * (uint32_t)g_brightness / 100;
    b = b * (uint32_t)g_brightness / 100;
    return (px & 0xFF000000) | (r << 16) | (g << 8) | b;
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
        for (int x = 0; x < g_gui_surface_w; x++) dst[x] = apply_brightness_px(src[x]);
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
        for (int xx = 0; xx < w; xx++) dst[xx] = apply_brightness_px(src[xx]);
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
        int italic_max = (g_text_italic && dh > 1) ? scale : 0;
        for (int yy = 0; yy < ch; yy++) {
            int sy = (off_y + yy) / scale;
            const uint8_t *cov_row = g->coverage + (uint32_t)sy * width;
            uint32_t *row = g_gui_surface +
                            (uint32_t)(cy + yy) * g_gui_surface_pitch + (uint32_t)cx;
            /* 顶部行（yy 小）位移最大，底部行位移为 0——字形看起来往右上方倾斜。 */
            int shift = italic_max ? (int)((int64_t)(dh - 1 - (off_y + yy)) * italic_max / (dh - 1)) : 0;
            for (int xx = 0; xx < cw; xx++) {
                uint32_t cov = smooth
                    ? cov_bilinear(g->coverage, width, g->height,
                                   off_x + xx, off_y + yy, scale)
                    : cov_row[(off_x + xx) / scale];
                if (cov == 0) continue;
                uint32_t a = cov * fg_a / 255;
                if (a == 0) continue;
                int dxx = xx + shift;
                if (dxx < 0 || cx + dxx >= g_gui_surface_w) continue;
                uint32_t dst = row[dxx];
                uint32_t inv = 255 - a;
                uint32_t dr = (dst >> 16) & 0xFF;
                uint32_t dg = (dst >> 8) & 0xFF;
                uint32_t db = dst & 0xFF;
                uint32_t out_r = (src_r * a + dr * inv) / 255;
                uint32_t out_g = (src_g * a + dg * inv) / 255;
                uint32_t out_b = (src_b * a + db * inv) / 255;
                row[dxx] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
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
        case GUI_APP_CLOCK:    return ICON_CLOCK;
        case GUI_APP_SETTINGS: return ICON_SYS;
        case GUI_APP_FILES:    return ICON_FILES;
        case GUI_APP_TASKMGR:  return ICON_SYS;
        case GUI_APP_SHORTCUTS: return ICON_SHORTCUTS;
        default:               return ICON_APPS;
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

int gui_text_width(const char *s, int scale) { return text_width(s, scale); }

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

static void append_uint64(char *buf, uint32_t cap, uint32_t *pos, unsigned long long v) {
    char tmp[24];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));
    while (n) append_char(buf, cap, pos, tmp[--n]);
}

static void append_ll(char *buf, uint32_t cap, uint32_t *pos, long long v) {
    /* -v on LLONG_MIN overflows signed long long -- negate in unsigned
     * space instead, which wraps around to the correct magnitude. */
    if (v < 0) {
        append_char(buf, cap, pos, '-');
        append_uint64(buf, cap, pos, 0ULL - (unsigned long long)v);
    } else {
        append_uint64(buf, cap, pos, (unsigned long long)v);
    }
}

/* Scientific-notation formatting for calculator results that overflow
 * long long (see app_calc.c's calc_to_sci) -- pure integer arithmetic
 * only, no float/double, since the kernel is built with SSE/x87 disabled
 * (see Makefile's CFLAGS: -mno-80387 -mno-mmx -mno-sse -mno-sse2). mant is
 * a <=9-significant-digit integer (any sign); the true value is
 * approximately mant * 10^exp. Renders as "D.DDDDDDDDe+NN". */
static void append_sci(char *buf, uint32_t cap, uint32_t *pos, long long mant, int exp) {
    if (mant < 0) append_char(buf, cap, pos, '-');
    unsigned long long m = mant < 0 ? (0ULL - (unsigned long long)mant) : (unsigned long long)mant;
    char digits[20];
    int nd = 0;
    if (m == 0) digits[nd++] = '0';
    while (m) { digits[nd++] = (char)('0' + (m % 10)); m /= 10; }
    int total_exp = exp + (nd - 1);
    append_char(buf, cap, pos, digits[nd - 1]);
    append_char(buf, cap, pos, '.');
    if (nd > 1) {
        for (int i = nd - 2; i >= 0; i--) append_char(buf, cap, pos, digits[i]);
    } else {
        append_char(buf, cap, pos, '0');
    }
    append_char(buf, cap, pos, 'e');
    append_char(buf, cap, pos, total_exp < 0 ? '-' : '+');
    int e = total_exp < 0 ? -total_exp : total_exp;
    if (e < 10) append_char(buf, cap, pos, '0');
    append_uint(buf, cap, pos, (uint32_t)e);
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
    st->note_sel_active = 0;
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

static void time_line(char *buf, uint32_t cap, int show_seconds) {
    uint8_t hour, min, sec, day, month, wday;
    uint32_t year;
    rtc_tz_read_local(&hour, &min, &sec, &day, &month, &year, &wday);
    (void)day; (void)month; (void)year; (void)wday;
    uint32_t pos = 0;
    buf[0] = 0;
    if (hour < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, hour);
    append_char(buf, cap, &pos, ':');
    if (min < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, min);
    if (show_seconds) {
        append_char(buf, cap, &pos, ':');
        if (sec < 10) append_char(buf, cap, &pos, '0');
        append_uint(buf, cap, &pos, sec);
    }
}

/* 日期串 "YYYY/MM/DD 周X"（Win11 任务栏第二行） */
static void date_line(char *buf, uint32_t cap) {
    uint8_t hour, min, sec, day, month, wday;
    uint32_t year;
    rtc_tz_read_local(&hour, &min, &sec, &day, &month, &year, &wday);
    (void)hour; (void)min; (void)sec;
    static const char *const WD[8] = {"", "日", "一", "二", "三", "四", "五", "六"};
    uint32_t pos = 0;
    buf[0] = 0;
    append_uint(buf, cap, &pos, year);
    append_char(buf, cap, &pos, '/');
    if (month < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, month);
    append_char(buf, cap, &pos, '/');
    if (day < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, day);
    if (wday >= 1 && wday <= 7) {
        append_char(buf, cap, &pos, ' ');
        append_str(buf, cap, &pos, "周");
        append_str(buf, cap, &pos, WD[wday]);
    }
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

// Integer pointer mapping with a light acceleration curve, applied IMMEDIATELY
// (no sub-pixel carry — carry made short nudges feel numb). Small moves get a
// higher base gain so short-distance motion actually travels; larger flicks
// accelerate so crossing the screen stays fast.
//
// Three tiers instead of two: each step up in gain (4 -> 5 -> 7 px/count) is
// smaller than a single 3 -> 5 jump would be, so crossing a tier boundary
// during a flick doesn't feel like a sudden gear-change. Every tier's formula
// is exact at its lower bound (no leftover rounding carried from a prior
// division), which avoids the +/-1px jitter a single post-hoc "* 7/5" scale
// would add as `a` grows.
static int clamp_delta(int v) {
    int s = v < 0 ? -1 : 1;
    int a = v < 0 ? -v : v;
    if (a == 0) return 0;
    int out;
    if (a <= 3)       out = a * 4;               // 精细控制：每计数 4px
    else if (a <= 8)  out = 16 + (a - 4) * 5;     // 过渡：每计数 5px
    else              out = 36 + (a - 8) * 7;     // 快速甩动：每计数 7px，便于跨屏
    out *= s;
    if (out > 260)  return 260;
    if (out < -260) return -260;
    return out;
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

// Cursor glyphs are simple closed polygons rasterized with a generic
// even-odd point-in-polygon test; the outline is whatever fill pixel has a
// non-fill neighbor. This handles both the concave pointer notch and the
// "dumbbell" resize double-arrows (two chevrons joined by a thin bar)
// without a separate rasterizer per shape.
static int polygon_point_inside(const int *vx, const int *vy, int n, int px, int py) {
    int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        int xi = vx[i], yi = vy[i];
        int xj = vx[j], yj = vy[j];
        if ((yi > py) != (yj > py)) {
            int xcross = xi + (xj - xi) * (py - yi) / (yj - yi);
            if (px < xcross) inside = !inside;
        }
    }
    return inside;
}

// 0 = outside (transparent, leaves the desktop showing through),
// 1 = interior fill, 2 = outline (borders the outside on any of 4 sides).
static int polygon_pixel_kind(const int *vx, const int *vy, int n, int bw, int bh, int px, int py) {
    if (!polygon_point_inside(vx, vy, n, px, py)) return 0;
    if (px <= 0 || py <= 0 || px >= bw - 1 || py >= bh - 1 ||
        !polygon_point_inside(vx, vy, n, px - 1, py) || !polygon_point_inside(vx, vy, n, px + 1, py) ||
        !polygon_point_inside(vx, vy, n, px, py - 1) || !polygon_point_inside(vx, vy, n, px, py + 1))
        return 2;
    return 1;
}

// Pointer: tip -> shoulder -> notch (reflex) -> tail tip -> back to tip.
#define CURSOR_ARROW_W 16   /* bounding box columns 0..15 */
#define CURSOR_ARROW_H 23   /* bounding box rows    0..22 */
static const int CURSOR_VX[4] = { 0, 15, 7, 0 };
static const int CURSOR_VY[4] = { 0, 18, 17, 22 };

// Resize double-arrows: two chevrons pointing away from center, joined by a
// thin bar. Vertical/diagonal variants are the same "dumbbell" shape rotated
// 90°/45°, with the vertex coordinates precomputed (no runtime float/trig).
#define CURSOR_RESIZE_H_W 24
#define CURSOR_RESIZE_H_H 13
static const int CURSOR_RESIZE_H_VX[10] = { 0, 7, 7, 17, 17, 23, 17, 17, 7, 7 };
static const int CURSOR_RESIZE_H_VY[10] = { 6, 0, 5, 5, 0, 6, 12, 7, 7, 12 };

#define CURSOR_RESIZE_V_W 13
#define CURSOR_RESIZE_V_H 24
static const int CURSOR_RESIZE_V_VX[10] = { 6, 0, 5, 5, 0, 6, 12, 7, 7, 12 };
static const int CURSOR_RESIZE_V_VY[10] = { 0, 7, 7, 17, 17, 23, 17, 17, 7, 7 };

// NW<->SE diagonal (top-left / bottom-right).
#define CURSOR_RESIZE_NWSE_W 19
#define CURSOR_RESIZE_NWSE_H 19
static const int CURSOR_RESIZE_NWSE_VX[10] = { 1, 10, 6, 13, 17, 17, 8, 12, 5, 1 };
static const int CURSOR_RESIZE_NWSE_VY[10] = { 1, 1, 5, 12, 8, 17, 17, 13, 6, 10 };

// NE<->SW diagonal (top-right / bottom-left).
#define CURSOR_RESIZE_NESW_W 19
#define CURSOR_RESIZE_NESW_H 19
static const int CURSOR_RESIZE_NESW_VX[10] = { 1, 1, 5, 12, 8, 17, 17, 13, 6, 10 };
static const int CURSOR_RESIZE_NESW_VY[10] = { 17, 8, 12, 5, 1, 1, 10, 6, 13, 17 };

// I-beam 文本光标：上下两条横杠 + 中间细竖杠（工字形），悬停在记事本/代码
// 工作台的文本框内时使用。#define 而非 WM_EDGE_* 值，避免跟窗口缩放边缘冲突。
#define CURSOR_HOVER_TEXT -50
#define CURSOR_TEXT_W 14
#define CURSOR_TEXT_H 22
static const int CURSOR_TEXT_VX[12] = { 0, 14, 14,  9,  9, 14, 14, 0, 0, 5, 5, 0 };
static const int CURSOR_TEXT_VY[12] = { 0,  0,  4,  4, 18, 18, 22, 22, 18, 18, 4, 4 };

static void draw_polygon_cursor(int x, int y, const int *vx, const int *vy, int n, int bw, int bh) {
    uint32_t c = rgb(148, 148, 148);
    uint32_t d = rgb(20, 27, 34);
    for (int row = 0; row < bh; row++) {
        for (int col = 0; col < bw; col++) {
            int k = polygon_pixel_kind(vx, vy, n, bw, bh, col, row);
            if (k == 1) rect(x + col, y + row, 1, 1, c);
            else if (k == 2) rect(x + col, y + row, 1, 1, d);
        }
    }
}

static void draw_cursor(int x, int y, int edge) {
    switch (edge) {
    case WM_EDGE_W: case WM_EDGE_E:
        draw_polygon_cursor(x, y, CURSOR_RESIZE_H_VX, CURSOR_RESIZE_H_VY, 10,
                             CURSOR_RESIZE_H_W, CURSOR_RESIZE_H_H);
        return;
    case WM_EDGE_N: case WM_EDGE_S:
        draw_polygon_cursor(x, y, CURSOR_RESIZE_V_VX, CURSOR_RESIZE_V_VY, 10,
                             CURSOR_RESIZE_V_W, CURSOR_RESIZE_V_H);
        return;
    case WM_EDGE_NW: case WM_EDGE_SE:
        draw_polygon_cursor(x, y, CURSOR_RESIZE_NWSE_VX, CURSOR_RESIZE_NWSE_VY, 10,
                             CURSOR_RESIZE_NWSE_W, CURSOR_RESIZE_NWSE_H);
        return;
    case WM_EDGE_NE: case WM_EDGE_SW:
        draw_polygon_cursor(x, y, CURSOR_RESIZE_NESW_VX, CURSOR_RESIZE_NESW_VY, 10,
                             CURSOR_RESIZE_NESW_W, CURSOR_RESIZE_NESW_H);
        return;
    case CURSOR_HOVER_TEXT:
        draw_polygon_cursor(x, y, CURSOR_TEXT_VX, CURSOR_TEXT_VY, 12,
                             CURSOR_TEXT_W, CURSOR_TEXT_H);
        return;
    default:
        draw_polygon_cursor(x, y, CURSOR_VX, CURSOR_VY, 4, CURSOR_ARROW_W, CURSOR_ARROW_H);
        return;
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
    if (key == KB_KEY_SHIFT_UP) return GUI_KEY_SHIFT_UP;
    if (key == KB_KEY_SHIFT_DOWN) return GUI_KEY_SHIFT_DOWN;
    if (key == KB_KEY_SHIFT_LEFT) return GUI_KEY_SHIFT_LEFT;
    if (key == KB_KEY_SHIFT_RIGHT) return GUI_KEY_SHIFT_RIGHT;
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

/* ── HAX GUI 应用画布接口（供内核 syscall 层调用） ───────────────────
 * 用户态 .hax 应用通过 int 0x80 调用这些函数，直接绘制到 GUI 的离屏表面
 * （g_gui_surface），再 present 到真实帧缓冲。当 GUI 未运行时 g_gui_surface
 * 为 NULL，gui_app_info 返回 0，应用据此可回退到文本模式。
 *
 * 注意：应用运行期间 GUI 主循环阻塞在 task_wait（见 console_exec_cmd），
 * 故应用在此期间“独占”整屏画布；退出后 GUI 重新接管并重绘。 */
static int     g_guiapp_mx = 0, g_guiapp_my = 0;
static uint8_t g_guiapp_btn = 0;

/* 全屏画布应用的"屏幕所有者"任务 id：非 0 时合成器让位（不绘制、不读输入），
 * 由该全屏应用独占 g_gui_surface 并自行 present；应用退出后由主循环清零。 */
uint32_t g_screen_owner_task = 0;

int gui_app_info(int *w, int *h) {
    if (!g_gui_surface) return 0;
    if (w) *w = g_gui_surface_w;
    if (h) *h = g_gui_surface_h;
    /* 首次查询时把鼠标置于画布中心，并登记为屏幕所有者 */
    if (g_guiapp_mx == 0 && g_guiapp_my == 0) {
        g_guiapp_mx = g_gui_surface_w / 2;
        g_guiapp_my = g_gui_surface_h / 2;
    }
    /* 合成器让位后不会再跑到帧尾的 gui_clip_clear()，若接手时恰好有一个残留的
     * 局部脏矩形处于激活状态，应用自己的 clear(0,0,W,H) 会被裁剪到那个旧矩形
     * 里——画面看起来像“只清空了一小块，其余仍是桌面残影”。取得屏幕所有权时
     * 主动清掉裁剪区，保证应用拿到的是完整无裁剪的画布。 */
    gui_clip_clear();
    g_screen_owner_task = task_get_id();
    return 1;
}

void gui_app_clear(uint32_t color) {
    if (!g_gui_surface) return;
    rect(0, 0, g_gui_surface_w, g_gui_surface_h, color);
}

void gui_app_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_gui_surface) return;
    rect(x, y, w, h, color);
}

void gui_app_text(int x, int y, const char *s, uint32_t color, int scale) {
    if (!g_gui_surface || !s) return;
    if (scale < 1) scale = 1;
    text(x, y, s, color, scale);
}

// 全屏画布应用独占 g_gui_surface 并自行 present（见上方注释），合成器的正常
// 光标叠加不会运行；若直接把光标画进 g_gui_surface，会被应用当成画布内容
// 永久保留（如 paint 会留下箭头形状的笔迹）。因此改为在 blit 到真实
// framebuffer 之后，直接在硬件帧缓冲上叠一份光标像素——不进入应用的画布。
static void present_screen_owner_cursor(const fb_info_t *fb, int x, int y) {
    uint32_t fb_pitch = (uint32_t)(fb->pitch / 4);
    int fbw = (int)fb->width, fbh = (int)fb->height;
    uint32_t c = rgb(148, 148, 148);
    uint32_t d = rgb(20, 27, 34);
#define OWNER_CURSOR_PX(px, py, col) do { \
        int _x = (px), _y = (py); \
        if (_x >= 0 && _y >= 0 && _x < fbw && _y < fbh) \
            fb->addr[(uint32_t)_y * fb_pitch + (uint32_t)_x] = (col); \
    } while (0)
    for (int row = 0; row < CURSOR_ARROW_H; row++) {
        for (int col = 0; col < CURSOR_ARROW_W; col++) {
            int k = polygon_pixel_kind(CURSOR_VX, CURSOR_VY, 4, CURSOR_ARROW_W, CURSOR_ARROW_H, col, row);
            if (k == 1) OWNER_CURSOR_PX(x + col, y + row, c);
            else if (k == 2) OWNER_CURSOR_PX(x + col, y + row, d);
        }
    }
#undef OWNER_CURSOR_PX
}

void gui_app_present(void) {
    if (!g_gui_surface) return;
    fb_info_t fb;
    if (fb_get_info(&fb) < 0) return;
    gui_present_surface(&fb);
    present_screen_owner_cursor(&fb, g_guiapp_mx, g_guiapp_my);
}

int gui_app_pollkey(void) {
    int k = key_poll();
    return k ? k : -1;
}

int gui_app_pollmouse(int *x, int *y) {
    mouse_event_t ev;
    while (mouse_poll(&ev)) {
        g_guiapp_mx += ev.dx;          /* 主循环约定：dy 正为下 */
        g_guiapp_my += ev.dy;
        g_guiapp_btn = ev.buttons;
    }
    if (g_guiapp_mx < 0) g_guiapp_mx = 0;
    if (g_guiapp_my < 0) g_guiapp_my = 0;
    if (g_gui_surface) {
        if (g_guiapp_mx > g_gui_surface_w - 1) g_guiapp_mx = g_gui_surface_w - 1;
        if (g_guiapp_my > g_gui_surface_h - 1) g_guiapp_my = g_gui_surface_h - 1;
    }
    if (x) *x = g_guiapp_mx;
    if (y) *y = g_guiapp_my;
    return (int)g_guiapp_btn;
}

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

/* 文件管理器面板布局 —— 绘制(draw_files_panel)和点击测试(hit_action)共用，
 * 避免两处几何不一致。三个列表框的高度(box_h)与可见行数(list_rows)跟随窗口
 * 高度：窗口内容区高 = win_h - 42(顶部偏移) - 32(底部状态带)，见
 * draw_one_window。旧实现的 206px 定高 + FILE_LIST_ROWS(8) 定行数有两个
 * 问题：8 行 x 30px = 240px 本来就画穿了 206px 的框底（最后两行叠在状态
 * 文字上、甚至越过窗口底边），而窗口放大/最大化后显示区域又完全不变。 */
typedef struct {
    int content_w;
    int side_w, main_w, detail_w;   /* main_x = tx+side_w+12, detail_x = main_x+main_w+12 */
    int box_h;       /* 三个列表框的公共高度 */
    int list_rows;   /* 文件列表可见行数 */
    int status_y;    /* 底部“已选择/显示 x-y”行的 y（相对 ty） */
} files_layout_t;

static void files_panel_layout(int win_w, int win_h, files_layout_t *L) {
    L->content_w = win_w - 60;
    L->side_w = 118;
    L->detail_w = 184;
    L->main_w = L->content_w - L->side_w - L->detail_w - 24;
    if (L->main_w < 260) {
        L->detail_w = 152;
        L->main_w = L->content_w - L->side_w - L->detail_w - 24;
    }
    if (L->main_w < 220) L->main_w = 220;

    int avail_h = win_h - 74;            /* 从 ty 到底部状态带的净高 */
    L->box_h = avail_h - 114 - 40;       /* 框顶在 ty+114，底部留状态条(28)+边距 */
    if (L->box_h < 206) L->box_h = 206;  /* 不小于原有设计尺寸 */
    /* 行从框顶 +32 开始（表头），底部留 8px 边距 */
    L->list_rows = (L->box_h - 40) / FILE_ROW_H;
    if (L->list_rows < 1) L->list_rows = 1;
    L->status_y = 114 + L->box_h + 14;
}

static void draw_files_panel(int tx, int ty, int win_w, int win_h, const gui_state_t *st) {
    char line[96];
    gui_state_t *mst = (gui_state_t *)st;
    files_layout_t L;
    files_panel_layout(win_w, win_h, &L);
    int content_w = L.content_w;
    int side_w = L.side_w;
    int main_x = tx + side_w + 12;
    int detail_w = L.detail_w;
    int main_w = L.main_w;
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

    vgradient(tx, ty + 114, side_w, L.box_h, rgb(22, 30, 40), rgb(14, 20, 28));
    border(tx, ty + 114, side_w, L.box_h, rgb(50, 72, 92));
    rect(tx, ty + 114, side_w, 1, rgb(58, 86, 110));
    text(tx + 14, ty + 128, "位置", rgb(194, 226, 242), 1);
    vgradient(tx + 10, ty + 154, side_w - 20, 28, rgb(38, 76, 104), rgb(22, 50, 70));
    rect(tx + 10, ty + 154, 3, 28, rgb(85, 180, 120));
    text_clipped(tx + 22, ty + 164, tx + side_w - 10, gui_file_path(mst), rgb(244, 250, 255), 1);
    text(tx + 22, ty + 202, "/home", rgb(132, 150, 162), 1);
    text(tx + 22, ty + 232, "/dev  /proc", rgb(132, 150, 162), 1);
    text(tx + 14, ty + 286, "Enter进入  Back上级", rgb(122, 142, 156), 1);

    vgradient(main_x, ty + 114, main_w, L.box_h, rgb(28, 40, 52), rgb(18, 26, 36));
    border(main_x, ty + 114, main_w, L.box_h, rgb(58, 86, 110));
    vgradient(main_x, ty + 114, main_w, 26, rgb(40, 60, 78), rgb(24, 36, 48));
    rect(main_x, ty + 139, main_w, 1, rgb(70, 100, 116));
    text(main_x + 14, ty + 122, "名称", rgb(218, 232, 240), 1);
    text(main_x + main_w - 166, ty + 122, "类型", rgb(218, 232, 240), 1);
    text(main_x + main_w - 82, ty + 122, "大小", rgb(218, 232, 240), 1);

    int list_y = ty + 146;
    if (count == 0) {
        text(main_x + 18, list_y + 18, "目录为空", rgb(190, 208, 218), 1);
        text(main_x + 18, list_y + 42, "点击新建或按 N 创建", rgb(148, 168, 180), 1);
        vgradient(detail_x, ty + 114, detail_w, L.box_h, rgb(22, 30, 40), rgb(14, 20, 28));
        border(detail_x, ty + 114, detail_w, L.box_h, rgb(50, 72, 92));
        text(detail_x + 12, ty + 130, "详细信息", rgb(194, 226, 242), 1);
        text(detail_x + 12, ty + 164, "未选择文件", rgb(148, 162, 174), 1);
        return;
    }

    int selected = st->selected_file;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= count) selected = (int)count - 1;
    uint32_t start = selected >= L.list_rows ? (uint32_t)(selected - (L.list_rows - 1)) : 0;
    uint32_t max = count - start;
    if (max > (uint32_t)L.list_rows) max = (uint32_t)L.list_rows;
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
        if (type == VFS_NODE_DIR) {
            blit_icon(main_x + 14, y - 8, 20, ICON_FOLDER_ROW);
        } else {
            uint32_t icon = type == VFS_NODE_CHARDEV ? rgb(102, 214, 255) : rgb(124, 220, 154);
            rect(main_x + 16, y - 2, 18, 14, icon);
            rect(main_x + 20, y - 7, 18, 7, rgb_lift(icon, -16));
        }
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
        vgradient(detail_x, ty + 114, detail_w, L.box_h, rgb(22, 30, 40), rgb(14, 20, 28));
        border(detail_x, ty + 114, detail_w, L.box_h, rgb(50, 72, 92));
        vgradient(detail_x, ty + 114, detail_w, 30, rgb(34, 50, 66), rgb(20, 30, 42));
        rect(detail_x, ty + 143, detail_w, 1, rgb(8, 14, 22));
        text(detail_x + 12, ty + 124, "详细信息", rgb(218, 232, 240), 1);
        if (type == VFS_NODE_DIR) {
            blit_icon(detail_x + 10, ty + 148, 32, ICON_FOLDER_ROW);
        } else {
            uint32_t icon = type == VFS_NODE_CHARDEV ? rgb(102, 214, 255) : rgb(124, 220, 154);
            rect(detail_x + 12, ty + 156, 30, 24, icon);
            rect(detail_x + 17, ty + 150, 28, 8, rgb_lift(icon, -16));
        }
        text_clipped(detail_x + 52, ty + 160, detail_x + detail_w - 12, name, rgb(238, 246, 255), 1);
        line_u32(line, sizeof(line), "大小: ", node ? node->size : 0, "B");
        text(detail_x + 12, ty + 194, line, rgb(210, 221, 230), 1);
        line2(line, sizeof(line), "类型: ", gui_node_type_label(type));
        text(detail_x + 12, ty + 216, line, rgb(210, 221, 230), 1);
        text(detail_x + 12, ty + 246, "预览", rgb(194, 226, 242), 1);
        /* 256 而非原来的 64：详情框高度现在跟随窗口，最大化后能放下更多预览 */
        char preview[256];
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
        for (uint32_t i = 0; i < n && py < ty + 114 + L.box_h - 8; i++) {
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

        int sy = ty + L.status_y;
        vgradient(tx, sy - 10, content_w, 28, rgb(28, 40, 54), rgb(16, 24, 34));
        rect(tx, sy - 10, content_w, 1, rgb(50, 72, 92));
        rect(tx, sy + 17, content_w, 1, rgb(8, 14, 22));
        border(tx, sy - 10, content_w, 28, rgb(46, 66, 86));
        if (st->rename_active) {
            int input_x = tx + 72;
            int hint_w = 132;
            int input_w = content_w - 84 - hint_w;
            if (input_w < 132) {
                hint_w = 0;
                input_w = content_w - 84;
            }
            if (input_w < 80) input_w = 80;
            text(tx + 12, sy, "新名称", rgb(216, 232, 244), 1);
            draw_inset_shell(input_x, sy - 5, input_w, 18, rgb(8, 14, 22));
            text_clipped(input_x + 6, sy, input_x + input_w - 8,
                         st->rename_buf, rgb(236, 246, 252), 1);
            int cursor_x = input_x + 6 + (int)st->rename_len * 6;
            if (cursor_x > input_x + input_w - 8) cursor_x = input_x + input_w - 8;
            rect(cursor_x, sy - 1, 2, 12, rgb(124, 220, 154));
            if (hint_w > 0)
                text(input_x + input_w + 10, sy, "Enter确认 Esc取消",
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
            text_clipped(tx + 12, sy, tx + content_w - 12, line, rgb(216, 232, 244), 1);
        }
    }
    if (count > (uint32_t)L.list_rows) {
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, "显示 ");
        append_uint(line, sizeof(line), &pos, start + 1);
        append_str(line, sizeof(line), &pos, "-");
        append_uint(line, sizeof(line), &pos, start + max);
        append_str(line, sizeof(line), &pos, " / ");
        append_uint(line, sizeof(line), &pos, count);
        text(main_x + main_w - 96, ty + L.status_y, line, rgb(148, 162, 174), 1);
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
        case GUI_APP_CLOCK:    return rgb(231, 76, 60);
        case GUI_APP_SETTINGS: return rgb(149, 165, 166);
        case GUI_APP_FILES:    return rgb(52, 152, 219);
        case GUI_APP_SHORTCUTS: return rgb(39, 174, 96);
        default:               return rgb(61, 174, 233);
    }
}

static int gui_app_grid_cols(int win_w) {
    int cols = (win_w - 8) / 132;
    if (cols < 2) cols = 2;
    if (cols > 5) cols = 5;   /* 5 列：9 个内置应用排 2 行，不溢出底部状态栏 */
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

        int isz = ui_s(52), ix = x + (w - isz) / 2, iy = y + ui_s(16);
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

// Ctrl+A 全选后，Backspace/Delete/输入字符 都应先清空整篇笔记（相当于替换选区）
// 删除 [start,end) 字节范围（Shift+方向键选区 或 Ctrl+A 全选后，输入/退格替换选中内容）
static void note_delete_range(gui_state_t *st, uint32_t start, uint32_t end) {
    if (end > st->note_len) end = st->note_len;
    st->note_sel_active = 0;
    if (start >= end) { st->note_cursor = start; return; }
    uint32_t removed = end - start;
    for (uint32_t i = start; i + removed <= st->note_len; i++)
        st->note_buf[i] = st->note_buf[i + removed];
    st->note_len -= removed;
    st->note_cursor = start;
    st->note_buf[st->note_len] = 0;
    st->note_dirty = 1;
    st->status = "编辑中（Ctrl+S 保存）";
}

// 选区范围 [*start,*end)；anchor==cursor 时无实际选中内容
static void note_sel_range(const gui_state_t *st, uint32_t *start, uint32_t *end) {
    uint32_t a = st->note_sel_anchor, b = st->note_cursor;
    *start = a < b ? a : b;
    *end   = a < b ? b : a;
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
                 "方向键移动  Ctrl+S 保存  Ctrl+A 全选",
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
    uint32_t sel_start = 0, sel_end = 0;
    if (st->note_sel_active) note_sel_range(st, &sel_start, &sel_end);
    utf8_state_t utf8;
    utf8_init(&utf8);
    for (uint32_t i = 0; i < st->note_len && y < ty + 280; i++) {
        if (i == st->note_cursor) {
            cursor_x = x;
            cursor_y = y;
            cursor_drawn = 1;
        }
        int in_sel = st->note_sel_active && i >= sel_start && i < sel_end;
        if (st->note_buf[i] == '\n') {
            if (in_sel) rect(x, y, 6, 16, rgb(40, 92, 132));
            x = edit_x + 8;
            y += 18;
            utf8_init(&utf8);
            continue;
        }

        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)st->note_buf[i], &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        if (in_sel) rect(x, y, gui_cp_advance(cp, 1), 16, rgb(40, 92, 132));
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
    // 在光标实际位置绘制闪烁竖线光标（每 15 帧切一次显隐，跟控制台/网址栏一致）
    static uint32_t note_caret_ticks = 0;
    note_caret_ticks++;
    if (cursor_y < ty + 280 && (note_caret_ticks / 15) % 2) {
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

static char gui_ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* 大小写不敏感——FAT32 短文件名（8.3 格式）落盘/读回都是全大写
 * （TEST.BMP），只按小写 ".bmp" 精确匹配的话，FAT32 盘上的文件后缀检测
 * 会全部失效，退回到内容嗅探甚至记事本，而不是真正按后缀分发。 */
static int gui_has_suffix(const char *path, const char *suffix) {
    if (!path || !suffix) return 0;
    uint32_t plen = (uint32_t)strlen(path);
    uint32_t slen = (uint32_t)strlen(suffix);
    if (slen == 0 || plen < slen) return 0;
    const char *p = path + plen - slen;
    for (uint32_t i = 0; i < slen; i++) {
        if (gui_ascii_lower(p[i]) != gui_ascii_lower(suffix[i])) return 0;
    }
    return 1;
}

static int gui_has_code_suffix(const char *path) {
    return gui_has_suffix(path, ".c") || gui_has_suffix(path, ".h") ||
           gui_has_suffix(path, ".cc") || gui_has_suffix(path, ".cpp") ||
           gui_has_suffix(path, ".hpp") || gui_has_suffix(path, ".asm") ||
           gui_has_suffix(path, ".S");
}

/* 内容嗅探兜底：没有 .bmp/代码后缀时，读文件开头一小段看像不像文本——
 * 含 NUL 字节，或控制字符占比过高，就当成二进制交给十六进制查看器，
 * 而不是像以前一样一律扔进记事本显示成乱码。纯文本文件（含没有后缀名
 * 的 README 之类）行为不变，仍然走记事本。 */
static int gui_looks_binary(vfs_node_t *node) {
    if (!node) return 0;
    uint8_t buf[64];
    int got = vfs_read(node, 0, buf, sizeof(buf));
    if (got <= 0) return 0;
    int suspicious = 0;
    for (int i = 0; i < got; i++) {
        uint8_t c = buf[i];
        if (c == 0) return 1;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) suspicious++;
    }
    return suspicious * 4 > got;
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
    st->code_sel_active = 0;
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

// 删除 [start,end) 字节范围（Shift+方向键选区 或 Ctrl+A 全选后，输入/退格替换选中内容）
static void code_delete_range(gui_state_t *st, uint32_t start, uint32_t end) {
    if (end > st->code_len) end = st->code_len;
    st->code_sel_active = 0;
    if (start >= end) { st->code_cursor = start; code_ensure_visible(st); return; }
    memmove(g_code_buf + start, g_code_buf + end, st->code_len - end + 1);
    st->code_len -= (end - start);
    st->code_cursor = start;
    st->code_modified = 1;
    st->code_error_line = 0;
    code_ensure_visible(st);
}

// 选区范围 [*start,*end)；anchor==cursor 时无实际选中内容
static void code_sel_range(const gui_state_t *st, uint32_t *start, uint32_t *end) {
    uint32_t a = st->code_sel_anchor, b = st->code_cursor;
    *start = a < b ? a : b;
    *end   = a < b ? b : a;
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
    uint32_t sel_start = 0, sel_end = 0;
    if (st->code_sel_active) code_sel_range(st, &sel_start, &sel_end);
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
        if (st->code_sel_active && sel_end > off && sel_start < off + len) {
            uint32_t line_sel_start = sel_start > off ? sel_start - off : 0;
            uint32_t line_sel_end = sel_end < off + len ? sel_end - off : len;
            int hl_x = l.editor_x + l.line_no_w + 10 + (int)line_sel_start * code_cell_w();
            int hl_w = (int)(line_sel_end - line_sel_start) * code_cell_w();
            if (sel_end > off + len) hl_w += code_cell_w();  // 选区跨行，把换行处也画出来
            if (hl_w < 1) hl_w = 1;
            rect(hl_x, y - 3, hl_w, l.row_h, rgb(40, 92, 132));
        } else if (st->code_error_line > 0 && (int)(line_idx + 1) == st->code_error_line) {
            rect(l.editor_x + l.line_no_w + 1, y - 3, l.editor_w - l.line_no_w - 4, l.row_h, rgb(70, 24, 30));
            rect(l.editor_x + l.line_no_w + 1, y - 3, 3, l.row_h, rgb(232, 86, 92));
        } else if (line_idx == cursor_line) {
            rect(l.editor_x + l.line_no_w + 1, y - 3, l.editor_w - l.line_no_w - 4, l.row_h, rgb(16, 28, 38));
        }
        text(l.editor_x + 8, y, num, rgb(102, 134, 154), 1);
        code_draw_highlighted_line(l.editor_x + l.line_no_w + 10, y,
                                   l.editor_x + l.editor_w - 10, line, n);
    }
    static uint32_t code_caret_ticks = 0;
    code_caret_ticks++;
    if ((int)cursor_line >= st->code_scroll && (int)cursor_line < st->code_scroll + l.view_rows &&
        (code_caret_ticks / 15) % 2) {
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
    int sb_w = 10;                          // scrollbar width
    int box_x = tx - 20;
    int box_y = ty - 4;
    int box_w = win_w - 20 - sb_w - 4;
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

    // clamp scroll offset
    int total = (int)st->console_line_count;
    int max_scroll = total - max_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (st->console_scroll > max_scroll) st->console_scroll = max_scroll;
    if (st->console_scroll < 0) st->console_scroll = 0;

    uint32_t start_idx = 0;
    if (total > max_lines) {
        int bottom_start = total - max_lines;
        start_idx = (uint32_t)(bottom_start - st->console_scroll);
    }

    // 绘制命令历史（控制台位图字体，与真 TUI 一致）
    int start_y = box_y + 12;
    for (uint32_t i = start_idx; i < st->console_line_count && start_y < input_y; i++) {
        const char *line = st->console_history[i];
        uint32_t color = cyber_text(0);
        if (strncmp(line, "hbos_gui_shell:", 15) == 0) {
            color = rgb(39, 174, 96);
        } else if (strncmp(line, "hbos_shell:", 11) == 0) {
            color = rgb(218, 68, 83);
        } else if (strncmp(line, "  ", 2) == 0) {
            color = rgb(160, 167, 173);
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

    // 垂直滚动条
    int sb_x = box_x + box_w + 4;
    int sb_y = box_y;
    int sb_h = box_h;
    rect(sb_x, sb_y, sb_w, sb_h, rgb(22, 26, 30));
    border(sb_x, sb_y, sb_w, sb_h, rgb(50, 58, 65));
    if (max_scroll > 0) {
        int thumb_h = sb_h * max_lines / (total > 0 ? total : 1);
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int thumb_range = sb_h - thumb_h;
        int thumb_y = sb_y + thumb_range - (thumb_range * st->console_scroll / max_scroll);
        rect(sb_x + 2, thumb_y + 1, sb_w - 4, thumb_h - 2, rgb(61, 174, 233));
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

/* 常见 HTML 实体解码成单个字符——共用给 browser_text_from_html（纯文本
 * 版，供保存网页）和 browser_render_from_html（带样式渲染版）两处，之前
 * 各自维护一份不同的小子集（一个只认 &amp;/&lt;/&gt;，另一个多认
 * &nbsp;/&quot;），现在统一成一份、认得更多真实页面常用的实体。返回 0
 * 表示不认识，*consumed 是除了开头 '&' 之外还要跳过的字节数（调用方
 * 自己会额外 +1 算上 '&' 本身）。没有对应字形的符号类实体（版权号/
 * 注册商标/商标）退化成字母近似，好过完全丢字符或者显示乱码——这是
 * 位图字体渲染器，不是每个 Unicode 符号都有字形。数字实体 &#NNN;
 * 只在落在可打印 ASCII 范围内才解码，避免把这个字体渲染不出来的
 * 字符实体解码成乱码。 */
static char br_decode_entity(const char *p, uint32_t *consumed) {
    *consumed = 0;
    if (strncmp(p, "&amp;", 5) == 0)   { *consumed = 4; return '&'; }
    if (strncmp(p, "&lt;", 4) == 0)    { *consumed = 3; return '<'; }
    if (strncmp(p, "&gt;", 4) == 0)    { *consumed = 3; return '>'; }
    if (strncmp(p, "&quot;", 6) == 0)  { *consumed = 5; return '"'; }
    if (strncmp(p, "&apos;", 6) == 0)  { *consumed = 5; return '\''; }
    if (strncmp(p, "&nbsp;", 6) == 0)  { *consumed = 5; return ' '; }
    if (strncmp(p, "&mdash;", 7) == 0) { *consumed = 6; return '-'; }
    if (strncmp(p, "&ndash;", 7) == 0) { *consumed = 6; return '-'; }
    if (strncmp(p, "&hellip;", 8) == 0){ *consumed = 7; return '.'; }
    if (strncmp(p, "&copy;", 6) == 0)  { *consumed = 5; return 'C'; }
    if (strncmp(p, "&reg;", 5) == 0)   { *consumed = 4; return 'R'; }
    if (strncmp(p, "&trade;", 7) == 0) { *consumed = 6; return 'T'; }
    if (p[1] == '#' && p[2] >= '0' && p[2] <= '9') {
        uint32_t k = 2, val = 0;
        while (p[k] >= '0' && p[k] <= '9' && val < 0x110000) { val = val * 10 + (uint32_t)(p[k] - '0'); k++; }
        if (p[k] == ';' && val >= 32 && val < 127) {
            *consumed = k;
            return (char)val;
        }
    }
    return 0;
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
            uint32_t consumed;
            char dec = br_decode_entity(html + i, &consumed);
            if (dec) { c = dec; i += consumed; }
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

// ── 浏览器分块渲染：把 HTML 转成 [1字节块类型][文本]\n... 的标记流，
// 供 draw_rendered_page 按标题/链接/列表/代码/分隔线等样式分别绘制，
// 而不是把所有内容拉平成一坨纯文字。browser_page 仍保留纯文本（供“保存网页”）。
enum {
    BRK_P      = '0',
    BRK_H1     = '1',
    BRK_H2     = '2',
    BRK_H3     = '3',
    BRK_LI     = '4',
    BRK_LINK   = '5',
    BRK_CODE   = '6',
    BRK_HR     = '7',
    BRK_STRONG = '8',
    BRK_QUOTE  = '9',
    BRK_IMG    = 'i',
    BRK_EM     = 'e', /* <em>/<i>：假斜体，见 g_text_italic */
    BRK_DT     = 'd', /* <dt>：术语，加粗 */
    BRK_DD     = 'D', /* <dd>：释义，缩进 */
    BRK_TROW   = 'r', /* 表格行：单元格文本用 BR_CELL_MARK 分隔，绘制端跨
                        * 连续 BRK_TROW 块统一算列宽后按列对齐（见
                        * br_table_scan/br_draw_table_row），<th> 行 meta
                        * 带加粗标志当表头画 */
    BRK_LI_NUM = 'n', /* <ol> 里的 <li>：编号是当普通文字写进去的（"1. "），
                        * 不是靠 browser_style_get 的 bullet 标志画点——所以
                        * 这个类型不带 bullet，缩进倒是跟 BRK_LI 一样 */
};
#define BR_STACK_MAX 8

/* 块前缀扩展编码——正常情况下渲染流仍是 [type][text]\n，零开销；只有
 * 块带链接和/或 CSS 覆盖时，type 后面才跟一个 0x01 (SOH) 标记 + 5 字节
 * 元数据（flags + RGB + link_index+1），draw_rendered_page() 据此在基础
 * 样式（browser_style_get）上打补丁。选这个方案是因为链接/CSS 覆盖只
 * 影响少数块，不值得把整条流都改成变长记录格式。 */
#define BR_META_MARK 0x01
#define BR_META_LEN  10 /* mark + flags + r + g + b + link_idx_plus1 + bg_r + bg_g + bg_b + flags2 */
#define BR_FLAG_COLOR        0x01
#define BR_FLAG_BOLD_ON      0x02
#define BR_FLAG_UNDER_ON     0x04
#define BR_FLAG_SCALE2       0x08
#define BR_FLAG_BG           0x10
#define BR_FLAG_ALIGN_CENTER 0x20
#define BR_FLAG_ALIGN_RIGHT  0x40
#define BR_FLAG_EXTRA_GAP    0x80
/* flags 字节 8 位已经用满，斜体单独开第二个 flags 字节。 */
#define BR_FLAG2_ITALIC_ON   0x01

/* 行内运行分隔符：块内样式切换（<strong>/<em>/<a> 开合）不再另起一行，
 * 而是在文本流里写 0x02 + 新 [type][可选 meta]，绘制端把同一块里的多个
 * 运行（run）连续排在同一视觉行里再统一折行——之前每次样式变化都强制
 * 换行，"This is <em>x</em> inline" 会被拆成三行、导航栏链接全部各占
 * 一行，是"看起来完全不像浏览器"的最大单一原因。 */
#define BR_RUN_MARK 0x02
#define BR_RUN_MAX  16

/* 表格单元格分隔字节（BRK_TROW 块内），以及列数上限（超出的单元格并进
 * 最后一列）。正文里的原始控制字节在发射端就被过滤，不会撞车。 */
#define BR_CELL_MARK 0x03
#define BR_TBL_COLS  8

static int br_is_inline_type(int t) {
    return t == BRK_LINK || t == BRK_STRONG || t == BRK_EM;
}

// 每种块类型对应的绘制样式：颜色、字号倍率、缩进、项目符号、等宽字体、
// 加粗（双次偏移描边模拟）、下划线（链接）、分隔线。让浏览器渲染标题/
// 列表/链接/代码块时有真实的视觉层次，而不是清一色的纯文字。
// （定义在发射端 browser_render_from_html 之前：行内标签的样式继承在
// 发射时就要查外层块的基础样式。）
typedef struct {
    uint32_t color;
    int scale;
    int indent;
    int bullet;
    int mono;
    int bold;
    int italic;
    int underline;
    int is_hr;
    int gap_after;   /* 块结束后追加的空行数（标题留白） */
    int has_bg;      /* 块背景（代码块/引用块用，让它们从正文里"浮"出来） */
    uint32_t bg_color;
    int left_bar;    /* 左侧强调竖线（引用块用，标出"这是引用"而不是普通缩进段落） */
    int align;       /* 0=left 1=center 2=right，CSS text-align 覆盖用 */
} browser_style_t;

static void browser_style_get(int type, browser_style_t *s) {
    uint32_t body_col = rgb(218, 230, 238);
    s->color = body_col; s->scale = 1; s->indent = 0; s->bullet = 0;
    s->mono = 0; s->bold = 0; s->italic = 0; s->underline = 0; s->is_hr = 0; s->gap_after = 0;
    s->has_bg = 0; s->bg_color = 0; s->left_bar = 0; s->align = 0;
    switch (type) {
        case BRK_P:   s->gap_after = 1; break; /* 段落之间留白——之前没有，所以文字全堆成一片 */
        case BRK_H1: s->color = rgb(255, 255, 255); s->scale = 2; s->bold = 1; s->gap_after = 1; break;
        case BRK_H2: s->color = rgb(120, 200, 255); s->bold = 1; s->gap_after = 1; break;
        case BRK_H3: s->color = rgb(150, 190, 225); s->bold = 1; s->gap_after = 1; break;
        case BRK_LI: s->color = body_col; s->indent = 16; s->bullet = 1; break;
        case BRK_LI_NUM: s->color = body_col; s->indent = 16; break;
        case BRK_LINK: s->color = rgb(78, 192, 236); s->underline = 1; break;
        case BRK_CODE:
            s->color = rgb(150, 235, 170); s->mono = 1; s->indent = 10;
            s->has_bg = 1; s->bg_color = rgb(24, 30, 38); s->gap_after = 1;
            break;
        case BRK_HR: s->is_hr = 1; break;
        case BRK_STRONG: s->color = rgb(255, 255, 255); s->bold = 1; break;
        case BRK_EM: s->color = body_col; s->italic = 1; break;
        case BRK_DT: s->color = rgb(255, 255, 255); s->bold = 1; break;
        case BRK_DD: s->color = body_col; s->indent = 20; s->gap_after = 1; break;
        case BRK_QUOTE:
            s->color = rgb(170, 180, 190); s->indent = 16;
            s->has_bg = 1; s->bg_color = rgb(28, 34, 42); s->left_bar = 1; s->gap_after = 1;
            break;
        case BRK_IMG: s->color = rgb(150, 140, 170); s->indent = 4; break;
        default: break;
    }
}

static int tag_ci_eq(const char *name, int len, const char *lit) {
    int i = 0;
    for (; i < len; i++) {
        char a = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (!lit[i] || a != lit[i]) return 0;
    }
    return lit[i] == 0;
}

static int browser_style_for_tag(const char *name, int len) {
    if (tag_ci_eq(name, len, "h1")) return BRK_H1;
    if (tag_ci_eq(name, len, "h2")) return BRK_H2;
    if (len == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] >= '3' && name[1] <= '6') return BRK_H3;
    if (tag_ci_eq(name, len, "li")) return BRK_LI;
    if (tag_ci_eq(name, len, "a")) return BRK_LINK;
    if (tag_ci_eq(name, len, "strong") || tag_ci_eq(name, len, "b")) return BRK_STRONG;
    if (tag_ci_eq(name, len, "em") || tag_ci_eq(name, len, "i")) return BRK_EM;
    if (tag_ci_eq(name, len, "pre") || tag_ci_eq(name, len, "code")) return BRK_CODE;
    if (tag_ci_eq(name, len, "blockquote")) return BRK_QUOTE;
    if (tag_ci_eq(name, len, "img")) return BRK_IMG;
    if (tag_ci_eq(name, len, "dt")) return BRK_DT;
    if (tag_ci_eq(name, len, "dd")) return BRK_DD;
    /* p/div/span 本身没有专属块类型（都渲染成普通段落 BRK_P），但必须在
     * 这里被"认出来"才会走 do_push 那条分支去解析它们的 class/style 属性
     * ——真实页面里 class/内联 style 多半就打在这几个标签上，不认出来的
     * 话 CSS 覆盖会被无声跳过（只有 h1-h6/a/strong 等自带类型的标签才有
     * 机会应用 CSS，明显不够用）。 */
    if (tag_ci_eq(name, len, "p") || tag_ci_eq(name, len, "div") || tag_ci_eq(name, len, "span")) return BRK_P;
    return -1;
}

/* ── 属性值提取：在一个标签的原始文本（"a href=\"x\" class='y'"）里找
 * name="value" 或 name='value'，大小写不敏感匹配属性名。供 href/class/
 * style/alt 属性解析共用。 */
static int br_attr_value(const char *tag, uint32_t tag_len, const char *name,
                          char *out, uint32_t out_cap) {
    uint32_t nlen = (uint32_t)strlen(name);
    for (uint32_t i = 0; i + nlen < tag_len; i++) {
        if (!tag_ci_eq(tag + i, (int)nlen, name)) continue;
        uint32_t p = i + nlen;
        while (p < tag_len && (tag[p] == ' ' || tag[p] == '\t')) p++;
        if (p >= tag_len || tag[p] != '=') continue;
        p++;
        while (p < tag_len && (tag[p] == ' ' || tag[p] == '\t')) p++;
        if (p >= tag_len) return 0;
        char quote = (tag[p] == '"' || tag[p] == '\'') ? tag[p] : 0;
        if (quote) p++;
        uint32_t start = p;
        while (p < tag_len && (quote ? tag[p] != quote : (tag[p] != ' ' && tag[p] != '>'))) p++;
        uint32_t vlen = p - start;
        if (vlen >= out_cap) vlen = out_cap - 1;
        memcpy(out, tag + start, vlen);
        out[vlen] = 0;
        return 1;
    }
    return 0;
}

/* ── CSS：内联 style="prop:value;..." 属性 + <style> 块里简单的
 * tagname{...} / .classname{...} 选择器，映射到 browser_style_t 能表示
 * 的那几个属性（color/font-weight/text-decoration/font-size）。不做级联
 * /优先级解析——同一个块如果内联和样式表都命中，内联覆盖样式表最后一条
 * 匹配规则，就这两层，不是完整 CSS 引擎。 */
typedef struct {
    uint32_t color; int has_color;
    int bold;        int has_bold;
    int italic;      int has_italic;
    int underline;   int has_underline;
    int scale2;       int has_scale;
    uint32_t bg_color; int has_bg;
    int align;         int has_align; /* 0=left 1=center 2=right */
    int gap;            int has_gap;  /* margin/margin-top/margin-bottom 非零 -> 块后多留一行；
                                        * 这是行文本渲染器，没有真正的像素级盒模型，margin 只
                                        * 映射成"要不要多留白"这一个二值信号，不是精确像素值 */
} css_decl_t;

#define CSS_RULE_MAX 32
typedef struct {
    char selector[32]; /* "tagname" 或 ".classname" */
    css_decl_t decl;
} css_rule_t;
static css_rule_t g_css_rules[CSS_RULE_MAX];
static int g_css_rule_count;

static int css_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int css_parse_color(const char *v, uint32_t *out) {
    while (*v == ' ') v++;
    if (*v == '#') {
        v++;
        int len = 0; while (v[len] && css_hex_digit(v[len]) >= 0) len++;
        if (len == 6) {
            uint32_t val = 0;
            for (int i = 0; i < 6; i++) val = (val << 4) | (uint32_t)css_hex_digit(v[i]);
            *out = val;
            return 1;
        }
        if (len == 3) {
            int r = css_hex_digit(v[0]), g = css_hex_digit(v[1]), b = css_hex_digit(v[2]);
            *out = (uint32_t)((r << 4 | r) << 16 | (g << 4 | g) << 8 | (b << 4 | b));
            return 1;
        }
        return 0;
    }
    static const struct { const char *name; uint32_t val; } named[] = {
        {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF3B30}, {"green", 0x34C759},
        {"blue", 0x0A84FF}, {"yellow", 0xFFD60A}, {"orange", 0xFF9500}, {"purple", 0xAF52DE},
        {"gray", 0x8E8E93}, {"grey", 0x8E8E93}, {"silver", 0xC0C0C0}, {"cyan", 0x32ADE6},
        {"pink", 0xFF2D55},
    };
    for (uint32_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        uint32_t nlen = (uint32_t)strlen(named[i].name);
        int match = 1;
        for (uint32_t j = 0; j < nlen; j++) {
            char a = v[j]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != named[i].name[j]) { match = 0; break; }
        }
        if (match && (v[nlen] == 0 || v[nlen] == ' ' || v[nlen] == ';')) {
            *out = named[i].val;
            return 1;
        }
    }
    return 0;
}

/* 解析一条 "prop:value" 声明，命中的属性写进 *d（没命中的属性名直接
 * 忽略——保证解析器不会被不认识的 CSS 属性卡住） */
static void css_apply_decl(css_decl_t *d, const char *prop, uint32_t plen, const char *val) {
    if (tag_ci_eq(prop, (int)plen, "color")) {
        uint32_t c;
        if (css_parse_color(val, &c)) { d->color = c; d->has_color = 1; }
    } else if (tag_ci_eq(prop, (int)plen, "font-weight")) {
        int is_bold = (strstr(val, "bold") != 0);
        for (const char *p = val; !is_bold && *p >= '0' && *p <= '9'; ) {
            int n = 0; while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
            is_bold = n >= 600;
            break;
        }
        d->bold = is_bold; d->has_bold = 1;
    } else if (tag_ci_eq(prop, (int)plen, "font-style")) {
        d->italic = (strstr(val, "italic") != 0) || (strstr(val, "oblique") != 0);
        d->has_italic = 1;
    } else if (tag_ci_eq(prop, (int)plen, "text-decoration")) {
        d->underline = (strstr(val, "underline") != 0);
        d->has_underline = 1;
    } else if (tag_ci_eq(prop, (int)plen, "font-size")) {
        int n = 0; const char *p = val;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        d->scale2 = (n >= 20) || strstr(val, "large") != 0;
        d->has_scale = 1;
    } else if (tag_ci_eq(prop, (int)plen, "background-color") || tag_ci_eq(prop, (int)plen, "background")) {
        uint32_t c;
        if (css_parse_color(val, &c)) { d->bg_color = c; d->has_bg = 1; }
    } else if (tag_ci_eq(prop, (int)plen, "text-align")) {
        int a = -1;
        if (strstr(val, "center")) a = 1;
        else if (strstr(val, "right")) a = 2;
        else if (strstr(val, "left")) a = 0;
        if (a >= 0) { d->align = a; d->has_align = 1; }
    } else if (tag_ci_eq(prop, (int)plen, "margin") || tag_ci_eq(prop, (int)plen, "margin-top") ||
               tag_ci_eq(prop, (int)plen, "margin-bottom")) {
        int n = 0; const char *p = val;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        d->gap = (n > 0) ? 1 : 0; d->has_gap = 1;
    }
}

/* 解析形如 "color:red; font-weight:bold" 的声明串（内联 style 属性值，
 * 或 <style> 规则花括号里的内容），逐条分号切开交给 css_apply_decl。 */
static void css_parse_decls(css_decl_t *d, const char *text, uint32_t len) {
    memset(d, 0, sizeof(*d));
    uint32_t i = 0;
    while (i < len) {
        while (i < len && (text[i] == ' ' || text[i] == ';')) i++;
        uint32_t start = i;
        while (i < len && text[i] != ';') i++;
        uint32_t seg_len = i - start;
        if (seg_len == 0) continue;
        const char *colon = memchr(text + start, ':', seg_len);
        if (!colon) continue;
        uint32_t plen = (uint32_t)(colon - (text + start));
        while (plen > 0 && text[start + plen - 1] == ' ') plen--;
        char val[64];
        uint32_t vstart = (uint32_t)(colon - text) + 1;
        uint32_t vlen = (start + seg_len) - vstart;
        while (vlen > 0 && text[vstart] == ' ') { vstart++; vlen--; }
        while (vlen > 0 && text[vstart + vlen - 1] == ' ') vlen--;
        if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
        memcpy(val, text + vstart, vlen);
        val[vlen] = 0;
        css_apply_decl(d, text + start, plen, val);
    }
}

/* 解析整个 <style>...</style> 块的内容："selector { decls } selector2 { decls2 } ..." */
static void css_parse_stylesheet(const char *css, uint32_t len) {
    uint32_t i = 0;
    while (i < len && g_css_rule_count < CSS_RULE_MAX) {
        while (i < len && (css[i] == ' ' || css[i] == '\n' || css[i] == '\t' || css[i] == '\r')) i++;
        uint32_t sel_start = i;
        while (i < len && css[i] != '{') i++;
        if (i >= len) break;
        uint32_t sel_end = i;
        while (sel_end > sel_start && (css[sel_end - 1] == ' ' || css[sel_end - 1] == '\n' ||
                                        css[sel_end - 1] == '\t' || css[sel_end - 1] == '\r')) sel_end--;
        uint32_t sel_len = sel_end - sel_start;
        i++; /* 跳过 '{' */
        uint32_t body_start = i;
        while (i < len && css[i] != '}') i++;
        uint32_t body_len = i - body_start;
        if (i < len) i++; /* 跳过 '}' */
        if (sel_len == 0 || sel_len >= sizeof(g_css_rules[0].selector)) continue;
        /* 只处理单一简单选择器（不支持逗号并列/组合选择器），多个用逗号
         * 分隔的选择器分别当独立规则处理一遍相同声明。 */
        uint32_t part_start = sel_start;
        for (uint32_t k = sel_start; k <= sel_end && g_css_rule_count < CSS_RULE_MAX; k++) {
            if (k == sel_end || css[k] == ',') {
                uint32_t ps = part_start, pe = k;
                while (ps < pe && css[ps] == ' ') ps++;
                while (pe > ps && css[pe - 1] == ' ') pe--;
                uint32_t plen = pe - ps;
                if (plen > 0 && plen < sizeof(g_css_rules[0].selector)) {
                    css_rule_t *r = &g_css_rules[g_css_rule_count++];
                    memcpy(r->selector, css + ps, plen);
                    r->selector[plen] = 0;
                    css_parse_decls(&r->decl, css + body_start, body_len);
                }
                part_start = k + 1;
            }
        }
    }
}

/* 给定标签名 + class 属性值，把匹配到的样式表规则和内联 style 合并成
 * 一份声明（内联优先级最高，最后应用）。 */
static void css_resolve_for_tag(const char *tag_name, int tag_name_len,
                                 const char *class_val, const char *inline_style,
                                 css_decl_t *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < g_css_rule_count; i++) {
        css_rule_t *r = &g_css_rules[i];
        int match = 0;
        if (r->selector[0] == '.') {
            if (class_val && class_val[0]) {
                /* class 属性可能是空格分隔的多个 class，逐个比较 */
                const char *p = class_val;
                uint32_t sel_len = (uint32_t)strlen(r->selector + 1);
                while (*p) {
                    while (*p == ' ') p++;
                    uint32_t cl = 0; while (p[cl] && p[cl] != ' ') cl++;
                    if (cl == sel_len && tag_ci_eq(p, (int)cl, r->selector + 1)) { match = 1; break; }
                    p += cl;
                }
            }
        } else {
            match = tag_ci_eq(tag_name, tag_name_len, r->selector);
        }
        if (!match) continue;
        if (r->decl.has_color) { out->color = r->decl.color; out->has_color = 1; }
        if (r->decl.has_bold) { out->bold = r->decl.bold; out->has_bold = 1; }
        if (r->decl.has_italic) { out->italic = r->decl.italic; out->has_italic = 1; }
        if (r->decl.has_underline) { out->underline = r->decl.underline; out->has_underline = 1; }
        if (r->decl.has_scale) { out->scale2 = r->decl.scale2; out->has_scale = 1; }
        if (r->decl.has_bg) { out->bg_color = r->decl.bg_color; out->has_bg = 1; }
        if (r->decl.has_align) { out->align = r->decl.align; out->has_align = 1; }
        if (r->decl.has_gap) { out->gap = r->decl.gap; out->has_gap = 1; }
    }
    if (inline_style && inline_style[0]) {
        css_decl_t inl;
        css_parse_decls(&inl, inline_style, (uint32_t)strlen(inline_style));
        if (inl.has_color) { out->color = inl.color; out->has_color = 1; }
        if (inl.has_bold) { out->bold = inl.bold; out->has_bold = 1; }
        if (inl.has_italic) { out->italic = inl.italic; out->has_italic = 1; }
        if (inl.has_underline) { out->underline = inl.underline; out->has_underline = 1; }
        if (inl.has_scale) { out->scale2 = inl.scale2; out->has_scale = 1; }
        if (inl.has_bg) { out->bg_color = inl.bg_color; out->has_bg = 1; }
        if (inl.has_align) { out->align = inl.align; out->has_align = 1; }
        if (inl.has_gap) { out->gap = inl.gap; out->has_gap = 1; }
    }
}

/* 把当前活跃的 link 下标 + CSS 覆盖编码成 BR_META_MARK 扩展前缀（没有
 * 覆盖也没有链接时不写这几个字节，保持零开销）。 */
static void br_emit_meta(char *out, uint32_t cap, uint32_t *pos, int link_idx, const css_decl_t *ov) {
    int has_meta = (link_idx >= 0) || (ov && (ov->has_color || ov->has_bold || ov->has_italic ||
                                              ov->has_underline || ov->has_scale || ov->has_bg ||
                                              ov->has_align || ov->has_gap));
    if (!has_meta) return;
    if (*pos + BR_META_LEN >= cap) return;
    uint8_t flags = 0, flags2 = 0;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    if (ov) {
        if (ov->has_color) { flags |= BR_FLAG_COLOR; r = (uint8_t)(ov->color >> 16); g = (uint8_t)(ov->color >> 8); b = (uint8_t)ov->color; }
        if (ov->has_bold && ov->bold) flags |= BR_FLAG_BOLD_ON;
        if (ov->has_italic && ov->italic) flags2 |= BR_FLAG2_ITALIC_ON;
        if (ov->has_underline && ov->underline) flags |= BR_FLAG_UNDER_ON;
        if (ov->has_scale && ov->scale2) flags |= BR_FLAG_SCALE2;
        if (ov->has_bg) { flags |= BR_FLAG_BG; bg_r = (uint8_t)(ov->bg_color >> 16); bg_g = (uint8_t)(ov->bg_color >> 8); bg_b = (uint8_t)ov->bg_color; }
        if (ov->has_align && ov->align == 1) flags |= BR_FLAG_ALIGN_CENTER;
        if (ov->has_align && ov->align == 2) flags |= BR_FLAG_ALIGN_RIGHT;
        if (ov->has_gap && ov->gap) flags |= BR_FLAG_EXTRA_GAP;
    }
    uint8_t link_byte = (link_idx >= 0 && link_idx < 254) ? (uint8_t)(link_idx + 1) : 0;
    out[(*pos)++] = (char)BR_META_MARK;
    out[(*pos)++] = (char)flags;
    out[(*pos)++] = (char)r;
    out[(*pos)++] = (char)g;
    out[(*pos)++] = (char)b;
    out[(*pos)++] = (char)link_byte;
    out[(*pos)++] = (char)bg_r;
    out[(*pos)++] = (char)bg_g;
    out[(*pos)++] = (char)bg_b;
    out[(*pos)++] = (char)flags2;
}

/* <title> 内容——被 skip_mode 捕获，不进正文渲染流；供窗口标题栏用
 * （见 draw_browser_app 里的 wm_set_app_title 调用）。 */
#define BROWSER_TITLE_CAP 64
static char g_browser_page_title[BROWSER_TITLE_CAP];
static uint32_t g_browser_title_len;

static void browser_render_from_html(gui_state_t *st, const char *html, char *out, uint32_t cap, uint32_t *out_len) {
    uint32_t pos = 0;
    int space = 1;
    int need_prefix = 1;
    int run_sep = 0;   /* 前缀写出时先写 BR_RUN_MARK（行内运行切换，见上） */
    int line_runs = 1; /* 当前行已写出的运行数：达到 BR_RUN_MAX 就退化成换行，
                        * 和解码端的 runs[] 定长数组保持一致 */
    int style_stack[BR_STACK_MAX];
    int link_idx_stack[BR_STACK_MAX];
    css_decl_t override_stack[BR_STACK_MAX];
    int stack_n = 0;
    int cur_style = BRK_P;
    int cur_link_idx = -1;
    css_decl_t cur_override;
    memset(&cur_override, 0, sizeof(cur_override));
    int skip_mode = 0;
    const char *skip_close = "";
    int in_style_block = 0;
    int in_title_block = 0;
    static char style_buf[4096];
    uint32_t style_buf_len = 0;
    /* 表格：不做真正的列宽计算/对齐（这是行文本渲染器，没有真正的表格
     * 布局引擎），把一整行 <tr> 的所有单元格文字用 " | " 接起来，当一个
     * 普通块输出——至少内容和行结构保住了，不是把表格内容打散丢进正文
     * 里变成读不懂的一堆字。<th> 所在行整行加粗，近似"表头"视觉效果。 */
    int in_row = 0;
    int in_cell = 0;
    int row_has_th = 0;
    static char row_buf[512];
    uint32_t row_buf_len = 0;
    /* 有序列表编号：只跟踪一层（不是真正的嵌套计数器栈），<ol> 套 <ul>
     * 套 <ol> 这种深嵌套的编号可能不完全准确，对这个渲染器的定位来说
     * 是可以接受的简化。 */
    int in_ordered_list = 0;
    int li_number = 0;

    g_css_rule_count = 0;
    st->browser_link_count = 0;

    g_browser_page_title[0] = 0;
    g_browser_title_len = 0;

#define BREMIT(c) do { if (pos + 1 < cap) out[pos++] = (char)(c); } while (0)
/* 收行：只要当前行有内容（前缀已写出，或有 pending 的行内运行分隔符）
 * 就补 '\n'。原来的判据是"最后一个字符不是空格"（!space），行尾恰好是
 * 空格时不收行，后面直接 BREMIT 块类型字节会把它当正文字符混进当前行；
 * run_sep 也必须在这里丢弃——真正换行之后是新块前缀，行首写 0x02 会让
 * 解码端错位。 */
#define BRFLUSH() do { if (!need_prefix || run_sep) { BREMIT('\n'); space = 1; need_prefix = 1; run_sep = 0; } } while (0)
#define BRMETA() br_emit_meta(out, cap, &pos, cur_link_idx, &cur_override)

    for (uint32_t i = 0; html[i] && pos + 2 < cap; i++) {
        char c = html[i];

        if (skip_mode) {
            if (c == '<' && html[i + 1] == '/') {
                uint32_t j = i + 2;
                char nm[16]; int nl = 0;
                while (html[j] && html[j] != '>' && nl < 15) nm[nl++] = html[j++];
                if (tag_ci_eq(nm, nl, skip_close)) {
                    skip_mode = 0;
                    i = j;
                    if (in_style_block) {
                        css_parse_stylesheet(style_buf, style_buf_len);
                        in_style_block = 0;
                    }
                    if (in_title_block) {
                        g_browser_page_title[g_browser_title_len] = 0;
                        in_title_block = 0;
                    }
                    continue;
                }
            }
            if (in_style_block && style_buf_len + 1 < sizeof(style_buf)) style_buf[style_buf_len++] = c;
            if (in_title_block && g_browser_title_len + 1 < sizeof(g_browser_page_title) &&
                c != '\r' && c != '\n')
                g_browser_page_title[g_browser_title_len++] = (c == '\t') ? ' ' : c;
            continue;
        }

        if (c == '<') {
            int closing = (html[i + 1] == '/');
            uint32_t tag_start = i + 1 + (closing ? 1 : 0);
            uint32_t j = tag_start;
            char nm[16]; int nl = 0;
            while (html[j] && html[j] != '>' && html[j] != ' ' && html[j] != '\t' &&
                   html[j] != '\n' && html[j] != '/' && nl < 15)
                nm[nl++] = html[j++];
            uint32_t attr_start = j;
            while (html[j] && html[j] != '>') j++;
            uint32_t attr_len = (j > attr_start) ? j - attr_start : 0;

            if (!closing && tag_ci_eq(nm, nl, "script")) {
                BRFLUSH();
                skip_mode = 1; in_style_block = 0;
                skip_close = "script";
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "style")) {
                BRFLUSH();
                skip_mode = 1; in_style_block = 1; style_buf_len = 0;
                skip_close = "style";
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "title")) {
                /* <title> 之前完全没被特殊处理，内容会直接混进正文渲染出来
                 * （网页标题变成页面第一行文字）；现在跟 script/style 一样
                 * 跳过不渲染，顺便捕获下来给窗口标题栏用。 */
                skip_mode = 1; in_title_block = 1; g_browser_title_len = 0;
                skip_close = "title";
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "br")) {
                /* <br> = 纯换行：行上有内容就收行；行首的 <br>（连续
                 * <br><br> 的第二个）才输出空块表示空一行。原来无条件
                 * 追加一个空块，单个 <br> 会带出一整行空白，和真实浏览器
                 * 行为不符。 */
                if (!need_prefix || run_sep) {
                    BREMIT('\n'); space = 1; need_prefix = 1; run_sep = 0;
                } else {
                    BREMIT(cur_style); BREMIT('\n'); space = 1;
                }
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "hr")) {
                BRFLUSH();
                BREMIT(BRK_HR); BREMIT('\n');
                need_prefix = 1; space = 1;
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "img")) {
                BRFLUSH();
                char alt[64];
                if (!br_attr_value(html + attr_start, attr_len, "alt", alt, sizeof(alt)) || !alt[0])
                    strcpy(alt, "图片");
                BREMIT(BRK_IMG);
                char label[80];
                uint32_t lp = 0;
                label[lp++] = '[';
                for (uint32_t k = 0; alt[k] && lp < sizeof(label) - 2; k++) label[lp++] = alt[k];
                label[lp++] = ']';
                label[lp] = 0;
                for (uint32_t k = 0; label[k] && pos + 1 < cap; k++) BREMIT(label[k]);
                BREMIT('\n');
                need_prefix = 1; space = 1;
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "tr")) {
                BRFLUSH();
                in_row = 1; in_cell = 0; row_has_th = 0; row_buf_len = 0;
                i = j;
                continue;
            }
            if (closing && tag_ci_eq(nm, nl, "tr")) {
                if (in_row) {
                    row_buf[row_buf_len < sizeof(row_buf) ? row_buf_len : sizeof(row_buf) - 1] = 0;
                    if (row_buf_len > 0) {
                        BREMIT(BRK_TROW);
                        css_decl_t row_ov;
                        memset(&row_ov, 0, sizeof(row_ov));
                        if (row_has_th) { row_ov.bold = 1; row_ov.has_bold = 1; }
                        br_emit_meta(out, cap, &pos, -1, &row_ov);
                        for (uint32_t k = 0; k < row_buf_len && pos + 1 < cap; k++) BREMIT(row_buf[k]);
                        BREMIT('\n');
                        need_prefix = 1; space = 1;
                    }
                    in_row = 0; in_cell = 0;
                }
                i = j;
                continue;
            }
            if (!closing && (tag_ci_eq(nm, nl, "td") || tag_ci_eq(nm, nl, "th"))) {
                if (in_row) {
                    if (row_buf_len > 0 && row_buf_len + 1 < sizeof(row_buf))
                        row_buf[row_buf_len++] = (char)BR_CELL_MARK;
                    in_cell = 1;
                    if (tag_ci_eq(nm, nl, "th")) row_has_th = 1;
                }
                i = j;
                continue;
            }
            if (closing && (tag_ci_eq(nm, nl, "td") || tag_ci_eq(nm, nl, "th"))) {
                in_cell = 0;
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "ol")) {
                in_ordered_list = 1; li_number = 0;
                i = j;
                continue;
            }
            if (closing && tag_ci_eq(nm, nl, "ol")) {
                in_ordered_list = 0;
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "ul")) {
                in_ordered_list = 0; /* <ol> 套 <ul> 时切回项目符号 */
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "li") && in_ordered_list) {
                BRFLUSH();
                li_number++;
                BREMIT(BRK_LI_NUM);
                char numbuf[8];
                int nn = 0;
                char tmp[8]; int tn = 0; int v = li_number;
                while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
                if (tn == 0) tmp[tn++] = '0';
                while (tn > 0) numbuf[nn++] = tmp[--tn];
                numbuf[nn++] = '.'; numbuf[nn++] = ' ';
                for (int k = 0; k < nn && pos + 1 < cap; k++) BREMIT(numbuf[k]);
                space = 0; need_prefix = 0;
                cur_style = BRK_LI_NUM; cur_link_idx = -1;
                memset(&cur_override, 0, sizeof(cur_override));
                i = j;
                continue;
            }
            int st_type = browser_style_for_tag(nm, nl);
            int old_style = cur_style;
            int new_style = cur_style;
            int do_push = 0, do_pop = 0;
            int new_link_idx = cur_link_idx;
            css_decl_t new_override = cur_override;
            if (!closing) {
                if (st_type >= 0) {
                    new_style = st_type;
                    do_push = 1;
                    char class_val[64]; class_val[0] = 0;
                    char style_val[128]; style_val[0] = 0;
                    br_attr_value(html + attr_start, attr_len, "class", class_val, sizeof(class_val));
                    br_attr_value(html + attr_start, attr_len, "style", style_val, sizeof(style_val));
                    css_decl_t resolved;
                    css_resolve_for_tag(nm, nl, class_val, style_val, &resolved);
                    /* 行内标签继承外层样式：<h1>里的<em>保持大字号/加粗，
                     * <p style="color:red">里的<b>保持红色——行内元素现在
                     * 和外层文字排在同一行，字号/颜色突变会非常扎眼。
                     * 只填充 resolved 里没有显式指定的字段。 */
                    if (br_is_inline_type(st_type)) {
                        browser_style_t pbs;
                        browser_style_get(cur_style, &pbs);
                        if (!resolved.has_scale && (pbs.scale >= 2 ||
                            (cur_override.has_scale && cur_override.scale2))) {
                            resolved.scale2 = 1; resolved.has_scale = 1;
                        }
                        if (!resolved.has_bold && (pbs.bold ||
                            (cur_override.has_bold && cur_override.bold))) {
                            resolved.bold = 1; resolved.has_bold = 1;
                        }
                        if (!resolved.has_color && cur_override.has_color) {
                            resolved.color = cur_override.color; resolved.has_color = 1;
                        }
                        if (!resolved.has_italic && cur_override.has_italic) {
                            resolved.italic = cur_override.italic; resolved.has_italic = 1;
                        }
                    }
                    new_override = resolved;
                    if (st_type == BRK_LINK) {
                        char href[96];
                        if (br_attr_value(html + attr_start, attr_len, "href", href, sizeof(href)) &&
                            st->browser_link_count < BROWSER_LINK_MAX) {
                            new_link_idx = st->browser_link_count;
                            uint32_t hl = (uint32_t)strlen(href);
                            if (hl >= sizeof(st->browser_link_href[0])) hl = sizeof(st->browser_link_href[0]) - 1;
                            memcpy(st->browser_link_href[st->browser_link_count], href, hl);
                            st->browser_link_href[st->browser_link_count][hl] = 0;
                            st->browser_link_count++;
                        }
                    }
                }
            } else if (st_type >= 0 && stack_n > 0 && style_stack[stack_n - 1] == st_type) {
                do_pop = 1;
                new_style = (stack_n > 1) ? style_stack[stack_n - 2] : BRK_P;
                new_link_idx = (stack_n > 1) ? link_idx_stack[stack_n - 2] : -1;
                new_override = (stack_n > 1) ? override_stack[stack_n - 2] : (css_decl_t){0};
            }
            int override_changed = memcmp(&new_override, &cur_override, sizeof(css_decl_t)) != 0;
            if (new_style != old_style || new_link_idx != cur_link_idx || override_changed) {
                /* 样式/链接/CSS 覆盖任一变化都得让新前缀被写出（比如
                 * "Hello <strong>world"，或者连续两个 class 不同的 <p> 因为
                 * 块类型都是 BRK_P 而被 BRFLUSH() 那条分支悄悄吞掉）。
                 * 行内标签（a/strong/em）的切换写运行分隔符而不是换行，
                 * 文字留在同一视觉行由绘制端流式排版。 */
                int inline_tr = br_is_inline_type(new_style) || br_is_inline_type(old_style);
                if (inline_tr && line_runs >= BR_RUN_MAX - 1) inline_tr = 0;
                if (need_prefix) {
                    /* 上一个前缀还没写出（标签之间没有文字）：只更新样式。
                     * 例外：pending 的运行分隔符碰上块级切换（比如段落末尾
                     * 的空 <strong></strong> 后面跟 </p><p>），得把它转成
                     * 真正的换行，否则下一段会接在上一行屁股后面。 */
                    if (run_sep && !inline_tr) { BREMIT('\n'); run_sep = 0; space = 1; }
                } else if (inline_tr) {
                    run_sep = 1; need_prefix = 1;
                    /* 注意不动 space：行内切换前后的空格语义要保持（
                     * "is <strong>bold</strong> ok" 里两个空格都得留下）。 */
                } else {
                    BREMIT('\n');
                    space = 1; need_prefix = 1;
                }
            } else {
                BRFLUSH();
            }
            if (do_push && stack_n < BR_STACK_MAX) {
                style_stack[stack_n] = st_type;
                link_idx_stack[stack_n] = new_link_idx;
                override_stack[stack_n] = new_override;
                stack_n++;
            }
            if (do_pop) stack_n--;
            cur_style = new_style;
            cur_link_idx = new_link_idx;
            cur_override = new_override;
            i = j;
            continue;
        }

        char ch = c;
        if (ch == '&') {
            uint32_t consumed;
            char dec = br_decode_entity(html + i, &consumed);
            if (dec) { ch = dec; i += consumed; }
        }
        if (ch == '\r') continue;
        /* 除 \n/\t 外的控制字节直接丢弃：0x01/0x02 在渲染流里是元数据
         * 标记，正文里混进原始控制字节会让解码端错位。 */
        if ((unsigned char)ch < 0x20 && ch != '\n' && ch != '\t') continue;
        if (cur_style == BRK_CODE) {
            /* <pre>/<code> 里之前跟其它文字一样把换行/连续空格全折叠掉，
             * 多行代码块被压成一整行、缩进也没了，代码块比没有语法高亮
             * 更严重的问题其实是这个。保留真实换行（另起一个 BRK_CODE 块）
             * 和每一个空格（不折叠）。 */
            if (ch == '\n') { BRFLUSH(); continue; }
            if (ch == '\t') ch = ' ';
            space = 0;
        } else {
            if (ch == '\n' || ch == '\t') ch = ' ';
            if (ch == ' ') {
                if (space) continue;
                space = 1;
            } else {
                space = 0;
            }
        }
        if (in_cell) {
            if (row_buf_len + 1 < sizeof(row_buf)) row_buf[row_buf_len++] = ch;
        } else if (!in_row) {
            /* in_row 但不在任何 <td>/<th> 里的杂散文字（格式不太规范的表格
             * 常见）直接丢弃，不然会混进正常文档流里，位置很怪 */
            if (need_prefix) {
                if (run_sep) { BREMIT(BR_RUN_MARK); run_sep = 0; line_runs++; }
                else line_runs = 1;
                BREMIT(cur_style); BRMETA(); need_prefix = 0;
            }
            BREMIT(ch);
        }
    }
    BRFLUSH();
    out[pos < cap ? pos : cap - 1] = 0;
    if (out_len) *out_len = pos;
#undef BREMIT
#undef BRMETA
#undef BRFLUSH
}

// 把纯文本消息同时写入 browser_page（保存用）与 browser_render（渲染用，单个
// 普通段落块），用于错误提示/初始占位文本，二者始终保持一致。
static void browser_set_plain(gui_state_t *st, const char *msg) {
    uint32_t n = (uint32_t)strlen(msg);
    if (n >= BROWSER_PAGE_CAP) n = BROWSER_PAGE_CAP - 1;
    memcpy(st->browser_page, msg, n);
    st->browser_page[n] = 0;
    st->browser_page_len = n;
    uint32_t rn = n;
    if (rn > BROWSER_PAGE_CAP - 2) rn = BROWSER_PAGE_CAP - 2;
    st->browser_render[0] = (char)BRK_P;
    memcpy(st->browser_render + 1, msg, rn);
    st->browser_render[1 + rn] = 0;
    st->browser_render_len = 1 + rn;
}

static void browser_set_plain2(gui_state_t *st, const char *a, const char *b) {
    char buf[192];
    line2(buf, sizeof(buf), a, b);
    browser_set_plain(st, buf);
}

static void browser_init(gui_state_t *st) {
    if (st->browser_loaded) return;
    strcpy(st->browser_url, "https://example.com/");
    st->browser_url_cursor = (uint32_t)strlen(st->browser_url);
    browser_set_plain(st, "输入网址后按 Enter 加载。当前 HTTPS 支持 TLS 1.3 + ChaCha20-Poly1305；部分网站会自动尝试 HTTP。");
    st->browser_scroll = 0;
    st->browser_loaded = 1;
}

/* 后退/前进历史：定长环状最近 BROWSER_HIST_MAX 条 URL + 当前位置下标。
 * 新导航（非后退/前进触发）会截断当前位置之后的“前进”条目，和标准浏览器
 * 语义一致；满了就整体前移丢最老的一条。 */
static void browser_hist_push(gui_state_t *st, const char *url) {
    if (st->browser_hist_pos < st->browser_hist_count - 1) {
        st->browser_hist_count = st->browser_hist_pos + 1;
    }
    if (st->browser_hist_count >= BROWSER_HIST_MAX) {
        memmove(st->browser_hist[0], st->browser_hist[1],
                (uint32_t)(BROWSER_HIST_MAX - 1) * BROWSER_URL_CAP);
        st->browser_hist_count = BROWSER_HIST_MAX - 1;
    }
    strncpy(st->browser_hist[st->browser_hist_count], url, BROWSER_URL_CAP - 1);
    st->browser_hist[st->browser_hist_count][BROWSER_URL_CAP - 1] = 0;
    st->browser_hist_count++;
    st->browser_hist_pos = st->browser_hist_count - 1;
}

static void browser_load_internal(gui_state_t *st, int push_history) {
    browser_init(st);
    char host[96];
    const char *path = "/";
    uint16_t port = 80;
    int https = 0;
    const char *err = gui_parse_url(st->browser_url, &https, host, sizeof(host), &port, &path);
    if (err) {
        browser_set_plain(st, err);
        st->status = "浏览器 URL 错误";
        return;
    }
    st->status = "浏览器加载中";
    if (!net_primary()->dhcp_ok && net_dhcp() < 0) {
        browser_set_plain2(st, "网络未配置: ", net_last_error());
        st->status = "浏览器网络失败";
        return;
    }
    uint32_t ip = 0;
    if (net_dns_resolve(host, &ip) < 0) {
        browser_set_plain2(st, "DNS 失败: ", net_last_error());
        st->status = "浏览器 DNS 失败";
        return;
    }
    static char response[BROWSER_FETCH_CAP];
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
        browser_set_plain2(st, "加载失败: ", https ? tls_error : net_last_error());
        st->status = "浏览器加载失败";
        return;
    }
    const char *body = http_body_ptr(response);
    browser_text_from_html(body, st->browser_page, BROWSER_PAGE_CAP, &st->browser_page_len);
    browser_render_from_html(st, body, st->browser_render, BROWSER_PAGE_CAP, &st->browser_render_len);
    st->browser_scroll = 0;
    if (!https || strcmp(st->status, "HTTPS 失败，已用 HTTP 回退") != 0)
        st->status = "浏览器加载完成";
    if (push_history) browser_hist_push(st, st->browser_url);
    /* <title> 抓到了就用它做窗口标题（"浏览器 - 页面标题"），跟真实浏览器
     * 标题栏/标签页显示网页标题一样；没有 <title> 就保持默认的"浏览器"。 */
    if (g_browser_page_title[0]) {
        static char win_title[BROWSER_TITLE_CAP + 16];
        uint32_t ap = 0;
        append_str(win_title, sizeof(win_title), &ap, "浏览器 - ");
        append_str(win_title, sizeof(win_title), &ap, g_browser_page_title);
        wm_set_app_title(GUI_APP_BROWSER, win_title);
    } else {
        wm_set_app_title(GUI_APP_BROWSER, "浏览器");
    }
}

static void browser_load(gui_state_t *st) {
    browser_load_internal(st, 1);
}

/* 解析 <a href> 目标：绝对 URL（含 scheme）原样用；"/xxx" 相对当前页面
 * 的 host 补全；其余相对路径相对当前路径的目录部分拼接（不处理 "../"
 * 折叠——真实页面里出现的比例不高，这里只做到“大部分链接能点开”）。
 * "#"/"javascript:"/"mailto:" 之类不导航。 */
static int browser_resolve_href(const gui_state_t *st, const char *href, char *out, uint32_t out_cap) {
    if (!href || !href[0]) return -1;
    if (href[0] == '#') return -1;
    if (strncmp(href, "javascript:", 11) == 0) return -1;
    if (strncmp(href, "mailto:", 7) == 0) return -1;
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        strncpy(out, href, out_cap - 1);
        out[out_cap - 1] = 0;
        return 0;
    }
    const char *cur = st->browser_url;
    const char *scheme_end = strstr(cur, "://");
    if (!scheme_end) return -1;
    const char *host_start = scheme_end + 3;
    const char *host_end = host_start;
    while (*host_end && *host_end != '/') host_end++;
    uint32_t prefix_len = (uint32_t)(host_end - cur); /* "scheme://host[:port]" */
    if (prefix_len >= out_cap) return -1;
    memcpy(out, cur, prefix_len);
    uint32_t pos = prefix_len;
    if (href[0] == '/') {
        for (uint32_t k = 0; href[k] && pos + 1 < out_cap; k++) out[pos++] = href[k];
    } else {
        /* 相对路径：取当前路径最后一个 '/' 之前的部分作为目录前缀 */
        const char *last_slash = host_end;
        for (const char *p = host_end; *p; p++) if (*p == '/') last_slash = p;
        uint32_t dir_len = (uint32_t)(last_slash - host_end) + 1;
        if (dir_len == 0) { out[pos++] = '/'; }
        else {
            for (uint32_t k = 0; k < dir_len && pos + 1 < out_cap; k++) out[pos++] = host_end[k];
        }
        for (uint32_t k = 0; href[k] && pos + 1 < out_cap; k++) out[pos++] = href[k];
    }
    out[pos < out_cap ? pos : out_cap - 1] = 0;
    return 0;
}

static void browser_navigate(gui_state_t *st, const char *url) {
    strncpy(st->browser_url, url, BROWSER_URL_CAP - 1);
    st->browser_url[BROWSER_URL_CAP - 1] = 0;
    st->browser_url_cursor = (uint32_t)strlen(st->browser_url);
    browser_load(st);
}

static void browser_go_back(gui_state_t *st) {
    if (st->browser_hist_pos <= 0) { st->status = "没有更早的页面"; return; }
    st->browser_hist_pos--;
    strncpy(st->browser_url, st->browser_hist[st->browser_hist_pos], BROWSER_URL_CAP - 1);
    st->browser_url[BROWSER_URL_CAP - 1] = 0;
    st->browser_url_cursor = (uint32_t)strlen(st->browser_url);
    browser_load_internal(st, 0);
}

static void browser_go_forward(gui_state_t *st) {
    if (st->browser_hist_pos + 1 >= st->browser_hist_count) { st->status = "没有更新的页面"; return; }
    st->browser_hist_pos++;
    strncpy(st->browser_url, st->browser_hist[st->browser_hist_pos], BROWSER_URL_CAP - 1);
    st->browser_url[BROWSER_URL_CAP - 1] = 0;
    st->browser_url_cursor = (uint32_t)strlen(st->browser_url);
    browser_load_internal(st, 0);
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


/* 链接点击命中区域——每次 draw_rendered_page 重新填充（滚动/窗口大小
 * 变化后旧的矩形就作废了），draw_browser_app 一帧内可能画两遍（滚动钳
 * 位重绘），以最后一次为准，供 hit_browser_link 做命中测试。 */
#define BROWSER_LINK_RECT_MAX 96
typedef struct { int x, y, w, h; int link_idx; } browser_link_rect_t;
static browser_link_rect_t g_browser_link_rects[BROWSER_LINK_RECT_MAX];
static int g_browser_link_rect_count;

static void browser_add_link_rect(int x, int y, int w, int h, int link_idx) {
    if (link_idx < 0 || g_browser_link_rect_count >= BROWSER_LINK_RECT_MAX) return;
    browser_link_rect_t *r = &g_browser_link_rects[g_browser_link_rect_count++];
    r->x = x; r->y = y; r->w = w; r->h = h; r->link_idx = link_idx;
}

/* 画一个块（一段 [start, start+seg_len) 文本，已经解出最终样式 bs 和可选
 * 链接下标）：处理 HR/空行/正常换行折行三种情况，链接块的每一折行都记一条
 * 命中矩形。返回更新后的 row_unit；cy 和 drawn_rows 通过指针原地更新。 */
static int browser_draw_segment(int x, int w, int *cy, int row_unit, int scroll, int max_lines,
                                 int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len,
                                 const browser_style_t *bsp, int link_idx) {
    browser_style_t bs = *bsp;
    int rh = gui_font_line_height() + 3;

    if (bs.is_hr) {
        if (row_unit >= scroll && *drawn_rows < max_lines) {
            rect(x, *cy + rh / 2, w, 2, rgb(70, 90, 105));
            *cy += rh; (*drawn_rows)++;
        }
        return row_unit + 1;
    }
    if (seg_len == 0) {
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        return row_unit + 1;
    }

    int text_x = x + bs.indent + (bs.bullet ? 14 : 0);
    int avail_w = w - bs.indent - (bs.bullet ? 14 : 0);
    int px_per_char = (bs.mono ? MONO_GLYPH_W : 8) * bs.scale;
    int max_cols = avail_w / (px_per_char > 0 ? px_per_char : 8);
    if (max_cols < 6) max_cols = 6;
    if (max_cols > 150) max_cols = 150;

    char line[160];
    uint32_t p = start;
    int first_row = 1;
    while (p < start + seg_len) {
        uint32_t lp = 0;
        while (p < start + seg_len && lp < (uint32_t)max_cols && lp + 1 < sizeof(line))
            line[lp++] = buf[p++];
        line[lp] = 0;

        if (row_unit >= scroll && *drawn_rows < max_lines) {
            int rowh = rh * bs.scale;
            /* 块背景/左侧强调竖线按行画（不是先算好整块高度再画一次），因为
             * 折行数量是边读边定的；每行背景横跨整个可用宽度，不管这一行
             * 文字本身多长——这样代码块/引用块才有"整块从正文里浮出来"的
             * 视觉效果，不是只有文字底下那一小条。 */
            if (bs.has_bg) rect(x, *cy - 1, w, rowh, bs.bg_color);
            if (bs.left_bar) rect(x, *cy - 1, 3, rowh, bs.color);
            int line_x = text_x;
            if (bs.align != 0 && !bs.mono) {
                int tw = text_width(line, bs.scale);
                if (bs.align == 1) line_x = x + (avail_w - tw) / 2 + bs.indent;
                else line_x = x + avail_w - tw;
            }
            if (bs.bullet && first_row)
                text(x + bs.indent, *cy, "•", bs.color, 1);
            if (bs.mono) {
                text_mono(line_x, *cy, line_x + avail_w, line, bs.color);
            } else {
                g_text_italic = bs.italic;
                if (bs.bold) text(line_x + 1, *cy, line, bs.color, bs.scale);
                text(line_x, *cy, line, bs.color, bs.scale);
                g_text_italic = 0;
                if (bs.underline) {
                    int tw = text_width(line, bs.scale);
                    rect(line_x, *cy + rowh - 3, tw, 1, bs.color);
                }
            }
            if (link_idx >= 0) {
                int tw = text_width(line, bs.scale);
                browser_add_link_rect(line_x, *cy, tw, rowh, link_idx);
            }
            *cy += rowh;
            (*drawn_rows)++;
        }
        row_unit += bs.scale;
        first_row = 0;
    }
    if (bs.gap_after) {
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        row_unit++;
    }
    return row_unit;
}

/* 解码一个 [type][可选 BR_META] 前缀：块基础样式 + meta 覆盖 + 链接下标。
 * 返回消费后的新下标。 */
static uint32_t br_read_style(const char *buf, uint32_t len, uint32_t i, int type,
                              browser_style_t *bs, int *link_idx) {
    *link_idx = -1;
    browser_style_get(type, bs);
    if (i < len && (unsigned char)buf[i] == BR_META_MARK && i + BR_META_LEN <= len) {
        uint8_t flags = (uint8_t)buf[i + 1];
        uint8_t r = (uint8_t)buf[i + 2], g = (uint8_t)buf[i + 3], b = (uint8_t)buf[i + 4];
        uint8_t link_byte = (uint8_t)buf[i + 5];
        uint8_t bg_r = (uint8_t)buf[i + 6], bg_g = (uint8_t)buf[i + 7], bg_b = (uint8_t)buf[i + 8];
        uint8_t flags2 = (uint8_t)buf[i + 9];
        i += BR_META_LEN;
        if (flags & BR_FLAG_COLOR) bs->color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        if (flags & BR_FLAG_BOLD_ON) bs->bold = 1;
        if (flags2 & BR_FLAG2_ITALIC_ON) bs->italic = 1;
        if (flags & BR_FLAG_UNDER_ON) bs->underline = 1;
        if (flags & BR_FLAG_SCALE2) bs->scale = 2;
        if (flags & BR_FLAG_BG) { bs->has_bg = 1; bs->bg_color = ((uint32_t)bg_r << 16) | ((uint32_t)bg_g << 8) | bg_b; }
        if (flags & BR_FLAG_ALIGN_CENTER) bs->align = 1;
        if (flags & BR_FLAG_ALIGN_RIGHT) bs->align = 2;
        if (flags & BR_FLAG_EXTRA_GAP) bs->gap_after = 1;
        if (link_byte) *link_idx = link_byte - 1;
    }
    return i;
}

/* 一个块内的一段同样式文本（见 BR_RUN_MARK）。 */
typedef struct {
    browser_style_t bs;
    int link_idx;
    uint32_t start, len;
} br_run_t;

/* 宽松的 UTF-8 解码：只需要"是不是 CJK"和字节数，不做严格校验。 */
static uint32_t br_utf8_next(const char *s, uint32_t pos, uint32_t end, uint32_t *cp_out) {
    uint8_t c = (uint8_t)s[pos];
    uint32_t n = 1, cp = c;
    if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
    if (pos + n > end) n = 1;
    if (n == 2) cp = ((uint32_t)(c & 0x1F) << 6) | ((uint8_t)s[pos + 1] & 0x3F);
    else if (n == 3) cp = ((uint32_t)(c & 0x0F) << 12) |
                          (((uint32_t)((uint8_t)s[pos + 1] & 0x3F)) << 6) |
                          ((uint8_t)s[pos + 2] & 0x3F);
    else if (n == 4) cp = 0x20000; /* 增补平面基本都是表意文字，按 CJK 处理 */
    *cp_out = cp;
    return n;
}

/* CJK/全角区（可以在任意字之后折行，不需要空格分词） */
static int br_cp_is_cjk(uint32_t cp) { return cp >= 0x2E80; }

/* 画一个已排好位置的词：粗体双描边 / 斜体 / 下划线 / 链接命中矩形。
 * py 是行顶；不同字号的运行落在同一行时按行底对齐（近似基线对齐）。 */
static void br_draw_tok(int px, int py, int rowh, int rh, const char *tok, int tw,
                        const browser_style_t *bs, int link_idx) {
    int ty_tok = py + (rowh - rh * bs->scale);
    g_text_italic = bs->italic;
    if (bs->bold) text(px + 1, ty_tok, tok, bs->color, bs->scale);
    text(px, ty_tok, tok, bs->color, bs->scale);
    g_text_italic = 0;
    if (bs->underline) rect(px, py + rowh - 3, tw, 1, bs->color);
    if (link_idx >= 0) browser_add_link_rect(px, py, tw, rowh, link_idx);
}

/* 流式排版一个块：把块内所有运行的词依次从左到右排，超宽按词折行（CJK
 * 按字折行），单个超长词按码点硬切。这是"行内元素不再各占一行 + 单词不再
 * 被从中间劈开"两件事的共同实现——之前 browser_draw_segment 按固定列数
 * 每 max_cols 个字节切一刀。行高取块内最大字号（标题里嵌行内元素时整行
 * 用大行高，小字号运行底对齐）。 */
static int browser_flow_block(int x, int w, int *cy, int row_unit, int scroll, int max_lines,
                              int *drawn_rows, const char *buf, const br_run_t *runs, int nrun) {
    int rh = gui_font_line_height() + 3;
    const browser_style_t *base = &runs[0].bs;
    int maxscale = 1, any_text = 0;
    for (int r = 0; r < nrun; r++) {
        if (runs[r].bs.scale > maxscale) maxscale = runs[r].bs.scale;
        if (runs[r].len) any_text = 1;
    }
    if (!any_text) { /* 空块 = 空一行（段落间距等机制依赖这个行为） */
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        return row_unit + 1;
    }
    int rowh = rh * maxscale;
    int text_x0 = x + base->indent + (base->bullet ? 14 : 0);
    int x_right = x + w - 6;
    int avail = x_right - text_x0;
    int pen = text_x0;
    int row_open = 0, first_row = 1;
    char tok[64];

#define BRF_VIS() (row_unit >= scroll && *drawn_rows < max_lines)
#define BRF_ROW_BEGIN() do { if (!row_open) { row_open = 1; if (BRF_VIS()) { \
            if (base->has_bg) rect(x, *cy - 1, w, rowh, base->bg_color); \
            if (base->left_bar) rect(x, *cy - 1, 3, rowh, base->color); \
            if (base->bullet && first_row) text(x + base->indent, *cy, "•", base->color, 1); \
        } } } while (0)
#define BRF_ROW_END() do { if (BRF_VIS()) { *cy += rowh; (*drawn_rows)++; } \
        row_unit += maxscale; row_open = 0; first_row = 0; pen = text_x0; } while (0)

    for (int r = 0; r < nrun; r++) {
        const browser_style_t *bs = &runs[r].bs;
        uint32_t p = runs[r].start, endp = runs[r].start + runs[r].len;
        int space_w = gui_cp_advance(' ', bs->scale);
        while (p < endp) {
            if (buf[p] == ' ') {
                p++;
                /* 行首空格吞掉；行内空格推进画笔（背景色已整行铺开，无须画） */
                if (row_open && pen > text_x0 && pen + space_w <= x_right) pen += space_w;
                continue;
            }
            /* 取一个词：连续非空格；CJK 单字自成一词（无空格也能折行） */
            uint32_t tl = 0, q = p;
            while (q < endp && buf[q] != ' ' && tl + 4 < sizeof(tok)) {
                uint32_t cp, n = br_utf8_next(buf, q, endp, &cp);
                if (br_cp_is_cjk(cp)) {
                    if (tl == 0) { memcpy(tok + tl, buf + q, n); tl += n; q += n; }
                    break;
                }
                memcpy(tok + tl, buf + q, n); tl += n; q += n;
            }
            tok[tl] = 0;
            int tw = text_width(tok, bs->scale);
            if (pen + tw > x_right && pen > text_x0) BRF_ROW_END();
            /* 单词本身超过整行宽度：按码点硬切成多行（长 URL/长标识符） */
            while (tw > avail && tl > 0) {
                uint32_t fit = 0; int fw = 0;
                while (fit < tl) {
                    uint32_t cp, n = br_utf8_next(tok, fit, tl, &cp);
                    char one[8];
                    memcpy(one, tok + fit, n); one[n] = 0;
                    int cw = text_width(one, bs->scale);
                    if (fw + cw > avail && fit > 0) break;
                    fw += cw; fit += n;
                }
                if (fit >= tl) break; /* 剩余部分能放下了，走正常绘制 */
                char save = tok[fit]; tok[fit] = 0;
                BRF_ROW_BEGIN();
                if (BRF_VIS()) br_draw_tok(pen, *cy, rowh, rh, tok, fw, bs, runs[r].link_idx);
                tok[fit] = save;
                memmove(tok, tok + fit, tl - fit);
                tl -= fit; tok[tl] = 0;
                tw = text_width(tok, bs->scale);
                BRF_ROW_END();
            }
            BRF_ROW_BEGIN();
            if (BRF_VIS() && tl) br_draw_tok(pen, *cy, rowh, rh, tok, tw, bs, runs[r].link_idx);
            pen += tw;
            p = q;
        }
    }
    if (row_open) BRF_ROW_END();
    if (base->gap_after) {
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        row_unit++;
    }
    return row_unit;
#undef BRF_VIS
#undef BRF_ROW_BEGIN
#undef BRF_ROW_END
}

/* 取 BRK_TROW 行文本里第 col 个单元格（BR_CELL_MARK 分隔）；最后一列
 * 把超出列数上限的分隔符当空格吞掉，内容不丢。返回字节长度。 */
static uint32_t br_table_cell(const char *buf, uint32_t start, uint32_t seg_len, int col,
                              char *out, uint32_t out_cap) {
    uint32_t p = start, end = start + seg_len;
    int c = 0;
    while (p < end && c < col) {
        if ((unsigned char)buf[p] == BR_CELL_MARK) c++;
        p++;
    }
    if (c < col) { out[0] = 0; return 0; }
    uint32_t n = 0;
    int merge_rest = (col == BR_TBL_COLS - 1);
    while (p < end && n + 1 < out_cap) {
        if ((unsigned char)buf[p] == BR_CELL_MARK) {
            if (!merge_rest) break;
            out[n++] = ' ';
            p++;
            continue;
        }
        out[n++] = buf[p++];
    }
    out[n] = 0;
    return n;
}

/* 跨连续 BRK_TROW 块统一计算列宽：每列取所有行里最宽的单元格 + 内边距，
 * 总宽超出可用宽度时按比例压缩（列内文字由 text_clipped 截断）。这是
 * "真正的表格列对齐"的核心——单看一行不可能知道别的行有多宽。 */
static void br_table_scan(const char *buf, uint32_t len, uint32_t pos, int avail_w,
                          int *col_w, int *ncols_out) {
    for (int c = 0; c < BR_TBL_COLS; c++) col_w[c] = 0;
    int ncols = 0;
    char cell[96];
    while (pos < len && (unsigned char)buf[pos] == BRK_TROW) {
        pos++;
        browser_style_t dummy; int li;
        pos = br_read_style(buf, len, pos, BRK_TROW, &dummy, &li);
        uint32_t start = pos;
        while (pos < len && buf[pos] != '\n') pos++;
        uint32_t seg_len = pos - start;
        if (pos < len) pos++;
        for (int c = 0; c < BR_TBL_COLS; c++) {
            uint32_t n = br_table_cell(buf, start, seg_len, c, cell, sizeof(cell));
            if (!n && c > 0) break;
            int w = text_width(cell, 1) + 14;
            if (w > col_w[c]) col_w[c] = w;
            if (c + 1 > ncols) ncols = c + 1;
        }
    }
    if (ncols < 1) ncols = 1;
    int total = 0;
    for (int c = 0; c < ncols; c++) total += col_w[c];
    if (total > avail_w && total > 0) {
        for (int c = 0; c < ncols; c++) {
            col_w[c] = col_w[c] * avail_w / total;
            if (col_w[c] < 36) col_w[c] = 36;
        }
    }
    *ncols_out = ncols;
}

/* 画一个表格行：单元格按列起点对齐（text_clipped 截断超宽内容）、列间
 * 细竖线、行下横线；表头行（meta 加粗）加底纹。 */
static int br_draw_table_row(int x, int *cy, int row_unit, int scroll, int max_lines,
                             int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len,
                             const browser_style_t *bs, const int *col_w, int ncols,
                             int last_row) {
    int rh = gui_font_line_height() + 3;
    int total_w = 0;
    for (int c = 0; c < ncols; c++) total_w += col_w[c];
    if (row_unit >= scroll && *drawn_rows < max_lines) {
        uint32_t grid_col = rgb(58, 74, 90);
        if (bs->bold) rect(x, *cy - 1, total_w, rh, rgb(34, 44, 56));
        int cx = x;
        char cell[128];
        for (int c = 0; c < ncols; c++) {
            uint32_t n = br_table_cell(buf, start, seg_len, c, cell, sizeof(cell));
            if (n) {
                int clip_r = cx + col_w[c] - 6;
                if (bs->bold) text_clipped(cx + 7, *cy, clip_r, cell, bs->color, 1);
                text_clipped(cx + 6, *cy, clip_r, cell, bs->color, 1);
            }
            if (c > 0) rect(cx, *cy - 1, 1, rh, grid_col);
            cx += col_w[c];
        }
        rect(x, *cy - 1, 1, rh, grid_col);
        rect(cx, *cy - 1, 1, rh, grid_col);
        rect(x, *cy + rh - 1, total_w + 1, 1, grid_col);
        if (row_unit == scroll || *drawn_rows == 0)
            rect(x, *cy - 1, total_w + 1, 1, grid_col);
        *cy += rh;
        (*drawn_rows)++;
    }
    row_unit++;
    if (last_row) { /* 表格结束后留一行空隙，和段落间距一致 */
        if (row_unit >= scroll && *drawn_rows < max_lines) { *cy += rh; (*drawn_rows)++; }
        row_unit++;
    }
    return row_unit;
}

// 按块类型渲染标记流（见 browser_render_from_html）：标题更大更亮、链接带
// 下划线、列表带圆点、代码用等宽字体、<hr> 画分隔线——而非纯文本平铺。
// 先把一个块解成运行数组（[type][meta?] 文本 { 0x02 [type][meta?] 文本 }*），
// 正常文本块交给 browser_flow_block 流式排版；代码块（mono，要保留空格逐字
// 排布）、HR、带对齐的单运行块走原来的整行路径。
// 返回内容总行数（row-unit 计），供调用方钳制滚动量。
static int draw_rendered_page(int x, int y, int w, int h, const char *buf, uint32_t len, int scroll) {
    g_browser_link_rect_count = 0;
    if (!len) return 0;
    int rh = gui_font_line_height() + 3;
    int max_lines = h / rh;
    if (max_lines < 1) max_lines = 1;
    int row_unit = 0;
    int drawn_rows = 0;
    int cy = y;
    uint32_t i = 0;
    int tbl_active = 0, tbl_ncols = 0;
    int tbl_col_w[BR_TBL_COLS];
    while (i < len) {
        uint32_t blk_pos = i;
        int type = (unsigned char)buf[i++];
        br_run_t runs[BR_RUN_MAX];
        int nrun = 1;
        i = br_read_style(buf, len, i, type, &runs[0].bs, &runs[0].link_idx);
        runs[0].start = i; runs[0].len = 0;
        while (i < len && buf[i] != '\n') {
            if ((unsigned char)buf[i] == BR_RUN_MARK && i + 1 < len && nrun < BR_RUN_MAX) {
                runs[nrun - 1].len = i - runs[nrun - 1].start;
                i++;
                int t2 = (unsigned char)buf[i++];
                i = br_read_style(buf, len, i, t2, &runs[nrun].bs, &runs[nrun].link_idx);
                runs[nrun].start = i; runs[nrun].len = 0;
                nrun++;
            } else {
                i++;
            }
        }
        runs[nrun - 1].len = i - runs[nrun - 1].start;
        if (i < len) i++;
        /* 多行代码块的行间不留空行：<pre> 的每一行是独立的 BRK_CODE 块
         * （见发射端），gap_after 只该出现在整个代码块结束后，否则代码
         * 变成隔行双倍行距、背景条也断开。 */
        if (type == BRK_CODE && i < len && (unsigned char)buf[i] == BRK_CODE)
            for (int r = 0; r < nrun; r++) runs[r].bs.gap_after = 0;
        if (type == BRK_TROW) {
            /* 表格：第一行时向前扫完整张表算列宽，之后每行按列对齐画 */
            if (!tbl_active) {
                br_table_scan(buf, len, blk_pos, w - 10, tbl_col_w, &tbl_ncols);
                tbl_active = 1;
            }
            int last_row = !(i < len && (unsigned char)buf[i] == BRK_TROW);
            row_unit = br_draw_table_row(x, &cy, row_unit, scroll, max_lines, &drawn_rows,
                                         buf, runs[0].start, runs[0].len, &runs[0].bs,
                                         tbl_col_w, tbl_ncols, last_row);
            if (last_row) tbl_active = 0;
            continue;
        }
        if (runs[0].bs.is_hr || runs[0].bs.mono || (nrun == 1 && runs[0].bs.align != 0)) {
            for (int r = 0; r < nrun; r++)
                row_unit = browser_draw_segment(x, w, &cy, row_unit, scroll, max_lines, &drawn_rows,
                                                buf, runs[r].start, runs[r].len, &runs[r].bs,
                                                runs[r].link_idx);
        } else {
            row_unit = browser_flow_block(x, w, &cy, row_unit, scroll, max_lines, &drawn_rows,
                                          buf, runs, nrun);
        }
    }
    return row_unit;
}

static void draw_browser_app(int tx, int ty, int win_w, int win_h, gui_state_t *st) {
    browser_init(st);
    int view_w = win_w - 72;
    if (view_w < 320) view_w = 320;
    /* 内容框跟着窗口高度走，不再是固定 196px——之前窗口最大化后底下一大片
     * 都是空的，只有顶上一小条固定区域在显示网页内容，用户反馈里点名说
     * "浏览器做得非常不好"就是这个。74 是其它 app（比如任务管理器）也在用
     * 的窗口顶部/底部固定内边距（ty 相对 win_y 的 42px 偏移 + 32px 底部
     * 留白），content_box_h 再减去内容框起始点相对 ty 的 146px 偏移。 */
    int content_box_h = win_h - 74 - 146;
    if (content_box_h < 80) content_box_h = 80;
    const net_device_t *dev = net_primary();
    char ipbuf[16];
    net_ipv4_to_str(dev->ip, ipbuf);
    text(tx, ty, "浏览器", rgb(78, 192, 236), 1);
    text(tx, ty + 30, "Enter加载  点击链接跳转  ↑↓滚动  Ctrl+S保存", rgb(148, 162, 174), 1);
    rect(tx, ty + 58, view_w, 32, rgb(6, 14, 22));
    border(tx, ty + 58, view_w, 32, rgb(78, 192, 236));
    text(tx + 10, ty + 68, st->browser_url, rgb(232, 242, 248), 1);
    // 闪烁插入光标：跟随实际光标位置（左右键可移动），超出输入框宽度时钳制在
    // 框内，避免长网址把光标画到边框外面。
    {
        uint32_t cur = st->browser_url_cursor;
        uint32_t n = (uint32_t)strlen(st->browser_url);
        if (cur > n) cur = n;
        char prefix[BROWSER_URL_CAP];
        memcpy(prefix, st->browser_url, cur);
        prefix[cur] = 0;
        int caret_x = tx + 10 + text_width(prefix, 1);
        if (caret_x > tx + view_w - 10) caret_x = tx + view_w - 10;
        static uint32_t browser_caret_ticks = 0;
        browser_caret_ticks++;
        if ((browser_caret_ticks / 15) % 2) {
            rect(caret_x + 1, ty + 66, 2, 18, rgb(78, 192, 236));
        }
    }
    draw_small_button(tx, ty + 104, 96, "Enter 加载", rgb(78, 192, 236));
    draw_small_button(tx + 108, ty + 104, 68, "保存", rgb(85, 180, 120));
    int can_back = st->browser_hist_pos > 0;
    int can_fwd  = st->browser_hist_pos + 1 < st->browser_hist_count;
    draw_small_button(tx + 178, ty + 104, 44, "后退",
                      can_back ? rgb(120, 150, 180) : rgb(60, 68, 78));
    draw_small_button(tx + 224, ty + 104, 44, "前进",
                      can_fwd ? rgb(120, 150, 180) : rgb(60, 68, 78));
    char line[128];
    uint32_t pos = 0;
    line[0] = 0;
    append_str(line, sizeof(line), &pos, dev->dhcp_ok ? "网络 " : "未配置 ");
    append_str(line, sizeof(line), &pos, net_driver_name(dev->driver));
    append_str(line, sizeof(line), &pos, " ");
    append_str(line, sizeof(line), &pos, ipbuf);
    append_str(line, sizeof(line), &pos, "  ");
    append_str(line, sizeof(line), &pos, st->status ? st->status : "浏览器就绪");
    text_clipped(tx, ty + 136, tx + view_w - 8, line, rgb(168, 190, 204), 1);
    /* 内容区：先画，若发现滚动越过末尾则钳回并重画一遍（含背景）。 */
    int inner_h = content_box_h - 24;
    int total = 0, max_scroll = 0, visible_rows = 0;
    for (int pass = 0; pass < 2; pass++) {
        rect(tx, ty + 146, view_w, content_box_h, rgb(4, 9, 14));
        border(tx, ty + 146, view_w, content_box_h, rgb(50, 74, 90));
        total = draw_rendered_page(tx + 12, ty + 158, view_w - 24, inner_h,
                                   st->browser_render, st->browser_render_len,
                                   st->browser_scroll);
        int rh = gui_font_line_height() + 3;
        visible_rows = inner_h / rh;
        max_scroll = total - visible_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (st->browser_scroll <= max_scroll) break;
        st->browser_scroll = max_scroll;
    }
    /* 滚动条：内容超出可视区域才画（跟真实浏览器一样，装得下就不显示），
     * 右边缘一条细槽 + 一段按比例算高度/位置的滑块，让人一眼知道页面
     * 还剩多少、看到哪了——之前完全没有任何视觉反馈，只能凭感觉滚。 */
    if (total > visible_rows && visible_rows > 0) {
        int sb_x = tx + view_w - 8;
        int sb_y = ty + 146 + 2;
        int sb_h = content_box_h - 4;
        rect(sb_x, sb_y, 4, sb_h, rgb(20, 26, 34));
        int thumb_h = sb_h * visible_rows / total;
        if (thumb_h < 16) thumb_h = 16;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int thumb_y = sb_y;
        if (max_scroll > 0) thumb_y += (sb_h - thumb_h) * st->browser_scroll / max_scroll;
        rect(sb_x, thumb_y, 4, thumb_h, rgb(90, 120, 145));
    }
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

static void draw_panel_window(int tx, int ty, int win_w, int win_h, int w, int h, gui_state_t *st, int panel) {
    if (panel == PANEL_FILES) draw_files_panel(tx, ty, win_w, win_h, st);
    else if (panel == PANEL_DISK) draw_disk_panel(tx, ty, win_w);
    else if (panel == PANEL_SYS) draw_resource_panel(tx, ty, win_w, w, h);
    else draw_apps_panel(tx, ty, win_w, st);
}

static void draw_app_window_body(int tx, int ty, int win_w, int win_h, gui_state_t *st, int mode) {
    if (gui_app_draw(st, mode, tx, ty, win_w, win_h)) return;
    if (mode == GUI_APP_NOTES) draw_notes_app(tx, ty, win_w, st);
    else if (mode == GUI_APP_UWC) draw_uwc_app(tx, ty, st);
    else if (mode == GUI_APP_SNAKE) draw_snake_app(tx, ty, st);
    else if (mode == GUI_APP_BROWSER) draw_browser_app(tx, ty, win_w, win_h, st);
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
        draw_panel_window(tx, ty, win_w, win_h, w, h, st, win->mode);
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
#define TB_BTN ui_s(46)
#define TB_GAP ui_s(8)
#define TBHIT_NONE  -1
#define TBHIT_START -2
#define TBHIT_CLOCK -3        /* 时钟/日期区域 → 日历弹窗 */
#define TBHIT_SHOWDESK -4     /* 最右侧竖条 → 显示桌面 */
#define TBHIT_BRIGHTNESS -5   /* 亮度图标 → 亮度滑杆弹窗 */
#define TBHIT_WIN_BASE 100
#define TB_SHOWDESK_W ui_s(10)

/* 亮度图标 + 滑杆弹窗的几何布局，绘制和点击测试共用，避免两处数字不一致。
 * 图标贴在显示桌面竖条的左边，时钟/日期文字再贴在图标左边——从右到左依次
 * 是 [显示桌面竖条][亮度图标][时钟/日期]。 */
#define BR_ICON_SZ  ui_s(22)
#define BR_ICON_GAP ui_s(10)
#define BR_POPUP_W  ui_s(200)
#define BR_POPUP_H  ui_s(78)
#define BR_MIN 20
#define BR_MAX 100

static void brightness_icon_xy(int w, int h, int *bx, int *by) {
    *bx = w - TB_SHOWDESK_W - ui_s(10) - BR_ICON_SZ;
    *by = h - TASKBAR_H + (TASKBAR_H - BR_ICON_SZ) / 2;
}

static void brightness_popup_xy(int w, int h, int *px, int *py) {
    *px = w - 16 - BR_POPUP_W;
    *py = h - TASKBAR_H - BR_POPUP_H - 8;
}

/* 滑杆轨道范围（弹窗内边距 18px），返回轨道左右端点 x 与竖直位置 y。 */
static void brightness_track_xy(int px, int py, int *tx0, int *tx1, int *ty) {
    *tx0 = px + ui_s(18);
    *tx1 = px + BR_POPUP_W - ui_s(18);
    *ty = py + ui_s(52);
}

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

// HBOS 品牌标志：字母 "H" 造型（取代 Win11 四方格），用于开始按钮/欢迎窗口等处。
// x,y,size 为外框左上角与边长；纯 rect 拼接，任意缩放不失真。
static void draw_hbos_logo(int x, int y, int size, uint32_t color) {
    int bar_w = size * 3 / 10;
    if (bar_w < 2) bar_w = 2;
    int mid_h = size * 2 / 10;
    if (mid_h < 2) mid_h = 2;
    int right_x = x + size - bar_w;
    rect(x, y, bar_w, size, color);
    rect(right_x, y, bar_w, size, color);
    int mid_y = y + (size - mid_h) / 2;
    rect(x + bar_w, mid_y, right_x - (x + bar_w), mid_h, color);
}

// 用户头像图标：外圈圆形底色 + 头部圆 + 肩部圆（被外圈裁切，只露出顶部弧线），
// 比例取自用户提供的参考图。x,y,size 为外接正方形左上角与边长。
static void draw_user_avatar(int x, int y, int size, uint32_t bg, uint32_t fg) {
    int r = size / 2;
    int cx = x + r, cy = y + r;
    int head_r  = r * 44 / 100;
    int head_cy = cy - r * 20 / 100;
    int body_r  = r * 62 / 100;
    int body_cy = cy + r * 58 / 100;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;  // outside outer circle: transparent
            int px = cx + dx, py = cy + dy;
            int hdy = py - head_cy;
            int bdy = py - body_cy;
            uint32_t c = bg;
            if (dx * dx + hdy * hdy <= head_r * head_r) c = fg;
            else if (dx * dx + bdy * bdy <= body_r * body_r) c = fg;
            rect(px, py, 1, 1, c);
        }
    }
}

// 亮度图标：太阳造型（实心圆 + 8 条短射线）。x,y,size 为外接正方形左上角与边长。
static void draw_brightness_icon(int x, int y, int size, uint32_t color) {
    int r = size / 2;
    int cx = x + r, cy = y + r;
    int core_r = r * 45 / 100;
    gui_fill_circle(cx, cy, core_r, color);
    int ray_len = r * 30 / 100;
    int ray_gap = r * 20 / 100;
    static const int DX8[8] = { 0, 100, 100, 100, 0, -100, -100, -100 };
    static const int DY8[8] = { -100, -100, 0, 100, 100, 100, 0, -100 };
    for (int i = 0; i < 8; i++) {
        int ux = DX8[i], uy = DY8[i];
        /* 归一化对角方向，避免斜射线比正射线长 */
        int norm = (ux != 0 && uy != 0) ? 71 : 100;
        int x0 = cx + ux * (core_r + ray_gap) / 100 * norm / 100;
        int y0 = cy + uy * (core_r + ray_gap) / 100 * norm / 100;
        int x1 = cx + ux * (core_r + ray_gap + ray_len) / 100 * norm / 100;
        int y1 = cy + uy * (core_r + ray_gap + ray_len) / 100 * norm / 100;
        gui_draw_thick_line(x0, y0, x1, y1, 2, color);
    }
}

/* 任务栏亮度弹窗：标题 + 百分比 + 可拖动滑杆。 */
static void draw_brightness_popup(int w, int h, const gui_state_t *st) {
    int light = st->theme_light;
    int px, py;
    brightness_popup_xy(w, h, &px, &py);
    soft_shadow(px - 2, py - 2, BR_POPUP_W + 4, BR_POPUP_H + 4);
    uint32_t bg = light ? 0xF6F5F6F8 : 0xF6202428;
    fill_round_rect(px, py, BR_POPUP_W, BR_POPUP_H, 10, bg, RR_ALL);
    uint32_t bd = light ? rgb(210, 214, 218) : rgb(52, 60, 68);
    border(px, py, BR_POPUP_W, BR_POPUP_H, bd);

    char line[24];
    uint32_t pos = 0; line[0] = 0;
    append_str(line, sizeof(line), &pos, "亮度  ");
    append_uint(line, sizeof(line), &pos, (uint32_t)st->brightness);
    append_char(line, sizeof(line), &pos, '%');
    text(px + 18, py + 14, line, light ? rgb(40, 44, 48) : rgb(220, 226, 232), 1);

    int tx0, tx1, ty;
    brightness_track_xy(px, py, &tx0, &tx1, &ty);
    uint32_t track_bg = light ? rgb(214, 218, 222) : rgb(52, 60, 68);
    fill_round_rect(tx0, ty - 3, tx1 - tx0, 6, 3, track_bg, RR_ALL);
    int fill_w = (tx1 - tx0) * (st->brightness - BR_MIN) / (BR_MAX - BR_MIN);
    if (fill_w > 0)
        fill_round_rect(tx0, ty - 3, fill_w, 6, 3, rgb(61, 174, 233), RR_ALL);
    int thumb_x = tx0 + fill_w;
    gui_fill_circle(thumb_x, ty, 8, rgb(255, 255, 255));
    gui_fill_circle(thumb_x, ty, 6, rgb(61, 174, 233));
}

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
            int lsz = ui_s(18);
            draw_hbos_logo(rx + (TB_BTN - lsz) / 2, ry + (TB_BTN - lsz) / 2, lsz, accent);
        } else if (i <= TB_PIN_COUNT) {
            int panel = g_taskbar_pins[i - 1].panel;
            int isz = ui_s(30), ix = rx + (TB_BTN - isz) / 2, iy = ry + (TB_BTN - isz) / 2;
            blit_icon(ix, iy, isz, panel_icon_id(panel));
        } else {
            int slot = wins[i - 1 - TB_PIN_COUNT];
            wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, slot);
            int act = (slot == st->wm.active_window);
            int isz = ui_s(30), ix = rx + (TB_BTN - isz) / 2, iy = ry + (TB_BTN - isz) / 2 - 2;
            if (win->kind == WM_WIN_PANEL)
                blit_icon(ix, iy, isz, panel_icon_id(win->mode));
            else
                blit_icon(ix, iy, isz, app_icon_id(win->mode));
            rect(rx + TB_BTN / 2 - 7, ry + TB_BTN - 1, 14, 3,
                 act ? accent : (light ? rgb(150, 154, 158) : rgb(150, 155, 160)));
        }
    }

    /* 亮度图标：贴在显示桌面竖条左边 */
    int bix, biy;
    brightness_icon_xy(w, h, &bix, &biy);
    if (st->brightness_popup_open)
        fill_round_rect(bix - 4, biy - 4, BR_ICON_SZ + 8, BR_ICON_SZ + 8, 8, 0x44FFFFFF, RR_ALL);
    draw_brightness_icon(bix, biy, BR_ICON_SZ, light ? cyber_text(1) : rgb(235, 238, 242));

    /* Win11 风格：时间在上、日期在下，各自右对齐，给亮度图标+显示桌面竖条让出空间 */
    char tline[32], dline[40];
    time_line(tline, sizeof(tline), st->taskbar_show_seconds);
    date_line(dline, sizeof(dline));
    uint32_t tcol = light ? cyber_text(1) : rgb(235, 238, 242);
    int lh = gui_font_line_height();
    int tw = text_width(tline, 1);
    int dw = text_width(dline, 1);
    int total_h = lh * 2;
    int top = h - TASKBAR_H + (TASKBAR_H - total_h) / 2;
    int clock_right = bix - BR_ICON_GAP;
    text(clock_right - tw, top,      tline, tcol, 1);
    text(clock_right - dw, top + lh, dline, tcol, 1);

    /* 显示桌面竖条（Win11 任务栏最右角）：一条分隔线提示可点 */
    rect(w - TB_SHOWDESK_W, h - TASKBAR_H + ui_s(8), 1, TASKBAR_H - ui_s(16),
         light ? rgb(150, 156, 162) : rgb(90, 98, 106));
}

static int taskbar_hit(int w, int h, const gui_state_t *st, int mx, int my) {
    if (my < h - TASKBAR_H) return TBHIT_NONE;

    /* 最右侧显示桌面竖条：整个任务栏高度都算命中 */
    if (mx >= w - TB_SHOWDESK_W) return TBHIT_SHOWDESK;

    /* 亮度图标 */
    int bix, biy;
    brightness_icon_xy(w, h, &bix, &biy);
    if (mx >= bix - 4 && mx < bix + BR_ICON_SZ + 4 &&
        my >= biy - 4 && my < biy + BR_ICON_SZ + 4)
        return TBHIT_BRIGHTNESS;

    /* 时钟/日期区域 → 日历弹窗（按实际文本宽度计算命中区，
     * 秒数开关与 draw_taskbar 保持一致，命中区和显示区才对得上；
     * 右边界贴着亮度图标，不与其重叠） */
    {
        char tline[32], dline[40];
        time_line(tline, sizeof(tline), st->taskbar_show_seconds);
        date_line(dline, sizeof(dline));
        int tw = text_width(tline, 1);
        int dw = text_width(dline, 1);
        int zone = (tw > dw ? tw : dw) + ui_s(20);
        if (mx >= bix - BR_ICON_GAP - zone) return TBHIT_CLOCK;
    }

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

/* ── 并发应用窗口合成 ───────────────────────────────────────── */
#define APPWIN_TITLE_H ui_s(26)

/* 把应用窗口表面（不透明）贴到桌面背缓冲 */
static void blit_app_surface(int dx, int dy, const uint32_t *src, int sw, int sh) {
    if (!g_gui_surface || !src) return;
    for (int yy = 0; yy < sh; yy++) {
        int py = dy + yy;
        if (py < 0 || py >= g_gui_surface_h) continue;
        uint32_t *drow = g_gui_surface + (uint32_t)py * g_gui_surface_pitch;
        const uint32_t *srow = src + (uint32_t)yy * sw;
        for (int xx = 0; xx < sw; xx++) {
            int px = dx + xx;
            if (px < 0 || px >= g_gui_surface_w) continue;
            drow[px] = srow[xx] | 0xFF000000;
        }
    }
}

/* 画一个应用窗口：标题栏（标题 + 关闭按钮）+ 内容表面 + 边框 */
static void draw_one_app_window(winsrv_window_t *win) {
    int wx = win->x, wy = win->y, ww = win->w, wh = win->h;
    int th = APPWIN_TITLE_H;
    soft_shadow(wx - 2, wy - th - 2, ww + 4, wh + th + 6);
    fill_round_rect(wx, wy - th, ww, th, 6, rgb(34, 40, 48), RR_TOP);
    text(wx + 10, wy - th + (th - gui_font_line_height()) / 2,
         win->title[0] ? win->title : "应用", rgb(235, 240, 245), 1);
    /* 关闭按钮 */
    int cbx = wx + ww - 20;
    text(cbx, wy - th + (th - gui_font_line_height()) / 2, "✕", rgb(240, 120, 120), 1);
    /* 内容 */
    blit_app_surface(wx, wy, win->surface, ww, wh);
    border(wx, wy, ww, wh, rgb(52, 62, 74));
}

static void draw_app_windows(void) {
    for (int i = 0; i < WINSRV_MAX; i++) {
        winsrv_window_t *win = winsrv_get(i);
        if (win) draw_one_app_window(win);
    }
}

static void draw_gui_screen(int w, int h, gui_state_t *st) {
    gui_sync_focus(st);
    draw_desktop(w, h, st);
    draw_app_windows();
    draw_start_menu(st);
    if (st->brightness_popup_open) draw_brightness_popup(w, h, st);
    draw_calendar_popup(w, h, st);
    draw_window_switcher(w, h, st);
    if (st->splash_ticks > 0)
        draw_splash_window(w, h, st->splash_ticks, st->theme_light);
    draw_context_menu(st);
    draw_toast(w, h, st);
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
    // Largest cursor glyph (horizontal resize) spans x..x+23, y..y+12; the
    // damage box must fully cover the widest/tallest of all glyph variants
    // (incl. the -2 top/left margin) or stray pixels leave a dotted trail.
    gui_dirty_add(omx - 2, omy - 2, 28, 27);
    gui_dirty_add(nmx - 2, nmy - 2, 28, 27);
}

static int cursor_overlaps_rect(int mx, int my, int rx, int ry, int rw, int rh) {
    return mx + 28 > rx && mx - 2 < rx + rw && my + 25 > ry && my - 2 < ry + rh;
}

static void draw_gui_frame(const fb_info_t *fb, int w, int h, gui_state_t *st, int mx, int my, int edge) {
    if (gui_dirty_is_full() || gui_dirty_count() == 0) {
        if (gui_dirty_count() == 0) gui_dirty_mark_full();
        draw_gui_screen(w, h, st);
        draw_cursor(mx, my, edge);
        gui_present_surface(fb);
        /* g_gui_surface alloc failed (OOM): draw_gui_screen drew straight
         * through fb_put_pixel/fb_fill_rect, which now land in the TUI's
         * off-screen back-buffer instead of the real framebuffer (see
         * graphics.c). gui_present_surface() no-ops without a surface, so
         * flush that back-buffer to the screen ourselves. */
        if (!g_gui_surface) console_present();
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
    if (!g_gui_surface) console_present();
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
    *w = ui_s(96); *h = ui_s(100);
    *x = ui_s(42);
    *y = ui_s(46) + i * ui_s(112);
}


static void draw_desktop_icons(void) {
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        const desktop_icon_t *d = &g_desktop_icons[i];
        int x, y, w, h;
        desktop_icon_rect(i, &x, &y, &w, &h);
        int isz = ui_s(58), ix = x + (w - isz) / 2, iy = y;
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
        else if (st->snap_preview == WM_SNAP_BOTTOM) {
            // 最小化预览：贴近任务栏的小框，提示窗口将收起
            pw = w / 5; px = (w - pw) / 2; ph = 64; py = h - TASKBAR_H - ph - 8;
        }
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
    if (gui_has_suffix(full, ".bmp")) {
        app_imgview_set_path(st, full);
        if (gui_open_window(st, WM_WIN_APP, GUI_APP_IMGVIEW, 0) >= 0)
            st->status = "已用图片查看器打开";
        return;
    }
    if (gui_looks_binary(node)) {
        app_hexview_set_path(st, full);
        if (gui_open_window(st, WM_WIN_APP, GUI_APP_HEXVIEW, 0) >= 0)
            st->status = "已用十六进制查看器打开";
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
    int panel = win->mode;
    if (panel == PANEL_FILES) {
        files_layout_t L;
        files_panel_layout(win_w, win_h, &L);
        int content_w = L.content_w;
        for (int i = 0; i < FILE_ACTION_COUNT; i++) {
            int x, y, bw;
            if (!gui_file_action_rect(content_w, i, &x, &y, &bw)) continue;
            if (mx >= tx + x && mx < tx + x + bw && my >= ty + y && my < ty + y + ACTION_H)
                return gui_file_actions[i].action;
        }
        int main_x = tx + L.side_w + 12;
        int main_w = L.main_w;
        int list_y = ty + 146;
        if (mx >= main_x && mx < main_x + main_w && my >= list_y - 8 && my < list_y - 8 + L.list_rows * FILE_ROW_H) {
            int selected = st->selected_file;
            if (selected < 0) selected = 0;
            uint32_t start = selected >= L.list_rows ? (uint32_t)(selected - (L.list_rows - 1)) : 0;
            int idx = (my - (list_y - 8)) / FILE_ROW_H;
            uint32_t file_idx = start + (uint32_t)idx;
            if (idx >= 0 && idx < L.list_rows && file_idx < gui_file_count((gui_state_t *)st))
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

// 记事本正文区点击定位：笔记是变宽比例字体（非等宽网格），逐字符重演
// draw_notes_app 的排版循环来反推 (mx,my) 落在哪个字节偏移上，保证跟渲染
// 完全一致。命中正文框返回 1 并写 *off；否则返回 0（不动光标）。
static int hit_note_editor(int w, int h, gui_state_t *st, int mx, int my, uint32_t *off) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window(&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_NOTES) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics(st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_h;
    int tx = win_x + 30;
    int ty = win_y + 42;
    int list_w = 150;
    int edit_x = tx + list_w + 18;
    int edit_w = win_w - list_w - 86;
    if (edit_w < 260) edit_w = 260;
    if (mx < edit_x || mx >= edit_x + edit_w || my < ty + 118 || my >= ty + 291)
        return 0;

    int x = edit_x + 8;
    int y = ty + 126;
    utf8_state_t utf8;
    utf8_init(&utf8);
    uint32_t i;
    for (i = 0; i < st->note_len && y < ty + 280; i++) {
        int row_top = y, row_bot = y + 18;
        if (st->note_buf[i] == '\n') {
            if (my >= row_top && my < row_bot) { if (off) *off = i; return 1; }
            x = edit_x + 8;
            y += 18;
            utf8_init(&utf8);
            continue;
        }
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)st->note_buf[i], &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';
        int advance = gui_cp_advance(cp, 1);
        if (my >= row_top && my < row_bot && mx < x + advance) {
            if (off) *off = (mx < x + advance / 2) ? i : i + 1;
            return 1;
        }
        x += advance;
        if (x > edit_x + edit_w - 16) {
            x = edit_x + 8;
            y += 18;
        }
    }
    if (off) *off = i;  // 点在最后一行之后：光标放到末尾
    return 1;
}

/* "Enter 加载" 按钮命中测试；矩形与 draw_browser_app 里画的那个必须保持一致。 */
static int hit_browser_load_button(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_BROWSER) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_w; (void)win_h;
    int tx = win_x + 30;
    int ty = win_y + 42;
    int bx = tx, by = ty + 104, bw = 96;
    return (mx >= bx && mx < bx + bw && my >= by && my < by + ACTION_H);
}

/* 0=没点中，1=后退，2=前进；矩形要和 draw_browser_app 里画的保持一致。 */
static int hit_browser_nav_button(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_BROWSER) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_w; (void)win_h;
    int tx = win_x + 30;
    int ty = win_y + 42;
    int by = ty + 104;
    if (mx >= tx + 178 && mx < tx + 178 + 44 && my >= by && my < by + ACTION_H) return 1;
    if (mx >= tx + 224 && mx < tx + 224 + 44 && my >= by && my < by + ACTION_H) return 2;
    return 0;
}

/* 点中某个渲染过的链接就返回它的下标，否则 -1；命中矩形是
 * draw_rendered_page() 上一次绘制时填进 g_browser_link_rects 的。 */
static int hit_browser_link(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return -1;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_BROWSER) return -1;
    (void)w; (void)h;
    for (int i = 0; i < g_browser_link_rect_count; i++) {
        browser_link_rect_t *r = &g_browser_link_rects[i];
        if (mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h) return r->link_idx;
    }
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
    if (key == KB_KEY_F6) {
        gui_focus_next_window(st, 1);
        if (st->wm.window_count > 1) st->switcher_ticks = 40;
        return;
    }
    gui_sync_focus(st);
    if (gui_app_handle_key(st, key)) return;
    if (st->app_mode == GUI_APP_NOTES) {
        note_load(st);
        if (key == 1) {  // Ctrl+A：全选
            if (st->note_len > 0) {
                st->note_sel_anchor = 0;
                st->note_cursor = st->note_len;
                st->note_sel_active = 1;
                st->status = "已全选（Backspace/输入 可替换）";
            } else {
                st->note_sel_active = 0;
                st->status = "笔记为空";
            }
            return;
        }
        int had_sel = st->note_sel_active;
        uint32_t sel_start = 0, sel_end = 0;
        if (had_sel) note_sel_range(st, &sel_start, &sel_end);

        // Shift+方向键：开始/延伸选区，光标移动端跟随移动，锚点端不动
        if (key == GUI_KEY_SHIFT_LEFT || key == GUI_KEY_SHIFT_RIGHT ||
            key == GUI_KEY_SHIFT_UP   || key == GUI_KEY_SHIFT_DOWN) {
            if (!had_sel) st->note_sel_anchor = st->note_cursor;
            if (key == GUI_KEY_SHIFT_LEFT) note_cursor_left(st);
            else if (key == GUI_KEY_SHIFT_RIGHT) note_cursor_right(st);
            else if (key == GUI_KEY_SHIFT_UP) note_cursor_vertical(st, -1);
            else note_cursor_vertical(st, 1);
            st->note_sel_active = (st->note_sel_anchor != st->note_cursor);
            return;
        }
        st->note_sel_active = 0;  // 除 Shift+方向键/Ctrl+A 外任何按键都取消选中
        if (key == 19) {  // Ctrl+S
            note_save(st);
        } else if (key == GUI_KEY_BACKSPACE || key == GUI_KEY_DELETE) {
            if (had_sel) note_delete_range(st, sel_start, sel_end);
            else if (key == GUI_KEY_BACKSPACE) note_backspace(st);
            else note_delete_forward(st);
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
            if (had_sel) note_delete_range(st, sel_start, sel_end);
            note_insert(st, '\n');
        } else if (key == '\t') {
            if (had_sel) note_delete_range(st, sel_start, sel_end);
            for (int i = 0; i < 4; i++) note_insert(st, ' ');
        } else if (key >= 32 && key <= 126) {
            if (had_sel) note_delete_range(st, sel_start, sel_end);
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
        uint32_t n = (uint32_t)strlen(st->browser_url);
        if (st->browser_url_cursor > n) st->browser_url_cursor = n;  /* 页面加载等外部改动后钳制 */
        if (key == '\n') browser_load(st);
        else if (key == 19) browser_save_page(st);
        else if (key == GUI_KEY_BACKSPACE) {
            if (st->browser_url_cursor > 0) {
                memmove(st->browser_url + st->browser_url_cursor - 1,
                        st->browser_url + st->browser_url_cursor,
                        n - st->browser_url_cursor + 1);
                st->browser_url_cursor--;
            }
        } else if (key == GUI_KEY_LEFT) {
            if (st->browser_url_cursor > 0) st->browser_url_cursor--;
        } else if (key == GUI_KEY_RIGHT) {
            if (st->browser_url_cursor < n) st->browser_url_cursor++;
        } else if (key == GUI_KEY_UP) {
            if (st->browser_scroll > 0) st->browser_scroll--;
        } else if (key == GUI_KEY_DOWN) {
            st->browser_scroll++;
        } else if (key >= 32 && key <= 126) {
            if (n + 1 < BROWSER_URL_CAP) {
                memmove(st->browser_url + st->browser_url_cursor + 1,
                        st->browser_url + st->browser_url_cursor,
                        n - st->browser_url_cursor + 1);
                st->browser_url[st->browser_url_cursor] = (char)key;
                st->browser_url_cursor++;
            }
        }
    } else if (st->app_mode == GUI_APP_CODE) {
        code_load(st);
        if (key == 1) {  // Ctrl+A：全选
            if (st->code_len > 0) {
                st->code_sel_anchor = 0;
                st->code_cursor = st->code_len;
                st->code_sel_active = 1;
                st->status = "已全选（Backspace/输入 可替换）";
            } else {
                st->code_sel_active = 0;
                st->status = "代码为空";
            }
            return;
        }
        int had_sel = st->code_sel_active;
        uint32_t sel_start = 0, sel_end = 0;
        if (had_sel) code_sel_range(st, &sel_start, &sel_end);

        // Shift+方向键：开始/延伸选区，光标移动端跟随移动，锚点端不动
        if (key == GUI_KEY_SHIFT_LEFT || key == GUI_KEY_SHIFT_RIGHT ||
            key == GUI_KEY_SHIFT_UP   || key == GUI_KEY_SHIFT_DOWN) {
            if (!had_sel) st->code_sel_anchor = st->code_cursor;
            if (key == GUI_KEY_SHIFT_LEFT) {
                if (st->code_cursor > 0) st->code_cursor--;
                code_ensure_visible(st);
            } else if (key == GUI_KEY_SHIFT_RIGHT) {
                if (st->code_cursor < st->code_len) st->code_cursor++;
                code_ensure_visible(st);
            } else if (key == GUI_KEY_SHIFT_UP) {
                code_move_vertical(st, -1);
            } else {
                code_move_vertical(st, 1);
            }
            st->code_sel_active = (st->code_sel_anchor != st->code_cursor);
            return;
        }
        st->code_sel_active = 0;  // 除 Shift+方向键/Ctrl+A 外任何按键都取消选中
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
        } else if (key == GUI_KEY_BACKSPACE || key == GUI_KEY_DELETE) {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
            else if (key == GUI_KEY_BACKSPACE) code_backspace(st);
            else code_delete_forward(st);
        } else if (key == 3) {
            code_set_output("Use Esc/window close to leave Code Workspace");
            st->status = "代码工作台保持打开";
        } else if (key == '\t') {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
            for (int i = 0; i < 4; i++) code_insert_char(st, ' ');
        } else if (key == '\n') {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
            code_insert_newline(st);
        } else if (key >= 32 && key <= 126) {
            if (had_sel) code_delete_range(st, sel_start, sel_end);
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
        } else if (key == GUI_KEY_PGUP) {
            st->console_scroll += 8;
        } else if (key == GUI_KEY_PGDOWN) {
            st->console_scroll -= 8;
            if (st->console_scroll < 0) st->console_scroll = 0;
        } else if (key >= 32 && key <= 126) {
            st->console_scroll = 0;  // typing snaps back to bottom
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
    if (key == KB_KEY_F6 || key == ' ') {
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

// F6 窗口切换器浮层：列出全部窗口并高亮当前焦点
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
    text(ox + pad, oy + pad, "切换窗口  (F6 循环)", cyber_text(st->theme_light), 1);
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

/* ── 启动器中的 .hax GUI 应用枚举 ──────────────────────────────
 * 仅 GUI 类应用进入启动器；数量受 SM_HAX_MAX 限制以约束面板高度。 */
static int gui_hax_count(void) {
    uint32_t n = hax_app_count();
    int c = 0;
    for (uint32_t i = 0; i < n; i++) {
        const hax_app_entry_t *e = hax_app_at(i);
        if (e && (e->kind & HAX_KIND_GUI)) c++;
    }
    if (c > SM_HAX_MAX) c = SM_HAX_MAX;
    return c;
}

/* 返回第 k 个 GUI 类 .hax 应用（k 从 0 起），越界返回 NULL */
static const hax_app_entry_t *gui_hax_at(int k) {
    uint32_t n = hax_app_count();
    int seen = 0;
    for (uint32_t i = 0; i < n; i++) {
        const hax_app_entry_t *e = hax_app_at(i);
        if (e && (e->kind & HAX_KIND_GUI)) {
            if (seen == k) return e;
            seen++;
        }
    }
    return 0;
}

/* 启动器动态总高度：内置 13 项 + 追加的 .hax 行 */
/* ── 开始菜单：统一应用条目（内置 + .hax）+ 搜索过滤 ──────────────
 * 键盘是 ASCII，应用名是中文，故每个内置应用附带 ASCII/拼音关键词供搜索。 */
enum { SM_K_PANEL = 0, SM_K_APP = 1, SM_K_HAX = 2 };
typedef struct {
    const char *name; const char *kw; int kind; int mode; int icon;
} sm_entry_t;

static const sm_entry_t sm_builtin[] = {
    {"文件管理器", "file files fm wenjian",    SM_K_PANEL, PANEL_FILES,     ICON_FILES},
    {"磁盘",       "disk cipan",               SM_K_PANEL, PANEL_DISK,      ICON_DISK},
    {"资源",       "resource sys ziyuan",      SM_K_PANEL, PANEL_SYS,       ICON_SYS},
    {"应用",       "app apps yingyong",        SM_K_PANEL, PANEL_APPS,      ICON_APPS},
    {"记事本",     "note notes jishi",         SM_K_APP,   GUI_APP_NOTES,   ICON_NOTES},
    {"计算器",     "calc calculator jisuan",   SM_K_APP,   GUI_APP_CALC,    ICON_CALC},
    {"贪吃蛇",     "snake game tanchishe",     SM_K_APP,   GUI_APP_SNAKE,   ICON_SNAKE},
    {"浏览器",     "web browser liulan",       SM_K_APP,   GUI_APP_BROWSER, ICON_BROWSER},
    {"代码",       "code daima",               SM_K_APP,   GUI_APP_CODE,    ICON_CODE},
    {"终端",       "term terminal shell",      SM_K_APP,   GUI_APP_DIAG,    ICON_TERM},
    {"时钟",       "clock time shizhong",      SM_K_APP,   GUI_APP_CLOCK,   ICON_CLOCK},
    {"设置",       "settings config shezhi",   SM_K_APP,   GUI_APP_SETTINGS,ICON_SYS},
    {"任务管理器", "task taskmgr process renwu guanli", SM_K_APP, GUI_APP_TASKMGR, ICON_SYS},
    {"快捷键",     "shortcuts keys kuaijiejian",        SM_K_APP, GUI_APP_SHORTCUTS, ICON_SHORTCUTS},
};
#define SM_BUILTIN_N ((int)(sizeof(sm_builtin) / sizeof(sm_builtin[0])))

static int sm_total_entries(void) { return SM_BUILTIN_N + gui_hax_count(); }

static int sm_entry_at(int idx, sm_entry_t *out) {
    if (idx < 0) return 0;
    if (idx < SM_BUILTIN_N) { *out = sm_builtin[idx]; return 1; }
    const hax_app_entry_t *e = gui_hax_at(idx - SM_BUILTIN_N);
    if (!e) return 0;
    out->name = e->name; out->kw = e->name;
    out->kind = SM_K_HAX; out->mode = idx - SM_BUILTIN_N; out->icon = ICON_APPS;
    return 1;
}

static char sm_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* q（已小写）是否为 hay 的子串（忽略大小写） */
static int sm_match(const char *hay, const char *q) {
    if (!q || !q[0]) return 1;
    if (!hay) return 0;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (q[j] && hay[i + j] && sm_lc(hay[i + j]) == q[j]) j++;
        if (!q[j]) return 1;
    }
    return 0;
}

static int sm_entry_matches(const sm_entry_t *e, const char *q) {
    return sm_match(e->name, q) || sm_match(e->kw, q);
}

static void sm_query_lc(const gui_state_t *st, char *q, int cap) {
    int n = 0;
    for (; st->sm_search[n] && n < cap - 1; n++) q[n] = sm_lc(st->sm_search[n]);
    q[n] = 0;
}

/* 取过滤后第 k 个可见条目 */
static int sm_visible_at(const gui_state_t *st, int k, sm_entry_t *out) {
    char q[24]; sm_query_lc(st, q, sizeof(q));
    int seen = 0, total = sm_total_entries();
    for (int i = 0; i < total; i++) {
        sm_entry_t e;
        if (!sm_entry_at(i, &e) || !sm_entry_matches(&e, q)) continue;
        if (seen == k) { *out = e; return 1; }
        seen++;
    }
    return 0;
}

static int sm_visible_count(const gui_state_t *st) {
    char q[24]; sm_query_lc(st, q, sizeof(q));
    int seen = 0, total = sm_total_entries();
    for (int i = 0; i < total; i++) {
        sm_entry_t e;
        if (sm_entry_at(i, &e) && sm_entry_matches(&e, q)) seen++;
    }
    return seen;
}

static void sm_launch(gui_state_t *st, const sm_entry_t *e) {
    if (e->kind == SM_K_PANEL) gui_open_panel_window(st, e->mode);
    else if (e->kind == SM_K_APP) gui_open_window(st, WM_WIN_APP, e->mode, 0);
    else {
        const hax_app_entry_t *he = gui_hax_at(e->mode);
        if (he) {
            char *av[1]; av[0] = (char *)he->name;
            if (he->kind & HAX_KIND_GUI_WIN) {
                /* 并发窗口应用（hax_win_open，如 wdemo）：必须非阻塞启动。
                 * 它靠合成器主循环持续跑帧才能把窗口内容合成到屏幕上，同步
                 * 等待会卡住本函数所在的合成器任务，窗口画面永远显示不出来。 */
                hax_app_spawn(he->name, 1, av);
                gui_toast(st, "已启动 ", he->name);
            } else {
                /* 其余应用（纯控制台或独占画布风格，即使声明了 HAX_KIND_GUI，
                 * 如 guess/paint）：同步等待退出，而非 fire-and-forget 的
                 * hax_app_spawn。纯控制台风格的 .hax 应用用 read(stdin) 直接
                 * 轮询键盘，若与合成器自身的按键轮询并发运行，两者会争抢同一份
                 * 全局键盘状态机（kb_poll_key 里的 shift/ctrl/ext_scancode
                 * 静态状态）。之前用异步 spawn 时，用户按 Esc 只会退出 GUI 主
                 * 循环本身（该按键从未被送到那个后台任务），应用则继续在后台
                 * 无限期存活、持续和 TUI 抢键盘，造成状态错乱甚至崩溃。改为
                 * 同步执行可保证本函数返回时应用已确实终止，不会残留后台任务。 */
                hax_app_run(he->name, 1, av);
                gui_dirty_mark_full();
                gui_toast(st, "已退出 ", he->name);
            }
        }
    }
}

static int gui_start_menu_h(void) {
    int total = sm_total_entries();
    int rows = (total + SM_GRID_COLS - 1) / SM_GRID_COLS;
    int extra_rows = rows - SM_GRID_ROWS;
    if (extra_rows < 0) extra_rows = 0;
    return SM_H + extra_rows * SM_CELL_H;
}

/* ── 右键上下文菜单 ─────────────────────────────────────────── */
#define CTX_ITEM_H ui_s(28)
#define CTX_W     ui_s(176)

static const char *const ctx_desktop_items[] = { "刷新桌面", "切换主题", "打开开始菜单" };
static const char *const ctx_window_items[]  = { "最小化", "最大化 / 还原", "关闭窗口" };
#define CTX_DESKTOP_N 3
#define CTX_WINDOW_N  3

static int ctx_item_count(int kind) {
    return kind == 2 ? CTX_WINDOW_N : CTX_DESKTOP_N;
}
static const char *ctx_item_label(int kind, int i) {
    return kind == 2 ? ctx_window_items[i] : ctx_desktop_items[i];
}

/* 返回光标下最顶层（z 序最高）未最小化窗口索引，无则 -1 */
static int gui_window_at(gui_state_t *st, int w, int h, int mx, int my) {
    for (int i = st->wm.window_count - 1; i >= 0; i--) {
        int idx = st->wm.z_order[i];
        wm_window_t *win = wm_get_window(&st->wm, idx);
        if (!win || !win->used || win->state == WM_STATE_MINIMIZED) continue;
        int wx, wy, ww, wh;
        gui_window_metrics(st, w, h, win, idx, &wx, &wy, &ww, &wh);
        if (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh) return idx;
    }
    return -1;
}

static void draw_context_menu(gui_state_t *st) {
    if (!st->ctx_open) return;
    int light = st->theme_light;
    int n = ctx_item_count(st->ctx_open);
    int mw = CTX_W, mh = n * CTX_ITEM_H + 8;
    int ox = st->ctx_x, oy = st->ctx_y;
    soft_shadow(ox, oy, mw, mh);
    fill_round_rect(ox, oy, mw, mh, 8, light ? 0xF8F4F6F8 : 0xF8222A33, RR_ALL);
    border(ox, oy, mw, mh, light ? rgb(200, 206, 212) : rgb(52, 62, 74));
    for (int i = 0; i < n; i++) {
        int iy = oy + 4 + i * CTX_ITEM_H;
        text(ox + 14, iy + (CTX_ITEM_H - gui_font_line_height()) / 2,
             ctx_item_label(st->ctx_open, i),
             light ? rgb(40, 44, 48) : rgb(220, 226, 232), 1);
    }
}

/* 命中测试：返回 item 索引，未命中返回 -1 */
static int ctx_hit(gui_state_t *st, int mx, int my) {
    if (!st->ctx_open) return -1;
    int n = ctx_item_count(st->ctx_open);
    int mw = CTX_W, mh = n * CTX_ITEM_H + 8;
    int ox = st->ctx_x, oy = st->ctx_y;
    if (mx < ox || mx >= ox + mw || my < oy || my >= oy + mh) return -1;
    int i = (my - oy - 4) / CTX_ITEM_H;
    if (i < 0 || i >= n) return -1;
    return i;
}

/* ── 日历弹窗（点击任务栏时钟弹出，Win11 风格） ─────────────── */

static int cal_is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int cal_days_in_month(int y, int m) {
    static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && cal_is_leap(y)) return 29;
    return D[m - 1];
}

/* Sakamoto 算法：返回 0=周日 .. 6=周六 */
static int cal_weekday(int y, int m, int d) {
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* 面板几何：贴任务栏右上方。cell/pad 均随 DPI 缩放。 */
static void cal_geometry(int w, int h, int *ox, int *oy, int *ow, int *oh,
                         int *pad, int *cell, int *head_h, int *wk_h) {
    *pad = ui_s(14);
    *cell = ui_s(36);
    *head_h = ui_s(40);
    *wk_h = ui_s(24);
    *ow = *pad * 2 + *cell * 7;
    *oh = *pad * 2 + *head_h + *wk_h + *cell * 6;
    *ox = w - *ow - ui_s(12);
    if (*ox < 8) *ox = 8;
    *oy = h - TASKBAR_H - *oh - ui_s(8);
    if (*oy < 8) *oy = 8;
}

static void draw_calendar_popup(int w, int h, gui_state_t *st) {
    if (!st->cal_open) return;
    int light = st->theme_light;
    int ox, oy, ow, oh, pad, cell, head_h, wk_h;
    cal_geometry(w, h, &ox, &oy, &ow, &oh, &pad, &cell, &head_h, &wk_h);

    uint32_t accent = rgb(61, 174, 233);
    uint32_t fg     = light ? rgb(40, 44, 48)    : rgb(222, 228, 234);
    uint32_t dim    = light ? rgb(130, 138, 146) : rgb(130, 140, 150);
    fill_round_rect(ox, oy, ow, oh, 12, light ? 0xF6F5F6F8 : 0xF6202428, RR_ALL);
    uint32_t bd = light ? rgb(210, 214, 218) : rgb(52, 60, 68);
    rect(ox + 12, oy, ow - 24, 1, bd);
    rect(ox + 12, oy + oh - 1, ow - 24, 1, bd);
    rect(ox, oy + 12, 1, oh - 24, bd);
    rect(ox + ow - 1, oy + 12, 1, oh - 24, bd);

    int lh = gui_font_line_height();

    /* 标题 "YYYY年M月" + 右侧 ‹ › 翻月按钮 */
    char title[24];
    {
        uint32_t pos = 0;
        title[0] = 0;
        append_uint(title, sizeof(title), &pos, (uint32_t)st->cal_year);
        append_str(title, sizeof(title), &pos, "年");
        append_uint(title, sizeof(title), &pos, (uint32_t)st->cal_month);
        append_str(title, sizeof(title), &pos, "月");
    }
    int hy = oy + pad;
    text(ox + pad, hy + (head_h - lh) / 2, title, fg, 1);
    int btn = ui_s(26);
    int nav_y = hy + (head_h - btn) / 2;
    int nx_next = ox + ow - pad - btn;
    int nx_prev = nx_next - btn - ui_s(6);
    fill_round_rect(nx_prev, nav_y, btn, btn, 6, light ? rgb(228, 231, 234) : rgb(46, 52, 58), RR_ALL);
    fill_round_rect(nx_next, nav_y, btn, btn, 6, light ? rgb(228, 231, 234) : rgb(46, 52, 58), RR_ALL);
    text(nx_prev + (btn - text_width("‹", 1)) / 2, nav_y + (btn - lh) / 2, "‹", fg, 1);
    text(nx_next + (btn - text_width("›", 1)) / 2, nav_y + (btn - lh) / 2, "›", fg, 1);

    /* 星期表头（周日起，与 CMOS wday 一致） */
    static const char *const WK[7] = {"日", "一", "二", "三", "四", "五", "六"};
    int gy = hy + head_h;
    for (int i = 0; i < 7; i++) {
        int cx = ox + pad + i * cell;
        int tw2 = text_width(WK[i], 1);
        text(cx + (cell - tw2) / 2, gy + (wk_h - lh) / 2, WK[i], dim, 1);
    }

    /* 当前真实日期（本地时区，与任务栏时钟一致；仅当浏览的正是本月时高亮今天） */
    uint8_t th, tmin, tsec, td, tm; uint32_t ty2; uint8_t twday;
    rtc_tz_read_local(&th, &tmin, &tsec, &td, &tm, &ty2, &twday);
    (void)th; (void)tmin; (void)tsec; (void)twday;
    int today = (st->cal_year == (int)ty2 && st->cal_month == tm) ? td : -1;

    /* 日期网格 */
    int first_wd = cal_weekday(st->cal_year, st->cal_month, 1);
    int ndays = cal_days_in_month(st->cal_year, st->cal_month);
    int rows_y = gy + wk_h;
    for (int d = 1; d <= ndays; d++) {
        int idx = first_wd + d - 1;
        int col = idx % 7, row = idx / 7;
        int cx = ox + pad + col * cell;
        int cy = rows_y + row * cell;
        char db[4];
        uint32_t pos = 0;
        db[0] = 0;
        append_uint(db, sizeof(db), &pos, (uint32_t)d);
        int tw2 = text_width(db, 1);
        if (d == today) {
            int r = cell - ui_s(6);
            fill_round_rect(cx + (cell - r) / 2, cy + (cell - r) / 2, r, r,
                            r / 2, accent, RR_ALL);
            text(cx + (cell - tw2) / 2, cy + (cell - lh) / 2, db,
                 rgb(255, 255, 255), 1);
        } else {
            text(cx + (cell - tw2) / 2, cy + (cell - lh) / 2, db, fg, 1);
        }
    }
}

/* 日历命中：-1=面板外 0=面板内空白 1=上月 2=下月 */
static int cal_hit(gui_state_t *st, int w, int h, int mx, int my) {
    if (!st->cal_open) return -1;
    int ox, oy, ow, oh, pad, cell, head_h, wk_h;
    cal_geometry(w, h, &ox, &oy, &ow, &oh, &pad, &cell, &head_h, &wk_h);
    if (mx < ox || mx >= ox + ow || my < oy || my >= oy + oh) return -1;
    int btn = ui_s(26);
    int nav_y = oy + pad + (head_h - btn) / 2;
    int nx_next = ox + ow - pad - btn;
    int nx_prev = nx_next - btn - ui_s(6);
    if (my >= nav_y && my < nav_y + btn) {
        if (mx >= nx_prev && mx < nx_prev + btn) return 1;
        if (mx >= nx_next && mx < nx_next + btn) return 2;
    }
    return 0;
}

static void cal_shift_month(gui_state_t *st, int dir) {
    st->cal_month += dir;
    if (st->cal_month < 1)  { st->cal_month = 12; st->cal_year--; }
    if (st->cal_month > 12) { st->cal_month = 1;  st->cal_year++; }
}

/* 打开日历时定位到当前年月（本地时区，与任务栏时钟一致） */
static void cal_open_now(gui_state_t *st) {
    uint8_t h, mi, s, d, m; uint32_t y; uint8_t wd;
    rtc_tz_read_local(&h, &mi, &s, &d, &m, &y, &wd);
    (void)h; (void)mi; (void)s; (void)d; (void)wd;
    st->cal_year = (int)y;
    st->cal_month = (m >= 1 && m <= 12) ? m : 1;
    st->cal_open = 1;
}

static void draw_start_menu(gui_state_t *st) {
    wm_state_t *wm = &st->wm;
    if (!wm->start_menu_open) return;
    int ox = wm->menu_x, oy = wm->menu_y;
    int light = st->theme_light;
    int mh = wm->menu_h > 0 ? wm->menu_h : SM_H;   /* 动态面板高度 */
    int extra = mh - SM_H;
    if (extra < 0) extra = 0;

    /* ── panel background ── */
    uint32_t bg = light ? 0xF6F5F6F8 : 0xF6202428;
    fill_round_rect(ox, oy, SM_W, mh, 12, bg, RR_ALL);
    uint32_t bd = light ? rgb(210, 214, 218) : rgb(52, 60, 68);
    /* top/bottom border lines for rounded rect */
    rect(ox + 12, oy, SM_W - 24, 1, bd);
    rect(ox + 12, oy + mh - 1, SM_W - 24, 1, bd);
    rect(ox, oy + 12, 1, mh - 24, bd);
    rect(ox + SM_W - 1, oy + 12, 1, mh - 24, bd);

    /* ── search bar ── */
    int sx = ox + SM_PAD, sy = oy + SM_SEARCH_TOP;
    int sw = SM_W - 2 * SM_PAD;
    vgradient(sx, sy, sw, SM_SEARCH_H,
              light ? rgb(240, 242, 244) : rgb(34, 40, 48),
              light ? rgb(232, 235, 238) : rgb(28, 34, 42));
    border(sx, sy, sw, SM_SEARCH_H, light ? rgb(190, 195, 200) : rgb(55, 65, 78));
    if (st->sm_search_len > 0) {
        text(sx + 12, sy + (SM_SEARCH_H - gui_font_line_height()) / 2,
             st->sm_search, light ? rgb(30, 34, 40) : rgb(232, 238, 244), 1);
        /* 光标 */
        int cxp = sx + 12 + text_width(st->sm_search, 1) + 1;
        rect(cxp, sy + 8, 1, SM_SEARCH_H - 16, light ? rgb(60, 66, 72) : rgb(200, 206, 212));
    } else {
        text(sx + 12, sy + (SM_SEARCH_H - gui_font_line_height()) / 2,
             "搜索应用…（输入名称或拼音，Enter 启动）",
             light ? rgb(160, 166, 172) : rgb(100, 110, 122), 1);
    }
    /* magnifier icon */
    int mg = sy + SM_SEARCH_H / 2;
    int mr = 6;
    /* simple circle + line for magnifier */
    rect(sx + sw - 22 + mr/2 + 2, mg - mr, 1, mr * 2, light ? rgb(140,146,152) : rgb(90,100,112));
    rect(sx + sw - 22 - mr, mg - 1, mr * 2, 1, light ? rgb(140,146,152) : rgb(90,100,112));

    /* ── "已固定" section label ── */
    int lty = oy + SM_PIN_TOP;
    text(ox + SM_PAD, lty + (SM_PIN_LABEL_H - gui_font_line_height()) / 2,
         "已固定", light ? rgb(60, 66, 72) : rgb(180, 186, 192), 1);
    text(ox + SM_W - SM_PAD - text_width("全部 >", 1),
         lty + (SM_PIN_LABEL_H - gui_font_line_height()) / 2,
         "全部 >", light ? rgb(61, 174, 233) : rgb(61, 174, 233), 1);

    /* ── 应用网格（按搜索过滤后顺序排布） ── */
    int cell_w = (SM_W - 2 * SM_PAD) / SM_GRID_COLS;
    int vis = sm_visible_count(st);
    for (int i = 0; i < vis; i++) {
        sm_entry_t e;
        if (!sm_visible_at(st, i, &e)) break;
        int col = i % SM_GRID_COLS;
        int row = i / SM_GRID_COLS;
        int cx = ox + SM_PAD + col * cell_w + cell_w / 2;
        int cy = oy + SM_GRID_TOP + row * SM_CELL_H;
        int isz = ui_s(36);
        int ix = cx - isz / 2;
        int iy = cy + ui_s(4);
        blit_icon(ix, iy, isz, e.icon);
        int tw = text_width(e.name, 1);
        int lx = cx - tw / 2;
        if (lx < ox + SM_PAD) lx = ox + SM_PAD;
        text_clipped(lx, iy + isz + 4, ox + SM_W - SM_PAD,
                     e.name, light ? rgb(40, 44, 48) : rgb(220, 226, 232), 1);
    }
    if (vis == 0) {
        text(ox + SM_PAD, oy + SM_GRID_TOP + 24, "无匹配应用",
             light ? rgb(120, 126, 132) : rgb(150, 156, 162), 1);
    }

    /* ── separator（随额外行下移） ── */
    int sep_y = oy + SM_SEP_Y + 2 + extra;
    rect(ox + SM_PAD, sep_y, SM_W - 2 * SM_PAD, 1,
         light ? rgb(210, 214, 218) : rgb(48, 56, 64));

    /* ── bottom bar（随额外行下移） ── */
    int bar_y = oy + SM_BAR_TOP + extra;
    /* left: user icon + name */
    int uisz = ui_s(28);
    int uix = ox + SM_PAD, uiy = bar_y + (SM_BAR_H - uisz) / 2;
    draw_user_avatar(uix, uiy, uisz,
                     light ? rgb(135, 206, 235) : rgb(90, 150, 180),
                     rgb(140, 140, 140));
    text(uix + uisz + 8, bar_y + (SM_BAR_H - gui_font_line_height()) / 2,
         "用户", light ? rgb(40, 44, 48) : rgb(210, 218, 226), 1);

    /* right: Shell + Power buttons */
    int btn_w = 88, btn_h = 30, gap = 8;
    int power_x = ox + SM_W - SM_PAD - btn_w;
    int shell_x = power_x - btn_w - gap;
    int btn_y = bar_y + (SM_BAR_H - btn_h) / 2;

    /* Shell button */
    vgradient(shell_x, btn_y, btn_w, btn_h,
              light ? rgb(220, 226, 232) : rgb(44, 54, 66),
              light ? rgb(208, 214, 220) : rgb(36, 44, 56));
    border(shell_x, btn_y, btn_w, btn_h, light ? rgb(180, 186, 192) : rgb(60, 72, 86));
    {
        int tw = text_width("返回Shell", 1);
        text(shell_x + (btn_w - tw) / 2, btn_y + (btn_h - gui_font_line_height()) / 2,
             "返回Shell", light ? rgb(40, 44, 48) : rgb(200, 210, 222), 1);
    }

    /* Power button */
    vgradient(power_x, btn_y, btn_w, btn_h,
              light ? rgb(240, 80, 80) : rgb(180, 30, 30),
              light ? rgb(220, 60, 60) : rgb(150, 20, 20));
    border(power_x, btn_y, btn_w, btn_h, light ? rgb(180, 40, 40) : rgb(120, 15, 15));
    {
        /* power symbol: circle arc + vertical bar */
        int pcx = power_x + btn_w / 2;
        int pcy = btn_y + btn_h / 2;
        rect(pcx - 1, pcy - 7, 2, 8, rgb(255, 255, 255));
        /* arc approximation via horizontal lines */
        for (int dy = -5; dy <= 5; dy++) {
            int dx = dy < 0 ? (6 - dy/2) : (6 + dy/2);
            if (dx > 8) dx = 8;
            rect(pcx - dx, pcy + dy, 1, 1, rgb(255, 255, 255));
            rect(pcx + dx, pcy + dy, 1, 1, rgb(255, 255, 255));
        }
        text(power_x + 20, btn_y + (btn_h - gui_font_line_height()) / 2,
             "关机", rgb(255, 255, 255), 1);
    }
}

/* 进入 GUI 的欢迎窗口 —— 与桌面其余部分统一的圆角现代风格（蓝/深主题）。 */
// 与 draw_window_frame 完全一致的 Breeze 窗框配色/圆角/标题栏样式（中性标题
// 栏 + 强调色边框 + 左上角圆角图标），让欢迎窗口看起来就是桌面里的一个普通
// 窗口，而不是一张单独的强调色卡片。
static void draw_splash_window(int w, int h, int ticks, int light) {
    int sw = ui_s(440), sh = ui_s(208);
    int sx = (w - sw) / 2, sy = (h - sh) / 2;
    int title_h = WM_TITLE_H;
    int R = 8;
    uint32_t accent   = rgb(61, 174, 233);
    uint32_t body     = light ? rgb(239, 240, 241) : rgb(42, 46, 50);
    uint32_t title_bg = light ? rgb(214, 217, 221) : rgb(49, 54, 59);
    uint32_t tcol     = light ? rgb(40, 44, 48)    : rgb(226, 232, 238);
    uint32_t scol     = light ? rgb(110, 120, 130) : rgb(150, 160, 170);
    int lh = gui_font_line_height();

    soft_shadow(sx, sy, sw, sh);
    soft_shadow(sx + 2, sy + 3, sw - 4, sh - 4);
    fill_round_rect(sx, sy, sw, sh, R, body, RR_ALL);
    fill_round_rect(sx, sy, sw, title_h, R, title_bg, RR_TOP);
    rect(sx + 1, sy + title_h, sw - 2, 1, light ? rgb(196, 200, 204) : rgb(60, 64, 69));
    rect(sx + R, sy, sw - 2 * R, 1, accent);
    rect(sx + R, sy + sh - 1, sw - 2 * R, 1, accent);
    rect(sx, sy + R, 1, sh - 2 * R, accent);
    rect(sx + sw - 1, sy + R, 1, sh - 2 * R, accent);

    /* 标题栏：圆角图标 + 标题 + 右侧版本号（与 draw_window_frame 一致） */
    int ico = 14, icy = sy + (title_h - ico) / 2;
    fill_round_rect(sx + 12, icy, ico, ico, 3, accent, RR_ALL);
    rect(sx + 12 + 4, icy + 5, ico - 8, ico - 9, rgb(255, 255, 255));
    text(sx + 34, sy + (title_h - lh) / 2, "欢迎使用 HBOS", rgb(255, 255, 255), 1);
    {
        const char *v = HBOS_VERSION_TAG;
        int vw = text_width(v, 1);
        text(sx + sw - 16 - vw, sy + (title_h - lh) / 2, v,
             light ? rgb(90, 96, 102) : rgb(190, 196, 202), 1);
    }

    /* 圆角 logo 方块：HBOS "H" 标志 */
    int bx = sx + 18, by = sy + title_h + 20;
    fill_round_rect(bx, by, 48, 48, 8, accent, RR_ALL);
    draw_hbos_logo(bx + 12, by + 12, 24, rgb(255, 255, 255));

    text(sx + 82, sy + title_h + 22, "HBOS 图形桌面", tcol, 1);
    text(sx + 82, sy + title_h + 44, "64 位 x86_64 · BIOS / UEFI 双启动", scol, 1);
    text(sx + 82, sy + title_h + 64, "并发多窗口 · 协作式多任务", scol, 1);

    /* 圆角进度条 */
    int bar_x = sx + 18, bar_w = sw - 36, bar_y = sy + sh - 32, bar_h = 8;
    fill_round_rect(bar_x, bar_y, bar_w, bar_h, 4,
                    light ? rgb(222, 227, 232) : rgb(24, 30, 38), RR_ALL);
    int filled = ticks > 0 ? bar_w * ticks / 90 : 0;
    if (filled > 8) fill_round_rect(bar_x, bar_y, filled, bar_h, 4, accent, RR_ALL);

    text(sx + 18, sy + sh - 16, "点击任意位置进入桌面", scol, 1);
}

/* toast 静止位置的几何（宽度随消息文字变化，位置固定在右下角），供
 * draw_toast 和调用方算脏矩形共用，避免两边算法不小心画不一致。 */
static void toast_metrics(int w, int h, gui_state_t *st, int *out_x, int *out_y, int *out_w, int *out_h) {
    int tw = text_width(st->toast_msg, 1);
    int bw = tw + ui_s(36);
    if (bw < ui_s(200)) bw = ui_s(200);
    int bh = ui_s(56);
    *out_x = w - bw - ui_s(20);
    *out_y = h - TASKBAR_H - bh - ui_s(16);
    *out_w = bw;
    *out_h = bh;
}

/* Win11 风格 toast 通知：右下角浮窗，自右滑入，定时淡出（由 toast_ticks 驱动） */
static void draw_toast(int w, int h, gui_state_t *st) {
    if (st->toast_ticks <= 0 || !st->toast_msg[0]) return;
    int light = st->theme_light;
    int bx, by, bw, bh;
    toast_metrics(w, h, st, &bx, &by, &bw, &bh);

    /* 进场：前 8 帧自右滑入 */
    int appear = TOAST_TICKS - st->toast_ticks;
    if (appear < 8) bx += (8 - appear) * 14;

    soft_shadow(bx, by, bw, bh);
    fill_round_rect(bx, by, bw, bh, 10,
                    light ? 0xFFF4F6F8 : 0xFF222A33, RR_ALL);
    /* 左侧强调色条 */
    fill_round_rect(bx, by, 5, bh, 10, rgb(61, 174, 233), RR_TL | RR_BL);
    text(bx + 16, by + 9,  "通知", rgb(61, 174, 233), 1);
    text(bx + 16, by + 30, st->toast_msg,
         light ? rgb(40, 44, 48) : rgb(225, 230, 235), 1);
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

    /* ── DPI 自适应：按屏幕高度计算全局缩放（基准 1080p = 100%）──────
     * 所有经 ui_s() 的布局/图标常量随之缩放；字号也按分辨率选基号。
     * 低分辨率（如 VMware 受限）下整体缩小，高分屏整体放大。 */
    g_ui_scale = h * 100 / 1080;
    if (g_ui_scale < 70)  g_ui_scale = 70;    /* 下限：避免过小 */
    if (g_ui_scale > 220) g_ui_scale = 220;   /* 上限：避免过大 */
    {
        int nb = gui_font_base_count();
        if (nb > 1) {
            /* 默认字号固定为最小档（16px）；只有明显高分屏才自动放大，
             * 避免常规分辨率下被自动挑成大字号。 */
            int slot = (g_ui_scale >= 140) ? nb - 1 : 0;
            gui_font_set_active(slot);
        }
    }

    int mx = w / 2;
    int my = h / 2;
    uint8_t last_buttons = 0;
    int brightness_dragging = 0;
    int code_text_dragging = 0;
    int note_text_dragging = 0;
    int dragging_window = -1;
    int drag_off_x = 0;
    int drag_off_y = 0;
    int drag_last_draw_x = 0;
    int drag_last_draw_y = 0;
    int drag_pending = 0;
    int appwin_drag = -1;        /* 正在拖动的应用窗口 id（winsrv） */
    int appwin_drag_dx = 0, appwin_drag_dy = 0;
    int resizing_window = -1;
    int resize_edge = WM_EDGE_NONE;
    int resize_orig_x = 0, resize_orig_y = 0, resize_orig_w = 0, resize_orig_h = 0;
    int resize_orig_win_x = 0, resize_orig_win_y = 0;
    int cursor_edge = WM_EDGE_NONE;
    uint32_t frame_tick = 0;          // 自增帧计数，用于双击计时
    uint32_t last_title_click = 0;    // 上次点击标题栏的帧
    uint8_t last_clock_sec = 0xFF;    // 任务栏时钟秒数，用于检测秒变化并触发重绘
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
    st.taskbar_show_seconds = 1;
    st.brightness = 100;
    gui_set_brightness(st.brightness);
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
    wm_set_app_title(GUI_APP_SETTINGS, "设置");
    wm_set_app_title(GUI_APP_FILES, "文件管理器");
    wm_set_app_title(GUI_APP_TASKMGR, "任务管理器");
    wm_set_app_title(GUI_APP_SHORTCUTS, "快捷键");
    wm_set_app_title(GUI_APP_IMGVIEW, "图片查看器");
    wm_set_app_title(GUI_APP_HEXVIEW, "十六进制查看器");

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
        /* 回收已退出应用留下的窗口 */
        if (winsrv_reap_dead() > 0) gui_dirty_mark_full();

        /* 全屏画布应用（hax_gui_*）拥有屏幕时，合成器让位：不绘制、不读输入，
         * 由该应用独占 g_gui_surface 并自行 present；其退出后收回屏幕。 */
        if (g_screen_owner_task != 0) {
            const task_t *ot = task_get_by_id(g_screen_owner_task);
            if (!ot || ot->state == TASK_TERMINATED) {
                g_screen_owner_task = 0;
                gui_dirty_mark_full();
            } else {
                task_yield();
                continue;
            }
        }

        wm_update_animations(&st.wm);

        int key = key_poll();
        /* F1: toggle the start menu (keyboard-only path to every app --
         * previously mouse-only via the taskbar logo / right-click menu;
         * once open, typing filters and Enter launches, see below). */
        if (key == KB_KEY_F1) {
            wm_toggle_start_menu(&st.wm);
            if (st.wm.start_menu_open) {
                st.wm.menu_h = gui_start_menu_h();
                st.sm_search[0] = 0; st.sm_search_len = 0;
                st.status = "开始菜单";
            }
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        /* F2 / F3: shrink / grow the GUI font (global, works in any app). */
        if (key == KB_KEY_F2 || key == KB_KEY_F3) {
            int slot = gui_font_active() + (key == KB_KEY_F3 ? 1 : -1);
            gui_font_set_active(slot);
            st.status = (key == KB_KEY_F3) ? "字体已放大" : "字体已缩小";
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        /* F4: toggle light/dark theme. */
        if (key == KB_KEY_F4) {
            st.theme_light = !st.theme_light;
            st.status = st.theme_light ? "已切换为浅色主题" : "已切换为深色主题";
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        /* F5: force a full desktop repaint (manual refresh). */
        if (key == KB_KEY_F5) {
            st.status = "桌面已刷新";
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        /* 开始菜单搜索：菜单打开时键盘输入用于过滤；Enter 启动首个匹配，Esc 关闭。 */
        if (key && st.wm.start_menu_open) {
            if (key == 27) {                         /* Esc 关闭 */
                wm_close_start_menu(&st.wm);
                st.sm_search[0] = 0; st.sm_search_len = 0;
            } else if (key == '\n' || key == '\r') { /* Enter 启动首个匹配 */
                sm_entry_t e;
                if (sm_visible_at(&st, 0, &e)) {
                    wm_close_start_menu(&st.wm);
                    st.sm_search[0] = 0; st.sm_search_len = 0;
                    sm_launch(&st, &e);
                }
            } else if (key == '\b' || key == GUI_KEY_BACKSPACE) {
                if (st.sm_search_len > 0) st.sm_search[--st.sm_search_len] = 0;
            } else if (key >= 32 && key < 127 &&
                       st.sm_search_len < (int)sizeof(st.sm_search) - 1) {
                st.sm_search[st.sm_search_len++] = (char)key;
                st.sm_search[st.sm_search_len] = 0;
            }
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        /* 键盘路由：存在应用窗口时，普通按键送给最顶层应用窗口（其拥有焦点）。
         * F2/F3/F4 已在上面处理，不受影响。 */
        if (key && winsrv_count() > 0 && !st.wm.start_menu_open) {
            winsrv_window_t *fw = 0;
            for (int i = WINSRV_MAX - 1; i >= 0; i--) { fw = winsrv_get(i); if (fw) break; }
            if (fw) {
                winsrv_push_event(fw, WINEV_KEY, key, 0, 0);
                gui_dirty_mark_full();
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
                continue;
            }
        }
        if (st.splash_ticks > 0 && key) {
            st.splash_ticks = 0;
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.brightness_popup_open) {
            st.brightness_popup_open = 0;
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.cal_open) {
            st.cal_open = 0;
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

            /* 亮度滑杆拖动：鼠标持续按住时跟随水平位置更新 */
            if (brightness_dragging) {
                if (left_down) {
                    int bpx, bpy; brightness_popup_xy(w, h, &bpx, &bpy);
                    int btx0, btx1, bty; brightness_track_xy(bpx, bpy, &btx0, &btx1, &bty);
                    int cx = mx;
                    if (cx < btx0) cx = btx0;
                    if (cx > btx1) cx = btx1;
                    int pct = BR_MIN + (cx - btx0) * (BR_MAX - BR_MIN) / (btx1 - btx0);
                    st.brightness = pct;
                    gui_set_brightness(pct);
                    redraw = 1;
                } else {
                    brightness_dragging = 0;
                }
            }

            /* 代码工作台鼠标框选：按住并拖动时光标跟随鼠标位置移动，锚点不动 */
            if (code_text_dragging) {
                if (left_down) {
                    uint32_t off = 0;
                    if (hit_code_editor(w, h, &st, mx, my, &off)) {
                        st.code_cursor = off;
                        st.code_sel_active = (st.code_sel_anchor != st.code_cursor);
                        code_ensure_visible(&st);
                        redraw = 1;
                    }
                } else {
                    code_text_dragging = 0;
                }
            }

            /* 记事本鼠标框选：同上 */
            if (note_text_dragging) {
                if (left_down) {
                    uint32_t off = 0;
                    if (hit_note_editor(w, h, &st, mx, my, &off)) {
                        st.note_cursor = off;
                        st.note_sel_active = (st.note_sel_anchor != st.note_cursor);
                        redraw = 1;
                    }
                } else {
                    note_text_dragging = 0;
                }
            }

            /* 应用窗口标题栏拖动 */
            if (appwin_drag >= 0) {
                winsrv_window_t *win = left_down ? winsrv_get(appwin_drag) : 0;
                if (win) {
                    win->x = mx - appwin_drag_dx;
                    win->y = my - appwin_drag_dy;
                    if (win->y < APPWIN_TITLE_H) win->y = APPWIN_TITLE_H;
                    if (win->x < 0) win->x = 0;
                    if (win->x > w - 40) win->x = w - 40;
                    redraw = 1;
                } else {
                    appwin_drag = -1;
                }
            }

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
                if (my <= 4) snap_hint = WM_SNAP_TOP;                      // 顶部→最大化
                else if (my >= h - TASKBAR_H - 4) snap_hint = WM_SNAP_BOTTOM; // 底部→最小化
                else if (mx <= 4) snap_hint = WM_SNAP_LEFT;
                else if (mx >= w - 5) snap_hint = WM_SNAP_RIGHT;
                else snap_hint = WM_SNAP_NONE;
                st.snap_preview = snap_hint;
                st.status = snap_hint == WM_SNAP_TOP    ? "松开最大化" :
                            snap_hint == WM_SNAP_BOTTOM ? "松开最小化" :
                            snap_hint != WM_SNAP_NONE   ? "松开吸附窗口" : "窗口已移动";
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
                    if (snap_hint == WM_SNAP_BOTTOM) {
                        wm_minimize_window(&st.wm, dragging_window);
                        gui_sync_focus(&st);
                        st.status = "窗口已最小化";
                    } else if (snap_hint != WM_SNAP_NONE) {
                        wm_snap_window(&st.wm, dragging_window, snap_hint);
                        gui_sync_focus(&st);
                        st.status = snap_hint == WM_SNAP_TOP ? "窗口已最大化" : "窗口已吸附";
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
            /* 悬停在记事本/代码工作台的文本框内时换成工字形文本光标（不影响
             * 窗口缩放边缘光标，那个优先级更高，上面已经赋过值了）。 */
            if (cursor_edge == WM_EDGE_NONE) {
                uint32_t hover_off = 0;
                if ((st.app_mode == GUI_APP_NOTES && hit_note_editor(w, h, &st, mx, my, &hover_off)) ||
                    (st.app_mode == GUI_APP_CODE && hit_code_editor(w, h, &st, mx, my, &hover_off)))
                    cursor_edge = CURSOR_HOVER_TEXT;
            }

            /* 右键：弹出上下文菜单（窗口上→窗口菜单，否则→桌面菜单） */
            if ((st.buttons & 2) && !(last_buttons & 2)) {
                if (st.wm.start_menu_open) wm_close_start_menu(&st.wm);
                int wi = gui_window_at(&st, w, h, mx, my);
                st.ctx_open   = (wi >= 0) ? 2 : 1;
                st.ctx_target = wi;
                int cn = ctx_item_count(st.ctx_open);
                int cmh = cn * CTX_ITEM_H + 8;
                st.ctx_x = mx; st.ctx_y = my;
                if (st.ctx_x + CTX_W > w) st.ctx_x = w - CTX_W;
                if (st.ctx_y + cmh > h - TASKBAR_H) st.ctx_y = h - TASKBAR_H - cmh;
                if (st.ctx_x < 0) st.ctx_x = 0;
                if (st.ctx_y < 0) st.ctx_y = 0;
                redraw = 1;
            }

            if ((st.buttons & 1) && !(last_buttons & 1)) {
                st.clicks++;
                /* 应用窗口命中优先（关闭按钮 / 标题栏拖动 / 内容点击） */
                int appwin_hit = 0;
                for (int i = WINSRV_MAX - 1; i >= 0 && !appwin_hit; i--) {
                    winsrv_window_t *win = winsrv_get(i);
                    if (!win) continue;
                    int th = APPWIN_TITLE_H;
                    int cbx = win->x + win->w - 22;
                    if (mx >= cbx && mx < win->x + win->w &&
                        my >= win->y - th && my < win->y) {            /* 关闭按钮 */
                        win->want_close = 1;
                        winsrv_push_event(win, WINEV_CLOSE, 0, 0, 0);
                        appwin_hit = 1;
                    } else if (mx >= win->x && mx < win->x + win->w &&
                               my >= win->y - th && my < win->y) {     /* 标题栏→拖动 */
                        appwin_drag = i;
                        appwin_drag_dx = mx - win->x;
                        appwin_drag_dy = my - win->y;
                        appwin_hit = 1;
                    } else if (mx >= win->x && mx < win->x + win->w &&
                               my >= win->y && my < win->y + win->h) { /* 内容→鼠标事件 */
                        winsrv_push_event(win, WINEV_MOUSE,
                                          mx - win->x, my - win->y, st.buttons);
                        appwin_hit = 1;
                    }
                }
                if (appwin_hit) {
                    redraw = 1;
                } else if (st.brightness_popup_open) {
                    /* 亮度弹窗打开时：命中滑杆轨道附近就开始拖动并跳到点击位置，
                     * 点在弹窗外（含再次点亮度图标）一律关闭，跟开始菜单一致。 */
                    int bpx, bpy; brightness_popup_xy(w, h, &bpx, &bpy);
                    int btx0, btx1, bty; brightness_track_xy(bpx, bpy, &btx0, &btx1, &bty);
                    if (mx >= bpx && mx < bpx + BR_POPUP_W && my >= bpy && my < bpy + BR_POPUP_H) {
                        if (my >= bty - 12 && my < bty + 14) {
                            int cx = mx;
                            if (cx < btx0) cx = btx0;
                            if (cx > btx1) cx = btx1;
                            int pct = BR_MIN + (cx - btx0) * (BR_MAX - BR_MIN) / (btx1 - btx0);
                            st.brightness = pct;
                            gui_set_brightness(pct);
                            brightness_dragging = 1;
                        }
                    } else {
                        st.brightness_popup_open = 0;
                    }
                    redraw = 1;
                } else if (st.ctx_open) {
                    /* 上下文菜单打开时，左键命中即执行，未命中即关闭 */
                    int ci = ctx_hit(&st, mx, my);
                    if (ci >= 0) {
                        if (st.ctx_open == 1) {           /* 桌面菜单 */
                            if (ci == 0) gui_dirty_mark_full();
                            else if (ci == 1) st.theme_light = !st.theme_light;
                            else if (ci == 2) {
                                wm_toggle_start_menu(&st.wm);
                                if (st.wm.start_menu_open) {
                                    int mh = gui_start_menu_h();
                                    st.wm.menu_h = mh;
                                    st.wm.menu_x = (w - SM_W) / 2;
                                    if (st.wm.menu_x < 8) st.wm.menu_x = 8;
                                    st.wm.menu_y = h - TASKBAR_H - mh - 8;
                                    if (st.wm.menu_y < 8) st.wm.menu_y = 8;
                                }
                            }
                        } else {                          /* 窗口菜单 */
                            int t = st.ctx_target;
                            if (ci == 0) { wm_minimize_window(&st.wm, t); gui_sync_focus(&st); }
                            else if (ci == 1) {
                                wm_window_t *mw = wm_get_window(&st.wm, t);
                                if (mw && mw->state == WM_STATE_MAXIMIZED)
                                    wm_restore_window(&st.wm, t);
                                else
                                    wm_maximize_window(&st.wm, t);
                                gui_sync_focus(&st);
                            } else if (ci == 2) {
                                gui_close_window(&st, t);
                            }
                        }
                    }
                    st.ctx_open = 0;
                    gui_dirty_mark_full();
                    redraw = 1;
                } else if (st.cal_open) {
                    /* 日历打开：‹ › 翻月，面板内保持，面板外（含再点时钟）关闭 */
                    int ch = cal_hit(&st, w, h, mx, my);
                    if (ch == 1) cal_shift_month(&st, -1);
                    else if (ch == 2) cal_shift_month(&st, 1);
                    else if (ch < 0) st.cal_open = 0;
                    gui_dirty_mark_full();
                    redraw = 1;
                } else if (st.wm.start_menu_open) {
                    int item = wm_hit_start_menu(&st.wm, mx, my);
                    if (item == SM_SEARCH_ITEM) {
                        /* 点搜索框：保持菜单打开，用键盘输入即可搜索 */
                        redraw = 1;
                    } else if (item == SM_SHELL_ITEM) {
                        wm_close_start_menu(&st.wm);
                        st.sm_search[0] = 0; st.sm_search_len = 0;
                        break;
                    } else if (item == SM_POWER_ITEM) {
                        acpi_poweroff();
                        break;
                    } else if (item >= SM_CELL_BASE) {
                        /* 网格单元 → 过滤后的实际应用（内置或 .hax，统一启动） */
                        sm_entry_t e;
                        int ok = sm_visible_at(&st, item - SM_CELL_BASE, &e);
                        wm_close_start_menu(&st.wm);
                        st.sm_search[0] = 0; st.sm_search_len = 0;
                        if (ok) sm_launch(&st, &e);
                        redraw = 1;
                    } else {
                        /* 菜单内其他空白处：关闭 */
                        wm_close_start_menu(&st.wm);
                        st.sm_search[0] = 0; st.sm_search_len = 0;
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
                            st.sm_search[0] = 0; st.sm_search_len = 0;  /* 每次打开清空搜索 */
                            if (st.wm.start_menu_open) {
                                /* Win11 style: center horizontally above taskbar.
                                 * 面板高度按 .hax GUI 应用数动态增长。 */
                                int mh = gui_start_menu_h();
                                st.wm.menu_h = mh;
                                st.wm.menu_x = (w - SM_W) / 2;
                                if (st.wm.menu_x < 8) st.wm.menu_x = 8;
                                st.wm.menu_y = h - TASKBAR_H - mh - 8;
                                if (st.wm.menu_y < 8) st.wm.menu_y = 8;
                            }
                            st.status = "开始菜单";
                        } else if (tb == TBHIT_BRIGHTNESS) {
                            st.brightness_popup_open = 1;
                            st.cal_open = 0;   /* 两个弹窗共用右上角位置，互斥显示 */
                            st.status = "屏幕亮度";
                        } else if (tb == TBHIT_CLOCK) {
                            /* 点时钟：开/关日历弹窗 */
                            if (st.cal_open) st.cal_open = 0;
                            else { cal_open_now(&st); st.brightness_popup_open = 0; }
                            st.status = "日历";
                        } else if (tb == TBHIT_SHOWDESK) {
                            /* 显示桌面：最小化全部可见窗口；再点恢复 */
                            int any = 0;
                            for (int i = 0; i < st.wm.window_count; i++) {
                                wm_window_t *sw2 = wm_get_window(&st.wm, i);
                                if (sw2 && sw2->used && sw2->state != WM_STATE_MINIMIZED) any = 1;
                            }
                            if (any) {
                                st.showdesk_mask = 0;
                                for (int i = 0; i < st.wm.window_count; i++) {
                                    wm_window_t *sw2 = wm_get_window(&st.wm, i);
                                    if (sw2 && sw2->used && sw2->state != WM_STATE_MINIMIZED) {
                                        st.showdesk_mask |= 1u << i;
                                        wm_minimize_window(&st.wm, i);
                                    }
                                }
                                st.showdesk_active = 1;
                                st.status = "已显示桌面";
                            } else if (st.showdesk_active) {
                                for (int i = 0; i < st.wm.window_count; i++) {
                                    wm_window_t *sw2 = wm_get_window(&st.wm, i);
                                    if ((st.showdesk_mask & (1u << i)) && sw2 &&
                                        sw2->used && sw2->state == WM_STATE_MINIMIZED)
                                        wm_restore_window(&st.wm, i);
                                }
                                st.showdesk_active = 0;
                                st.status = "已恢复窗口";
                            }
                            gui_sync_focus(&st);
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
                                    dragging_window = title_idx;
                                    // Dragging a maximized window restores it
                                    // first so it follows the cursor (standard
                                    // behavior) instead of staying stuck fullscreen.
                                    wm_window_t *dw = wm_get_window(&st.wm, title_idx);
                                    int win_x, win_y, win_w, win_h;
                                    if (dw && dw->state == WM_STATE_MAXIMIZED) {
                                        wm_restore_window(&st.wm, title_idx);
                                        gui_window_metrics(&st, w, h, NULL, title_idx, &win_x, &win_y, &win_w, &win_h);
                                        drag_off_x = win_w / 2;
                                        drag_off_y = WM_TITLE_H / 2;
                                        gui_set_active_window_pos(&st, mx - drag_off_x, my - drag_off_y);
                                    } else {
                                        gui_window_metrics(&st, w, h, NULL, title_idx, &win_x, &win_y, &win_w, &win_h);
                                        drag_off_x = mx - win_x;
                                        drag_off_y = my - win_y;
                                    }
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
                                } else if (hit_browser_load_button(w, h, &st, mx, my)) {
                                    browser_load(&st);
                                } else if (hit_browser_nav_button(w, h, &st, mx, my) == 1) {
                                    browser_go_back(&st);
                                } else if (hit_browser_nav_button(w, h, &st, mx, my) == 2) {
                                    browser_go_forward(&st);
                                } else if (hit_browser_link(w, h, &st, mx, my) >= 0) {
                                    int link_idx = hit_browser_link(w, h, &st, mx, my);
                                    char resolved[BROWSER_URL_CAP];
                                    if (browser_resolve_href(&st, st.browser_link_href[link_idx],
                                                             resolved, sizeof(resolved)) == 0) {
                                        browser_navigate(&st, resolved);
                                    } else {
                                        st.status = "该链接不支持跳转";
                                    }
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
                                            uint32_t code_off = 0, note_off = 0;
                                            if (hit_code_editor(w, h, &st, mx, my, &code_off)) {
                                                st.code_cursor = code_off;
                                                st.code_sel_anchor = code_off;
                                                st.code_sel_active = 0;
                                                code_text_dragging = 1;
                                                code_ensure_visible(&st);
                                                st.status = "已移动代码光标";
                                            } else if (hit_note_editor(w, h, &st, mx, my, &note_off)) {
                                                st.note_cursor = note_off;
                                                st.note_sel_anchor = note_off;
                                                st.note_sel_active = 0;
                                                note_text_dragging = 1;
                                                st.status = "已移动笔记光标";
                                            } else {
                                                int aw_x, aw_y, aw_w, aw_h;
                                                int aw = st.wm.active_window;
                                                gui_window_metrics(&st, w, h, NULL, aw, &aw_x, &aw_y, &aw_w, &aw_h);
                                                int atx = aw_x + 30, aty = aw_y + 42;
                                                if (!gui_app_on_click(&st, mx, my, atx, aty, aw_w, aw_h))
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
        /* 有并发应用窗口时，每帧重绘以反映它们的实时刷新 */
        if (winsrv_count() > 0) {
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        /* 控制台/记事本/代码工作台都有闪烁光标，静置无输入时也要靠这个定时
         * 重绘才能继续闪烁——否则会卡在某一帧的显隐状态，跟之前任务栏时钟
         * 卡住不走是同一类问题（见 gui_dirty 那段注释）。 */
        if ((st.app_mode == GUI_APP_DIAG || st.app_mode == GUI_APP_NOTES ||
             st.app_mode == GUI_APP_CODE) && (frame_tick % 20 == 0)) {
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.switcher_ticks > 0) {
            st.switcher_ticks--;
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        /* splash/toast 之前每帧都 gui_dirty_mark_full()（整屏重绘），和上面
         * 任务栏时钟注释里说的是同一类问题——欢迎窗口显示的那 90 帧、每条
         * 通知显示的那几秒，整屏重绘的开销让 GUI 明显发卡（用户实测反馈：
         * "显示通知浮窗和欢迎界面没消失时特别卡"）。这两个浮层的绘制区域
         * 都是已知的固定矩形（splash 屏幕居中定长；toast 右下角，宽度随
         * 消息文字变、滑入动画只会让它更靠右），改成只标记这块区域，不用
         * 每帧都重画整个桌面+所有窗口。 */
        if (st.splash_ticks > 0) {
            st.splash_ticks--;
            int sw = ui_s(440), sh = ui_s(208);
            gui_damage_region((w - sw) / 2, (h - sh) / 2, sw, sh, w, h);
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.toast_ticks > 0) {
            st.toast_ticks--;
            int tx, ty, tw2, th2;
            toast_metrics(w, h, &st, &tx, &ty, &tw2, &th2);
            gui_damage_region(tx, ty, tw2 + 8 * 14, th2, w, h); /* +滑入动画最大右侧偏移 */
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        /* 任务栏时钟只在重绘时刷新；若没有输入/动画驱动重绘（长时间静置桌面），
         * 时钟会卡住不走。这里独立检测秒数变化，保证时钟始终实时前进。
         * 只标记任务栏这一小条为脏区域（而非 gui_dirty_mark_full 整屏重绘）——
         * 整屏重绘开销大，每秒强制一次会造成周期性卡顿，表现为"鼠标又变卡"。
         * 设置里关掉"显示秒数"后，显示的文本每分钟才变一次，没必要再每秒强制
         * 重绘一次（那样纯粹是浪费）——改成只在跨分钟（秒数归零）时才重绘。 */
        {
            uint8_t cur_sec = cmos_second();
            int need_redraw = st.taskbar_show_seconds
                ? (cur_sec != last_clock_sec)
                : (cur_sec == 0 && last_clock_sec != 0);
            last_clock_sec = cur_sec;
            if (need_redraw) {
                gui_damage_region(0, h - TASKBAR_H, w, TASKBAR_H, w, h);
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            }
        }
        if (wm_has_active_animations(&st.wm)) {
            /* If any window is moving/resizing (maximize/restore), repaint the
             * whole frame — per-window damage only covers the CURRENT rect, so a
             * growing/shrinking window leaves trails of its previous frames.
             * Opacity-only animations (open/minimize fade) can use cheap
             * per-window damage. */
            int geom_anim = 0;
            for (int i = 0; i < st.wm.window_count; i++) {
                wm_window_t *win = wm_get_window(&st.wm, i);
                if (win && (win->anim_type == WM_ANIM_MAXIMIZE ||
                            win->anim_type == WM_ANIM_RESTORE)) { geom_anim = 1; break; }
            }
            if (geom_anim) {
                gui_dirty_mark_full();
            } else {
                gui_dirty_reset();
                for (int i = 0; i < st.wm.window_count; i++) {
                    wm_window_t *win = wm_get_window(&st.wm, i);
                    if (win && win->anim_type != WM_ANIM_NONE)
                        gui_damage_window(&st, w, h, i);
                }
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
void gui_blit_rgb888(int x, int y, const uint8_t *rgbdata, int img_w, int img_h,
                      int clip_x, int clip_y, int clip_w, int clip_h) {
    if (!g_gui_surface || !rgbdata || img_w <= 0 || img_h <= 0 || clip_w <= 0 || clip_h <= 0) return;
    int x0 = x, y0 = y, x1 = x + img_w, y1 = y + img_h;
    if (x0 < clip_x) x0 = clip_x;
    if (y0 < clip_y) y0 = clip_y;
    if (x1 > clip_x + clip_w) x1 = clip_x + clip_w;
    if (y1 > clip_y + clip_h) y1 = clip_y + clip_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_gui_surface_w) x1 = g_gui_surface_w;
    if (y1 > g_gui_surface_h) y1 = g_gui_surface_h;
    for (int py = y0; py < y1; py++) {
        int src_y = py - y;
        uint32_t *drow = g_gui_surface + (uint32_t)py * g_gui_surface_pitch;
        const uint8_t *srow = rgbdata + (uint32_t)src_y * img_w * 3;
        for (int px = x0; px < x1; px++) {
            const uint8_t *p = srow + (px - x) * 3;
            drow[px] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
}
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
void gui_append_ll(char *buf, uint32_t cap, uint32_t *pos, long long v) { append_ll(buf, cap, pos, v); }
void gui_append_sci(char *buf, uint32_t cap, uint32_t *pos, long long mant, int exp) {
    append_sci(buf, cap, pos, mant, exp);
}

void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    while (1) {
        rect(x0, y0, 1, 1, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
}

void gui_draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint32_t color) {
    if (thickness <= 1) { gui_draw_line(x0, y0, x1, y1, color); return; }
    int half = thickness / 2;
    /* draw several parallel offsets for thickness */
    int dx = x1 - x0, dy = y1 - y0;
    /* perpendicular direction */
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (ady > adx) {
        /* mostly vertical: spread horizontally */
        for (int i = -half; i <= half; i++)
            gui_draw_line(x0 + i, y0, x1 + i, y1, color);
    } else {
        /* mostly horizontal: spread vertically */
        for (int i = -half; i <= half; i++)
            gui_draw_line(x0, y0 + i, x1, y1 + i, color);
    }
}

void gui_fill_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while (dx * dx + dy * dy <= r * r) dx++;
        rect(cx - dx + 1, cy + dy, 2 * dx - 2, 1, color);
    }
}

void gui_draw_circle(int cx, int cy, int r, uint32_t color) {
    int x = 0, y = r, d = 3 - 2 * r;
    while (x <= y) {
        rect(cx + x, cy - y, 1, 1, color); rect(cx - x, cy - y, 1, 1, color);
        rect(cx + x, cy + y, 1, 1, color); rect(cx - x, cy + y, 1, 1, color);
        rect(cx + y, cy - x, 1, 1, color); rect(cx - y, cy - x, 1, 1, color);
        rect(cx + y, cy + x, 1, 1, color); rect(cx - y, cy + x, 1, 1, color);
        if (d < 0) d += 4 * x + 6; else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

void gui_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color) {
    fill_round_rect(x, y, w, h, r, color, RR_TL | RR_TR | RR_BL | RR_BR);
}
