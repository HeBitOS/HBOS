#include <stdbool.h>
#include <stdint.h>

#include "../acpi.h"
#include "../block.h"
#include "../core/pmm.h"
#include "../core/task.h"
#include "../fcntl.h"
#include "../fs.h"
#include "../graphics/font_cjk.h"
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
#include "../shell/shell.h"
#include "tool.h"

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

#define GUI_APP_NONE  -1
#define GUI_APP_NOTES 0
#define GUI_APP_CALC  1
#define GUI_APP_UWC   2
#define GUI_APP_SNAKE 3
#define GUI_APP_BROWSER 4
#define GUI_APP_CODE 5
#define GUI_APP_DIAG 6
#define GUI_APP_CLOCK 7

#define ACTION_W 116
#define ACTION_H 28
#define FILE_ACTION_COUNT 7
#define GUI_MOUSE_POLL_BUDGET 16
#define TASKBAR_H 44
#define NOTE_EDIT_CAP 512
#define BROWSER_URL_CAP 160
#define BROWSER_PAGE_CAP 2048
#define CODE_EDIT_CAP 4096
#define CODE_OUTPUT_CAP 256
#define CODE_VIEW_ROWS 10
#define SNAKE_W 16
#define SNAKE_H 10
#define SNAKE_MAX (SNAKE_W * SNAKE_H)
#define GUI_PAGE_SIZE 4096ULL
#define FILE_LIST_ROWS 8
#define FILE_ROW_H 26
#define NOTE_FILE_ROWS 7
#define GUI_PATH_MAX 256
#define CODE_CMD_SAVE 1
#define CODE_CMD_RUN  2
#define CODE_CMD_OPEN 3

typedef struct {
    int active;
    int selected_file;
    int selected_app;
    int app_mode;
    int calc_value;
    int calc_acc;
    int calc_input;
    int calc_last_lhs;
    int calc_last_rhs;
    int calc_just_evaluated;
    char calc_op;
    char calc_last_op;
    int calc_has_input;
    int calc_error;
    int snake_x;
    int snake_y;
    int snake_tx;
    int snake_ty;
    int snake_score;
    int snake_len;
    int snake_dx;
    int snake_dy;
    int snake_alive;
    uint8_t snake_last_sec;
    int snake_body_x[SNAKE_MAX];
    int snake_body_y[SNAKE_MAX];
    int win_x;
    int win_y;
    int clicks;
    uint8_t buttons;
    wm_state_t wm;
    int last_clicked_file;
    char file_path[GUI_PATH_MAX];
    char note_buf[NOTE_EDIT_CAP];
    uint32_t note_len;
    uint32_t note_cursor;
    int note_dirty;
    int note_loaded;
    char note_name[MAX_FILENAME];
    char browser_url[BROWSER_URL_CAP];
    char browser_page[BROWSER_PAGE_CAP];
    uint32_t browser_page_len;
    int browser_loaded;
    int browser_scroll;
    char code_path[GUI_PATH_MAX];
    uint32_t code_len;
    uint32_t code_cursor;
    int code_loaded;
    int code_modified;
    int code_scroll;
    int code_error_line;
    int code_view_rows;
    int rename_active;
    char rename_buf[MAX_FILENAME];
    uint32_t rename_len;
    int delete_confirm_index;
    const char *status;
    int splash_ticks;
    int snap_preview;   // 拖动吸附预览：WM_SNAP_*
    uint8_t clock_last_sec;
    int switcher_ticks; // 切换器浮层剩余显示帧数
    int theme_light;    // 主题：0-深色赛博 1-浅色赛博
    char console_input[80];
    uint32_t console_input_len;
    char console_history[16][80];
    uint32_t console_line_count;
} gui_state_t;

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);

static inline uint32_t cyber_bg_top(int light) { return light ? rgb(230, 235, 242) : rgb(18, 12, 28); }
static inline uint32_t cyber_bg_bot(int light) { return light ? rgb(200, 208, 220) : rgb(8, 5, 14); }
static inline uint32_t cyber_neon_pink(int light) { return light ? rgb(220, 0, 100) : rgb(255, 0, 128); }
static inline uint32_t cyber_neon_cyan(int light) { return light ? rgb(0, 160, 200) : rgb(0, 240, 255); }
static inline uint32_t cyber_neon_yellow(int light) { return light ? rgb(200, 170, 0) : rgb(255, 230, 0); }
static inline uint32_t cyber_neon_purple(int light) { return light ? rgb(140, 40, 200) : rgb(196, 116, 230); }
static inline uint32_t cyber_text(int light) { return light ? rgb(15, 20, 30) : rgb(240, 248, 252); }
static inline uint32_t cyber_text_muted(int light) { return light ? rgb(100, 110, 125) : rgb(168, 188, 202); }
static inline uint32_t cyber_border(int light) { return light ? rgb(140, 160, 180) : rgb(80, 0, 120); }
static inline uint32_t cyber_card_bg_top(int light) { return light ? rgb(245, 247, 250) : rgb(22, 15, 34); }
static inline uint32_t cyber_card_bg_bot(int light) { return light ? rgb(220, 226, 235) : rgb(10, 8, 18); }

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
} gui_app_t;

typedef struct {
    const char *label;
    int action;
} gui_file_action_t;

static const gui_app_t gui_apps[] = {
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

static void gui_set_surface(uint32_t *surface, int w, int h, uint32_t pitch_px) {
    g_gui_surface = surface;
    g_gui_surface_w = surface ? w : 0;
    g_gui_surface_h = surface ? h : 0;
    g_gui_surface_pitch = surface ? pitch_px : 0;
}

static void gui_present_surface(const fb_info_t *fb) {
    if (!fb || !g_gui_surface) return;
    uint32_t fb_pitch = (uint32_t)(fb->pitch / 4);
    for (int y = 0; y < g_gui_surface_h; y++) {
        uint32_t *dst = fb->addr + (uint32_t)y * fb_pitch;
        uint32_t *src = g_gui_surface + (uint32_t)y * g_gui_surface_pitch;
        for (int x = 0; x < g_gui_surface_w; x++) dst[x] = src[x];
    }
}

static void rect(int x, int y, int w, int h, uint32_t color);

static void rect_alpha(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
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

static void draw_panel_shell(int x, int y, int w, int h, uint32_t top,
                             uint32_t bottom, uint32_t border_c, uint32_t accent) {
    if (w <= 0 || h <= 0) return;
    vgradient(x, y, w, h, top, bottom);
    rect(x, y, w, 1, rgb_lift(border_c, 28));
    rect(x + 1, y + 1, w - 2, 1, rgb_lift(top, 18));
    rect(x, y + h - 1, w, 1, rgb_lift(bottom, -28));
    rect(x, y, 1, h, rgb_lift(border_c, 8));
    rect(x + w - 1, y, 1, h, rgb_lift(bottom, -18));
    if (accent) {
        rect(x, y + 1, 4, h - 2, accent);
        rect(x + 4, y + 1, 1, h - 2, rgb_lift(accent, -36));
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

static const uint8_t *glyph(char c) {
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    static const uint8_t unknown[7] = {14,17,1,2,4,0,4};
    static const uint8_t table[][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,30,1,1,17,14},
        {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
        {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
        {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
        {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
        {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
        {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
        {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
        {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
        {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
        {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
    };
    static const uint8_t lower[][7] = {
        {0,0,14,1,15,17,15}, {16,16,30,17,17,17,30},
        {0,0,14,16,16,17,14}, {1,1,15,17,17,17,15},
        {0,0,14,17,31,16,14}, {6,8,8,30,8,8,8},
        {0,0,15,17,17,15,1}, {16,16,30,17,17,17,17},
        {4,0,12,4,4,4,14}, {2,0,6,2,2,18,12},
        {16,16,18,20,24,20,18}, {12,4,4,4,4,4,14},
        {0,0,26,21,21,17,17}, {0,0,30,17,17,17,17},
        {0,0,14,17,17,17,14}, {0,0,30,17,17,30,16},
        {0,0,15,17,17,15,1}, {0,0,22,24,16,16,16},
        {0,0,15,16,14,1,30}, {8,8,30,8,8,9,6},
        {0,0,17,17,17,19,13}, {0,0,17,17,17,10,4},
        {0,0,17,17,21,21,10}, {0,0,17,10,4,10,17},
        {0,0,17,17,17,15,1}, {0,0,31,2,4,8,31}
    };
    static const uint8_t colon[7] = {0,4,4,0,4,4,0};
    static const uint8_t dot[7] = {0,0,0,0,0,4,4};
    static const uint8_t dash[7] = {0,0,0,31,0,0,0};
    static const uint8_t slash[7] = {1,1,2,4,8,16,16};
    static const uint8_t plus[7] = {0,4,4,31,4,4,0};
    static const uint8_t percent[7] = {17,2,4,8,16,17,0};
    static const uint8_t equal[7] = {0,0,31,0,31,0,0};
    static const uint8_t gt[7] = {16,8,4,2,4,8,16};
    static const uint8_t lt[7] = {1,2,4,8,4,2,1};
    static const uint8_t bang[7] = {4,4,4,4,4,0,4};
    static const uint8_t question[7] = {14,17,1,2,4,0,4};
    static const uint8_t quote[7] = {10,10,10,0,0,0,0};
    static const uint8_t apos[7] = {4,4,4,0,0,0,0};
    static const uint8_t comma[7] = {0,0,0,0,0,4,8};
    static const uint8_t semicolon[7] = {0,4,4,0,4,4,8};
    static const uint8_t underscore[7] = {0,0,0,0,0,0,31};
    static const uint8_t star[7] = {0,21,14,31,14,21,0};
    static const uint8_t hash[7] = {10,31,10,10,31,10,0};
    static const uint8_t dollar[7] = {4,15,20,14,5,30,4};
    static const uint8_t amp[7] = {12,18,20,8,21,18,13};
    static const uint8_t at[7] = {14,17,23,21,23,16,14};
    static const uint8_t caret[7] = {4,10,17,0,0,0,0};
    static const uint8_t tilde[7] = {0,0,8,21,2,0,0};
    static const uint8_t lparen[7] = {2,4,8,8,8,4,2};
    static const uint8_t rparen[7] = {8,4,2,2,2,4,8};
    static const uint8_t lbrack[7] = {14,8,8,8,8,8,14};
    static const uint8_t rbrack[7] = {14,2,2,2,2,2,14};
    static const uint8_t lbrace[7] = {2,4,4,8,4,4,2};
    static const uint8_t rbrace[7] = {8,4,4,2,4,4,8};
    static const uint8_t backslash[7] = {16,16,8,4,2,1,1};
    static const uint8_t pipe[7] = {4,4,4,4,4,4,4};

    if (c == ' ') return blank;
    if (c >= 'a' && c <= 'z') return lower[c - 'a'];
    if (c >= '0' && c <= '9') return table[c - '0'];
    if (c >= 'A' && c <= 'Z') return table[10 + c - 'A'];
    if (c == ':') return colon;
    if (c == '.') return dot;
    if (c == '-') return dash;
    if (c == '/') return slash;
    if (c == '+') return plus;
    if (c == '%') return percent;
    if (c == '=') return equal;
    if (c == '>') return gt;
    if (c == '<') return lt;
    if (c == '!') return bang;
    if (c == '?') return question;
    if (c == '"') return quote;
    if (c == '\'') return apos;
    if (c == ',') return comma;
    if (c == ';') return semicolon;
    if (c == '_') return underscore;
    if (c == '*') return star;
    if (c == '#') return hash;
    if (c == '$') return dollar;
    if (c == '&') return amp;
    if (c == '@') return at;
    if (c == '^') return caret;
    if (c == '~') return tilde;
    if (c == '(') return lparen;
    if (c == ')') return rparen;
    if (c == '[') return lbrack;
    if (c == ']') return rbrack;
    if (c == '{') return lbrace;
    if (c == '}') return rbrace;
    if (c == '\\') return backslash;
    if (c == '|') return pipe;
    return unknown;
}

static int draw_cjk_text_glyph(int x, int y, uint32_t cp, uint32_t color, int scale) {
    const uint8_t *bitmap = cjk_font_lookup(cp);
    if (!bitmap) return 0;
    for (int row = 0; row < CJK_GLYPH_SIZE; row++) {
        uint8_t byte0 = bitmap[row * 2];
        uint8_t byte1 = bitmap[row * 2 + 1];
        for (int col = 0; col < 8; col++) {
            if (byte0 & (0x80 >> col))
                rect(x + col * scale, y + row * scale, scale, scale, color);
            if (byte1 & (0x80 >> col))
                rect(x + (8 + col) * scale, y + row * scale, scale, scale, color);
        }
    }
    return CJK_GLYPH_SIZE * scale;
}

static int draw_text_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale) {
    if (cp < 0x80) {
        const uint8_t *g = glyph((char)cp);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (g[row] & (1 << (4 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
        return 6 * scale;
    }

    int advance = draw_cjk_text_glyph(x, y, cp, color, scale);
    if (advance == 0) {
        const uint8_t *g = glyph('?');
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (g[row] & (1 << (4 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
        advance = 6 * scale;
    }
    return advance;
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

static void text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale) {
    if (!s || max_x <= x) return;
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        int advance = draw_text_codepoint(x, y, cp, color, scale);
        if (x + advance > max_x) {
            rect(x, y, max_x - x + 1, 7 * scale + scale, rgb(18, 27, 36));
            break;
        }
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

static int clamp_delta(int v) {
    /* Linear 1:1 mapping with a safety cap to prevent runaway */
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
    soft_shadow(x, y, w, ACTION_H);
    draw_panel_shell(x, y, w, ACTION_H,
                     rgb_lift(color, 34), rgb_lift(color, -24),
                     rgb_lift(color, -46), color);
    rect(x + 7, y + 5, w - 14, 1, rgb_lift(color, 72));
    text_clipped(x + 11, y + 10, x + w - 10, label, rgb(252, 254, 255), 1);
}

static uint32_t gui_file_action_color(int action) {
    if (action == 0) return rgb(85, 180, 120);
    if (action == 1) return rgb(23, 147, 209);
    if (action == 9) return rgb(204, 156, 74);
    if (action == 7) return rgb(124, 220, 154);
    if (action == 8) return rgb(102, 214, 255);
    if (action == 2) return rgb(102, 214, 255);
    if (action == 3) return rgb(244, 194, 82);
    if (action == 4) return rgb(234, 82, 82);
    return rgb(132, 150, 162);
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

static void draw_task_button(int x, int y, int w, const char *label, int active, uint32_t color, int light) {
    uint32_t top = active ? rgb_lift(color, 20) : (light ? rgb(240, 244, 248) : rgb(31, 38, 43));
    uint32_t bottom = active ? rgb_lift(color, -32) : (light ? rgb(220, 225, 230) : rgb(16, 20, 24));
    uint32_t outline = active ? rgb_lift(color, 42) : (light ? rgb(180, 185, 192) : rgb(39, 48, 55));
    draw_panel_shell(x, y, w, 32, top, bottom, outline,
                     active ? color : rgb_lift(color, -54));
    rect(x + 8, y + 25, w - 16, 2,
         active ? cyber_neon_yellow(light) : (light ? rgb(190, 195, 200) : rgb(48, 58, 62)));
    text_clipped(x + 12, y + 11, x + w - 12, label,
                 active ? cyber_text(light) : cyber_text_muted(light), 1);
}

static void draw_icon(int x, int y, int active, const char *label, uint32_t color, int light) {
    if (active) soft_shadow(x, y, 82, 58);
    draw_panel_shell(x, y, 82, 58,
                     active ? rgb_lift(color, 12) : (light ? rgb(240, 244, 248) : rgb(30, 37, 42)),
                     active ? rgb_lift(color, -28) : (light ? rgb(220, 225, 230) : rgb(15, 19, 23)),
                     active ? rgb_lift(color, 48) : (light ? rgb(180, 185, 192) : rgb(39, 50, 58)),
                     color);
    int gx = x + 14, gy = y + 12;
    vgradient(gx, gy, 22, 22, rgb_lift(color, 30), rgb_lift(color, -10));
    border(gx, gy, 22, 22, rgb_lift(color, -30));
    rect(gx + 5, gy + 7, 12, 8, light ? rgb(20, 30, 40) : rgb(240, 248, 252));
    text_clipped(x + 42, y + 22, x + 78, label,
                 active ? cyber_text(light) : cyber_text_muted(light), 1);
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

enum {
    GUI_KEY_UP = 1001,
    GUI_KEY_DOWN,
    GUI_KEY_LEFT,
    GUI_KEY_RIGHT,
    GUI_KEY_BACKSPACE,
    GUI_KEY_DELETE,
    GUI_KEY_HOME,
    GUI_KEY_END,
    GUI_KEY_PGUP,
    GUI_KEY_PGDOWN,
};

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

static void draw_wallpaper(int w, int h, int light) {
    char line[32];
    int tb_y = h - TASKBAR_H;
    if (light) {
        vgradient(0, 0, w, tb_y, rgb(235, 240, 248), rgb(215, 220, 228));
    } else {
        vgradient(0, 0, w, tb_y, rgb(25, 10, 35), rgb(5, 5, 15));
    }
    int sb_x = 112;
    if (light) {
        vgradient(0, 0, sb_x, tb_y, rgb(220, 225, 232), rgb(200, 205, 212));
    } else {
        vgradient(0, 0, sb_x, tb_y, rgb(15, 10, 24), rgb(5, 4, 10));
    }
    rect(sb_x - 2, 0, 1, tb_y, cyber_neon_pink(light));
    rect(sb_x - 1, 0, 1, tb_y, cyber_neon_cyan(light));
    rect(sb_x, 0, 1, tb_y, light ? rgb(180, 185, 192) : rgb(10, 10, 20));

    for (int yy = 18; yy < tb_y - 10; yy += 34) {
        int offset = (yy * 3) & 95;
        rect(sb_x + offset, yy, w - sb_x - offset - 18, 1, light ? rgb(210, 215, 222) : rgb(40, 0, 60));
        rect(sb_x + 34 + offset, yy + 9, w - sb_x - offset - 90, 1, light ? rgb(225, 220, 215) : rgb(0, 40, 50));
    }

    int dot = 0;
    for (int yy = 84; yy < tb_y - 20; yy += 28) {
        for (int xx = 140; xx < w - 30; xx += 28) {
            uint8_t r_val = (dot & 1) ? 220 : 0;
            uint8_t g_val = (dot & 1) ? 0 : 220;
            uint8_t b_val = (dot & 1) ? 180 : 255;
            if (light) {
                r_val = (uint8_t)(r_val * 7 / 10);
                g_val = (uint8_t)(g_val * 7 / 10);
                b_val = (uint8_t)(b_val * 7 / 10);
            }
            uint8_t factor = (xx * 3 + yy * 7 + dot) % 6;
            if (factor == 0) {
                rect(xx, yy, 2, 2, rgb((uint8_t)(r_val/3), (uint8_t)(g_val/3), (uint8_t)(b_val/3)));
            } else {
                rect(xx, yy, 1, 1, rgb((uint8_t)(r_val/8), (uint8_t)(g_val/8), (uint8_t)(b_val/8)));
            }
            dot++;
        }
    }

    if (light) {
        vgradient(0, tb_y, w, TASKBAR_H, rgb(210, 216, 224), rgb(195, 200, 210));
    } else {
        vgradient(0, tb_y, w, TASKBAR_H, rgb(15, 5, 25), rgb(5, 2, 10));
    }
    rect(0, tb_y - 2, w, 1, cyber_neon_pink(light));
    rect(0, tb_y - 1, w, 1, cyber_neon_cyan(light));
    rect(0, tb_y, w, 1, light ? rgb(180, 185, 192) : rgb(10, 10, 20));

    hgradient(8, h - 38, 100, 32, cyber_neon_pink(light), light ? rgb(160, 0, 70) : rgb(180, 0, 90));
    rect(8, h - 38, 100, 1, light ? rgb(255, 120, 200) : rgb(255, 100, 180));
    rect(8, h - 7, 100, 1, light ? rgb(100, 0, 40) : rgb(80, 0, 40));
    border(8, h - 38, 100, 32, cyber_neon_cyan(light));
    rect(20, h - 28, 7, 7, cyber_neon_yellow(light));
    text(36, h - 28, "开始", rgb(255, 255, 255), 1);

    time_line(line, sizeof(line));
    draw_panel_shell(w - 102, h - 38, 94, 32,
                     light ? rgb(225, 230, 238) : rgb(28, 34, 37),
                     light ? rgb(210, 215, 222) : rgb(12, 15, 18),
                     cyber_border(light), cyber_neon_cyan(light));
    text(w - 88, h - 28, line, cyber_text(light), 1);
}

static void draw_desktop_tile(int x, int y, int w, int h, const char *title,
                              const char *value, uint32_t accent, int light) {
    soft_shadow(x, y, w, h);
    draw_panel_shell(x, y, w, h, cyber_card_bg_top(light), cyber_card_bg_bot(light),
                     cyber_border(light), accent);
    rect(x + 14, y + 12, 18, 4, accent);
    rect(x + 14, y + 19, 30, 1, rgb_lift(accent, -32));
    text(x + 16, y + 30, title, cyber_text_muted(light), 1);
    text_clipped(x + 16, y + 52, x + w - 12, value, cyber_text(light), 1);
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
        if ((i & 1) == 1) rect(main_x + 1, y - 8, main_w - 2, FILE_ROW_H, rgb(30, 44, 56));
        if ((int)file_idx == selected) {
            vgradient(main_x + 6, y - 7, main_w - 12, 24, rgb(28, 130, 180), rgb(18, 90, 130));
            rect(main_x + 6, y - 7, 4, 24, rgb(124, 220, 154));
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

static void draw_apps_panel(int tx, int ty, int win_w, const gui_state_t *st) {
    (void)win_w;
    char line[96];
    text(tx, ty, "应用程序", rgb(24, 112, 166), 1);
    line_u32(line, sizeof(line), "图形应用: ", gui_app_count(), "");
    text(tx + 112, ty + 2, line, rgb(94, 112, 124), 1);
    draw_button(tx, ty + 42, "打开 Enter", rgb(23, 147, 209));

    int selected = st->selected_app;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= gui_app_count()) selected = (int)gui_app_count() - 1;
    uint32_t max = gui_app_count();
    for (uint32_t i = 0; i < max; i++) {
        const gui_app_t *app = &gui_apps[i];
        if (!app) continue;
        int card_w = 250;
        int card_h = 62;
        int col = (int)(i & 1);
        int row = (int)(i >> 1);
        int x = tx + col * (card_w + 18);
        int y = ty + 82 + row * (card_h + 10);
        uint32_t accent = app->mode == GUI_APP_NOTES ? rgb(85, 180, 120) :
                          app->mode == GUI_APP_CALC ? rgb(23, 147, 209) :
                          app->mode == GUI_APP_UWC ? rgb(244, 194, 82) :
                          app->mode == GUI_APP_SNAKE ? rgb(124, 220, 154) :
                          app->mode == GUI_APP_CODE ? rgb(102, 214, 255) :
                          app->mode == GUI_APP_DIAG ? rgb(240, 168, 90) :
                                                        rgb(78, 192, 236);
        if ((int)i == selected) {
            vgradient(x, y, card_w, card_h, rgb_lift(accent, 8), rgb_lift(accent, -28));
            border(x, y, card_w, card_h, rgb_lift(accent, 50));
        } else {
            vgradient(x, y, card_w, card_h, rgb(32, 44, 56), rgb(20, 30, 40));
            border(x, y, card_w, card_h, rgb(46, 66, 84));
        }
        rect(x, y, card_w, 1, (int)i == selected ? rgb_lift(accent, 80) : rgb(56, 78, 98));
        rect(x, y, 6, card_h, accent);
        rect(x, y + card_h - 1, card_w, 1, rgb(6, 10, 16));
        vgradient(x + 20, y + 14, 30, 30, rgb_lift(accent, 24), rgb_lift(accent, -16));
        border(x + 20, y + 14, 30, 30, rgb_lift(accent, -30));
        rect(x + 27, y + 21, 16, 16, rgb(14, 22, 32));
        if ((int)i == selected) {
            rect(x + card_w - 30, y + card_h - 22, 14, 6, accent);
        }
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, app->name);
        text_clipped(x + 66, y + 14, x + card_w - 12, line, rgb(238, 246, 252), 1);
        text_clipped(x + 66, y + 36, x + card_w - 12, app->description, rgb(168, 188, 202), 1);
    }
    text(tx, ty + 374, "方向键选择  Enter 打开  鼠标点击卡片选择", rgb(132, 150, 166), 1);
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

static void calc_clear(gui_state_t *st) {
    st->calc_value = 0;
    st->calc_acc = 0;
    st->calc_input = 0;
    st->calc_last_lhs = 0;
    st->calc_last_rhs = 0;
    st->calc_op = 0;
    st->calc_last_op = 0;
    st->calc_has_input = 0;
    st->calc_error = 0;
    st->calc_just_evaluated = 0;
    st->status = "计算器已清空";
}

static int calc_apply(gui_state_t *st, int rhs) {
    if (!st->calc_op) return rhs;
    if (st->calc_op == '+') return st->calc_acc + rhs;
    if (st->calc_op == '-') return st->calc_acc - rhs;
    if (st->calc_op == '*') return st->calc_acc * rhs;
    if (st->calc_op == '/') {
        if (rhs == 0) {
            st->calc_error = 1;
            return st->calc_acc;
        }
        return st->calc_acc / rhs;
    }
    return rhs;
}

static void calc_digit(gui_state_t *st, int digit) {
    if (st->calc_error) calc_clear(st);
    if (st->calc_just_evaluated && !st->calc_op) {
        st->calc_acc = 0;
        st->calc_input = 0;
        st->calc_value = 0;
        st->calc_just_evaluated = 0;
    }
    if (!st->calc_has_input) {
        st->calc_input = 0;
        st->calc_has_input = 1;
    }
    if (st->calc_input < 10000000)
        st->calc_input = st->calc_input * 10 + digit;
    st->calc_value = st->calc_input;
    st->status = "输入数字";
}

static void calc_operator(gui_state_t *st, char op) {
    if (st->calc_error) calc_clear(st);
    st->calc_just_evaluated = 0;
    if (st->calc_has_input) {
        if (st->calc_op) st->calc_acc = calc_apply(st, st->calc_input);
        else st->calc_acc = st->calc_input;
    }
    st->calc_op = op;
    st->calc_has_input = 0;
    st->calc_input = 0;
    st->calc_value = st->calc_acc;
    st->status = "已选择运算符";
}

static void calc_equal(gui_state_t *st) {
    if (st->calc_error) return;
    int rhs = st->calc_has_input ? st->calc_input : st->calc_acc;
    int lhs = st->calc_acc;
    char op = st->calc_op;
    st->calc_value = calc_apply(st, rhs);
    if (st->calc_error) {
        st->status = "不能除以 0";
        return;
    }
    st->calc_last_lhs = lhs;
    st->calc_last_rhs = rhs;
    st->calc_last_op = op;
    st->calc_acc = st->calc_value;
    st->calc_input = st->calc_value;
    st->calc_has_input = 1;
    st->calc_op = 0;
    st->calc_just_evaluated = 1;
    st->status = "计算完成";
}

static void calc_backspace(gui_state_t *st) {
    if (!st->calc_has_input) return;
    st->calc_just_evaluated = 0;
    st->calc_input /= 10;
    st->calc_value = st->calc_input;
    st->status = "已删除一位";
}

static void draw_calc_app(int tx, int ty, gui_state_t *st) {
    char line[96];
    text(tx, ty, "计算器", rgb(102, 214, 255), 1);
    text(tx, ty + 42, "数字输入  + - * / 运算  Enter 求值  C 清空", rgb(210, 221, 230), 1);
    uint32_t pos = 0;
    line[0] = 0;
    if (st->calc_error) append_str(line, sizeof(line), &pos, "ERROR");
    else append_int(line, sizeof(line), &pos, st->calc_value);
    vgradient(tx, ty + 88, 300, 74, rgb(8, 14, 22), rgb(2, 6, 12));
    rect(tx, ty + 88, 300, 1, rgb(38, 90, 130));
    rect(tx, ty + 161, 300, 1, rgb(8, 14, 22));
    border(tx, ty + 88, 300, 74, rgb(48, 132, 196));
    text(tx + 18, ty + 98, st->calc_just_evaluated ? "结果" : "当前", rgb(132, 196, 232), 1);
    text_clipped(tx + 22, ty + 126, tx + 290, line,
                 st->calc_error ? rgb(232, 88, 96) : rgb(235, 242, 250), 2);

    pos = 0;
    line[0] = 0;
    if (st->calc_op) {
        append_str(line, sizeof(line), &pos, "算式: ");
        append_int(line, sizeof(line), &pos, st->calc_acc);
        append_char(line, sizeof(line), &pos, ' ');
        append_char(line, sizeof(line), &pos, st->calc_op);
        append_char(line, sizeof(line), &pos, ' ');
        if (st->calc_has_input) append_int(line, sizeof(line), &pos, st->calc_input);
        else append_char(line, sizeof(line), &pos, '_');
    } else if (st->calc_just_evaluated && st->calc_last_op) {
        append_str(line, sizeof(line), &pos, "上次: ");
        append_int(line, sizeof(line), &pos, st->calc_last_lhs);
        append_char(line, sizeof(line), &pos, ' ');
        append_char(line, sizeof(line), &pos, st->calc_last_op);
        append_char(line, sizeof(line), &pos, ' ');
        append_int(line, sizeof(line), &pos, st->calc_last_rhs);
        append_str(line, sizeof(line), &pos, " = ");
        append_int(line, sizeof(line), &pos, st->calc_value);
    } else {
        append_str(line, sizeof(line), &pos, "输入数字后选择运算符");
    }
    text(tx, ty + 184, line, rgb(210, 221, 230), 1);

    static const char *ops = "+-*/";
    for (int i = 0; i < 4; i++) {
        int ox = tx + i * 42;
        vgradient(ox, ty + 216, 36, 24, rgb(48, 68, 86), rgb(24, 38, 52));
        rect(ox, ty + 216, 36, 1, rgb(70, 100, 120));
        rect(ox, ty + 239, 36, 1, rgb(8, 14, 22));
        border(ox, ty + 216, 36, 24, rgb(28, 50, 68));
        char s[2] = {ops[i], 0};
        text(ox + 14, ty + 224, s, rgb(238, 244, 250), 1);
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

// 读取 CMOS 日期，格式化为 YYYY-MM-DD
static void date_line(char *buf, uint32_t cap) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t cent = cmos_read(0x32);
    if ((status_b & 0x04) == 0) {
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        year = bcd_to_bin(year);
        cent = bcd_to_bin(cent);
    }
    uint32_t full_year = (cent ? (uint32_t)cent * 100 : 2000) + year;
    uint32_t pos = 0;
    buf[0] = 0;
    append_uint(buf, cap, &pos, full_year);
    append_char(buf, cap, &pos, '-');
    if (mon < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, mon);
    append_char(buf, cap, &pos, '-');
    if (day < 10) append_char(buf, cap, &pos, '0');
    append_uint(buf, cap, &pos, day);
}

static const char *weekday_name(void) {
    uint8_t status_b = cmos_read(0x0b);
    uint8_t wd = cmos_read(0x06);
    if ((status_b & 0x04) == 0) wd = bcd_to_bin(wd);
    static const char *names[] = {
        "", "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    if (wd >= 1 && wd <= 7) return names[wd];
    return "";
}

static void draw_clock_app(int tx, int ty, int win_w, gui_state_t *st) {
    (void)st;
    char line[48];
    text(tx, ty, "时钟", rgb(102, 214, 255), 1);

    int box_w = win_w - 76;
    if (box_w < 280) box_w = 280;
    int box_h = 150;
    int by = ty + 48;
    soft_shadow(tx, by, box_w, box_h);
    draw_panel_shell(tx, by, box_w, box_h, rgb(18, 30, 44), rgb(6, 12, 20),
                     rgb(40, 76, 104), rgb(102, 214, 255));

    // 大号时间显示（scale 4）
    time_line(line, sizeof(line));
    int digit_w = 6 * 4;       // 字形宽 6px * scale
    int tw = (int)strlen(line) * (digit_w + 4);
    int cx = tx + (box_w - tw) / 2;
    if (cx < tx + 12) cx = tx + 12;
    text(cx, by + 30, line, rgb(120, 224, 255), 4);

    // 日期 + 星期
    date_line(line, sizeof(line));
    text(tx + 24, by + box_h - 36, line, rgb(200, 224, 240), 2);
    text(tx + box_w - 120, by + box_h - 32, weekday_name(), rgb(160, 200, 224), 1);

    text(tx, by + box_h + 24, "时间来自 CMOS 实时时钟，每秒自动刷新", rgb(120, 150, 168), 1);
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
    if (idx < 0 || idx > 2 || content_w <= 0) return 0;
    int gap = 8;
    int width = 68;
    int total = width * 3 + gap * 2;
    int left = content_w - total;
    if (left < 124 && content_w >= 124 + total) left = 124;
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

static void handle_code_command(gui_state_t *st, int cmd) {
    if (cmd == CODE_CMD_SAVE) {
        code_load(st);
        (void)code_save(st);
    } else if (cmd == CODE_CMD_RUN) {
        code_run_current(st);
    } else if (cmd == CODE_CMD_OPEN) {
        code_open_selected(st);
    }
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

static int code_draw_span(int x, int y, int max_x, const char *s,
                          uint32_t start, uint32_t len, uint32_t color) {
    char tmp[48];
    while (len > 0 && x < max_x) {
        uint32_t n = len;
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, s + start, n);
        tmp[n] = 0;
        text_clipped(x, y, max_x, tmp, color, 1);
        x += (int)n * 6;
        start += n;
        len -= n;
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
        int cx = l.editor_x + l.line_no_w + 10 + (int)cursor_col * 6;
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



static void console_append_history(gui_state_t *st, const char *line) {
    if (st->console_line_count < 16) {
        strncpy(st->console_history[st->console_line_count], line, 79);
        st->console_history[st->console_line_count][79] = 0;
        st->console_line_count++;
    } else {
        for (int i = 0; i < 15; i++) {
            strcpy(st->console_history[i], st->console_history[i + 1]);
        }
        strncpy(st->console_history[15], line, 79);
        st->console_history[15][79] = 0;
    }
}

static void console_exec_cmd(gui_state_t *st) {
    if (st->console_input_len == 0) {
        console_append_history(st, "hbos_gui_shell:/#");
        return;
    }

    char cmd_line[120];
    uint32_t cpos = 0;
    cmd_line[0] = 0;
    append_str(cmd_line, sizeof(cmd_line), &cpos, "hbos_gui_shell:/# ");
    append_str(cmd_line, sizeof(cmd_line), &cpos, st->console_input);
    console_append_history(st, cmd_line);

    char *cmd = st->console_input;
    while (*cmd == ' ') cmd++;
    
    if (strcmp(cmd, "help") == 0) {
        console_append_history(st, "Available commands:");
        console_append_history(st, "  help      Show this help message");
        console_append_history(st, "  ls        List files in root directory");
        console_append_history(st, "  cat <f>   Display contents of file <f>");
        console_append_history(st, "  mem       Show memory usage info");
        console_append_history(st, "  tasks     List running processes");
        console_append_history(st, "  clear     Clear screen history");
        console_append_history(st, "  neofetch  Show system neofetch/logo");
    } else if (strcmp(cmd, "clear") == 0) {
        st->console_line_count = 0;
    } else if (strcmp(cmd, "ls") == 0) {
        char entry[VFS_MAX_NAME];
        uint32_t entry_type;
        uint32_t idx = 0;
        while (vfs_readdir_at("/", idx, entry, &entry_type) == 0) {
            char line[80];
            uint32_t pos = 0;
            append_str(line, sizeof(line), &pos, entry_type == VFS_NODE_DIR ? "[DIR]  " : "[FILE] ");
            append_str(line, sizeof(line), &pos, entry);
            console_append_history(st, line);
            idx++;
        }
        if (idx == 0) {
            console_append_history(st, "Directory is empty.");
        }
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        if (*arg == 0) {
            console_append_history(st, "Usage: cat <filename>");
        } else {
            char full_path[128];
            full_path[0] = 0;
            uint32_t pos = 0;
            if (arg[0] != '/') {
                append_str(full_path, sizeof(full_path), &pos, "/");
            }
            append_str(full_path, sizeof(full_path), &pos, arg);

            vfs_node_t *node = vfs_lookup(full_path);
            if (!node) {
                console_append_history(st, "cat: File not found.");
            } else {
                char buf[512];
                int got = vfs_read(node, 0, buf, sizeof(buf) - 1);
                if (got < 0) {
                    console_append_history(st, "cat: Read error.");
                } else {
                    buf[got] = 0;
                    char *line_start = buf;
                    for (int i = 0; i <= got; i++) {
                        if (buf[i] == '\n' || buf[i] == '\r' || i == got) {
                            char old_c = buf[i];
                            buf[i] = 0;
                            if (strlen(line_start) > 0) {
                                console_append_history(st, line_start);
                            }
                            buf[i] = old_c;
                            line_start = buf + i + 1;
                        }
                    }
                }
            }
        }
    } else if (strcmp(cmd, "mem") == 0) {
        uint64_t total = pmm_get_total_mem();
        uint64_t free = pmm_get_free_mem();
        uint64_t used = total > free ? total - free : 0;
        char line[80];
        uint32_t pos = 0;
        
        pos = 0; line[0] = 0;
        append_str(line, sizeof(line), &pos, "Total Memory: ");
        append_uint(line, sizeof(line), &pos, (uint32_t)(total / 1024));
        append_str(line, sizeof(line), &pos, " KB");
        console_append_history(st, line);

        pos = 0; line[0] = 0;
        append_str(line, sizeof(line), &pos, "Used Memory:  ");
        append_uint(line, sizeof(line), &pos, (uint32_t)(used / 1024));
        append_str(line, sizeof(line), &pos, " KB");
        console_append_history(st, line);

        pos = 0; line[0] = 0;
        append_str(line, sizeof(line), &pos, "Free Memory:  ");
        append_uint(line, sizeof(line), &pos, (uint32_t)(free / 1024));
        append_str(line, sizeof(line), &pos, " KB");
        console_append_history(st, line);
    } else if (strcmp(cmd, "tasks") == 0) {
        uint32_t count = (uint32_t)task_get_count();
        char line[80];
        uint32_t pos = 0;
        append_str(line, sizeof(line), &pos, "Active Tasks: ");
        append_uint(line, sizeof(line), &pos, count);
        console_append_history(st, line);
        for (uint32_t i = 0; i < count && i < 10; i++) {
            const task_t *task = task_get_active(i);
            if (task) {
                pos = 0;
                line[0] = 0;
                append_str(line, sizeof(line), &pos, " PID: ");
                append_uint(line, sizeof(line), &pos, task->id);
                append_str(line, sizeof(line), &pos, " | ");
                append_str(line, sizeof(line), &pos, task->name);
                append_str(line, sizeof(line), &pos, " (");
                append_str(line, sizeof(line), &pos, task_state_name(task->state));
                append_str(line, sizeof(line), &pos, ")");
                console_append_history(st, line);
            }
        }
    } else if (strcmp(cmd, "neofetch") == 0) {
        console_append_history(st, "   /\\_/\\      HBOS (HeBitOS) v0.1");
        console_append_history(st, "  ( o.o )     Kernel: Coop-Multitasking");
        console_append_history(st, "   > ^ <      Arch: x86_64");
        console_append_history(st, "  /     \\     UI: Cyberpunk Neon Terminal");
        console_append_history(st, " |       |    Status: Online & Ready");
    } else {
        char err[120];
        uint32_t pos = 0;
        err[0] = 0;
        append_str(err, sizeof(err), &pos, "hbos_shell: command not found: ");
        append_str(err, sizeof(err), &pos, cmd);
        console_append_history(st, err);
    }

    st->console_input_len = 0;
    st->console_input[0] = 0;
}

static void draw_diag_app(int tx, int ty, int win_w, int win_h, gui_state_t *st) {
    int box_x = tx - 20;
    int box_y = ty - 4;
    int box_w = win_w - 20;
    int box_h = win_h - 74;

    // 半透明背景填充 (Alpha 0xD0, Obsidian Blue/Black)
    rect_alpha(box_x, box_y, box_w, box_h, 0xD002050E);
    // 霓虹青边框
    border(box_x, box_y, box_w, box_h, cyber_neon_cyan(0));

    // 绘制命令历史
    int start_y = box_y + 12;
    for (uint32_t i = 0; i < st->console_line_count; i++) {
        const char *line = st->console_history[i];
        uint32_t color = cyber_text(0);
        if (strncmp(line, "hpos_gui_shell:", 15) == 0 || strncmp(line, "hbos_gui_shell:", 15) == 0) {
            color = rgb(0, 240, 255); // 霓虹蓝 prompt
        } else if (strncmp(line, "hbos_shell:", 11) == 0) {
            color = cyber_neon_pink(0); // 霓虹粉 error
        } else if (strncmp(line, "  ", 2) == 0) {
            color = cyber_neon_yellow(0); // 琥珀黄细节
        }
        text(box_x + 12, start_y, line, color, 1);
        start_y += 16;
    }

    // 绘制当前输入行
    int input_y = box_y + box_h - 24;
    text(box_x + 12, input_y, "hbos_gui_shell:/# ", rgb(0, 255, 128), 1); // 霓虹绿 prompt
    int prompt_w = 18 * 6;
    text(box_x + 12 + prompt_w, input_y, st->console_input, cyber_text(0), 1);

    // 闪烁光标
    static uint32_t cursor_ticks = 0;
    cursor_ticks++;
    int cursor_visible = (cursor_ticks / 15) % 2;
    if (cursor_visible) {
        int input_len = (int)strlen(st->console_input);
        int cursor_x = box_x + 12 + prompt_w + input_len * 6;
        rect(cursor_x, input_y, 6, 8, rgb(0, 255, 128));
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
    text(tx, ty + 30, "Enter加载  S保存  方向键滚动", rgb(148, 162, 174), 1);
    rect(tx, ty + 58, view_w, 32, rgb(6, 14, 22));
    border(tx, ty + 58, view_w, 32, rgb(78, 192, 236));
    text(tx + 10, ty + 68, st->browser_url, rgb(232, 242, 248), 1);
    rect(tx + view_w - 12, ty + 68, 6, 14, rgb(78, 192, 236));
    draw_small_button(tx, ty + 104, 96, "Enter 加载", rgb(78, 192, 236));
    draw_small_button(tx + 108, ty + 104, 68, "S 保存", rgb(85, 180, 120));
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
    if (state != WM_STATE_MAXIMIZED) soft_shadow(x, y, win_w, win_h);

    uint32_t body_top = active ? cyber_bg_top(light) : (light ? rgb(240, 244, 248) : rgb(23, 28, 32));
    uint32_t body_bot = active ? cyber_bg_bot(light) : (light ? rgb(225, 230, 236) : rgb(15, 18, 22));
    uint32_t title_top = active ? cyber_neon_pink(light) : (light ? rgb(210, 216, 222) : rgb(72, 86, 92));
    uint32_t title_bot = active ? (light ? rgb(160, 0, 70) : rgb(120, 0, 60)) : (light ? rgb(190, 195, 202) : rgb(48, 58, 64));
    uint32_t border_c = active ? cyber_neon_cyan(light) : (light ? rgb(180, 186, 192) : rgb(60, 72, 78));
    uint32_t hl = active ? cyber_neon_yellow(light) : (light ? rgb(200, 205, 210) : rgb(106, 120, 124));

    vgradient(x + 1, y + 1, win_w - 2, WM_TITLE_H, title_top, title_bot);
    rect(x, y, win_w, 1, hl);
    rect(x + 1, y + 2, win_w - 2, 1, rgb_lift(title_top, 28));
    rect(x + 1, y + WM_TITLE_H, win_w - 2, 1, light ? rgb(200, 205, 210) : rgb(8, 12, 16));
    rect(x, y + WM_TITLE_H, win_w, 1, active ? cyber_neon_cyan(light) : (light ? rgb(190, 195, 200) : rgb(44, 54, 58)));
    rect(x + 1, y + WM_TITLE_H + 1, win_w - 2, 1, light ? rgb(220, 225, 230) : rgb(42, 52, 56));

    vgradient_alpha(x + 1, y + WM_TITLE_H + 2, win_w - 2, win_h - WM_TITLE_H - 4,
                    0xD0000000 | (body_top & 0x00FFFFFF),
                    0xD0000000 | (body_bot & 0x00FFFFFF));
    rect(x, y + win_h - 1, win_w, 1, light ? rgb(180, 185, 190) : rgb(5, 8, 10));
    rect(x, y, 1, win_h, rgb_lift(body_top, 18));
    rect(x + win_w - 1, y, 1, win_h, rgb_lift(body_bot, -10));
    border(x, y, win_w, win_h, border_c);

    int btn_x = x + win_w - WM_BTN_W - 8;
    int btn_y = y + 7;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20,
                     rgb(226, 86, 84), rgb(150, 42, 46),
                     rgb(86, 26, 30), rgb(226, 86, 84));
    draw_window_control_icon(btn_x + 7, btn_y + 6, GUI_CTRL_CLOSE, 0,
                             rgb(252, 238, 236));

    btn_x -= WM_BTN_W + WM_BTN_GAP;
    uint32_t nb_top = active ? (light ? rgb(180, 210, 220) : rgb(72, 91, 96)) : (light ? rgb(220, 225, 230) : rgb(54, 64, 70));
    uint32_t nb_bot = active ? (light ? rgb(140, 170, 180) : rgb(42, 53, 58)) : (light ? rgb(190, 195, 200) : rgb(32, 38, 44));
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20, nb_top, nb_bot,
                     light ? rgb(160, 165, 170) : rgb(22, 30, 34), cyber_neon_cyan(light));
    draw_window_control_icon(btn_x + 6, btn_y + 4, GUI_CTRL_MAX,
                             state == WM_STATE_MAXIMIZED, rgb(236, 242, 240));

    btn_x -= WM_BTN_W + WM_BTN_GAP;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20, nb_top, nb_bot,
                     light ? rgb(160, 165, 170) : rgb(22, 30, 34), cyber_neon_yellow(light));
    draw_window_control_icon(btn_x + 6, btn_y + 4, GUI_CTRL_MIN, 0,
                             rgb(236, 242, 240));

    rect(x + 13, y + 12, 10, 10, active ? cyber_neon_yellow(light) : (light ? rgb(160, 165, 170) : rgb(162, 172, 172)));
    rect(x + 16, y + 15, 4, 4, light ? rgb(240, 244, 248) : rgb(18, 26, 28));
    text_clipped(x + 31, y + 11, x + win_w - WM_BTN_W * 3 - WM_BTN_GAP * 2 - 16,
                 title, light ? rgb(255, 255, 255) : rgb(252, 254, 255), 1);
}

static void draw_panel_window(int tx, int ty, int win_w, int w, int h, gui_state_t *st, int panel) {
    if (panel == PANEL_FILES) draw_files_panel(tx, ty, win_w, st);
    else if (panel == PANEL_DISK) draw_disk_panel(tx, ty, win_w);
    else if (panel == PANEL_SYS) draw_resource_panel(tx, ty, win_w, w, h);
    else draw_apps_panel(tx, ty, win_w, st);
}

static void draw_app_window_body(int tx, int ty, int win_w, int win_h, gui_state_t *st, int mode) {
    if (mode == GUI_APP_NOTES) draw_notes_app(tx, ty, win_w, st);
    else if (mode == GUI_APP_CALC) draw_calc_app(tx, ty, st);
    else if (mode == GUI_APP_UWC) draw_uwc_app(tx, ty, st);
    else if (mode == GUI_APP_SNAKE) draw_snake_app(tx, ty, st);
    else if (mode == GUI_APP_BROWSER) draw_browser_app(tx, ty, win_w, st);
    else if (mode == GUI_APP_CODE) draw_code_app(tx, ty, win_w, win_h, st);
    else if (mode == GUI_APP_DIAG) draw_diag_app(tx, ty, win_w, win_h, st);
    else if (mode == GUI_APP_CLOCK) draw_clock_app(tx, ty, win_w, st);
}

static void draw_one_window(int w, int h, gui_state_t *st, int idx) {
    wm_window_t *win = wm_get_window(&st->wm, idx);
    if (!win || win->state == WM_STATE_MINIMIZED) return;
    int old_active = st->active;
    int old_app = st->app_mode;
    int old_x = st->win_x;
    int old_y = st->win_y;
    int win_x, win_y, win_w, win_h;
    gui_window_metrics(st, w, h, win, idx, &win_x, &win_y, &win_w, &win_h);
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
}

static void draw_taskbar_windows(int h, const gui_state_t *st) {
    int x = 118;
    int task_y = h - 36;
    for (int i = 0; i < st->wm.window_count && x < 610; i++) {
        wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, i);
        if (!win) continue;
        uint32_t color = win->kind == WM_WIN_PANEL ? rgb(85, 180, 120) : cyber_neon_cyan(st->theme_light);
        int width = 104;
        draw_task_button(x, task_y, width, gui_window_title(win),
                         i == st->wm.active_window, color, st->theme_light);
        x += width + 8;
    }
}

static void draw_gui_screen(int w, int h, gui_state_t *st) {
    gui_sync_focus(st);
    draw_desktop(w, h, st);
    draw_start_menu(st);
    draw_window_switcher(w, h, st);
    if (st->splash_ticks > 0)
        draw_splash_window(w, h, st->splash_ticks, st->theme_light);
}

static void draw_gui_frame(const fb_info_t *fb, int w, int h, gui_state_t *st, int mx, int my, int edge) {
    draw_gui_screen(w, h, st);
    draw_cursor(mx, my, edge);
    gui_present_surface(fb);
}

static void draw_desktop(int w, int h, gui_state_t *st) {
    draw_wallpaper(w, h, st->theme_light);
    char line[96];

    soft_shadow(126, 12, w - 138, 60);
    draw_panel_shell(126, 12, w - 138, 60,
                     0xD0000000 | (cyber_card_bg_top(st->theme_light) & 0x00FFFFFF),
                     0xD0000000 | (cyber_card_bg_bot(st->theme_light) & 0x00FFFFFF),
                     cyber_border(st->theme_light), cyber_neon_cyan(st->theme_light));
    rect(146, 62, 118, 2, cyber_neon_pink(st->theme_light));
    text(146, 18, "系统概览", cyber_text(st->theme_light), 2);
    text(148, 56, "左侧启动栏打开工具，桌面直接显示当前状态", cyber_text_muted(st->theme_light), 1);

    draw_icon(15, 92, st->active == PANEL_FILES, "文件", rgb(85, 180, 120), st->theme_light);
    draw_icon(15, 162, st->active == PANEL_DISK, "磁盘", rgb(230, 184, 74), st->theme_light);
    draw_icon(15, 232, st->active == PANEL_SYS, "资源", rgb(196, 116, 230), st->theme_light);
    draw_icon(15, 302, st->active == PANEL_APPS, "应用", rgb(23, 147, 209), st->theme_light);

    uint64_t total = pmm_get_total_mem();
    uint64_t free = pmm_get_free_mem();
    uint64_t used = total > free ? total - free : 0;
    line_u32(line, sizeof(line), "", (uint32_t)(used / 1024), "K used");
    draw_desktop_tile(142, 88, 150, 74, "内存", line, cyber_neon_purple(st->theme_light), st->theme_light);

    line_u32(line, sizeof(line), "", fs_get_count(), " files");
    draw_desktop_tile(310, 88, 150, 74, "文件", line, rgb(85, 180, 120), st->theme_light);

    line2(line, sizeof(line), "FS: ", fs_backend_name());
    draw_desktop_tile(478, 88, 190, 74, "文件系统", line, cyber_neon_cyan(st->theme_light), st->theme_light);

    line2(line, sizeof(line), "Block: ", block_backend_name());
    draw_desktop_tile(142, 182, 246, 74, "磁盘", line, rgb(230, 184, 74), st->theme_light);

    line_u32(line, sizeof(line), "", (uint32_t)task_get_count(), " tasks");
    draw_desktop_tile(406, 182, 126, 74, "任务", line, rgb(124, 220, 154), st->theme_light);

    line_u32(line, sizeof(line), "", gui_app_count(), " apps");
    draw_desktop_tile(550, 182, 118, 74, "应用", line, cyber_neon_cyan(st->theme_light), st->theme_light);

    soft_shadow(142, 282, 250, 126);
    draw_panel_shell(142, 282, 250, 126,
                     0xD0000000 | (cyber_card_bg_top(st->theme_light) & 0x00FFFFFF),
                     0xD0000000 | (cyber_card_bg_bot(st->theme_light) & 0x00FFFFFF),
                     cyber_border(st->theme_light), rgb(85, 180, 120));
    rect(160, 316, 48, 2, rgb(85, 180, 120));
    text(160, 298, "最近文件", cyber_text(st->theme_light), 1);
    char entry[VFS_MAX_NAME];
    uint32_t entry_type;
    uint32_t count = 0;
    while (vfs_readdir_at("/", count, entry, &entry_type) == 0) count++;
    if (count == 0) {
        text(160, 330, "根目录为空", cyber_text_muted(st->theme_light), 1);
        text(160, 354, "按 N 或点文件中新建", cyber_text_muted(st->theme_light), 1);
    } else {
        uint32_t shown = count > 4 ? 4 : count;
        for (uint32_t i = 0; i < shown; i++) {
            if (vfs_readdir_at("/", i, entry, &entry_type) < 0) continue;
            line[0] = 0;
            uint32_t pos = 0;
            append_str(line, sizeof(line), &pos, entry);
            append_str(line, sizeof(line), &pos, "  ");
            append_str(line, sizeof(line), &pos, gui_node_type_label(entry_type));
            text_clipped(160, 328 + (int)i * 20, 380, line, cyber_text(st->theme_light), 1);
        }
    }

    soft_shadow(418, 282, 250, 126);
    draw_panel_shell(418, 282, 250, 126,
                     0xD0000000 | (cyber_card_bg_top(st->theme_light) & 0x00FFFFFF),
                     0xD0000000 | (cyber_card_bg_bot(st->theme_light) & 0x00FFFFFF),
                     cyber_border(st->theme_light), cyber_neon_cyan(st->theme_light));
    rect(436, 316, 48, 2, cyber_neon_cyan(st->theme_light));
    text(436, 298, "快捷操作", cyber_text(st->theme_light), 1);
    text(436, 328, "N 新建文件", cyber_text_muted(st->theme_light), 1);
    text(436, 350, "Enter 打开当前项", cyber_text_muted(st->theme_light), 1);
    text(436, 372, "Tab/Space 切换窗口", cyber_text_muted(st->theme_light), 1);

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

    draw_taskbar_windows(h, st);
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

static const gui_app_t *selected_gui_app(gui_state_t *st) {
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
    const gui_app_t *app = selected_gui_app(st);
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
        int y = ty + 42;
        if (mx >= tx && mx < tx + ACTION_W && my >= y && my < y + ACTION_H) return 6;
        int card_w = 250;
        int card_h = 62;
        for (uint32_t i = 0; i < gui_app_count(); i++) {
            int col = (int)(i & 1);
            int row = (int)(i >> 1);
            int x = tx + col * (card_w + 18);
            int cy = ty + 82 + row * (card_h + 10);
            if (mx >= x && mx < x + card_w && my >= cy && my < cy + card_h)
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
    for (int cmd = CODE_CMD_SAVE; cmd <= CODE_CMD_OPEN; cmd++) {
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
    int col = (mx - (l.editor_x + l.line_no_w + 10)) / 6;
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

static int hit_panel_shortcut(int w, int h, int mx, int my) {
    (void)w;
    (void)h;
    if (mx >= 15 && mx < 97) {
        if (my >= 92 && my < 150) return PANEL_FILES;
        if (my >= 162 && my < 220) return PANEL_DISK;
        if (my >= 232 && my < 290) return PANEL_SYS;
        if (my >= 302 && my < 360) return PANEL_APPS;
    }
    return -1;
}

static int hit_task_window(int h, const gui_state_t *st, int mx, int my) {
    (void)h;
    return wm_hit_taskbar((wm_state_t *)&st->wm, mx, my);
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

// 是否有未最小化的时钟窗口可见（需每秒刷新）
static int gui_clock_visible(gui_state_t *st) {
    for (int i = 0; i < st->wm.window_count; i++) {
        wm_window_t *win = wm_get_window(&st->wm, i);
        if (win && win->kind == WM_WIN_APP && win->mode == GUI_APP_CLOCK &&
            win->state != WM_STATE_MINIMIZED)
            return 1;
    }
    return 0;
}

// 时钟/任务栏时间每秒刷新一次
static int clock_auto_tick(gui_state_t *st) {
    uint8_t sec = cmos_second();
    if (sec == st->clock_last_sec) return 0;
    st->clock_last_sec = sec;
    return gui_clock_visible(st);
}

static void handle_app_key(gui_state_t *st, int key) {
    if (key == '\t' && st->app_mode != GUI_APP_CODE) {
        gui_focus_next_window(st, 1);
        if (st->wm.window_count > 1) st->switcher_ticks = 40;
        return;
    }
    gui_sync_focus(st);
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
    } else if (st->app_mode == GUI_APP_CALC) {
        if (key >= '0' && key <= '9') calc_digit(st, key - '0');
        else if (key == '+' || key == '-' || key == '*' || key == '/') calc_operator(st, (char)key);
        else if (key == '\n' || key == '=') calc_equal(st);
        else if (key == GUI_KEY_BACKSPACE) calc_backspace(st);
        else if (key == 'c') calc_clear(st);
        else if (key == GUI_KEY_LEFT) {
            st->calc_value--;
            st->calc_input = st->calc_value;
            st->calc_has_input = 1;
        } else if (key == GUI_KEY_RIGHT) {
            st->calc_value++;
            st->calc_input = st->calc_value;
            st->calc_has_input = 1;
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
        else if (key == 's' || key == 'S') browser_save_page(st);
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
            if (st->console_input_len > 0) {
                st->console_input_len--;
                st->console_input[st->console_input_len] = 0;
            }
        } else if (key == '\n' || key == '\r') {
            console_exec_cmd(st);
        } else if (key >= 32 && key <= 126) {
            if (st->console_input_len + 1 < 80) {
                st->console_input[st->console_input_len++] = (char)key;
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

    soft_shadow(mx, my, mw, mh);
    draw_panel_shell(mx, my, mw, mh,
                     0xD0000000 | (cyber_card_bg_top(st->theme_light) & 0x00FFFFFF),
                     0xD0000000 | (cyber_card_bg_bot(st->theme_light) & 0x00FFFFFF),
                     cyber_border(st->theme_light), cyber_neon_cyan(st->theme_light));
    vgradient(mx + 1, my + 2, mw - 2, 30, cyber_neon_pink(st->theme_light), st->theme_light ? rgb(160, 0, 70) : rgb(180, 0, 90));
    rect(mx + 1, my + 2, mw - 2, 1, st->theme_light ? rgb(255, 120, 200) : rgb(255, 100, 180));
    rect(mx + 1, my + 32, mw - 2, 1, st->theme_light ? rgb(200, 205, 210) : rgb(9, 20, 24));
    rect(mx + 15, my + 25, 78, 2, cyber_neon_yellow(st->theme_light));
    text(mx + 16, my + 11, "HBOS 工作站", rgb(255, 255, 255), 1);

    static const char *menu_items[] = {
        "文件管理器", "磁盘管理器", "资源管理器", "应用程序",
        "记事本", "计算器", "贪吃蛇", "浏览器", "代码工作台",
        "控制台终端", "时钟", "返回 Shell", "关机"
    };
    uint32_t menu_colors[13] = {
        rgb(85, 180, 120), rgb(230, 184, 74), rgb(196, 116, 230),
        rgb(23, 147, 209), rgb(85, 180, 120), rgb(102, 214, 255),
        rgb(124, 220, 154), rgb(78, 192, 236), rgb(102, 214, 255),
        rgb(240, 168, 90), rgb(102, 214, 255), rgb(204, 156, 74),
        rgb(226, 86, 84)
    };
    int count = sizeof(menu_items) / sizeof(menu_items[0]);
    for (int i = 0; i < count; i++) {
        int iy = my + 40 + i * 28;
        uint32_t accent = menu_colors[i];
        if (iy + 26 > my + mh) break;
        uint32_t item_top = i >= 11 ? (st->theme_light ? rgb(250, 230, 220) : rgb(40, 15, 20)) : (st->theme_light ? rgb(240, 244, 248) : rgb(20, 12, 28));
        uint32_t item_bot = i >= 11 ? (st->theme_light ? rgb(240, 215, 205) : rgb(25, 8, 12)) : (st->theme_light ? rgb(225, 230, 236) : rgb(12, 8, 18));
        draw_panel_shell(mx + 6, iy, mw - 12, 26,
                         0xD0000000 | (item_top & 0x00FFFFFF),
                         0xD0000000 | (item_bot & 0x00FFFFFF),
                         st->theme_light ? rgb(190, 195, 200) : rgb(60, 0, 90), accent);
        rect(mx + 18, iy + 8, 10, 10, accent);
        rect(mx + 21, iy + 11, 4, 4, st->theme_light ? rgb(240, 244, 248) : rgb(18, 24, 26));
        text(mx + 38, iy + 8, menu_items[i], cyber_text(st->theme_light), 1);
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

    text_clipped(sx + 14, sy + 10, sx + sw - 20, "HBOS  v0.1 beta2", rgb(255, 255, 255), 1);

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
    int cursor_edge = WM_EDGE_NONE;
    uint32_t frame_tick = 0;          // 自增帧计数，用于双击计时
    uint32_t last_title_click = 0;    // 上次点击标题栏的帧
    int last_title_idx = -1;          // 上次点击的窗口
    int snap_hint = WM_SNAP_NONE;     // 拖动中预测的吸附目标
    gui_state_t st = {
        .active = PANEL_FILES,
        .selected_file = 0,
        .selected_app = 0,
        .app_mode = GUI_APP_NONE,
        .calc_value = 0,
        .snake_x = 5,
        .snake_y = 3,
        .snake_tx = 9,
        .snake_ty = 5,
        .snake_score = 0,
        .clicks = 0,
        .buttons = 0,
        .last_clicked_file = -1,
        .delete_confirm_index = -1,
        .status = "就绪",
        .theme_light = 0,
    };
    wm_init(&st.wm, w, h);
    st.console_input[0] = 0;
    st.console_input_len = 0;
    st.console_line_count = 0;
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
    gui_open_panel_window(&st, PANEL_FILES);

    uint64_t surface_bytes = (uint64_t)w * (uint64_t)h * sizeof(uint32_t);
    size_t surface_pages = (size_t)((surface_bytes + GUI_PAGE_SIZE - 1) / GUI_PAGE_SIZE);
    uint64_t surface_phys = pmm_alloc_blocks(surface_pages);
    if (surface_phys) {
        gui_set_surface((uint32_t *)(uintptr_t)surface_phys, w, h, (uint32_t)w);
    } else {
        st.status = "图形缓冲分配失败";
    }

    st.splash_ticks = 90;
    draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
    while (1) {
        int key = key_poll();
        if (st.splash_ticks > 0 && key) {
            st.splash_ticks = 0;
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.rename_active) {
            gui_cancel_rename(&st);
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.delete_confirm_index >= 0) {
            gui_cancel_delete_confirm(&st);
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 && st.wm.window_count > 0) {
            gui_close_window(&st, st.wm.active_window);
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            continue;
        }
        if (key == 27 || (key == 'q' && st.app_mode == GUI_APP_NONE)) break;
        if (key) {
            gui_sync_focus(&st);
            if (st.app_mode == GUI_APP_NONE) handle_key(&st, key);
            else handle_app_key(&st, key);
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
                    int new_x = rw->x, new_y = rw->y;
                    int new_w = resize_orig_w, new_h = resize_orig_h;

                    if (resize_edge == WM_EDGE_E || resize_edge == WM_EDGE_NE || resize_edge == WM_EDGE_SE)
                        new_w = resize_orig_w + dx;
                    if (resize_edge == WM_EDGE_W || resize_edge == WM_EDGE_NW || resize_edge == WM_EDGE_SW) {
                        new_w = resize_orig_w - dx;
                        new_x = resize_orig_x + dx;
                    }
                    if (resize_edge == WM_EDGE_S || resize_edge == WM_EDGE_SE || resize_edge == WM_EDGE_SW)
                        new_h = resize_orig_h + dy;
                    if (resize_edge == WM_EDGE_N || resize_edge == WM_EDGE_NE || resize_edge == WM_EDGE_NW) {
                        new_h = resize_orig_h - dy;
                        new_y = resize_orig_y + dy;
                    }
                    if (new_w < 200) new_w = 200;
                    if (new_h < 120) new_h = 120;
                    rw->w = new_w;
                    rw->h = new_h;
                    rw->x = new_x;
                    rw->y = new_y;
                    redraw = 1;
                    st.status = "调整窗口大小";
                }
            } else if (!left_down) {
                resizing_window = -1;
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
                            }
                        }
                    } else {
                        int task_idx = hit_task_window(h, &st, mx, my);
                        if (task_idx >= 0) {
                            wm_window_t *tw = wm_get_window(&st.wm, task_idx);
                            if (tw && tw->state == WM_STATE_MINIMIZED) {
                                wm_restore_window(&st.wm, task_idx);
                            }
                            gui_focus_window(&st, task_idx);
                            st.status = "已切换窗口";
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
                                if (code_cmd) {
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
                                int panel = hit_panel_shortcut(w, h, mx, my);
                                if (panel >= 0) {
                                    gui_open_panel_window(&st, panel);
                                } else if (mx >= 10 && mx < 106 && my >= h - 36 && my <= h) {
                                    wm_toggle_start_menu(&st.wm);
                                    st.status = "开始菜单";
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
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            } else if (mx != old_mx || my != old_my) {
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            }
        }
        if (snake_auto_tick(&st)) {
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (clock_auto_tick(&st)) {
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.app_mode == GUI_APP_DIAG && (frame_tick % 20 == 0)) {
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.switcher_ticks > 0) {
            st.switcher_ticks--;
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        if (st.splash_ticks > 0) {
            st.splash_ticks--;
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
