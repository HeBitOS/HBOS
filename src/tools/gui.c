#include <stdbool.h>
#include <stdint.h>

#include "../block.h"
#include "../core/pmm.h"
#include "../core/task.h"
#include "../fs.h"
#include "../graphics/font_cjk.h"
#include "../graphics/graphics.h"
#include "../input/mouse.h"
#include "../net.h"
#include "../string.h"
#include "../tls.h"
#include "../user/app.h"
#include "tool.h"

extern void task_yield(void);

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

#define ACTION_W 116
#define ACTION_H 28
#define GUI_MOUSE_POLL_BUDGET 16
#define TASKBAR_H 44
#define NOTE_EDIT_CAP 512
#define BROWSER_URL_CAP 160
#define BROWSER_PAGE_CAP 2048
#define SNAKE_W 16
#define SNAKE_H 10
#define SNAKE_MAX (SNAKE_W * SNAKE_H)
#define GUI_MAX_WINDOWS 8
#define GUI_PAGE_SIZE 4096ULL
#define FILE_LIST_ROWS 8
#define FILE_ROW_H 26
#define NOTE_FILE_ROWS 7

#define GUI_WIN_PANEL 0
#define GUI_WIN_APP   1

typedef struct {
    int used;
    int kind;
    int mode;
    int x;
    int y;
} gui_window_t;

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
    gui_window_t windows[GUI_MAX_WINDOWS];
    int window_count;
    int active_window;
    int last_clicked_file;
    char note_buf[NOTE_EDIT_CAP];
    uint32_t note_len;
    int note_loaded;
    char note_name[MAX_FILENAME];
    char browser_url[BROWSER_URL_CAP];
    char browser_page[BROWSER_PAGE_CAP];
    uint32_t browser_page_len;
    int browser_loaded;
    int browser_scroll;
    const char *status;
} gui_state_t;

typedef struct {
    const char *name;
    const char *description;
    int mode;
} gui_app_t;

static const gui_app_t gui_apps[] = {
    {"记事本", "编辑笔记文件", GUI_APP_NOTES},
    {"计算器", "方向键调整数值", GUI_APP_CALC},
    {"文件统计", "统计选中文件行数和字节", GUI_APP_UWC},
    {"贪吃蛇", "方向键移动", GUI_APP_SNAKE},
    {"浏览器", "打开 HTTP/HTTPS 网页", GUI_APP_BROWSER},
};

static uint32_t gui_app_count(void) {
    return (uint32_t)(sizeof(gui_apps) / sizeof(gui_apps[0]));
}

static const char *panel_title(int panel) {
    if (panel == PANEL_FILES) return "文件管理器";
    if (panel == PANEL_DISK) return "磁盘管理器";
    if (panel == PANEL_SYS) return "资源管理器";
    return "应用程序";
}

static const char *app_title(int mode) {
    if (mode == GUI_APP_NOTES) return "记事本";
    if (mode == GUI_APP_CALC) return "计算器";
    if (mode == GUI_APP_UWC) return "文件统计";
    if (mode == GUI_APP_SNAKE) return "贪吃蛇";
    if (mode == GUI_APP_BROWSER) return "浏览器";
    return "应用窗口";
}

static file_t *selected_file(gui_state_t *st);
static void draw_desktop(int w, int h, gui_state_t *st);
static void gui_sync_focus(gui_state_t *st);

static void default_window_metrics(int w, int h, int *win_x, int *win_y, int *win_w, int *win_h) {
    *win_w = w > 920 ? 790 : w - 120;
    *win_h = h > 620 ? 430 : h - 140;
    *win_x = w > 900 ? 132 : 110;
    if (*win_x + *win_w > w - 28) *win_x = w - *win_w - 28;
    if (*win_x < 100) *win_x = 100;
    *win_y = 42;
}

static void clamp_window(gui_state_t *st, int w, int h, int win_w, int win_h) {
    if (st->win_x < 4) st->win_x = 4;
    if (st->win_y < 4) st->win_y = 4;
    if (st->win_x + win_w > w - 4) st->win_x = w - win_w - 4;
    if (st->win_y + win_h > h - TASKBAR_H - 4) st->win_y = h - TASKBAR_H - win_h - 4;
    if (st->win_x < 4) st->win_x = 4;
    if (st->win_y < 4) st->win_y = 4;
}

static gui_window_t *gui_active_window(gui_state_t *st) {
    if (st->active_window < 0 || st->active_window >= st->window_count) return 0;
    if (!st->windows[st->active_window].used) return 0;
    return &st->windows[st->active_window];
}

static const char *gui_window_title(const gui_window_t *win) {
    if (!win || !win->used) return "窗口";
    if (win->kind == GUI_WIN_PANEL) return panel_title(win->mode);
    return app_title(win->mode);
}

static void gui_window_metrics(int w, int h, const gui_window_t *win, int idx,
                               int *win_x, int *win_y, int *win_w, int *win_h) {
    int def_x = 0;
    int def_y = 0;
    default_window_metrics(w, h, &def_x, &def_y, win_w, win_h);
    *win_x = win && win->x ? win->x : def_x + idx * 24;
    *win_y = win && win->y ? win->y : def_y + idx * 18;
    if (*win_x + *win_w > w - 4) *win_x = w - *win_w - 4;
    if (*win_y + *win_h > h - TASKBAR_H - 4) *win_y = h - TASKBAR_H - *win_h - 4;
    if (*win_x < 4) *win_x = 4;
    if (*win_y < 4) *win_y = 4;
}

static void gui_sync_focus(gui_state_t *st) {
    gui_window_t *win = gui_active_window(st);
    if (!win) {
        st->app_mode = GUI_APP_NONE;
        st->win_x = 0;
        st->win_y = 0;
        return;
    }
    st->win_x = win->x;
    st->win_y = win->y;
    if (win->kind == GUI_WIN_PANEL) {
        st->app_mode = GUI_APP_NONE;
        st->active = win->mode;
    } else {
        st->app_mode = win->mode;
        st->active = PANEL_APPS;
    }
}

static void gui_store_focus(gui_state_t *st) {
    gui_window_t *win = gui_active_window(st);
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
    if (idx < 0 || idx >= st->window_count) return;
    if (!st->windows[idx].used) return;
    st->active_window = idx;
    gui_sync_focus(st);
}

static int gui_open_window(gui_state_t *st, int kind, int mode, int unique) {
    if (unique) {
        for (int i = 0; i < st->window_count; i++) {
            if (st->windows[i].used && st->windows[i].kind == kind && st->windows[i].mode == mode) {
                gui_focus_window(st, i);
                st->status = "窗口已打开";
                return i;
            }
        }
    }
    if (st->window_count >= GUI_MAX_WINDOWS) {
        st->status = "窗口数量已满";
        return -1;
    }
    int idx = st->window_count++;
    gui_window_t *win = &st->windows[idx];
    win->used = 1;
    win->kind = kind;
    win->mode = mode;
    win->x = 130 + (idx % 5) * 28;
    win->y = 62 + (idx % 5) * 22;
    gui_focus_window(st, idx);
    st->status = "窗口已打开";
    return idx;
}

static void gui_close_window(gui_state_t *st, int idx) {
    if (idx < 0 || idx >= st->window_count) return;
    for (int i = idx; i + 1 < st->window_count; i++) st->windows[i] = st->windows[i + 1];
    st->window_count--;
    if (st->window_count <= 0) {
        st->window_count = 0;
        st->active_window = -1;
    } else if (st->active_window >= st->window_count) {
        st->active_window = st->window_count - 1;
    } else if (st->active_window > idx) {
        st->active_window--;
    }
    st->status = "窗口已关闭";
    gui_sync_focus(st);
}

static void gui_focus_next_window(gui_state_t *st, int dir) {
    if (st->window_count <= 0) return;
    int idx = st->active_window;
    if (idx < 0) idx = 0;
    else idx = (idx + dir + st->window_count) % st->window_count;
    gui_focus_window(st, idx);
    st->status = "已切换窗口";
}

static void gui_open_panel_window(gui_state_t *st, int panel) {
    (void)gui_open_window(st, GUI_WIN_PANEL, panel, 1);
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

static uint8_t cmos_read(uint8_t reg) {
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
    if (v > 48) return 48;
    if (v < -48) return -48;
    return v;
}

static void draw_button(int x, int y, const char *label, uint32_t color) {
    rect(x, y, ACTION_W, ACTION_H, rgb(16, 30, 42));
    rect(x, y, 4, ACTION_H, color);
    rect(x + 4, y + ACTION_H - 2, ACTION_W - 4, 2, rgb(6, 12, 18));
    border(x, y, ACTION_W, ACTION_H, rgb(82, 118, 140));
    text(x + 13, y + 10, label, rgb(244, 250, 255), 1);
}

static void draw_small_button(int x, int y, int w, const char *label, uint32_t color) {
    rect(x, y, w, ACTION_H, rgb(16, 30, 42));
    rect(x, y, 4, ACTION_H, color);
    rect(x + 4, y + ACTION_H - 2, w - 4, 2, rgb(6, 12, 18));
    border(x, y, w, ACTION_H, rgb(82, 118, 140));
    text(x + 13, y + 10, label, rgb(244, 250, 255), 1);
}

static void draw_task_button(int x, int y, int w, const char *label, int active, uint32_t color) {
    rect(x, y, w, 32, active ? rgb(30, 50, 67) : rgb(14, 22, 31));
    rect(x, y, 4, 32, color);
    border(x, y, w, 32, active ? rgb(23, 147, 209) : rgb(50, 67, 80));
    text(x + 12, y + 8, label, rgb(226, 236, 244), 1);
}

static void draw_icon(int x, int y, int active, const char *label, uint32_t color) {
    uint32_t base = active ? rgb(32, 86, 116) : rgb(21, 48, 64);
    rect(x, y, 82, 58, base);
    rect(x, y, 4, 58, active ? color : rgb(58, 82, 96));
    border(x, y, 82, 58, active ? rgb(176, 218, 244) : rgb(50, 76, 92));
    rect(x + 14, y + 12, 20, 20, color);
    rect(x + 18, y + 16, 12, 12, rgb(238, 246, 252));
    text(x + 42, y + 22, label, rgb(232, 242, 248), 1);
}

static void draw_cursor(int x, int y) {
    for (int i = 0; i < 18; i++) rect(x, y + i, 2, 2, rgb(238, 246, 255));
    for (int i = 0; i < 12; i++) rect(x + i, y + i, 2, 2, rgb(238, 246, 255));
    rect(x + 5, y + 13, 9, 3, rgb(20, 27, 34));
    rect(x + 8, y + 15, 4, 7, rgb(20, 27, 34));
}

enum {
    GUI_KEY_UP = 1001,
    GUI_KEY_DOWN,
    GUI_KEY_LEFT,
    GUI_KEY_RIGHT,
    GUI_KEY_BACKSPACE,
};

static int key_poll(void) {
    static int ext = 0;
    static int shift = 0;
    while (inb(0x64) & 0x01) {
        uint8_t st = inb(0x64);
        if (st & 0x20) return 0;
        uint8_t sc = inb(0x60);
        if (sc == 0xE0) {
            ext = 1;
            continue;
        }
        if (sc == 0x2A || sc == 0x36) {
            shift = 1;
            ext = 0;
            continue;
        }
        if (sc == 0xAA || sc == 0xB6) {
            shift = 0;
            ext = 0;
            continue;
        }
        if (sc & 0x80) {
            ext = 0;
            continue;
        }
        if (ext) {
            ext = 0;
            if (sc == 0x1C) return '\n';
            if (sc == 0x35) return '/';
            if (sc == 0x48) return GUI_KEY_UP;
            if (sc == 0x50) return GUI_KEY_DOWN;
            if (sc == 0x4B) return GUI_KEY_LEFT;
            if (sc == 0x4D) return GUI_KEY_RIGHT;
            continue;
        }
        if (sc == 0x01) return 27;
        if (sc == 0x0E) return GUI_KEY_BACKSPACE;
        if (sc == 0x0F) return '\t';
        if (sc == 0x1C) return '\n';
        if (sc == 0x02) return shift ? '!' : '1';
        if (sc == 0x03) return shift ? '@' : '2';
        if (sc == 0x04) return shift ? '#' : '3';
        if (sc == 0x05) return shift ? '$' : '4';
        if (sc == 0x06) return shift ? '%' : '5';
        if (sc == 0x07) return shift ? '^' : '6';
        if (sc == 0x08) return shift ? '&' : '7';
        if (sc == 0x09) return shift ? '*' : '8';
        if (sc == 0x0A) return shift ? '(' : '9';
        if (sc == 0x0B) return shift ? ')' : '0';
        if (sc == 0x0C) return shift ? '_' : '-';
        if (sc == 0x0D) return shift ? '+' : '=';
        if (sc == 0x10) return shift ? 'Q' : 'q';
        if (sc == 0x11) return shift ? 'W' : 'w';
        if (sc == 0x12) return shift ? 'E' : 'e';
        if (sc == 0x13) return shift ? 'R' : 'r';
        if (sc == 0x14) return shift ? 'T' : 't';
        if (sc == 0x15) return shift ? 'Y' : 'y';
        if (sc == 0x16) return shift ? 'U' : 'u';
        if (sc == 0x17) return shift ? 'I' : 'i';
        if (sc == 0x18) return shift ? 'O' : 'o';
        if (sc == 0x19) return shift ? 'P' : 'p';
        if (sc == 0x1A) return shift ? '{' : '[';
        if (sc == 0x1B) return shift ? '}' : ']';
        if (sc == 0x1E) return shift ? 'A' : 'a';
        if (sc == 0x1F) return shift ? 'S' : 's';
        if (sc == 0x20) return shift ? 'D' : 'd';
        if (sc == 0x21) return shift ? 'F' : 'f';
        if (sc == 0x22) return shift ? 'G' : 'g';
        if (sc == 0x23) return shift ? 'H' : 'h';
        if (sc == 0x24) return shift ? 'J' : 'j';
        if (sc == 0x25) return shift ? 'K' : 'k';
        if (sc == 0x26) return shift ? 'L' : 'l';
        if (sc == 0x27) return shift ? ':' : ';';
        if (sc == 0x28) return shift ? '"' : '\'';
        if (sc == 0x29) return shift ? '~' : '`';
        if (sc == 0x2B) return shift ? '|' : '\\';
        if (sc == 0x2C) return shift ? 'Z' : 'z';
        if (sc == 0x2D) return shift ? 'X' : 'x';
        if (sc == 0x2E) return shift ? 'C' : 'c';
        if (sc == 0x2F) return shift ? 'V' : 'v';
        if (sc == 0x30) return shift ? 'B' : 'b';
        if (sc == 0x31) return shift ? 'N' : 'n';
        if (sc == 0x32) return shift ? 'M' : 'm';
        if (sc == 0x33) return shift ? '<' : ',';
        if (sc == 0x34) return shift ? '>' : '.';
        if (sc == 0x35) return shift ? '?' : '/';
        if (sc == 0x37) return '*';
        if (sc == 0x39) return ' ';
        if (sc == 0x47) return '7';
        if (sc == 0x48) return '8';
        if (sc == 0x49) return '9';
        if (sc == 0x4A) return '-';
        if (sc == 0x4B) return '4';
        if (sc == 0x4C) return '5';
        if (sc == 0x4D) return '6';
        if (sc == 0x4E) return '+';
        if (sc == 0x4F) return '1';
        if (sc == 0x50) return '2';
        if (sc == 0x51) return '3';
        if (sc == 0x52) return '0';
        if (sc == 0x53) return '.';
    }
    return 0;
}

static void draw_wallpaper(int w, int h) {
    char line[32];
    rect(0, 0, w, h, rgb(18, 27, 36));
    rect(0, 0, 112, h - TASKBAR_H, rgb(12, 22, 31));
    rect(112, 0, w - 112, 62, rgb(22, 34, 44));
    rect(112, 61, w - 112, 1, rgb(58, 82, 98));
    rect(0, h - TASKBAR_H, w, TASKBAR_H, rgb(16, 27, 37));
    rect(0, h - TASKBAR_H - 1, w, 1, rgb(90, 130, 154));
    rect(10, h - 36, 96, 30, rgb(27, 116, 166));
    border(10, h - 36, 96, 30, rgb(176, 218, 244));
    text(26, h - 27, "开始", rgb(244, 250, 255), 1);
    time_line(line, sizeof(line));
    rect(w - 96, h - 36, 86, 30, rgb(22, 38, 50));
    border(w - 96, h - 36, 86, 30, rgb(58, 86, 104));
    text(w - 82, h - 27, line, rgb(224, 238, 246), 1);
}

static void draw_desktop_tile(int x, int y, int w, int h, const char *title,
                              const char *value, uint32_t accent) {
    rect(x, y, w, h, rgb(25, 38, 48));
    rect(x, y, 5, h, accent);
    border(x, y, w, h, rgb(66, 92, 108));
    text(x + 16, y + 12, title, rgb(154, 176, 190), 1);
    text(x + 16, y + 36, value, rgb(232, 240, 246), 1);
}

static void draw_usage_bar(int x, int y, int w, int h, uint32_t used, uint32_t total, uint32_t color) {
    uint32_t filled = total ? (used * (uint32_t)w) / total : 0;
    if (filled > (uint32_t)w) filled = (uint32_t)w;
    rect(x, y, w, h, rgb(5, 9, 14));
    rect(x, y, (int)filled, h, color);
    border(x, y, w, h, rgb(48, 72, 86));
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

    text(tx, ty, "文件管理器", rgb(124, 220, 154), 1);
    line_u32(line, sizeof(line), "文件: ", fs_get_count(), " / 64");
    text(tx + 112, ty + 2, line, rgb(168, 188, 202), 1);
    rect(tx, ty + 30, content_w, 30, rgb(9, 18, 27));
    rect(tx + 1, ty + 31, content_w - 2, 8, rgb(17, 34, 48));
    border(tx, ty + 30, content_w, 30, rgb(66, 91, 108));
    text(tx + 12, ty + 40, "根目录 /", rgb(230, 238, 245), 1);

    draw_small_button(tx, ty + 72, 76, "新建", rgb(85, 180, 120));
    draw_small_button(tx + 84, ty + 72, 76, "打开", rgb(23, 147, 209));
    draw_small_button(tx + 168, ty + 72, 76, "追加", rgb(102, 214, 255));
    draw_small_button(tx + 252, ty + 72, 76, "清空", rgb(244, 194, 82));
    draw_small_button(tx + 336, ty + 72, 76, "删除", rgb(234, 82, 82));

    rect(tx, ty + 114, side_w, 206, rgb(11, 23, 32));
    border(tx, ty + 114, side_w, 206, rgb(53, 78, 94));
    text(tx + 14, ty + 128, "位置", rgb(194, 226, 242), 1);
    rect(tx + 10, ty + 154, side_w - 20, 28, rgb(28, 66, 88));
    rect(tx + 10, ty + 154, 3, 28, rgb(85, 180, 120));
    text(tx + 22, ty + 164, "根目录", rgb(244, 250, 255), 1);
    text(tx + 22, ty + 202, "文档", rgb(132, 150, 162), 1);
    text(tx + 22, ty + 232, "系统", rgb(132, 150, 162), 1);
    text(tx + 14, ty + 286, "滚轮切换", rgb(122, 142, 156), 1);

    rect(main_x, ty + 114, main_w, 206, rgb(24, 35, 44));
    border(main_x, ty + 114, main_w, 206, rgb(68, 96, 112));
    rect(main_x, ty + 114, main_w, 26, rgb(32, 48, 60));
    rect(main_x, ty + 139, main_w, 1, rgb(72, 100, 116));
    text(main_x + 14, ty + 122, "名称", rgb(208, 226, 236), 1);
    text(main_x + main_w - 166, ty + 122, "类型", rgb(208, 226, 236), 1);
    text(main_x + main_w - 82, ty + 122, "大小", rgb(208, 226, 236), 1);

    int list_y = ty + 146;
    uint32_t count = fs_get_count();
    if (count == 0) {
        text(main_x + 18, list_y + 18, "暂无文件", rgb(190, 208, 218), 1);
        text(main_x + 18, list_y + 42, "点击新建或按 N 创建", rgb(148, 168, 180), 1);
        rect(detail_x, ty + 114, detail_w, 206, rgb(11, 23, 32));
        border(detail_x, ty + 114, detail_w, 206, rgb(53, 78, 94));
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
        file_t *f = fs_get_file(file_idx);
        if (!f) continue;
        int y = list_y + (int)i * FILE_ROW_H;
        if ((i & 1) == 1) rect(main_x + 1, y - 8, main_w - 2, FILE_ROW_H, rgb(29, 43, 54));
        if ((int)file_idx == selected) {
            rect(main_x + 6, y - 7, main_w - 12, 24, rgb(24, 122, 170));
            rect(main_x + 6, y - 7, 4, 24, rgb(85, 180, 120));
        }
        rect(main_x + 16, y - 2, 18, 14, rgb(244, 194, 82));
        rect(main_x + 20, y - 7, 18, 7, rgb(230, 184, 74));
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, f->name);
        uint32_t row_fg = (int)file_idx == selected ? rgb(248, 252, 255) : rgb(218, 232, 240);
        uint32_t meta_fg = (int)file_idx == selected ? rgb(228, 244, 252) : rgb(150, 170, 182);
        text(main_x + 48, y, line, row_fg, 1);
        text(main_x + main_w - 126, y, f->type ? "目录" : "文件", meta_fg, 1);
        line_u32(line, sizeof(line), "", f->size, "B");
        text(main_x + main_w - 64, y, line, meta_fg, 1);
    }

    file_t *f = fs_get_file((uint32_t)selected);
    if (f) {
        rect(detail_x, ty + 114, detail_w, 206, rgb(11, 23, 32));
        border(detail_x, ty + 114, detail_w, 206, rgb(53, 78, 94));
        rect(detail_x, ty + 114, detail_w, 30, rgb(18, 36, 48));
        text(detail_x + 12, ty + 124, "详细信息", rgb(194, 226, 242), 1);
        rect(detail_x + 12, ty + 156, 30, 24, rgb(244, 194, 82));
        rect(detail_x + 17, ty + 150, 28, 8, rgb(230, 184, 74));
        text(detail_x + 52, ty + 160, f->name, rgb(238, 246, 255), 1);
        line_u32(line, sizeof(line), "大小: ", f->size, "B");
        text(detail_x + 12, ty + 194, line, rgb(210, 221, 230), 1);
        line_u32(line, sizeof(line), "容量: ", f->capacity, "B");
        text(detail_x + 12, ty + 216, line, rgb(210, 221, 230), 1);
        text(detail_x + 12, ty + 246, "预览", rgb(194, 226, 242), 1);
        char preview[64];
        uint32_t n = fs_read_file_data(f, 0, preview, sizeof(preview) - 1);
        preview[n] = 0;
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

        rect(tx, ty + 330, content_w, 28, rgb(9, 18, 27));
        border(tx, ty + 330, content_w, 28, rgb(53, 78, 94));
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, "已选择: ");
        append_str(line, sizeof(line), &pos, f->name);
        append_str(line, sizeof(line), &pos, "  ");
        append_uint(line, sizeof(line), &pos, f->size);
        append_str(line, sizeof(line), &pos, "B  再次点击/Enter 打开编辑");
        text(tx + 12, ty + 340, line, rgb(210, 221, 230), 1);
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

    text(tx, ty + 226, "活动任务", rgb(215, 152, 244), 1);
    uint32_t max = (uint32_t)task_get_count();
    if (max > 4) max = 4;
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
        text(tx + 18, ty + 248 + (int)i * 20, line, rgb(220, 230, 238), 1);
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
        int card_h = 78;
        int col = (int)(i & 1);
        int row = (int)(i >> 1);
        int x = tx + col * (card_w + 18);
        int y = ty + 88 + row * (card_h + 16);
        uint32_t accent = app->mode == GUI_APP_NOTES ? rgb(85, 180, 120) :
                          app->mode == GUI_APP_CALC ? rgb(23, 147, 209) :
                          app->mode == GUI_APP_UWC ? rgb(244, 194, 82) :
                          app->mode == GUI_APP_SNAKE ? rgb(124, 220, 154) :
                                                        rgb(78, 192, 236);
        rect(x, y, card_w, card_h, (int)i == selected ? rgb(32, 58, 74) : rgb(24, 36, 46));
        rect(x, y, 6, card_h, accent);
        border(x, y, card_w, card_h, (int)i == selected ? rgb(80, 152, 192) : rgb(66, 92, 108));
        rect(x + 20, y + 18, 34, 34, accent);
        rect(x + 26, y + 24, 22, 22, rgb(28, 40, 50));
        if ((int)i == selected) {
            rect(x + card_w - 26, y + 12, 10, 10, rgb(36, 126, 176));
        }
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, app->name);
        text(x + 70, y + 18, line, rgb(230, 240, 246), 1);
        text(x + 70, y + 42, app->description, rgb(156, 176, 188), 1);
    }
    text(tx, ty + 286, "方向键选择  Enter 打开  鼠标点击卡片选择", rgb(94, 112, 124), 1);
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
    st->status = "笔记已保存";
}

static void note_insert(gui_state_t *st, char c) {
    if (st->note_len + 1 >= NOTE_EDIT_CAP) {
        st->status = "笔记已满";
        return;
    }
    st->note_buf[st->note_len++] = c;
    st->note_buf[st->note_len] = 0;
    note_save(st);
}

static void note_backspace(gui_state_t *st) {
    if (st->note_len == 0) return;
    st->note_len--;
    while (st->note_len > 0 &&
           ((uint8_t)st->note_buf[st->note_len] & 0xC0) == 0x80) {
        st->note_len--;
    }
    st->note_buf[st->note_len] = 0;
    note_save(st);
}

static void draw_notes_app(int tx, int ty, int win_w, gui_state_t *st) {
    note_load(st);
    int list_w = 150;
    int edit_x = tx + list_w + 18;
    int edit_w = win_w - list_w - 86;
    if (edit_w < 260) edit_w = 260;
    text(tx, ty, "记事本", rgb(124, 220, 154), 1);
    text(tx, ty + 40, "选择左侧文件后直接编辑，输入会自动保存", rgb(148, 162, 174), 1);
    char line[96];

    rect(tx, ty + 70, list_w, 222, rgb(8, 18, 27));
    border(tx, ty + 70, list_w, 222, rgb(43, 64, 78));
    text(tx + 12, ty + 82, "文件", rgb(190, 224, 242), 1);
    uint32_t count = fs_get_count();
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
            file_t *f = fs_get_file(file_idx);
            if (!f) continue;
            int y = ty + 112 + (int)i * FILE_ROW_H;
            if ((int)file_idx == selected) rect(tx + 8, y - 6, list_w - 16, 24, rgb(24, 60, 86));
            text(tx + 18, y, f->name, rgb(220, 230, 238), 1);
        }
    }

    line2(line, sizeof(line), "文件: ", gui_note_name(st));
    text(edit_x, ty + 70, line, rgb(210, 221, 230), 1);
    line_u32(line, sizeof(line), "大小: ", st->note_len, "B");
    text(edit_x, ty + 92, line, rgb(210, 221, 230), 1);
    rect(edit_x, ty + 118, edit_w, 174, rgb(5, 10, 16));
    border(edit_x, ty + 118, edit_w, 174, rgb(85, 180, 120));
    int x = edit_x + 8;
    int y = ty + 126;
    utf8_state_t utf8;
    utf8_init(&utf8);
    for (uint32_t i = 0; i < st->note_len && y < ty + 280; i++) {
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

        int advance = draw_text_codepoint(x, y, cp, rgb(220, 230, 238), 1);
        x += advance;
        if (x > edit_x + edit_w - 16) {
            x = edit_x + 8;
            y += 18;
        }
    }
    if (y < ty + 280) rect(x, y + 12, 8, 2, rgb(124, 220, 154));
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
    rect(tx, ty + 88, 300, 74, rgb(5, 10, 16));
    border(tx, ty + 88, 300, 74, rgb(23, 147, 209));
    text(tx + 18, ty + 98, st->calc_just_evaluated ? "结果" : "当前", rgb(132, 196, 232), 1);
    text(tx + 22, ty + 126, line, rgb(235, 242, 250), 2);

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

    rect(tx, ty + 216, 36, 24, rgb(18, 38, 52));
    rect(tx + 42, ty + 216, 36, 24, rgb(18, 38, 52));
    rect(tx + 84, ty + 216, 36, 24, rgb(18, 38, 52));
    rect(tx + 126, ty + 216, 36, 24, rgb(18, 38, 52));
    text(tx + 12, ty + 224, "+", rgb(235, 242, 250), 1);
    text(tx + 54, ty + 224, "-", rgb(235, 242, 250), 1);
    text(tx + 96, ty + 224, "*", rgb(235, 242, 250), 1);
    text(tx + 138, ty + 224, "/", rgb(235, 242, 250), 1);
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
    text(tx, ty, "浏览器", rgb(78, 192, 236), 1);
    text(tx, ty + 30, "编辑地址后按 Enter 加载，方向键滚动", rgb(148, 162, 174), 1);
    rect(tx, ty + 58, view_w, 32, rgb(6, 14, 22));
    border(tx, ty + 58, view_w, 32, rgb(78, 192, 236));
    text(tx + 10, ty + 68, st->browser_url, rgb(232, 242, 248), 1);
    rect(tx + view_w - 12, ty + 68, 6, 14, rgb(78, 192, 236));
    draw_small_button(tx, ty + 104, 96, "Enter 加载", rgb(78, 192, 236));
    text(tx + 112, ty + 112, st->status ? st->status : "浏览器就绪", rgb(168, 190, 204), 1);
    rect(tx, ty + 146, view_w, 196, rgb(4, 9, 14));
    border(tx, ty + 146, view_w, 196, rgb(50, 74, 90));
    draw_wrapped_text(tx + 12, ty + 158, view_w - 24, 172, st->browser_page, st->browser_scroll);
}

static void draw_window_frame(int x, int y, int win_w, int win_h, const char *title, int active) {
    rect(x + 7, y + 8, win_w, win_h, rgb(5, 10, 15));
    rect(x, y, win_w, win_h, active ? rgb(20, 32, 42) : rgb(18, 27, 36));
    border(x, y, win_w, win_h, active ? rgb(58, 138, 184) : rgb(70, 92, 108));
    rect(x + 1, y + 1, win_w - 2, 34, active ? rgb(24, 112, 166) : rgb(94, 120, 136));
    rect(x + 1, y + 35, win_w - 2, 1, rgb(178, 197, 208));
    rect(x + win_w - 34, y + 7, 22, 20, active ? rgb(204, 58, 66) : rgb(146, 70, 76));
    border(x + win_w - 34, y + 7, 22, 20, rgb(112, 42, 48));
    text(x + win_w - 27, y + 13, "x", rgb(250, 236, 236), 1);
    text(x + 16, y + 10, title, rgb(244, 250, 255), 1);
}

static void draw_panel_window(int tx, int ty, int win_w, int w, int h, gui_state_t *st, int panel) {
    if (panel == PANEL_FILES) draw_files_panel(tx, ty, win_w, st);
    else if (panel == PANEL_DISK) draw_disk_panel(tx, ty, win_w);
    else if (panel == PANEL_SYS) draw_resource_panel(tx, ty, win_w, w, h);
    else draw_apps_panel(tx, ty, win_w, st);
}

static void draw_app_window_body(int tx, int ty, int win_w, gui_state_t *st, int mode) {
    if (mode == GUI_APP_NOTES) draw_notes_app(tx, ty, win_w, st);
    else if (mode == GUI_APP_CALC) draw_calc_app(tx, ty, st);
    else if (mode == GUI_APP_UWC) draw_uwc_app(tx, ty, st);
    else if (mode == GUI_APP_SNAKE) draw_snake_app(tx, ty, st);
    else if (mode == GUI_APP_BROWSER) draw_browser_app(tx, ty, win_w, st);
}

static void draw_one_window(int w, int h, gui_state_t *st, int idx) {
    gui_window_t *win = &st->windows[idx];
    if (!win->used) return;
    int old_active = st->active;
    int old_app = st->app_mode;
    int old_x = st->win_x;
    int old_y = st->win_y;
    int win_x, win_y, win_w, win_h;
    gui_window_metrics(w, h, win, idx, &win_x, &win_y, &win_w, &win_h);
    draw_window_frame(win_x, win_y, win_w, win_h, gui_window_title(win), idx == st->active_window);

    st->win_x = win_x;
    st->win_y = win_y;
    if (idx == st->active_window) gui_store_focus(st);

    int tx = win_x + 30;
    int ty = win_y + 62;
    if (win->kind == GUI_WIN_PANEL) {
        st->active = win->mode;
        st->app_mode = GUI_APP_NONE;
        draw_panel_window(tx, ty, win_w, w, h, st, win->mode);
    } else {
        st->active = PANEL_APPS;
        st->app_mode = win->mode;
        draw_app_window_body(tx, ty, win_w, st, win->mode);
    }
    rect(win_x + 1, win_y + win_h - 31, win_w - 2, 30, rgb(22, 34, 44));
    rect(win_x + 1, win_y + win_h - 32, win_w - 2, 1, rgb(62, 88, 104));
    text(win_x + 24, win_y + win_h - 22, idx == st->active_window ? st->status : "单击任务栏切换", rgb(168, 190, 204), 1);

    st->active = old_active;
    st->app_mode = old_app;
    st->win_x = old_x;
    st->win_y = old_y;
}

static void draw_taskbar_windows(int h, const gui_state_t *st) {
    int x = 118;
    int task_y = h - 36;
    for (int i = 0; i < st->window_count && x < 610; i++) {
        const gui_window_t *win = &st->windows[i];
        if (!win->used) continue;
        uint32_t color = win->kind == GUI_WIN_PANEL ? rgb(85, 180, 120) : rgb(23, 147, 209);
        int width = 104;
        draw_task_button(x, task_y, width, gui_window_title(win), i == st->active_window, color);
        x += width + 8;
    }
}

static void draw_gui_screen(int w, int h, gui_state_t *st) {
    gui_sync_focus(st);
    draw_desktop(w, h, st);
}

static void draw_gui_frame(const fb_info_t *fb, int w, int h, gui_state_t *st, int mx, int my) {
    draw_gui_screen(w, h, st);
    draw_cursor(mx, my);
    gui_present_surface(fb);
}

static void draw_desktop(int w, int h, gui_state_t *st) {
    draw_wallpaper(w, h);
    char line[96];

    text(22, 18, "HBOS", rgb(244, 250, 255), 2);
    text(22, 48, "图形桌面", rgb(190, 224, 242), 1);
    text(140, 20, "系统概览", rgb(230, 240, 246), 2);
    text(142, 48, "左侧启动栏打开工具，桌面直接显示当前状态", rgb(150, 172, 186), 1);

    draw_icon(15, 92, st->active == PANEL_FILES, "文件", rgb(85, 180, 120));
    draw_icon(15, 162, st->active == PANEL_DISK, "磁盘", rgb(230, 184, 74));
    draw_icon(15, 232, st->active == PANEL_SYS, "资源", rgb(196, 116, 230));
    draw_icon(15, 302, st->active == PANEL_APPS, "应用", rgb(23, 147, 209));

    uint64_t total = pmm_get_total_mem();
    uint64_t free = pmm_get_free_mem();
    uint64_t used = total > free ? total - free : 0;
    line_u32(line, sizeof(line), "", (uint32_t)(used / 1024), "K used");
    draw_desktop_tile(142, 88, 150, 74, "内存", line, rgb(196, 116, 230));

    line_u32(line, sizeof(line), "", fs_get_count(), " files");
    draw_desktop_tile(310, 88, 150, 74, "文件", line, rgb(85, 180, 120));

    line2(line, sizeof(line), "FS: ", fs_backend_name());
    draw_desktop_tile(478, 88, 190, 74, "文件系统", line, rgb(23, 147, 209));

    line2(line, sizeof(line), "Block: ", block_backend_name());
    draw_desktop_tile(142, 182, 246, 74, "磁盘", line, rgb(230, 184, 74));

    line_u32(line, sizeof(line), "", (uint32_t)task_get_count(), " tasks");
    draw_desktop_tile(406, 182, 126, 74, "任务", line, rgb(124, 220, 154));

    line_u32(line, sizeof(line), "", gui_app_count(), " apps");
    draw_desktop_tile(550, 182, 118, 74, "应用", line, rgb(102, 214, 255));

    rect(142, 282, 250, 126, rgb(25, 38, 48));
    border(142, 282, 250, 126, rgb(66, 92, 108));
    text(158, 298, "最近文件", rgb(230, 240, 246), 1);
    uint32_t count = fs_get_count();
    if (count == 0) {
        text(158, 328, "暂无文件", rgb(150, 172, 186), 1);
        text(158, 352, "按 N 或点文件中新建", rgb(150, 172, 186), 1);
    } else {
        uint32_t shown = count > 4 ? 4 : count;
        for (uint32_t i = 0; i < shown; i++) {
            file_t *f = fs_get_file(i);
            if (!f) continue;
            line[0] = 0;
            uint32_t pos = 0;
            append_str(line, sizeof(line), &pos, f->name);
            append_str(line, sizeof(line), &pos, "  ");
            append_uint(line, sizeof(line), &pos, f->size);
            append_str(line, sizeof(line), &pos, "B");
            text(158, 328 + (int)i * 20, line, rgb(208, 226, 236), 1);
        }
    }

    rect(418, 282, 250, 126, rgb(25, 38, 48));
    border(418, 282, 250, 126, rgb(66, 92, 108));
    text(434, 298, "快捷操作", rgb(230, 240, 246), 1);
    text(434, 328, "N 新建文件", rgb(208, 226, 236), 1);
    text(434, 350, "Enter 打开当前项", rgb(208, 226, 236), 1);
    text(434, 372, "Tab/Space 切换窗口", rgb(208, 226, 236), 1);

    for (int i = 0; i < st->window_count; i++) {
        if (i != st->active_window) draw_one_window(w, h, st, i);
    }
    if (st->active_window >= 0 && st->active_window < st->window_count)
        draw_one_window(w, h, st, st->active_window);

    draw_taskbar_windows(h, st);
}

static file_t *selected_file(gui_state_t *st) {
    uint32_t count = fs_get_count();
    if (count == 0) {
        st->selected_file = 0;
        return 0;
    }
    if (st->selected_file < 0) st->selected_file = 0;
    if ((uint32_t)st->selected_file >= count) st->selected_file = (int)count - 1;
    return fs_get_file((uint32_t)st->selected_file);
}

static int file_display_index(file_t *target) {
    uint32_t count = fs_get_count();
    for (uint32_t i = 0; i < count; i++) {
        if (fs_get_file(i) == target) return (int)i;
    }
    return 0;
}

static void gui_select_file(gui_state_t *st, int index) {
    uint32_t count = fs_get_count();
    if (count == 0) {
        st->selected_file = 0;
        st->last_clicked_file = -1;
        return;
    }
    if (index < 0) index = 0;
    if ((uint32_t)index >= count) index = (int)count - 1;
    st->selected_file = index;

    file_t *f = fs_get_file((uint32_t)index);
    if (f) gui_set_note_name(st, f->name);
}

static void gui_create_note(gui_state_t *st) {
    char name[MAX_FILENAME];
    file_t *f = 0;
    uint32_t index = 0;
    for (; index < MAX_FILES; index++) {
        gui_make_note_name(name, sizeof(name), index);
        if (fs_find_file(name)) continue;
        f = fs_create_file(name);
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
        st->note_loaded = 1;
        gui_set_note_name(st, f->name);
        st->note_loaded = 1;
    }
    (void)fs_sync();
    gui_select_file(st, file_display_index(f));
}

static void gui_append_note(gui_state_t *st) {
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
    file_t *f = selected_file(st);
    if (!f) {
        st->status = "未选择文件";
        return;
    }
    char name[MAX_FILENAME];
    uint32_t i = 0;
    while (f->name[i] && i + 1 < sizeof(name)) {
        name[i] = f->name[i];
        i++;
    }
    name[i] = 0;
    if (fs_delete_file(name) < 0) st->status = "删除失败";
    else {
        st->status = "文件已删除";
        if (strcmp(name, gui_note_name(st)) == 0) {
            st->note_len = 0;
            st->note_buf[0] = 0;
            st->note_name[0] = 0;
            st->note_loaded = 1;
        }
    }
    if (fs_get_count() > 0) gui_select_file(st, st->selected_file > 0 ? st->selected_file - 1 : 0);
    else st->selected_file = 0;
    (void)fs_sync();
}

static void gui_truncate_selected(gui_state_t *st) {
    file_t *f = selected_file(st);
    if (!f) {
        st->status = "未选择文件";
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
    if (gui_open_window(st, GUI_WIN_APP, app->mode, 0) < 0) return;
    if (app->mode == GUI_APP_SNAKE) {
        snake_reset(st);
    }
    st->status = "应用已打开";
}

static void gui_open_selected_file(gui_state_t *st) {
    file_t *f = selected_file(st);
    if (!f) {
        st->status = "未选择文件";
        return;
    }
    gui_set_note_name(st, f->name);
    if (gui_open_window(st, GUI_WIN_APP, GUI_APP_NOTES, 0) >= 0)
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
        gui_select_file(st, gui_step_selection(st->selected_file, fs_get_count(), steps));
        st->status = "滚轮选择文件";
    } else if (st->app_mode == GUI_APP_NONE && st->active == PANEL_APPS) {
        st->selected_app = gui_step_selection(st->selected_app, gui_app_count(), steps);
        st->status = "滚轮选择应用";
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
}

static int hit_window_titlebar(int w, int h, const gui_state_t *st, int mx, int my) {
    for (int i = st->window_count - 1; i >= 0; i--) {
        int win_x, win_y, win_w, win_h;
        gui_window_metrics(w, h, &st->windows[i], i, &win_x, &win_y, &win_w, &win_h);
        (void)win_h;
        if (mx >= win_x && mx < win_x + win_w - 40 &&
            my >= win_y && my < win_y + 34) return i;
    }
    return -1;
}

static int hit_window_close(int w, int h, const gui_state_t *st, int mx, int my) {
    for (int i = st->window_count - 1; i >= 0; i--) {
        int win_x, win_y, win_w, win_h;
        gui_window_metrics(w, h, &st->windows[i], i, &win_x, &win_y, &win_w, &win_h);
        (void)win_h;
        if (mx >= win_x + win_w - 34 && mx < win_x + win_w - 8 &&
            my >= win_y + 4 && my < win_y + 30) return i;
    }
    return -1;
}

static int hit_action(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->active_window < 0 || st->active_window >= st->window_count) return -1;
    const gui_window_t *win = &st->windows[st->active_window];
    if (!win->used || win->kind != GUI_WIN_PANEL) return -1;
    int win_x, win_y, win_w, win_h;
    gui_window_metrics(w, h, win, st->active_window, &win_x, &win_y, &win_w, &win_h);
    int tx = win_x + 30;
    int ty = win_y + 62;
    (void)win_h;
    int panel = win->mode;
    if (panel == PANEL_FILES) {
        int y = ty + 72;
        if (my >= y && my < y + ACTION_H) {
            if (mx >= tx && mx < tx + 76) return 0;
            if (mx >= tx + 84 && mx < tx + 160) return 1;
            if (mx >= tx + 168 && mx < tx + 244) return 2;
            if (mx >= tx + 252 && mx < tx + 328) return 3;
            if (mx >= tx + 336 && mx < tx + 412) return 4;
        }
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
        int list_y = ty + 146;
        if (mx >= main_x && mx < main_x + main_w && my >= list_y - 8 && my < list_y - 8 + FILE_LIST_ROWS * FILE_ROW_H) {
            int selected = st->selected_file;
            if (selected < 0) selected = 0;
            uint32_t start = selected >= FILE_LIST_ROWS ? (uint32_t)selected - (FILE_LIST_ROWS - 1) : 0;
            int idx = (my - (list_y - 8)) / FILE_ROW_H;
            uint32_t file_idx = start + (uint32_t)idx;
            if (idx >= 0 && idx < FILE_LIST_ROWS && file_idx < fs_get_count()) return FILE_ACTION_BASE + (int)file_idx;
        }
    } else if (panel == PANEL_DISK) {
        int y = ty + 194;
        if (mx >= tx && mx < tx + ACTION_W && my >= y && my < y + ACTION_H) return 5;
    } else if (panel == PANEL_APPS) {
        int y = ty + 42;
        if (mx >= tx && mx < tx + ACTION_W && my >= y && my < y + ACTION_H) return 6;
        int card_w = 250;
        int card_h = 78;
        for (uint32_t i = 0; i < gui_app_count(); i++) {
            int col = (int)(i & 1);
            int row = (int)(i >> 1);
            int x = tx + col * (card_w + 18);
            int cy = ty + 88 + row * (card_h + 16);
            if (mx >= x && mx < x + card_w && my >= cy && my < cy + card_h)
                return APP_ACTION_BASE + (int)i;
        }
    }
    return -1;
}

static int hit_note_file(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->active_window < 0 || st->active_window >= st->window_count) return -1;
    const gui_window_t *win = &st->windows[st->active_window];
    if (!win->used || win->kind != GUI_WIN_APP || win->mode != GUI_APP_NOTES) return -1;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics(w, h, win, st->active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_w;
    (void)win_h;
    int tx = win_x + 30;
    int ty = win_y + 62;
    int list_w = 150;
    int list_y = ty + 112;
    if (mx < tx || mx >= tx + list_w || my < list_y - 8) return -1;

    uint32_t count = fs_get_count();
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
    int x = 118;
    int task_y = h - 36;
    if (my < task_y || my >= task_y + 32) return -1;
    for (int i = 0; i < st->window_count && x < 610; i++) {
        if (!st->windows[i].used) continue;
        if (mx >= x && mx < x + 104) return i;
        x += 112;
    }
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
    if (key == '\t') {
        gui_focus_next_window(st, 1);
        return;
    }
    gui_sync_focus(st);
    if (st->app_mode == GUI_APP_NOTES) {
        note_load(st);
        if (key == GUI_KEY_BACKSPACE) note_backspace(st);
        else if (key == '\n') note_insert(st, '\n');
        else if (key >= 32 && key <= 126) note_insert(st, (char)key);
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
        else if (key == GUI_KEY_DOWN && (uint32_t)(st->selected_file + 1) < fs_get_count()) gui_select_file(st, st->selected_file + 1);
    } else if (st->app_mode == GUI_APP_SNAKE) {
        if (key == '\n') snake_reset(st);
        else if (key == GUI_KEY_LEFT) snake_turn(st, -1, 0);
        else if (key == GUI_KEY_RIGHT) snake_turn(st, 1, 0);
        else if (key == GUI_KEY_UP) snake_turn(st, 0, -1);
        else if (key == GUI_KEY_DOWN) snake_turn(st, 0, 1);
    } else if (st->app_mode == GUI_APP_BROWSER) {
        browser_init(st);
        if (key == '\n') browser_load(st);
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
    }
}

static void handle_key(gui_state_t *st, int key) {
    gui_sync_focus(st);
    if (key == '\t' || key == ' ') {
        gui_focus_next_window(st, 1);
    } else if (key == GUI_KEY_RIGHT && st->window_count > 0 && st->app_mode == GUI_APP_NONE) {
        st->active = (st->active + 1) & 3;
        gui_active_window(st)->mode = st->active;
        st->status = "已切换面板";
    } else if (key == GUI_KEY_LEFT && st->window_count > 0 && st->app_mode == GUI_APP_NONE) {
        st->active = (st->active + 3) & 3;
        gui_active_window(st)->mode = st->active;
        st->status = "已切换面板";
    } else if (key == GUI_KEY_UP && st->active == PANEL_FILES) {
        if (st->selected_file > 0) gui_select_file(st, st->selected_file - 1);
        st->status = "已选择文件";
    } else if (key == GUI_KEY_DOWN && st->active == PANEL_FILES) {
        if ((uint32_t)(st->selected_file + 1) < fs_get_count()) gui_select_file(st, st->selected_file + 1);
        st->status = "已选择文件";
    } else if (key == GUI_KEY_UP && st->active == PANEL_APPS) {
        if (st->selected_app > 0) st->selected_app--;
        st->status = "已选择应用";
    } else if (key == GUI_KEY_DOWN && st->active == PANEL_APPS) {
        if ((uint32_t)(st->selected_app + 1) < gui_app_count()) st->selected_app++;
        st->status = "已选择应用";
    } else if (key == 'n') {
        gui_create_note(st);
        gui_open_panel_window(st, PANEL_FILES);
    } else if (key == 'a') {
        gui_append_note(st);
        gui_open_panel_window(st, PANEL_FILES);
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
    } else if (key == '\n') {
        if (st->active == PANEL_FILES) gui_open_selected_file(st);
        else if (st->active == PANEL_DISK) gui_install(st);
        else if (st->active == PANEL_APPS) gui_open_selected_app(st);
    }
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
        .active_window = -1,
        .last_clicked_file = -1,
        .status = "就绪",
    };

    (void)block_init();
    if (mouse_init() < 0) st.status = "未检测到鼠标";
    gui_open_panel_window(&st, PANEL_FILES);

    uint64_t surface_bytes = (uint64_t)w * (uint64_t)h * sizeof(uint32_t);
    size_t surface_pages = (size_t)((surface_bytes + GUI_PAGE_SIZE - 1) / GUI_PAGE_SIZE);
    uint64_t surface_phys = pmm_alloc_blocks(surface_pages);
    if (surface_phys) {
        gui_set_surface((uint32_t *)(uintptr_t)surface_phys, w, h, (uint32_t)w);
    } else {
        st.status = "图形缓冲分配失败";
    }

    draw_gui_frame(&fb, w, h, &st, mx, my);
    while (1) {
        int key = key_poll();
        if (key == 27 && st.window_count > 0) {
            gui_close_window(&st, st.active_window);
            draw_gui_frame(&fb, w, h, &st, mx, my);
            continue;
        }
        if (key == 27 || key == 'q') break;
        if (key) {
            gui_sync_focus(&st);
            if (st.app_mode == GUI_APP_NONE) handle_key(&st, key);
            else handle_app_key(&st, key);
            draw_gui_frame(&fb, w, h, &st, mx, my);
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
            if (mx > w - 24) mx = w - 24;
            if (my > h - 24) my = h - 24;

            int redraw = 0;
            if (acc_dz != 0) {
                handle_wheel(&st, acc_dz);
                redraw = 1;
            }

            int left_down = (st.buttons & 1) != 0;
            if (dragging_window >= 0 && left_down) {
                int win_x, win_y, win_w, win_h;
                gui_focus_window(&st, dragging_window);
                gui_window_metrics(w, h, &st.windows[dragging_window], dragging_window, &win_x, &win_y, &win_w, &win_h);
                (void)win_x;
                (void)win_y;
                gui_set_active_window_pos(&st, mx - drag_off_x, my - drag_off_y);
                clamp_window(&st, w, h, win_w, win_h);
                gui_store_focus(&st);
                st.status = "窗口已移动";
                drag_pending = 1;
                int moved = mx - drag_last_draw_x;
                if (moved < 0) moved = -moved;
                int moved_y = my - drag_last_draw_y;
                if (moved_y < 0) moved_y = -moved_y;
                if (moved + moved_y >= 12) {
                    drag_last_draw_x = mx;
                    drag_last_draw_y = my;
                    redraw = 1;
                    drag_pending = 0;
                }
            } else if (!left_down) {
                if (dragging_window >= 0 && drag_pending) redraw = 1;
                dragging_window = -1;
                drag_pending = 0;
            }

            if ((st.buttons & 1) && !(last_buttons & 1)) {
                st.clicks++;
                int close_idx = hit_window_close(w, h, &st, mx, my);
                if (close_idx >= 0) {
                    gui_close_window(&st, close_idx);
                } else {
                    int task_idx = hit_task_window(h, &st, mx, my);
                    if (task_idx >= 0) {
                        gui_focus_window(&st, task_idx);
                        st.status = "已切换窗口";
                    } else {
                        int title_idx = hit_window_titlebar(w, h, &st, mx, my);
                        if (title_idx >= 0) {
                            gui_focus_window(&st, title_idx);
                    int win_x, win_y, win_w, win_h;
                            gui_window_metrics(w, h, &st.windows[title_idx], title_idx, &win_x, &win_y, &win_w, &win_h);
                    (void)win_w;
                    (void)win_h;
                            dragging_window = title_idx;
                    drag_off_x = mx - win_x;
                    drag_off_y = my - win_y;
                            drag_last_draw_x = mx;
                            drag_last_draw_y = my;
                            drag_pending = 0;
                    st.status = "拖动窗口";
                        } else if (st.app_mode != GUI_APP_NONE) {
                            int note_file = hit_note_file(w, h, &st, mx, my);
                            if (note_file >= 0) {
                                gui_select_file(&st, note_file);
                                st.status = "已切换编辑文件";
                            } else {
                                st.status = "应用内请使用键盘";
                            }
                        } else {
                    int panel = hit_panel_shortcut(w, h, mx, my);
                    if (panel >= 0) {
                                gui_open_panel_window(&st, panel);
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
                redraw = 1;
            }
            last_buttons = st.buttons;

            if (redraw) {
                draw_gui_frame(&fb, w, h, &st, mx, my);
            } else if (mx != old_mx || my != old_my) {
                draw_gui_frame(&fb, w, h, &st, mx, my);
            }
        }
        if (snake_auto_tick(&st)) {
            draw_gui_frame(&fb, w, h, &st, mx, my);
        }
        task_yield();
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
