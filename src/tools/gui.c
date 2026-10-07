#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "../acpi.h"
#include "../ui/theme.h"
#include "../ui/ui_tokens.h"
#include "../block.h"
#include "../core/pmm.h"
#include "../core/task.h"
#include "../core/wait.h"
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
#include "../elf.h"
#include "../api/gui_service.h"
#include "../user/app.h"
#include "../user/hax_app.h"
#include "../version.h"
#include "../vfs.h"
#include "../gui/wm.h"
#include "../gui/winsrv.h"
#include "../gui/gui_state.h"
#include "../gui/gui_dirty.h"
#include "../gui/gui_draw.h"
#include "../gui/gui_html.h"
#include "../gui/gui_app.h"
#include "../gui/gui_panel.h"
#include "../gui/browser_backend.h"
#include "../gui/browser_layout.h"

/* app_imgview.c / app_hexview.c 各自路径 setter，供 gui_open_selected_file()
 * 打开对应查看器前写入要打开的文件路径 */
void app_imgview_set_path(gui_state_t *st, const char *path);
void app_hexview_set_path(gui_state_t *st, const char *path);
void app_snake_reset(gui_state_t *st);
int  gui_has_code_suffix(const char *path);
void app_code_set_path(gui_state_t *st, const char *path);
void app_code_load(gui_state_t *st);
void app_code_clamp_scroll(gui_state_t *st);
void app_code_ensure_visible(gui_state_t *st);
void app_code_open_selected(gui_state_t *st);
void app_code_gui_run(gui_state_t *st, const fb_info_t *fb);
void app_code_command(gui_state_t *st, int cmd);
void app_code_set_cursor(gui_state_t *st, uint32_t off, int keep_selection);
void app_code_begin_selection(gui_state_t *st, uint32_t off);
int  app_code_hit_command(int w, int h, const gui_state_t *st, int mx, int my);
int  app_code_hit_editor(int w, int h, gui_state_t *st, int mx, int my, uint32_t *off);
int  app_code_hit_file(int w, int h, const gui_state_t *st, int mx, int my);
int  app_code_wheel(gui_state_t *st, int steps);
void app_notes_create(gui_state_t *st);
void app_notes_append(gui_state_t *st);
int app_notes_hit_file(int w, int h, const gui_state_t *st, int mx, int my);
int app_notes_hit_editor(int w, int h, gui_state_t *st, int mx, int my, uint32_t *off);
void app_diag_welcome(gui_state_t *st);
void app_diag_install_sink(gui_state_t *st);
void app_diag_remove_sink(void);
#include "../rtc_tz.h"
#include "../shell/shell.h"
#include "tool.h"
#include "cc.h"

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
#define CODE_VIEW_ROWS 10
#define MONO_GLYPH_W GUI_MONO_GLYPH_W
#define MONO_GLYPH_H GUI_MONO_GLYPH_H
#define GUI_PAGE_SIZE 4096ULL
#define FILE_LIST_ROWS 8
#define NOTE_FILE_ROWS 7

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b);

// Palette — dark branch follows KDE Breeze Dark (cool grays, single blue accent
// #3daee9). The historical cyber_* names are kept so call sites need no churn;
// only the values changed. Light branch retuned toward Breeze Light.
/*
 * 过渡层：把这些历史函数体改为读取主题令牌。
 *
 * 它们原本各自硬编码深浅两套数值，是 472 处硬编码颜色的中枢——图标主题
 * 只需改这一处，整屏就会跟着变。参数 light 保留是为了不改几百个调用点；
 * 实际取值一律来自 ui_theme()，因此"深浅"由**当前主题**决定，而不是由
 * 调用方传的布尔值决定。这样外部主题包（可能既非纯深也非纯浅）才能生效。
 *
 * 后续 P1 会把调用点逐个改成直接读令牌，届时这些函数删除。
 */
static inline uint32_t cyber_bg_top(int light) { (void)light; return ui_theme()->bg; }
static inline uint32_t cyber_bg_bot(int light) { (void)light; return ui_shade(ui_theme()->bg, -8); }
static inline uint32_t cyber_neon_pink(int light) { (void)light; return ui_theme()->accent; }
static inline uint32_t cyber_neon_cyan(int light) { (void)light; return ui_theme()->accent; }
static inline uint32_t cyber_neon_yellow(int light) { (void)light; return ui_theme()->warn; }
static inline uint32_t cyber_neon_purple(int light) { (void)light; return ui_theme()->ok; }
static inline uint32_t cyber_text(int light) { (void)light; return ui_theme()->fg; }
static inline uint32_t cyber_text_muted(int light) { (void)light; return ui_theme()->dim; }
static inline uint32_t cyber_border(int light) { (void)light; return ui_theme()->line2; }
static inline uint32_t cyber_card_bg_top(int light) { (void)light; return ui_theme()->surface; }
static inline uint32_t cyber_card_bg_bot(int light) { (void)light; return ui_theme()->surface2; }

/* ── 主题应用 ──────────────────────────────────────────────
 * 唯一的"把深浅意图落到主题层"的入口。F4、设置应用、service 跟随三条路径
 * 都经此函数，避免任何一条漏掉导致"提示已切换但画面没变"。
 * 记录已生效的值用于变更检测（见主循环里的说明）。 */
static int g_applied_theme_light = -1;

static void gui_apply_theme(int light) {
    const gui_theme_t *want = light ? ui_theme_builtin_light()
                                    : ui_theme_builtin_dark();
    if (ui_theme_set(want->name) == 0)
        g_applied_theme_light = light;
}

typedef struct {
    const char *name;
    const char *description;
    int mode;
    /* 建议窗口尺寸（0 = 用框架默认值 790x430）。
     * 此前应用无法表达"我需要更高的窗口"，只能忍受默认尺寸导致内容被
     * 裁掉（UI 组件页就踩到了）。让它随注册一起声明，避免在别处再开一张
     * 尺寸表——那正是旧设计里"三处清单不同步"的成因。 */
    int want_w, want_h;
} gui_app_meta_t;

typedef struct {
    const char *label;
    int action;
} gui_file_action_t;

/* 应用列表：**直接由模块注册表派生**，不再手工维护第二份清单。
 *
 * 重构前这里是独立的一张表，新增应用必须同步改它，漏改就不出现在应用
 * 列表里。现在自动跟随 g_modules[]，只保留"排序偏好"这种真正属于视图的
 * 信息。gui_app_meta_t 保留是为了兼容既有调用点（selected_gui_app 等）。 */
static uint32_t gui_app_count(void);

static const gui_app_meta_t *gui_app_meta_at(uint32_t i) {
    static gui_app_meta_t m;
    const gui_app_module_t *mod = gui_app_module_at((int)i);
    if (!mod) return 0;
    m.name = mod->name;
    m.description = mod->desc;
    m.mode = mod->mode;
    m.want_w = mod->want_w;
    m.want_h = mod->want_h;
    return &m;
}

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
    return (uint32_t)gui_app_module_count();
}

static file_t *selected_file(gui_state_t *st);
void gui_select_file(gui_state_t *st, int index);
static int gui_select_path(gui_state_t *st, const char *path);
static void draw_desktop(int w, int h, gui_state_t *st);
static void draw_start_menu(gui_state_t *st);
static void draw_context_menu(gui_state_t *st);
static void draw_calendar_popup(int w, int h, gui_state_t *st);
static void draw_window_switcher(int w, int h, gui_state_t *st);
void gui_sync_focus(gui_state_t *st);

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

void gui_window_metrics(gui_state_t *st, int w, int h, const wm_window_t *win, int idx,
                               int *win_x, int *win_y, int *win_w, int *win_h) {
    wm_get_window_rect(&st->wm, idx, win_x, win_y, win_w, win_h);
    (void)w; (void)h; (void)win;
}

void gui_sync_focus(gui_state_t *st) {
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
        /* 套用注册表里声明的建议尺寸：应用自己知道内容需要多大，比在
         * 框架里维护一张尺寸表更不容易漏配（新增应用自动带上）。 */
        if (kind == WM_WIN_APP) {
            for (uint32_t i = 0; i < gui_app_count(); i++) {
                if (gui_app_meta_at(i)->mode == mode &&
                    (gui_app_meta_at(i)->want_w > 0 || gui_app_meta_at(i)->want_h > 0)) {
                    wm_window_t *w = wm_get_window(&st->wm, idx);
                    if (w) {
                        wm_resize_window(&st->wm, idx,
                                         gui_app_meta_at(i)->want_w > 0 ? gui_app_meta_at(i)->want_w : w->w,
                                         gui_app_meta_at(i)->want_h > 0 ? gui_app_meta_at(i)->want_h : w->h);
                    }
                    break;
                }
            }
        }
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

/* 应用/面板块 → 图标 id。
 * 依赖 GUI_APP_* 与 ICON_* 常量，属于"应用元数据"而非纯渲染，
 * 因此留在 gui.c（渲染层不该认识应用编号）。 */
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

/* 斜体渲染开关随渲染层迁走；gui.c 的浏览器路径需要读写它 */
int *gd_text_italic_ptr(void);
#define g_text_italic (*gd_text_italic_ptr())

/* 这两个仍是 gui.c 本地实现（依赖 ui_s 与 gui_state_t），但被前面的
 * 帧绘制流程提前调用，故需前向声明。 */
static void draw_splash_window(int w, int h, int ticks, int light);
static void draw_toast(int w, int h, gui_state_t *st);

/* ── 渲染层转发 ─────────────────────────────────────────────
 * 实现已移到 src/gui/gui_draw_impl.c。gui.c 与浏览器引擎有 300+ 处使用
 * 这些短名（rect/text/border/fill_round_rect…）。这里保留同名 static
 * inline 转发，因此调用点零改动——机械改 300 处的风险远大于一层 inline。
 * 全部 static，不导出符号。 */

static inline void rect(int x,int y,int w,int h,uint32_t c){ gd_rect(x,y,w,h,c); }
static inline void rect_alpha(int x,int y,int w,int h,uint32_t c){ gd_rect_alpha(x,y,w,h,c); }
static inline void border(int x,int y,int w,int h,uint32_t c){ gd_border(x,y,w,h,c); }
static inline void vgradient(int x,int y,int w,int h,uint32_t t,uint32_t b){ gd_vgradient(x,y,w,h,t,b); }
static inline void hgradient(int x,int y,int w,int h,uint32_t l,uint32_t r){ gd_hgradient(x,y,w,h,l,r); }
static inline void soft_shadow(int x,int y,int w,int h){ gd_soft_shadow(x,y,w,h); }
static inline void fill_round_rect(int x,int y,int w,int h,int r,int c,uint32_t col){ gd_fill_round_rect(x,y,w,h,r,c,col); }
static inline void draw_panel_shell(int x,int y,int w,int h,uint32_t t,uint32_t b,uint32_t bc,uint32_t a){ gd_draw_panel_shell(x,y,w,h,t,b,bc,a); }
static inline void draw_inset_shell(int x,int y,int w,int h,uint32_t bc){ gd_draw_inset_shell(x,y,w,h,bc); }
static inline void blit_glyph(int x,int y,const gui_glyph_t *g,uint32_t c,int s){ gd_blit_glyph(x,y,g,c,s); }
static inline void blit_mono_glyph(int x,int y,const uint8_t *g,uint32_t c,int s){ gd_blit_mono_glyph(x,y,g,c,s); }
static inline int  text_width(const char *s,int sc){ return gd_text_width(s,sc); }
static inline void text(int x,int y,const char *s,uint32_t c,int sc){ gd_text(x,y,s,c,sc); }
static inline void text_clipped(int x,int y,int mx,const char *s,uint32_t c,int sc){ gd_text_clipped(x,y,mx,s,c,sc); }
static inline uint32_t rgb_mix(uint32_t a,uint32_t b,int t){ return gd_rgb_mix(a,b,t); }
static inline uint32_t rgb_lift(uint32_t c,int d){ return gd_rgb_lift(c,d); }
static inline uint32_t apply_layer_opacity(uint32_t c){ return gd_apply_layer_opacity(c); }
static inline void append_char(char *b,uint32_t cap,uint32_t *p,char c){ gd_append_char(b,cap,p,c); }
static inline void append_str(char *b,uint32_t cap,uint32_t *p,const char *s){ gd_append_str(b,cap,p,s); }
static inline void append_uint(char *b,uint32_t cap,uint32_t *p,uint32_t v){ gd_append_uint(b,cap,p,v); }
static inline void append_int(char *b,uint32_t cap,uint32_t *p,int v){ gd_append_int(b,cap,p,v); }
static inline void append_ll(char *b,uint32_t cap,uint32_t *p,long long v){ gd_append_ll(b,cap,p,v); }
static inline void append_sci(char *b,uint32_t cap,uint32_t *p,long long m,int e){ gd_append_sci(b,cap,p,m,e); }
static inline void blit_icon(int x,int y,int sz,int id){ gd_blit_icon(x,y,sz,id); }
static inline void blit_icon_rgba(int x,int y,int sz,const uint32_t *s,int t){ gd_blit_icon_rgba(x,y,sz,s,t); }
static inline int  draw_text_codepoint(int x,int y,uint32_t cp,uint32_t c,int sc){ return gd_draw_text_codepoint(x,y,cp,c,sc); }
static inline int  text_mono(int x,int y,int mx,const char *s,uint32_t c){ return gd_text_mono(x,y,mx,s,c); }
static inline int  draw_mono_char(int x,int y,char c,uint32_t col){ return gd_draw_mono_char(x,y,c,col); }
static inline void round_corner(int bx,int by,int r,int cx,int cy,uint32_t c){ gd_round_corner(bx,by,r,cx,cy,c); }
static inline void vgradient_alpha(int x,int y,int w,int h,uint32_t t,uint32_t b){ gd_vgradient_alpha(x,y,w,h,t,b); }
static inline void draw_window_control_icon(int x,int y,int k,int r,uint32_t c){ gd_draw_window_control_icon(x,y,k,r,c); }

const char *gui_file_path(gui_state_t *st) {
    if (!st->file_path[0]) {
        st->file_path[0] = '/';
        st->file_path[1] = 0;
    }
    return st->file_path;
}

void gui_set_file_path(gui_state_t *st, const char *path) {
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

uint32_t gui_file_count(gui_state_t *st) {
    char name[VFS_MAX_NAME];
    uint32_t type;
    uint32_t count = 0;
    const char *path = gui_file_path(st);
    while (vfs_readdir_at(path, count, name, &type) == 0) count++;
    return count;
}

int gui_file_entry(gui_state_t *st, uint32_t index, char *name,
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

int gui_selected_entry(gui_state_t *st, char *name, uint32_t *type,
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

file_t *gui_selected_regular_file(gui_state_t *st) {
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

void gui_set_note_name(gui_state_t *st, const char *name) {
    uint32_t i = 0;
    while (name && name[i] && i + 1 < sizeof(st->note_name)) {
        st->note_name[i] = name[i];
        i++;
    }
    st->note_name[i] = 0;
    st->note_loaded = 0;
    st->note_sel_active = 0;
}

const char *gui_note_name(gui_state_t *st) {
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
    text(x + 13, y + 10, label, UI_FG, 1);
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
    text_clipped(tx, ty, x + w - 4, label, UI_FG, 1);
}

static uint32_t gui_file_action_color(int action) {
    // Breeze button semantics: primary action = accent blue, destructive = red,
    // everything else = neutral surface so the toolbar reads calm and flat.
    if (action == 1) return UI_ACCENT;   // 打开 — primary
    if (action == 3) return UI_ERR;     // 清空 — destructive
    if (action == 4) return UI_ERR;     // 删除 — destructive
    return UI_SURFACE3;                            // neutral surface（原硬编码，改用令牌）
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

/* 手型链接光标（食指伸出的简化手掌轮廓），悬停在浏览器链接上时用——
 * 和文本工字光标一样走 CURSOR_HOVER_* 伪 edge 值。 */
#define CURSOR_HOVER_LINK -51
#define CURSOR_HAND_W 15
#define CURSOR_HAND_H 18
static const int CURSOR_HAND_VX[10] = { 5, 8, 8, 13, 14, 12, 4, 2, 2, 5 };
static const int CURSOR_HAND_VY[10] = { 0, 0, 7,  8, 13, 17, 17, 12, 9, 9 };

static void draw_polygon_cursor(int x, int y, const int *vx, const int *vy, int n, int bw, int bh) {
    uint32_t c = UI_DIM;
    uint32_t d = UI_SURFACE;
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
    case CURSOR_HOVER_LINK:
        draw_polygon_cursor(x, y, CURSOR_HAND_VX, CURSOR_HAND_VY, 10,
                             CURSOR_HAND_W, CURSOR_HAND_H);
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

/* ── HAX GUI 应用画布接口（供内核 syscall 层调用） ───────────────────
 * 用户态 .hax 应用通过 int 0x80 调用这些函数，直接绘制到 GUI 的离屏表面
 * （gd_surface_ptr()），再 present 到真实帧缓冲。当 GUI 未运行时 gd_surface_ptr()
 * 为 NULL，gui_app_info 返回 0，应用据此可回退到文本模式。
 *
 * 注意：应用运行期间 GUI 主循环阻塞在 task_wait（见 console_exec_cmd），
 * 故应用在此期间“独占”整屏画布；退出后 GUI 重新接管并重绘。 */
static int     g_guiapp_mx = 0, g_guiapp_my = 0;
static uint8_t g_guiapp_btn = 0;

/* 全屏画布应用的"屏幕所有者"任务 id：非 0 时合成器让位（不绘制、不读输入），
 * 由该全屏应用独占 gd_surface_ptr() 并自行 present；应用退出后由主循环清零。 */
uint32_t g_screen_owner_task = 0;

int gui_app_info(int *w, int *h) {
    if (!gd_surface_ptr()) return 0;
    if (w) *w = gd_surface_w();
    if (h) *h = gd_surface_h();
    /* 首次查询时把鼠标置于画布中心，并登记为屏幕所有者 */
    if (g_guiapp_mx == 0 && g_guiapp_my == 0) {
        g_guiapp_mx = gd_surface_w() / 2;
        g_guiapp_my = gd_surface_h() / 2;
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
    if (!gd_surface_ptr()) return;
    rect(0, 0, gd_surface_w(), gd_surface_h(), color);
}

void gui_app_rect(int x, int y, int w, int h, uint32_t color) {
    if (!gd_surface_ptr()) return;
    rect(x, y, w, h, color);
}

void gui_app_text(int x, int y, const char *s, uint32_t color, int scale) {
    if (!gd_surface_ptr() || !s) return;
    if (scale < 1) scale = 1;
    text(x, y, s, color, scale);
}

// 全屏画布应用独占 gd_surface_ptr() 并自行 present（见上方注释），合成器的正常
// 光标叠加不会运行；若直接把光标画进 gd_surface_ptr()，会被应用当成画布内容
// 永久保留并形成光标笔迹。因此改为在 blit 到真实
// framebuffer 之后，直接在硬件帧缓冲上叠一份光标像素——不进入应用的画布。
static void present_screen_owner_cursor(const fb_info_t *fb, int x, int y) {
    uint32_t fb_pitch = (uint32_t)(fb->pitch / 4);
    int fbw = (int)fb->width, fbh = (int)fb->height;
    uint32_t c = UI_DIM;
    uint32_t d = UI_SURFACE;
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
    if (!gd_surface_ptr()) return;
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
    if (gd_surface_ptr()) {
        if (g_guiapp_mx > gd_surface_w() - 1) g_guiapp_mx = gd_surface_w() - 1;
        if (g_guiapp_my > gd_surface_h() - 1) g_guiapp_my = gd_surface_h() - 1;
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
    if (!src || !gd_surface_ptr() || sw <= 0 || sh <= 0) {
        rect(0, 0, DW, DH, UI_SURFACE);
        return;
    }
    int cx = 0, cy = 0, cw = DW, ch = DH;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx >= gd_surface_w() || cy >= gd_surface_h()) return;
    if (cx + cw > gd_surface_w()) cw = gd_surface_w() - cx;
    if (cy + ch > gd_surface_h()) ch = gd_surface_h() - cy;
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
        uint32_t *drow = gd_surface_ptr() +
                         (uint32_t)dy * gd_surface_pitch() + (uint32_t)cx;
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
    rect_alpha(0, tb_y, w, TASKBAR_H, (0xF0u << 24) | (light ? 0xE9EBEEu : 0x20242Au));
    rect(0, tb_y, w, 1, UI_SURFACE3);
}

static void draw_usage_bar(int x, int y, int w, int h, uint32_t used, uint32_t total, uint32_t color) {
    uint32_t filled = total ? (used * (uint32_t)w) / total : 0;
    if (filled > (uint32_t)w) filled = (uint32_t)w;
    draw_inset_shell(x, y, w, h, UI_SURFACE3);
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
    /* 盘晚就绪/启动时未检测到时，打开文件面板顺手重试探测+挂载 */
    (void)block_init();
    (void)fs_retry_mount();
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
    text(tx, ty, "文件管理器", UI_OK, 1);
    line_u32(line, sizeof(line), "当前: ", count, " 项");
    text(tx + 112, ty + 2, line, UI_FG2, 1);
    vgradient(tx, ty + 30, content_w, 30, UI_SURFACE2, UI_SURFACE);
    rect(tx, ty + 30, content_w, 1, UI_ACCENT);
    rect(tx, ty + 59, content_w, 1, UI_BG);
    border(tx, ty + 30, content_w, 30, UI_ACCENT);
    line2(line, sizeof(line), "位置 ", gui_file_path(mst));
    text_clipped(tx + 12, ty + 42, tx + content_w - 12, line, UI_FG, 1);

    draw_file_action_bar(tx, ty, content_w);

    vgradient(tx, ty + 114, side_w, L.box_h, UI_SURFACE, UI_BG);
    border(tx, ty + 114, side_w, L.box_h, UI_ACCENT);
    rect(tx, ty + 114, side_w, 1, UI_ACCENT);
    text(tx + 14, ty + 128, "位置", UI_FG, 1);
    vgradient(tx + 10, ty + 154, side_w - 20, 28, UI_ACCENT, UI_SURFACE2);
    rect(tx + 10, ty + 154, 3, 28, UI_OK);
    text_clipped(tx + 22, ty + 164, tx + side_w - 10, gui_file_path(mst), UI_FG, 1);
    text(tx + 22, ty + 202, "/home", UI_DIM, 1);
    text(tx + 22, ty + 232, "/dev  /proc", UI_DIM, 1);
    text(tx + 14, ty + 286, "Enter进入  Back上级", UI_DIM, 1);

    vgradient(main_x, ty + 114, main_w, L.box_h, UI_SURFACE, UI_SURFACE);
    border(main_x, ty + 114, main_w, L.box_h, UI_ACCENT);
    vgradient(main_x, ty + 114, main_w, 26, UI_SURFACE3, UI_SURFACE);
    rect(main_x, ty + 139, main_w, 1, UI_ACCENT);
    text(main_x + 14, ty + 122, "名称", UI_FG, 1);
    text(main_x + main_w - 166, ty + 122, "类型", UI_FG, 1);
    text(main_x + main_w - 82, ty + 122, "大小", UI_FG, 1);

    int list_y = ty + 146;
    if (count == 0) {
        text(main_x + 18, list_y + 18, "目录为空", UI_FG, 1);
        text(main_x + 18, list_y + 42, "点击新建或按 N 创建", UI_FG2, 1);
        vgradient(detail_x, ty + 114, detail_w, L.box_h, UI_SURFACE, UI_BG);
        border(detail_x, ty + 114, detail_w, L.box_h, UI_ACCENT);
        text(detail_x + 12, ty + 130, "详细信息", UI_FG, 1);
        text(detail_x + 12, ty + 164, "未选择文件", UI_FG2, 1);
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
        if ((i & 1) == 1) rect(main_x + 1, y - 8, main_w - 2, FILE_ROW_H, UI_SURFACE2);
        if ((int)file_idx == selected) {
            rect(main_x + 6, y - 7, main_w - 12, 24, UI_ACCENT);
        }
        if (type == VFS_NODE_DIR) {
            blit_icon(main_x + 14, y - 8, 20, ICON_FOLDER_ROW);
        } else {
            uint32_t icon = type == VFS_NODE_CHARDEV ? UI_ACCENT : UI_OK;
            rect(main_x + 16, y - 2, 18, 14, icon);
            rect(main_x + 20, y - 7, 18, 7, rgb_lift(icon, -16));
        }
        uint32_t pos = 0;
        line[0] = 0;
        append_str(line, sizeof(line), &pos, name);
        uint32_t row_fg = (int)file_idx == selected ? UI_FG : UI_FG;
        uint32_t meta_fg = (int)file_idx == selected ? UI_FG : UI_FG2;
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
        vgradient(detail_x, ty + 114, detail_w, L.box_h, UI_SURFACE, UI_BG);
        border(detail_x, ty + 114, detail_w, L.box_h, UI_ACCENT);
        vgradient(detail_x, ty + 114, detail_w, 30, UI_SURFACE2, UI_SURFACE);
        rect(detail_x, ty + 143, detail_w, 1, UI_BG);
        text(detail_x + 12, ty + 124, "详细信息", UI_FG, 1);
        if (type == VFS_NODE_DIR) {
            blit_icon(detail_x + 10, ty + 148, 32, ICON_FOLDER_ROW);
        } else {
            uint32_t icon = type == VFS_NODE_CHARDEV ? UI_ACCENT : UI_OK;
            rect(detail_x + 12, ty + 156, 30, 24, icon);
            rect(detail_x + 17, ty + 150, 28, 8, rgb_lift(icon, -16));
        }
        text_clipped(detail_x + 52, ty + 160, detail_x + detail_w - 12, name, UI_FG, 1);
        line_u32(line, sizeof(line), "大小: ", node ? node->size : 0, "B");
        text(detail_x + 12, ty + 194, line, UI_FG, 1);
        line2(line, sizeof(line), "类型: ", gui_node_type_label(type));
        text(detail_x + 12, ty + 216, line, UI_FG, 1);
        text(detail_x + 12, ty + 246, "预览", UI_FG, 1);
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
            px += draw_text_codepoint(px, py, cp, UI_FG, 1);
            if (px > detail_x + detail_w - 20) {
                px = detail_x + 12;
                py += 18;
            }
        }

        int sy = ty + L.status_y;
        vgradient(tx, sy - 10, content_w, 28, UI_SURFACE, UI_SURFACE);
        rect(tx, sy - 10, content_w, 1, UI_ACCENT);
        rect(tx, sy + 17, content_w, 1, UI_BG);
        border(tx, sy - 10, content_w, 28, UI_ACCENT);
        if (st->rename_active) {
            int input_x = tx + 72;
            int hint_w = 132;
            int input_w = content_w - 84 - hint_w;
            if (input_w < 132) {
                hint_w = 0;
                input_w = content_w - 84;
            }
            if (input_w < 80) input_w = 80;
            text(tx + 12, sy, "新名称", UI_FG, 1);
            draw_inset_shell(input_x, sy - 5, input_w, 18, UI_BG);
            text_clipped(input_x + 6, sy, input_x + input_w - 8,
                         st->rename_buf, UI_FG, 1);
            int cursor_x = input_x + 6 + (int)st->rename_len * 6;
            if (cursor_x > input_x + input_w - 8) cursor_x = input_x + input_w - 8;
            rect(cursor_x, sy - 1, 2, 12, UI_OK);
            if (hint_w > 0)
                text(input_x + input_w + 10, sy, "Enter确认 Esc取消",
                     UI_FG2, 1);
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
            text_clipped(tx + 12, sy, tx + content_w - 12, line, UI_FG, 1);
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
        text(main_x + main_w - 96, ty + L.status_y, line, UI_FG2, 1);
    }
}

static void draw_disk_panel(int tx, int ty, int win_w) {
    char line[96];
    /* 磁盘面板自愈：重扫块设备并尝试重挂载（不重置 ramfs） */
    (void)block_init();
    (void)fs_retry_mount();
    text(tx, ty, "磁盘管理器", UI_WARN, 1);
    line2(line, sizeof(line), "块设备: ", block_backend_name());
    text(tx, ty + 38, line, UI_FG, 1);
    line_u32(line, sizeof(line), "扇区数: ", block_sector_count(), "");
    text(tx, ty + 60, line, UI_FG, 1);
    line2(line, sizeof(line), "文件系统: ", fs_backend_name());
    text(tx, ty + 82, line, UI_FG, 1);
    line_u32(line, sizeof(line), "起始 LBA: ", fs_disk_start_lba(), "");
    text(tx, ty + 104, line, UI_FG, 1);

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
    text(tx, ty + 132, line, UI_FG, 1);
    draw_usage_bar(tx, ty + 154, win_w - 60, 18, used, cap, UI_WARN);
    draw_button(tx, ty + 194, "安装 I", UI_WARN);
    text(tx + 120, ty + 204, fs_is_disk() ? "HBFS 持久化已就绪" : "当前使用 RAMFS", UI_ACCENT, 1);
}

static void draw_resource_panel(int tx, int ty, int win_w, int w, int h) {
    char line[96];
    uint64_t total = pmm_get_total_mem();
    uint64_t free = pmm_get_free_mem();
    uint64_t used = total > free ? total - free : 0;
    text(tx, ty, "资源管理器", UI_ACCENT, 1);
    line_u32(line, sizeof(line), "内存已用: ", (uint32_t)(used / 1024), "K");
    text(tx, ty + 38, line, UI_FG, 1);
    draw_usage_bar(tx, ty + 60, win_w - 60, 16, (uint32_t)(used / 1024), (uint32_t)(total / 1024), UI_ACCENT);
    line_u32(line, sizeof(line), "内存可用: ", (uint32_t)(free / 1024), "K");
    text(tx, ty + 86, line, UI_FG, 1);
    line_u32(line, sizeof(line), "任务数: ", (uint32_t)task_get_count(), "");
    text(tx, ty + 110, line, UI_FG, 1);
    line_u32(line, sizeof(line), "应用数: ", hbos_app_count(), "");
    text(tx, ty + 132, line, UI_FG, 1);
    line_u32(line, sizeof(line), "文件数: ", fs_get_count(), "");
    text(tx, ty + 154, line, UI_FG, 1);
    line2(line, sizeof(line), "文件系统: ", fs_backend_name());
    text(tx, ty + 176, line, UI_FG, 1);
    line_u32(line, sizeof(line), "显示: ", (uint32_t)w, "X");
    uint32_t pos = 0;
    while (line[pos]) pos++;
    append_uint(line, sizeof(line), &pos, (uint32_t)h);
    text(tx, ty + 198, line, UI_FG, 1);
    line2(line, sizeof(line), "鼠标: ", mouse_backend_name());
    text(tx, ty + 220, line, UI_FG, 1);
    line_u32(line, sizeof(line), "USB HID: ", (uint32_t)hid_device_count(), "");
    text(tx, ty + 242, line, UI_FG, 1);

    text(tx, ty + 270, "活动任务", UI_ACCENT, 1);
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
        text(tx + 18, ty + 292 + (int)i * 20, line, UI_FG, 1);
    }
}

static uint32_t app_accent(int mode) {
    switch (mode) {
        case GUI_APP_NOTES:   return UI_OK;
        case GUI_APP_CALC:    return UI_ACCENT;
        case GUI_APP_UWC:     return UI_ACCENT;
        case GUI_APP_SNAKE:   return UI_OK;
        case GUI_APP_BROWSER: return UI_ACCENT;
        case GUI_APP_CODE:    return UI_ACCENT;
        case GUI_APP_DIAG:    return UI_WARN;
        case GUI_APP_CLOCK:    return UI_ERR;
        case GUI_APP_SETTINGS: return UI_FG2;
        case GUI_APP_FILES:    return UI_ACCENT;
        case GUI_APP_SHORTCUTS: return UI_OK;
        default:               return UI_ACCENT;
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
    text(tx, ty, "应用程序", UI_FG, 1);
    line_u32(line, sizeof(line), "共 ", gui_app_count(), " 个应用");
    text(tx + 96, ty + 2, line, UI_FG2, 1);

    int cw = win_w - 60;  // content width (panel is inset 30px each side)

    int selected = st->selected_app;
    if (selected < 0) selected = 0;
    if ((uint32_t)selected >= gui_app_count()) selected = (int)gui_app_count() - 1;

    for (uint32_t i = 0; i < gui_app_count(); i++) {
        const gui_app_meta_t *app = gui_app_meta_at(i);
        int x, y, w, h;
        gui_app_tile_rect(tx, ty, cw, i, &x, &y, &w, &h);
        uint32_t accent = app_accent(app->mode);
        int sel = ((int)i == selected);

        // Translucent glass tile; selected gets an accent wash + ring.
        fill_round_rect(x, y, w, h, UI_RADIUS, ((sel ? 0x99u : 0x66u) << 24) | (UI_SURFACE3 & 0xFFFFFFu), RR_ALL);
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
             sel ? UI_FG : UI_FG, 1);
    }
    uint32_t cols = (uint32_t)gui_app_grid_cols(cw);
    uint32_t rows = (gui_app_count() + cols - 1) / cols;
    text(tx, ty + 64 + (int)rows * 126 + 6,
         "单击图标启动 · 方向键选择 · Enter 打开",
         UI_FG, 1);
}




static char gui_ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* 大小写不敏感——FAT32 短文件名（8.3 格式）落盘/读回都是全大写
 * （TEST.BMP），只按小写 ".bmp" 精确匹配的话，FAT32 盘上的文件后缀检测
 * 会全部失效，退回到内容嗅探甚至记事本，而不是真正按后缀分发。 */
int gui_has_suffix(const char *path, const char *suffix) {
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

#include "../png.h"
#include "../jpeg.h"
#include "../bmp.h"
void gui_blit_rgb888(int x, int y, const uint8_t *rgbdata, int img_w, int img_h,
                     int clip_x, int clip_y, int clip_w, int clip_h);

/* ── 浏览器内联图片 ──
 * <img> 现在真的抓取并渲染（PNG/BMP），不再只画 [图片] 占位。固定 4 个槽，
 * 每个一块静态 RGB 缓冲，避免换页时反复分配并减少堆碎片。抓取/解码在
 * 页面加载后统一做（browser_fetch_images），渲染流里 BRK_IMG 块带一个
 * 槽位字节，绘制时命中就贴图、否则退回占位文字。*/
/* 每槽 260×200×3 ≈ 152KB；8 槽 ≈ 1.2MB，比原来 5×560×360×3≈2.9MB 省一半以上，
 * 同时可以展示 4 行 ×2 列 = 8 张视频封面。URL 改写为 240w_180h，解码后
 * 尺寸不超过 260×200 的上限。 */
#define BR_IMG_SLOTS 8
#define BR_IMG_MAX_W 260
#define BR_IMG_MAX_H 200
#define BR_IMG_FETCH_MAX 6 /* 每页最多实际拉 6 张；其余保留占位，避免串行 TLS/软件解码拖住 UI */
static uint8_t g_br_img_rgb[BR_IMG_SLOTS][BR_IMG_MAX_W * BR_IMG_MAX_H * 3];
static int g_br_img_w[BR_IMG_SLOTS];
static int g_br_img_h[BR_IMG_SLOTS];
static char g_br_img_src[BR_IMG_SLOTS][256]; /* bilibili CDN URL 最长约140字符，留128余量 */
static int g_br_img_count;

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

/* ── HTML/CSS 解析层转发（实现见 src/gui/gui_html.c）────────────
 * 与渲染层同样处理：保留同名 static inline 短名，既有调用点零改动。 */
static inline char br_decode_entity(const char *p, uint32_t *c){ return gh_br_decode_entity(p,c); }
static inline void browser_text_from_html(const char *h,char *o,uint32_t cap,uint32_t *l){ gh_browser_text_from_html(h,o,cap,l); }
static inline int  br_is_inline_type(int t){ return gh_br_is_inline_type(t); }
static inline int  tag_ci_eq(const char *n,int l,const char *lit){ return gh_tag_ci_eq(n,l,lit); }
static inline int  css_simple_selector_matches(const char *s,const char *t,int tl,const char *c,const char *i){ return gh_css_simple_selector_matches(s,t,tl,c,i); }
static inline void browser_style_get(int t, browser_style_t *s){ gh_browser_style_get(t,s); }
static inline int  browser_style_for_tag(const char *n,int l){ return gh_browser_style_for_tag(n,l); }
static inline int br_attr_value(const char *t,uint32_t tl,const char *n,char *o,uint32_t oc){ return gh_br_attr_value(t,tl,n,o,oc); }
static inline int  css_parse_color(const char *s,uint32_t *o){ return gh_css_parse_color(s,o); }
static inline void css_parse_decls(css_decl_t *d,const char *t,uint32_t l){ gh_css_parse_decls(d,t,l); }
static inline void css_parse_stylesheet(const char *c,uint32_t l){ gh_css_parse_stylesheet(c,l); }
static inline void css_resolve_for_tag(const char *t,int tl,const char *cl,const char *id,const char *is,css_decl_t *o){ gh_css_resolve_for_tag(t,tl,cl,id,is,o); }

/* 把当前活跃的 link 下标 + CSS 覆盖编码成 BR_META_MARK 扩展前缀（没有
 * 覆盖也没有链接时不写这几个字节，保持零开销）。 */
static void br_emit_meta(char *out, uint32_t cap, uint32_t *pos, int link_idx, const css_decl_t *ov) {
    int has_meta = (link_idx >= 0) || (ov && (ov->has_color || ov->has_bold || ov->has_italic ||
                                              ov->has_underline || ov->has_scale || ov->has_bg ||
                                              ov->has_align || ov->has_gap || ov->has_indent ||
                                              ov->has_border));
    if (!has_meta) return;
    if (*pos + BR_META_LEN >= cap) return;
    uint8_t flags = 0, flags2 = 0;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    if (ov) {
        if (ov->has_color) { flags |= BR_FLAG_COLOR; r = (uint8_t)(ov->color >> 16); g = (uint8_t)(ov->color >> 8); b = (uint8_t)ov->color; }
        if (ov->has_bold && ov->bold) flags |= BR_FLAG_BOLD_ON;
        if (ov->has_italic && ov->italic) flags2 |= BR_FLAG2_ITALIC_ON;
        if (ov->has_indent) flags2 |= (uint8_t)((ov->indent_lvl & 3) << BR_FLAG2_INDENT_SHIFT);
        if (ov->has_border && ov->border) flags2 |= BR_FLAG2_BORDER;
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

/* OpenGraph meta 标签：bilibili 视频页几乎是纯 CSR，<body> 正文很少；
 * 但 <head> 里有完整的 og:title/og:description/og:image——把它们提取出来
 * 作为页面首个区块输出，视频页就不再是空白了。 */
#define BR_OG_CAP 256
static char g_br_og_title[BR_OG_CAP];
static char g_br_og_desc[BR_OG_CAP];
static char g_br_og_image[BR_OG_CAP]; /* URL，交给 browser_fetch_images 的槽 0 */
static int  g_br_og_has_image;        /* og:image 是否已登记进图片槽 */

/* 外链样式表：<link rel="stylesheet"> 同步抓取并喂给 css_parse_stylesheet。
 * 每页最多 2 张、单张 ≤64KB——真实站点样式表动辄几百 KB，且大部分是
 * 这个渲染器表达不了的布局规则，抓前两张把颜色/字重这类能用的捞出来
 * 就是收益的大头。 */
static void browser_fetch_stylesheet(gui_state_t *st, const char *href);
static int g_br_css_fetches;
static int g_br_script_fetches;   /* 外链 <script src> 抓取计数（每页上限 10） */
static char g_br_current_url[BROWSER_URL_CAP];   /* 当前页 URL（传给 js -u） */

/* 大小写不敏感子串查找：HTML 标签名/属性可能任意大小写（<SCRIPT>、
 * <ScRiPt src=...>），解析器现有 tag_ci_eq 只比较已提取的标签名，这里
 * 需要在整个 body 上直接找 "<script" / "</script>" / "src"。 */
static const char *body_strcasestr(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return NULL;
    size_t nl = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && (p[i] | 0x20) == (needle[i] | 0x20)) i++;
        if (i == nl) return p;
    }
    return NULL;
}

static const char *browser_skipped_tag_close(const char *name, int len) {
    static const char *const tags[] = {
        "svg", "noscript", "template", "select", "iframe", "canvas", "video", "audio"
    };
    for (uint32_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (tag_ci_eq(name, len, tags[i])) return tags[i];
    }
    return 0;
}

/* 子资源（样式表/图片）抓取预算：页面加载是同步的，受限网络里每个打不通
 * 的 CDN 域名要吃满 DNS+TCP 超时（各约 2 秒），6 个子资源就能把 GUI 冻住
 * 30 秒——用户实测百度首页正是这个数。对策：同一次加载内 DNS 结果缓存
 * （失败也记，负缓存让同域名后续资源直接跳过），累计失败 2 次就放弃余下
 * 全部子资源。 */
#define BR_SUB_HOSTS 8
static char g_br_dns_host[BR_SUB_HOSTS][96];
static uint32_t g_br_dns_ip[BR_SUB_HOSTS];
static int g_br_dns_n;
static int g_br_sub_fails;
static uint64_t g_br_sub_deadline;

static int br_sub_budget_expired(void) {
    return (int64_t)(pit_get_ticks() - g_br_sub_deadline) >= 0;
}

static int br_sub_resolve(const char *host, uint32_t *ip, int *cached) {
    if (cached) *cached = 0;
    for (int i = 0; i < g_br_dns_n; i++) {
        if (strcmp(g_br_dns_host[i], host) == 0) {
            if (cached) *cached = 1;         /* 命中缓存（含负缓存） */
            *ip = g_br_dns_ip[i];
            return g_br_dns_ip[i] ? 0 : -1;
        }
    }
    int ok = net_dns_resolve(host, ip);
    if (g_br_dns_n < BR_SUB_HOSTS) {
        uint32_t hl = (uint32_t)strlen(host);
        if (hl >= sizeof(g_br_dns_host[0])) hl = sizeof(g_br_dns_host[0]) - 1;
        memcpy(g_br_dns_host[g_br_dns_n], host, hl);
        g_br_dns_host[g_br_dns_n][hl] = 0;
        g_br_dns_ip[g_br_dns_n] = (ok == 0) ? *ip : 0;
        g_br_dns_n++;
    }
    return ok;
}

/* 图床变换后缀改写（定义于下方 browser_fetch_images 前）。在图片槽
 * 注册阶段就调用它：把 bilibili 的 `@...webp` 封面改写成 `@240w_180h`，
 * 否则注册阶段的 .webp 过滤会把封面直接丢掉。 */
static void br_img_url_rewrite(char *url, uint32_t cap);

/* ── LiteJS 卡片：<hb-card> 原始内部 HTML → BRK_CARD 块 ──────────────
 * 载荷格式：[0x10|slot][标题][0x04 元数据][0x05 时长]\n；链接走标准
 * BR_META（link_idx），图片复用 g_br_img_* 槽位。发射端在
 * browser_render_from_html 里捕获 <hb-card> 内部原始 HTML 后调用这里。 */

/* 剥标签 + 解实体 + 折叠空白，得到卡片标题/纯文本 */
static void br_card_plain_text(const char *html, uint32_t len, char *out, uint32_t cap) {
    uint32_t o = 0;
    int sp = 1;
    for (uint32_t i = 0; i < len && o + 2 < cap; i++) {
        char ch = html[i];
        if (ch == '<') {
            while (i < len && html[i] != '>') i++;
            continue;
        }
        if (ch == '&') {
            uint32_t consumed;
            char dec = br_decode_entity(html + i, &consumed);
            if (dec) { ch = dec; i += consumed; }
        }
        if (ch == '\n' || ch == '\t' || ch == ' ') {
            if (sp) continue;
            sp = 1;
        } else {
            sp = 0;
        }
        if ((unsigned char)ch >= 0x20) out[o++] = ch;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* 提取卡片内第一张 <img> 的 src（data-src/data-original/src 优先序），
 * 登记进图片槽位；不可解码格式/无源返回 0（占位）。 */
static int br_card_img_slot(const char *inner, uint32_t len) {
    const char *im = body_strcasestr(inner, "<img");
    if (!im || (uint32_t)(im - inner) >= len) return 0;
    uint32_t start = (uint32_t)(im - inner);
    uint32_t j = start;
    while (j < len && inner[j] != '>') j++;
    if (j >= len) return 0;
    char src[160];
    int has_src = br_attr_value(inner + start, j - start, "data-src", src, sizeof(src)) ||
                  br_attr_value(inner + start, j - start, "data-original", src, sizeof(src)) ||
                  br_attr_value(inner + start, j - start, "src", src, sizeof(src));
    if (!has_src || !src[0]) return 0;
    /* bilibili 封面是 `@...webp`，先改写成 jpg 缩放参数，避免被过滤 */
    br_img_url_rewrite(src, sizeof(src));
    uint32_t sl0 = (uint32_t)strlen(src);
    if ((sl0 >= 4 && (tag_ci_eq(src + sl0 - 4, 4, ".svg") ||
                      tag_ci_eq(src + sl0 - 4, 4, ".gif"))) ||
        (sl0 >= 5 && (tag_ci_eq(src + sl0 - 5, 5, ".webp") ||
                      tag_ci_eq(src + sl0 - 5, 5, ".avif"))) ||
        strncmp(src, "data:", 5) == 0)
        return 0;
    if (g_br_img_count >= BR_IMG_SLOTS) return 0;
    int s = g_br_img_count++;
    uint32_t sl = (uint32_t)strlen(src);
    if (sl >= sizeof(g_br_img_src[0])) sl = sizeof(g_br_img_src[0]) - 1;
    memcpy(g_br_img_src[s], src, sl);
    g_br_img_src[s][sl] = 0;
    g_br_img_w[s] = 0;
    g_br_img_h[s] = 0;
    return s + 1;
}

/* 提取卡片内第一个 <a href>，登记为浏览器链接，返回 link_idx（-1 无） */
static int br_card_link(gui_state_t *st, const char *inner, uint32_t len) {
    const char *a = body_strcasestr(inner, "<a");
    if (!a || (uint32_t)(a - inner) >= len) return -1;
    uint32_t start = (uint32_t)(a - inner);
    uint32_t j = start;
    while (j < len && inner[j] != '>') j++;
    if (j >= len) return -1;
    char href[96];
    if (!br_attr_value(inner + start, j - start, "href", href, sizeof(href)) ||
        !href[0])
        return -1;
    if (st->browser_link_count >= BROWSER_LINK_MAX) return -1;
    int idx = st->browser_link_count;
    uint32_t hl = (uint32_t)strlen(href);
    if (hl >= sizeof(st->browser_link_href[0])) hl = sizeof(st->browser_link_href[0]) - 1;
    memcpy(st->browser_link_href[idx], href, hl);
    st->browser_link_href[idx][hl] = 0;
    st->browser_link_count++;
    return idx;
}

/* 把一段字符串写入渲染流，控制字节折叠成空格（与正文发射端一致，防止
 * 卡片载荷里混入 0x01-0x05 流标记） */
static void br_card_text_emit(char *out, uint32_t cap, uint32_t *pos,
                              const char *text) {
    for (const char *p = text; *p && *pos + 1 < cap; p++) {
        char ch = *p;
        if ((unsigned char)ch < 0x20) ch = ' ';
        out[(*pos)++] = ch;
    }
}

/* 组装并输出一个 BRK_CARD 块 */
static void br_emit_card(gui_state_t *st, char *out, uint32_t cap, uint32_t *pos,
                         const char *inner, uint32_t inner_len,
                         const char *meta, const char *dur) {
    char title[96];
    br_card_plain_text(inner, inner_len, title, sizeof(title));
    if (!title[0] || *pos + 2 >= cap) return;
    int slot = br_card_img_slot(inner, inner_len);
    int link_idx = br_card_link(st, inner, inner_len);
    css_decl_t ov;
    memset(&ov, 0, sizeof(ov));
    out[(*pos)++] = (char)BRK_CARD;
    br_emit_meta(out, cap, pos, link_idx, &ov);
    out[(*pos)++] = (char)(0x10 | slot);
    br_card_text_emit(out, cap, pos, title);
    out[(*pos)++] = (char)BR_CARD_META;
    br_card_text_emit(out, cap, pos, meta ? meta : "");
    out[(*pos)++] = (char)BR_CARD_DUR;
    br_card_text_emit(out, cap, pos, dur ? dur : "");
    out[(*pos)++] = '\n';
}

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
    /* LiteJS 卡片捕获：<hb-card> 的原始内部 HTML 暂存，闭合时统一提取 */
    int card_capture = 0;
    static char card_buf[BR_CARD_CAP];
    uint32_t card_len = 0;
    static char card_meta[64];
    static char card_dur[32];
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
    /* 列表嵌套栈：每层记录是否有序 + 当前编号。之前只有单层
     * in_ordered_list/li_number，嵌套 </ol> 一关就把外层的状态也清了
     * （外层后续项丢编号），嵌套时内外层编号也互相踩。 */
#define BR_LIST_DEPTH_MAX 5
    int list_depth = 0;                  /* 0 = 不在任何列表里 */
    int ol_flag[BR_LIST_DEPTH_MAX + 1];  /* 该层是不是 <ol> */
    int ol_num[BR_LIST_DEPTH_MAX + 1];   /* 该层当前编号 */
    ol_flag[0] = 0; ol_num[0] = 0;

    gh_css_reset();
    st->browser_link_count = 0;
    g_br_img_count = 0;
    g_br_css_fetches = 0;
    g_br_script_fetches = 0;
    strncpy(g_br_current_url, st->browser_url, sizeof(g_br_current_url) - 1);
    g_br_current_url[sizeof(g_br_current_url) - 1] = 0;
    g_br_dns_n = 0;
    g_br_sub_fails = 0;
    /* CSS/图片是可降级内容，整页最多给 15 秒。主体已经抓到后不能因为某个
     * CDN 不回 TLS record 而永远停在“加载中”。 */
    g_br_sub_deadline = pit_deadline_after_ms(15000);

    g_browser_page_title[0] = 0;
    g_browser_title_len = 0;
    g_br_og_title[0] = 0;
    g_br_og_desc[0] = 0;
    g_br_og_image[0] = 0;
    g_br_og_has_image = 0;

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
            if (in_title_block) {
                /* 标题栏同样要过滤字体没有字形的码点（颜文字/假名等），
                 * 否则标题栏上一堆 '?'（"(゜-゜)つロ" 这种）——逻辑和正文
                 * 解析那份一致，见上面 gui_font_lookup 那段注释。 */
                if ((uint8_t)c >= 0xE0 && (uint8_t)c == (uint8_t)html[i]) {
                    uint8_t b0 = (uint8_t)c;
                    uint32_t n = (b0 >= 0xF0) ? 4 : 3;
                    uint32_t cp = b0 & (uint32_t)((n == 4) ? 0x07 : 0x0F);
                    int valid = 1;
                    for (uint32_t k = 1; k < n; k++) {
                        uint8_t bk = (uint8_t)html[i + k];
                        if ((bk & 0xC0) != 0x80) { valid = 0; break; }
                        cp = (cp << 6) | (uint32_t)(bk & 0x3F);
                    }
                    gui_glyph_t title_gtmp;
                    if (valid && !gui_font_lookup(cp, &title_gtmp)) {
                        i += n - 1;
                        continue;
                    }
                }
                if (g_browser_title_len + 1 < sizeof(g_browser_page_title) && c != '\r' && c != '\n')
                    g_browser_page_title[g_browser_title_len++] = (c == '\t') ? ' ' : c;
            }
            continue;
        }

        if (c == '<') {
            /* 卡片捕获模式：除 </hb-card> 闭合外，整个 <...> 原样进缓冲
             * （内层 <a>/<img> 等标签也要保留，闭合时才统一提取）。 */
            if (card_capture) {
                int tclosing = (html[i + 1] == '/');
                uint32_t tj = i + 1 + (tclosing ? 1 : 0);
                char tn[16]; int tl = 0;
                while (html[tj] && html[tj] != '>' && html[tj] != ' ' &&
                       html[tj] != '\t' && html[tj] != '\n' &&
                       html[tj] != '/' && tl < 15)
                    tn[tl++] = html[tj++];
                while (html[tj] && html[tj] != '>') tj++;
                if (!(tclosing && tag_ci_eq(tn, tl, "hb-card"))) {
                    for (uint32_t k = i; k <= tj && card_len + 1 < sizeof(card_buf); k++)
                        card_buf[card_len++] = html[k];
                    i = tj;
                    continue;
                }
                /* </hb-card>：落到下面的 hb-card 闭合分支 */
            }
            /* HTML 注释 <!--...--> 及 Vue SSR 分隔注释 <!--[--><!--]--> ——
             * 直接跳到 --> 结束符，不输出任何内容。 */
            if (html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                i += 4;
                while (html[i] && !(html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>'))
                    i++;
                if (html[i]) i += 2; /* 停在 '>' 上，外层 for 的 i++ 正好跳过它 */
                continue;
            }
            /* <!DOCTYPE ...> 和其他 <!xxx> 声明：跳到 > */
            if (html[i + 1] == '!') {
                while (html[i] && html[i] != '>') i++;
                continue;
            }
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
            /* <svg>：内联矢量图——全是 path/polygon 数据，没有可渲染的文本。
             * <noscript>：JS 降级 fallback——我们没有 JS 引擎，内容往往是
             * "请开启 JavaScript" 或 Vue 空壳，渲染出来只增加噪音。
             * <template>：Vue 3 模板元素，SSR 渲染时残留空壳。
             * <select>/<option>：下拉框，其文本会混入正文。
             * <iframe>/<canvas>/<video>/<audio>：无法渲染的媒体/嵌入元素。
             * 以上都像 script/style 一样整体跳过。 */
            const char *skipped_close = closing ? 0 : browser_skipped_tag_close(nm, nl);
            if (skipped_close) {
                BRFLUSH();
                skip_mode = 1; in_style_block = 0;
                skip_close = skipped_close;
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "link")) {
                char relv[24], hrefv[160];
                if (br_attr_value(html + attr_start, attr_len, "rel", relv, sizeof(relv)) &&
                    tag_ci_eq(relv, (int)strlen(relv), "stylesheet") &&
                    br_attr_value(html + attr_start, attr_len, "href", hrefv, sizeof(hrefv)))
                    browser_fetch_stylesheet(st, hrefv);
                i = j;
                continue;
            }
            /* <meta property="og:title/description/image" content="...">
             * bilibili 视频页 <body> 基本是 Vue 空壳，但 <head> 有完整 og: 标签。
             * 提取后在页面最前插入一个"视频卡片"，视频页不再空白。
             * 同时处理 <meta name="description"> 作为 og:description 的回退。 */
            if (!closing && tag_ci_eq(nm, nl, "meta")) {
                char prop[48], cont[BR_OG_CAP];
                int has_cont = br_attr_value(html + attr_start, attr_len, "content", cont, sizeof(cont));
                if (has_cont && cont[0]) {
                    if (br_attr_value(html + attr_start, attr_len, "property", prop, sizeof(prop))) {
                        if (tag_ci_eq(prop, (int)strlen(prop), "og:title") && !g_br_og_title[0])
                            strncpy(g_br_og_title, cont, BR_OG_CAP - 1);
                        else if (tag_ci_eq(prop, (int)strlen(prop), "og:description") && !g_br_og_desc[0])
                            strncpy(g_br_og_desc, cont, BR_OG_CAP - 1);
                        else if (tag_ci_eq(prop, (int)strlen(prop), "og:image") && !g_br_og_image[0])
                            strncpy(g_br_og_image, cont, BR_OG_CAP - 1);
                    }
                    /* name="description" 作为 og:description 回退 */
                    if (!g_br_og_desc[0] &&
                        br_attr_value(html + attr_start, attr_len, "name", prop, sizeof(prop)) &&
                        tag_ci_eq(prop, (int)strlen(prop), "description"))
                        strncpy(g_br_og_desc, cont, BR_OG_CAP - 1);
                }
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
                char src[160];
                int slot = 0; /* 0 = 无槽（占位），1..BR_IMG_SLOTS = 有槽 */
                /* 真实站点普遍懒加载：src 是占位图，真实地址在 data-src/
                 * data-original 里——优先取懒加载属性。另外把明显解不了的
                 * 格式（svg/webp/gif）直接排除，不浪费本来就只有 4 个的
                 * 图片槽位。 */
                int has_src = br_attr_value(html + attr_start, attr_len, "data-src", src, sizeof(src)) ||
                              br_attr_value(html + attr_start, attr_len, "data-original", src, sizeof(src)) ||
                              br_attr_value(html + attr_start, attr_len, "src", src, sizeof(src));
                if (has_src && src[0]) {
                    /* bilibili 封面常见 `xxx.jpg@672w_378h_1c_1s.webp`：先改写
                     * 成 jpg 缩放参数再检查扩展名，否则 .webp 过滤会把封面
                     * 直接丢掉，页面「一张图都没有」。 */
                    br_img_url_rewrite(src, sizeof(src));
                    uint32_t sl0 = (uint32_t)strlen(src);
                    if ((sl0 >= 4 && (tag_ci_eq(src + sl0 - 4, 4, ".svg") ||
                                      tag_ci_eq(src + sl0 - 4, 4, ".gif"))) ||
                        (sl0 >= 5 && (tag_ci_eq(src + sl0 - 5, 5, ".webp") ||
                                      tag_ci_eq(src + sl0 - 5, 5, ".avif"))) ||
                        strncmp(src, "data:", 5) == 0)
                        has_src = 0;
                }
                if (has_src && src[0] && g_br_img_count < BR_IMG_SLOTS) {
                    int s = g_br_img_count++;
                    uint32_t sl = (uint32_t)strlen(src);
                    if (sl >= sizeof(g_br_img_src[0])) sl = sizeof(g_br_img_src[0]) - 1;
                    memcpy(g_br_img_src[s], src, sl);
                    g_br_img_src[s][sl] = 0;
                    g_br_img_w[s] = 0; g_br_img_h[s] = 0; /* 加载后再解码填充 */
                    slot = s + 1;
                }
                BREMIT(BRK_IMG);
                /* 槽位标记字节：加 0x10 偏移避开 0x01/0x02/0x03 三个流标记
                 * （否则 slot=1 的 0x01 会被 br_read_style 当成 BR_META_MARK
                 * 误吞后续字节）。绘制端取低 4 位。 */
                BREMIT((char)(0x10 | slot));
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
            if (!closing && (tag_ci_eq(nm, nl, "ol") || tag_ci_eq(nm, nl, "ul"))) {
                if (list_depth < BR_LIST_DEPTH_MAX) list_depth++;
                ol_flag[list_depth] = tag_ci_eq(nm, nl, "ol");
                ol_num[list_depth] = 0;
                i = j;
                continue;
            }
            if (closing && (tag_ci_eq(nm, nl, "ol") || tag_ci_eq(nm, nl, "ul"))) {
                if (list_depth > 0) list_depth--;
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "li") && ol_flag[list_depth]) {
                BRFLUSH();
                ol_num[list_depth]++;
                int li_number = ol_num[list_depth];
                BREMIT(BRK_LI_NUM);
                memset(&cur_override, 0, sizeof(cur_override));
                if (list_depth > 1) {
                    cur_override.indent_lvl = (list_depth - 1 > 3) ? 3 : list_depth - 1;
                    cur_override.has_indent = 1;
                    br_emit_meta(out, cap, &pos, -1, &cur_override);
                }
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
                i = j;
                continue;
            }
            /* LiteJS 卡片/网格语义：<hb-grid> 开网格，<hb-card> 捕获内容 */
            if (!closing && tag_ci_eq(nm, nl, "hb-grid")) {
                BRFLUSH();
                BREMIT(BRK_GRID); BREMIT('\n');
                need_prefix = 1; space = 1;
                i = j;
                continue;
            }
            if (!closing && tag_ci_eq(nm, nl, "hb-card")) {
                BRFLUSH();
                card_capture = 1;
                card_len = 0;
                card_meta[0] = 0;
                card_dur[0] = 0;
                br_attr_value(html + attr_start, attr_len, "data-meta",
                              card_meta, sizeof(card_meta));
                br_attr_value(html + attr_start, attr_len, "data-dur",
                              card_dur, sizeof(card_dur));
                i = j;
                continue;
            }
            if (closing && tag_ci_eq(nm, nl, "hb-card")) {
                card_capture = 0;
                if (card_len > 0)
                    br_emit_card(st, out, cap, &pos, card_buf, card_len,
                                 card_meta, card_dur);
                need_prefix = 1; space = 1;
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
                    char id_val[64]; id_val[0] = 0;
                    char style_val[128]; style_val[0] = 0;
                    br_attr_value(html + attr_start, attr_len, "class", class_val, sizeof(class_val));
                    br_attr_value(html + attr_start, attr_len, "id", id_val, sizeof(id_val));
                    br_attr_value(html + attr_start, attr_len, "style", style_val, sizeof(style_val));
                    css_decl_t resolved;
                    css_resolve_for_tag(nm, nl, class_val, id_val, style_val, &resolved);
                    /* 行内标签继承外层样式：<h1>里的<em>保持大字号/加粗，
                     * <p style="color:red">里的<b>保持红色——行内元素现在
                     * 和外层文字排在同一行，字号/颜色突变会非常扎眼。
                     * 只填充 resolved 里没有显式指定的字段。 */
                    if (st_type == BRK_LI && list_depth > 1 && !resolved.has_indent) {
                        resolved.indent_lvl = (list_depth - 1 > 3) ? 3 : list_depth - 1;
                        resolved.has_indent = 1;
                    }
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

        /* 卡片捕获：普通字符（非 '<'）原样进缓冲，实体保持原始形式 */
        if (card_capture) {
            if (card_len + 1 < sizeof(card_buf)) card_buf[card_len++] = c;
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
        /* 字体没有的符号类码点整段跳过：私有区图标字（大站图标字体全在
         * 这）、emoji、零宽字符、变体选择符、杂项符号、日文假名等——不过滤
         * 的话每个都画成 '?'（百度热榜每行开头的 "?"、B 站标题里的颜文字
         * "(゜-゜)つロ" 都是这么来的）。原先是手工维护的码点区间黑名单，
         * 挂一漏万；现在直接问字体有没有这个字形，字体是唯一真相源，新站点
         * 用到黑名单没覆盖的区间不用再补规则。只在字节确实来自原文（不是
         * 实体解码产物，那些都是 ASCII）时才做序列判断。 */
        if ((uint8_t)ch >= 0xE0 && (uint8_t)ch == (uint8_t)html[i]) {
            uint8_t b0 = (uint8_t)ch;
            uint32_t n = (b0 >= 0xF0) ? 4 : 3;
            uint32_t cp = b0 & (uint32_t)((n == 4) ? 0x07 : 0x0F);
            int valid = 1;
            for (uint32_t k = 1; k < n; k++) {
                uint8_t bk = (uint8_t)html[i + k];
                if ((bk & 0xC0) != 0x80) { valid = 0; break; }
                cp = (cp << 6) | (uint32_t)(bk & 0x3F);
            }
            gui_glyph_t br_gtmp;
            if (valid && !gui_font_lookup(cp, &br_gtmp)) {
                i += n - 1;
                continue;
            }
        }
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
    st->browser_required_caps = HIVE_WEB_CAP_HTML_STATIC;
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

/* 浏览器自己的状态文本：st->status 是所有应用共享的一个指针，别的应用
 * （比如开始菜单）留下的状态不该出现在浏览器的状态气泡里——气泡只在
 * st->status 仍指向浏览器最后一次设置的字符串时才显示。 */
static const char *g_browser_status;
static void br_status(gui_state_t *st, const char *msg) {
    st->status = msg;
    g_browser_status = msg;
}

/* 前置声明：图片 src 相对当前页面 URL 补全，逻辑和 <a href> 完全一样。 */
static int browser_resolve_href(const gui_state_t *st, const char *href, char *out, uint32_t out_cap);

/* HTTP 状态码（"HTTP/1.x NNN ..."），解析失败返回 -1。 */
static int br_http_status(const char *buf) {
    if (strncmp(buf, "HTTP/", 5) != 0) return -1;
    const char *p = buf + 5;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int code = 0;
    while (*p >= '0' && *p <= '9') code = code * 10 + (*p++ - '0');
    return code ? code : -1;
}

/* 提取 <meta http-equiv="refresh" content="N; url=URL">（大小写不敏感）。
 * 无 JS 站点的标准跳转兜底——百度 https 首页就是一个纯 JS 跳转页，靠
 * <noscript> 里这个 meta 把无脚本浏览器导到真实内容页；不认它就停在
 * 一页空白。只扫 <head>（body 里出现的 refresh 极少且多是广告刷新）。 */
static int br_meta_refresh(const char *body, char *out, uint32_t cap) {
    const char *head_end = strstr(body, "</head>");
    const char *scan_end = head_end ? head_end : body + strlen(body);
    for (const char *p = body; p < scan_end; p++) {
        if (*p != '<') continue;
        if (!tag_ci_eq(p + 1, 5, "meta ") && !tag_ci_eq(p + 1, 4, "meta")) continue;
        const char *tag_end = p;
        while (tag_end < scan_end && *tag_end != '>') tag_end++;
        /* 必须同时含 http-equiv=refresh 和 content= */
        int is_refresh = 0;
        for (const char *q = p; q + 7 < tag_end; q++)
            if (tag_ci_eq(q, 7, "refresh")) { is_refresh = 1; break; }
        if (!is_refresh) continue;
        const char *c = 0;
        for (const char *q = p; q + 8 < tag_end; q++)
            if (tag_ci_eq(q, 8, "content=")) { c = q + 8; break; }
        if (!c) continue;
        char quote = (*c == '"' || *c == '\'') ? *c : 0;
        if (quote) c++;
        /* content = "秒数 ; url = 目标"，找 "url=" */
        const char *u = 0;
        for (const char *q = c; q < tag_end; q++) {
            if (quote && *q == quote) break;
            if (tag_ci_eq(q, 4, "url=")) { u = q + 4; break; }
        }
        if (!u) continue;
        while (*u == ' ' || *u == '\'' || *u == '"') u++;
        uint32_t n = 0;
        while (u + n < tag_end && u[n] != '"' && u[n] != '\'' &&
               u[n] != '>' && u[n] != ' ' && n + 1 < cap) {
            out[n] = u[n];
            n++;
        }
        out[n] = 0;
        return n ? 0 : -1;
    }
    return -1;
}

/* 提取 Location 响应头的值（大小写不敏感、去前导空格、到行尾）。 */
static int br_http_location(const char *buf, char *out, uint32_t cap) {
    const char *hdr_end = strstr(buf, "\r\n\r\n");
    const char *p = buf;
    while (p && (!hdr_end || p < hdr_end)) {
        p = strstr(p, "\r\n");
        if (!p) break;
        p += 2;
        if (tag_ci_eq(p, 9, "location:")) {
            p += 9;
            while (*p == ' ') p++;
            uint32_t n = 0;
            while (p[n] && p[n] != '\r' && p[n] != '\n' && n + 1 < cap) {
                out[n] = p[n];
                n++;
            }
            out[n] = 0;
            return n ? 0 : -1;
        }
    }
    return -1;
}

static void browser_fetch_stylesheet(gui_state_t *st, const char *href) {
    if (g_br_css_fetches >= 2 || g_br_sub_fails >= 4 || br_sub_budget_expired()) return;
    br_status(st, "正在加载网页样式");
    static char cssbuf[64 * 1024];
    char url[192];
    if (browser_resolve_href(st, href, url, sizeof(url)) < 0) return;
    int https = 0; char host[96]; const char *path = "/"; uint16_t port = 80;
    if (gui_parse_url(url, &https, host, sizeof(host), &port, &path)) return;
    uint32_t ip = 0; int cached = 0;
    if (br_sub_resolve(host, &ip, &cached) < 0) { if (!cached) g_br_sub_fails++; return; }
    uint32_t len = 0;
    int ok = https ? tls_https_get_with_idle_limit(host, ip, port, path,
                                                    cssbuf, sizeof(cssbuf), &len, 12)
                   : net_http_request("GET", host, ip, port, path, cssbuf, sizeof(cssbuf), &len);
    if (ok < 0) { g_br_sub_fails++; return; }
    if (br_sub_budget_expired()) return;
    g_br_css_fetches++;
    const char *body = http_body_ptr(cssbuf);
    uint32_t body_off = (uint32_t)(body - cssbuf);
    if (body_off >= len) return;
    css_parse_stylesheet(body, len - body_off);
}

/* 外链 <script src="..."> 抓取：走与 stylesheet 相同的网络路径（TLS
 * 由内核处理），正文写入 ramfs 文件，供 jspage/浏览器管线按 HTML 顺序
 * 执行。返回 0 成功；失败/超预算返回 -1（调用方继续处理剩余脚本）。 */
static int browser_fetch_script_to_file(const char *href, const char *path) {
    if (g_br_script_fetches >= 10 || g_br_sub_fails >= 4 ||
        br_sub_budget_expired())
        return -1;
    static char jsbuf[256 * 1024];
    char url[192];
    if (browser_resolve_href(NULL, href, url, sizeof(url)) < 0) return -1;
    int https = 0; char host[96]; const char *path_s = "/"; uint16_t port = 80;
    if (gui_parse_url(url, &https, host, sizeof(host), &port, &path_s)) return -1;
    uint32_t ip = 0; int cached = 0;
    if (br_sub_resolve(host, &ip, &cached) < 0) { if (!cached) g_br_sub_fails++; return -1; }
    uint32_t len = 0;
    int ok = https ? tls_https_get_with_idle_limit(host, ip, port, path_s,
                                                    jsbuf, sizeof(jsbuf), &len, 15)
                   : net_http_request("GET", host, ip, port, path_s,
                                      jsbuf, sizeof(jsbuf), &len);
    if (ok < 0) { g_br_sub_fails++; return -1; }
    if (br_sub_budget_expired()) return -1;
    g_br_script_fetches++;
    const char *body = http_body_ptr(jsbuf);
    uint32_t body_off = (uint32_t)(body - jsbuf);
    if (body_off >= len) return -1;
    uint32_t blen = len - body_off;
    /* ramfs 单文件上限 128KB。宁可跳过超大 bundle，也不能写入半截 JS：
     * 半截源码只会产生 SyntaxError，并污染后续依赖判断。 */
    if (blen > 120U * 1024U) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < blen) {
        long w = write(fd, body + off, blen - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
    return (off == blen) ? 0 : -1;
}

/* 图床变换后缀改写（B 站 hdslb 等）：
 * 1. 有 '@' 参数的 URL（如 xxx.jpg@672w_378h...webp）→ 把 '@' 后整段换成 "240w_180h"
 * 2. bilibili CDN 直链（i0/i1/i2.hdslb.com，无 '@'）→ 在 URL 末尾追加 "@240w_180h"
 *    确保 og:image 等绝对路径图片也走图床缩放，不拉全尺寸原图把解码缓冲打爆。
 * 其他 URL 原样不动。 */
static void br_img_url_rewrite(char *url, uint32_t cap) {
    uint32_t n = (uint32_t)strlen(url);
    /* 情形1：有图床转换参数 */
    for (uint32_t i = 4; i + 1 < n; i++) {
        if (url[i] != '@') continue;
        int is_img_ext =
            (i >= 4 && tag_ci_eq(url + i - 4, 4, ".jpg")) ||
            (i >= 4 && tag_ci_eq(url + i - 4, 4, ".png")) ||
            (i >= 5 && tag_ci_eq(url + i - 5, 5, ".jpeg"));
        if (!is_img_ext) continue;
        const char *spec = "240w_180h";
        uint32_t p = i + 1;
        for (const char *q = spec; *q && p + 1 < cap; q++) url[p++] = *q;
        url[p] = 0;
        return;
    }
    /* 情形2：bilibili CDN 直链，末尾是 .jpg/.png，没有 '@' — 追加缩放参数 */
    int is_bili_cdn = (strstr(url, "hdslb.com/") != 0) ||
                      (strstr(url, "bilibili.com/bfs/") != 0);
    if (!is_bili_cdn) return;
    int ends_jpg = (n >= 4 && tag_ci_eq(url + n - 4, 4, ".jpg")) ||
                   (n >= 5 && tag_ci_eq(url + n - 5, 5, ".jpeg"));
    int ends_png = (n >= 4 && tag_ci_eq(url + n - 4, 4, ".png"));
    if ((ends_jpg || ends_png) && n + 10 < cap) {
        const char *spec = "@240w_180h";
        for (const char *q = spec; *q; q++) url[n++] = *q;
        url[n] = 0;
    }
}

/* 页面加载后统一抓取并解码所有已登记的内联图片。每张独立抓取一次
 * 提取内联 <script> 交给 ring3 quickjs 运行时（js.hax）执行，把脚本的
 * document.title 和 console/print/document.write 输出读回：title 更新窗口
 * 标题栏，输出作为 BRK_CODE 块追加到渲染流末尾。v1 只执行无 src 的内联
 * 脚本，忽略外链脚本和 DOM 操作（脚本内 document.write 文本原样展示）。
 *
 * browser_exec_scripts_core 是不依赖 GUI 的核心（jspage 命令与浏览器
 * 共用）：返回 0 表示有脚本执行；title/out 由调用者提供缓冲。 */
/* /system/js_pNN.js 路径构造（内联与外链脚本统一编号，供 js.hax
 * 多文件逐个执行）。 */
static void build_js_path(char *path, uint32_t cap, int index) {
    static const char pfx[] = "/system/js_p";
    static const char sfx[] = ".js";
    uint32_t pi = 0;
    for (const char *q = pfx; *q && pi < cap - 1; q++) path[pi++] = *q;
    if (index >= 10) path[pi++] = (char)('0' + index / 10);
    path[pi++] = (char)('0' + index % 10);
    for (const char *q = sfx; *q && pi < cap - 1; q++) path[pi++] = *q;
    path[pi] = 0;
}

int browser_exec_scripts_core(const char *body, char *title, uint32_t title_cap,
                              char *out, uint32_t out_cap) {
    const char *p = body;
    int script_count = 0;
    int ran = 0;

    if (!body) return 0;
    /* v1: each inline script is written to its own file and evaluated in
     * its own unit — a SyntaxError in one (e.g. Vite import.meta chunks)
     * must not block the remaining scripts.  External src= scripts are
     * skipped. */
    while (*p && script_count < 24) {
        const char *s = body_strcasestr(p, "<script");
        if (!s) break;
        const char *gt = strchr(s, '>');
        if (!gt) break;
        /* 外链脚本（<script src=...>）：内核网络栈抓取（TLS），按 HTML
         * 出现顺序与内联脚本统一编号，js.hax 逐个执行。 */
        const char *src_attr = body_strcasestr(s, "src");
        if (src_attr && src_attr < gt) {
            char src_url[256];
            uint32_t tag_len = (uint32_t)(gt - s);
            if (br_attr_value(s, tag_len, "src", src_url, sizeof(src_url)) > 0) {
                char path[32];
                build_js_path(path, sizeof(path), script_count);
                if (browser_fetch_script_to_file(src_url, path) == 0)
                    script_count++;
            }
            p = gt + 1;
            continue;
        }
        const char *end = body_strcasestr(gt + 1, "</script>");
        if (!end) break;
        size_t n = (size_t)(end - (gt + 1));
        /* Bilibili 的 window.__INITIAL_STATE__ 约 87KB，必须完整执行；
         * 截断 JS 比跳过更糟。ramfs 单文件上限 128KB，留 8KB 余量。 */
        if (n > 120U * 1024U) { p = end + 9; continue; }
        if (n > 0) {
            char path[32];
            build_js_path(path, sizeof(path), script_count);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                size_t off = 0;
                while (off < n) {
                    long w = write(fd, gt + 1 + off, n - off);
                    if (w <= 0) break;
                    off += (size_t)w;
                }
                close(fd);
                script_count++;
            }
        }
        p = end + 9;
    }
    if (!script_count) return 0;

    /* 页面原始 HTML 写给 js.hax 解析成 DOM（脚本执行前建树） */
    int pfd = open("/system/page.html", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pfd >= 0) {
        size_t boff = 0;
        size_t blen = strlen(body);
        /* DOM 快照限制在 ramfs 128KB 单文件上限内；关键 INITIAL_STATE
         * 脚本另外完整写入 js_pN.js，不依赖快照尾部。 */
        if (blen > 120U * 1024U) blen = 120U * 1024U;
        while (boff < blen) {
            long w = write(pfd, body + boff, blen - boff);
            if (w <= 0) break;
            boff += (size_t)w;
        }
        close(pfd);
    }

    char *js_argv[16];
    int a = 0;
    js_argv[a++] = "js";
    js_argv[a++] = "-w";
    js_argv[a++] = "/system/js_out.txt";
    js_argv[a++] = "-p";
    js_argv[a++] = "/system/page.html";
    js_argv[a++] = "-u";
    js_argv[a++] = g_br_current_url[0] ? g_br_current_url : "about:blank";
    /* 内置 Vue 运行时兜底：页面自己带 Vue（bilibili 的 vue.min.js 等）
     * 会随后覆盖 window.Vue，预载只保证 window.Vue 一定存在。 */
    if (access("/system/vue.global.prod.js", F_OK) == 0 && a < 14)
        js_argv[a++] = "/system/vue.global.prod.js";
    for (int i = 0; i < script_count && a < 15; i++) {
        static char path[16][32];
        build_js_path(path[i], sizeof(path[i]), i);
        js_argv[a++] = path[i];
    }
    js_argv[a] = 0;
    (void)hax_app_run("js", a, js_argv);
    ran = 1;

    /* 读回 title（第一行）+ 渲染出的迷你 HTML（其余行） */
    char tmp[32768];
    int ofd = open("/system/js_out.txt", O_RDONLY);
    if (ofd < 0) return ran;
    long olen = 0;
    for (;;) {
        long n = read(ofd, tmp + olen, (size_t)(sizeof(tmp) - 1 - olen));
        if (n <= 0) break;
        olen += n;
        if (olen >= (long)sizeof(tmp) - 1) break;
    }
    close(ofd);
    if (olen <= 0) return ran;
    tmp[olen] = 0;

    char *nl = strchr(tmp, '\n');
    size_t title_len = nl ? (size_t)(nl - tmp) : (size_t)olen;
    if (title && title_cap && title_len > 0 && title_len < title_cap) {
        memcpy(title, tmp, title_len);
        title[title_len] = 0;
    }
    if (out && out_cap) {
        const char *js_out = nl ? nl + 1 : "";
        size_t jlen = strlen(js_out);
        if (jlen >= out_cap) jlen = out_cap - 1;
        if (jlen > 0) {
            memcpy(out, js_out, jlen);
            out[jlen] = 0;
        } else {
            out[0] = 0;
        }
    }
    return ran;
}

static void browser_exec_scripts(gui_state_t *st, const char *body) {
    static char js_title[BROWSER_TITLE_CAP];
    static char js_out[24 * 1024];

    if (!browser_exec_scripts_core(body, js_title, sizeof(js_title),
                                   js_out, sizeof(js_out)))
        return;
    if (js_title[0]) {
        uint32_t tlen = (uint32_t)strlen(js_title);
        if (tlen < BROWSER_TITLE_CAP) {
            memcpy(g_browser_page_title, js_title, tlen);
            g_browser_page_title[tlen] = 0;
            g_browser_title_len = tlen;
        }
    }
    size_t jlen = strlen(js_out);
    if (jlen == 0) return;
    /* js.hax 渲染的是脚本执行后的最终 DOM（Vue 等框架已挂载）：用它
     * 替换内核静态解析结果，重新走一遍内核渲染器拿完整样式（BRK_H1/
     * BRK_LINK/BRK_IMG…）。输入与输出必须是独立缓冲；解析器从左向右读
     * mini HTML，同时向输出写 BRK 标记，就地解析会在读完前破坏源码。 */
    browser_render_from_html(st, js_out, st->browser_render,
                             BROWSER_PAGE_CAP, &st->browser_render_len);
}

/* browser_fetch_images：
 * （串行，复用同一个静态响应缓冲），解码进对应槽位；失败/超尺寸/JPEG
 * 等不支持的就把该槽宽高留 0，绘制端画占位。 */
static void browser_fetch_images(gui_state_t *st) {
    static uint8_t imgresp[BROWSER_FETCH_CAP];
    for (int s = 0; s < g_br_img_count && s < BR_IMG_FETCH_MAX; s++) {
        g_br_img_w[s] = 0; g_br_img_h[s] = 0;
        if (g_br_sub_fails >= BR_IMG_SLOTS || br_sub_budget_expired()) continue;
        br_status(st, "正在加载网页图片");
        char url[256];
        if (browser_resolve_href(st, g_br_img_src[s], url, sizeof(url)) < 0) continue;
        br_img_url_rewrite(url, sizeof(url));
        int https = 0; char host[96]; const char *path = "/"; uint16_t port = 80;
        if (gui_parse_url(url, &https, host, sizeof(host), &port, &path)) continue;
        uint32_t ip = 0; int cached = 0;
        if (br_sub_resolve(host, &ip, &cached) < 0) { if (!cached) g_br_sub_fails++; continue; }
        uint32_t len = 0;
        int ok = https ? tls_https_get_with_idle_limit(host, ip, port, path,
                                                        (char *)imgresp, sizeof(imgresp), &len, 12)
                       : net_http_request("GET", host, ip, port, path, (char *)imgresp, sizeof(imgresp), &len);
        if (ok < 0) { g_br_sub_fails++; continue; }
        if (br_sub_budget_expired()) continue;
        const char *body = http_body_ptr((char *)imgresp);
        uint32_t body_off = (uint32_t)((const uint8_t *)body - imgresp);
        if (body_off >= len) continue;
        uint32_t blen = len - body_off;
        /* 大图即使最终像素尺寸合规，软件 JPEG/PNG 解码也可能长时间占用协作式
         * 调度器。CDN 缩略图正常远小于 256KB，超过就降级为占位。 */
        if (blen > 256 * 1024) continue;
        const uint8_t *b = (const uint8_t *)body;
        int w = 0, h = 0, dok;
        br_status(st, "正在解码网页图片");
        if (blen >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G')
            dok = png_decode(b, blen, g_br_img_rgb[s], sizeof(g_br_img_rgb[s]),
                             BR_IMG_MAX_W, BR_IMG_MAX_H, &w, &h);
        else if (blen >= 4 && b[0] == 0xFF && b[1] == 0xD8)
            dok = jpeg_decode(b, blen, g_br_img_rgb[s], sizeof(g_br_img_rgb[s]),
                              BR_IMG_MAX_W, BR_IMG_MAX_H, &w, &h);
        else
            dok = bmp_decode(b, blen, g_br_img_rgb[s], sizeof(g_br_img_rgb[s]),
                             BR_IMG_MAX_W, BR_IMG_MAX_H, &w, &h);
        if (dok == 0) { g_br_img_w[s] = w; g_br_img_h[s] = h; }
    }
}

static void browser_load_internal(gui_state_t *st, int push_history) {
    browser_init(st);
    br_status(st, "浏览器加载中");
    if (!net_primary()->dhcp_ok && net_dhcp() < 0) {
        browser_set_plain2(st, "网络未配置: ", net_last_error());
        br_status(st, "浏览器网络失败");
        return;
    }
    static char response[BROWSER_FETCH_CAP];
    uint32_t len = 0;
    /* file:// 本地页面：直接读文件渲染，绕过网络与重定向循环。本地
     * HTML 是路线图阶段 6 的回归载体，也用于内联 <script> 的 JS 测试。 */
    if (strncmp(st->browser_url, "file://", 7) == 0) {
        int lfd = open(st->browser_url + 7, O_RDONLY);
        if (lfd < 0) {
            browser_set_plain2(st, "无法读取本地文件: ", st->browser_url + 7);
            br_status(st, "浏览器本地文件失败");
            return;
        }
        long n = read(lfd, response, sizeof(response) - 1);
        close(lfd);
        if (n <= 0) {
            browser_set_plain(st, "本地文件为空");
            br_status(st, "浏览器本地文件失败");
            return;
        }
        len = (uint32_t)n;
        response[len] = 0;
        goto render_page;
    }
    /* 重定向循环：真实网站首页 301/302 到 www/https 非常普遍，之前浏览器
     * 不跟随（只有 shell 的 wget 会），用户看到的就是一页 301 Moved 文本。
     * 最多 4 跳防环；每跳把地址栏更新为当前 URL（既是 Chrome 行为，也让
     * browser_resolve_href 用它做相对 Location 的基准）。 */
    for (int hop = 0; ; hop++) {
        char host[96];
        const char *path = "/";
        uint16_t port = 80;
        int https = 0;
        const char *err = gui_parse_url(st->browser_url, &https, host, sizeof(host), &port, &path);
        if (err) {
            browser_set_plain(st, err);
            br_status(st, "浏览器 URL 错误");
            return;
        }
        uint32_t ip = 0;
        if (net_dns_resolve(host, &ip) < 0) {
            browser_set_plain2(st, "DNS 失败: ", net_last_error());
            br_status(st, "浏览器 DNS 失败");
            return;
        }
        int ok = https ? tls_https_get(host, ip, port, path, response, sizeof(response), &len)
                       : net_http_request("GET", host, ip, port, path, response, sizeof(response), &len);
        const char *tls_error = https ? tls_last_error() : "";
        if (ok < 0 && https) {
            port = 80;
            ok = net_http_request("GET", host, ip, port, path, response, sizeof(response), &len);
            if (ok == 0) br_status(st, "HTTPS 失败，已用 HTTP 回退");
        }
        if (ok < 0) {
            browser_set_plain2(st, "加载失败: ", https ? tls_error : net_last_error());
            br_status(st, "浏览器加载失败");
            return;
        }
        int code = br_http_status(response);
        char loc[160];
        int is_http_redir = (code == 301 || code == 302 || code == 303 ||
                             code == 307 || code == 308);
        if (is_http_redir) {
            if (br_http_location(response, loc, sizeof(loc)) < 0) break;
        } else {
            /* 非 3xx：看正文里有没有 <meta refresh> 跳转（无 JS 兜底）。
             * 没有就按普通页渲染。 */
            if (br_meta_refresh(http_body_ptr(response), loc, sizeof(loc)) < 0) break;
        }
        if (hop >= 3) {
            browser_set_plain(st, "重定向次数过多");
            br_status(st, "浏览器加载失败");
            return;
        }
        char next_url[BROWSER_URL_CAP];
        if (browser_resolve_href(st, loc, next_url, sizeof(next_url)) < 0) break;
        strncpy(st->browser_url, next_url, BROWSER_URL_CAP - 1);
        st->browser_url[BROWSER_URL_CAP - 1] = 0;
        st->browser_url_cursor = (uint32_t)strlen(st->browser_url);
    }
render_page:
    const char *body = http_body_ptr(response);
    st->browser_required_caps =
        hive_browser_detect_requirements(body, strlen(body));
    browser_text_from_html(body, st->browser_page, BROWSER_PAGE_CAP, &st->browser_page_len);
    browser_render_from_html(st, body, st->browser_render, BROWSER_PAGE_CAP, &st->browser_render_len);
    /* 执行内联 <script>（v1：无 src、quickjs ring3 运行时），输出追加到
     * 渲染流末尾。放在 og 卡片/图片之前，JS 写的标题优先。 */
    browser_exec_scripts(st, body);
    /* og 卡片：视频页（和其他 CSR 页面）body 几乎是空的，但 <head> 里有
     * og:title/og:description/og:image。若解析后正文很少，就把 og 信息作为
     * "视频卡片"插到渲染缓冲最前——用 memmove 挪出头部空间，写入一个
     * H2（标题）+ P（描述）+ 可选 IMG 块。 */
    if (g_br_og_title[0] && st->browser_render_len < 800) {
        static char og_card[512];
        uint32_t cp = 0;
#define OGBYTE(c) do { if (cp + 1 < sizeof(og_card)) og_card[cp++] = (char)(c); } while (0)
#define OGSTR(s)  do { for (const char *_p = (s); *_p; _p++) OGBYTE(*_p); } while (0)
        /* og:image → 先占一个图片槽（如果还有空槽且 og:image URL 有效） */
        if (g_br_og_image[0] && g_br_img_count < BR_IMG_SLOTS && !g_br_og_has_image) {
            int s = g_br_img_count++;
            uint32_t sl = (uint32_t)strlen(g_br_og_image);
            if (sl >= sizeof(g_br_img_src[0])) sl = sizeof(g_br_img_src[0]) - 1;
            memcpy(g_br_img_src[s], g_br_og_image, sl);
            g_br_img_src[s][sl] = 0;
            g_br_og_has_image = 1;
            /* 插入一个 BRK_IMG 块（槽 0x10|s） */
            OGBYTE(BRK_IMG); OGBYTE((char)(0x10 | (s + 1))); OGBYTE('\n');
        }
        /* H2 = 视频标题 */
        OGBYTE(BRK_H2);
        OGSTR(g_br_og_title);
        OGBYTE('\n');
        /* P = 描述（若有） */
        if (g_br_og_desc[0]) {
            OGBYTE(BRK_P);
            OGSTR(g_br_og_desc);
            OGBYTE('\n');
        }
        OGBYTE(BRK_HR); OGBYTE('\n');
#undef OGBYTE
#undef OGSTR
        og_card[cp] = 0;
        /* 把 og_card 插到 browser_render 最前面 */
        uint32_t rem = st->browser_render_len;
        if (cp + rem + 1 <= BROWSER_PAGE_CAP) {
            memmove(st->browser_render + cp, st->browser_render, rem + 1);
            memcpy(st->browser_render, og_card, cp);
            st->browser_render_len += cp;
        }
    }
    browser_fetch_images(st); /* 渲染登记了图片 src（含 og:image 占的槽），逐张抓取解码 */
    st->browser_scroll = 0;
    if (strcmp(st->status, "HTTPS 失败，已用 HTTP 回退") != 0)
        br_status(st, "浏览器加载完成");
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

/* browser_load_internal 的后台任务包装。task_create 的入口必须是
 * void (*)(void*)，这里把 gui_state_t* 通过 arg 传入。不要在此清
 * browser_loading：GUI 主循环用该标志检测任务终止并触发最终重绘；这里
 * 提前清零会让页面数据已经生成但屏幕永远停在加载前一帧。 */
static void browser_fetch_bg(void *arg) {
    gui_state_t *st = (gui_state_t *)arg;
    browser_load_internal(st, st->browser_push_hist);
}

static void browser_load(gui_state_t *st) {
    if (st->browser_loading) return; /* 上一次加载还没结束，忽略本次请求 */
    st->browser_loading = 1;
    st->browser_push_hist = 1;
    st->browser_load_tid = (uint32_t)task_create("browser-fetch", browser_fetch_bg, st);
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
    if (href[0] == '/' && href[1] == '/') {
        /* 协议相对 URL（"//news.baidu.com/x"）：继承当前页面的 scheme。
         * 大站 CDN 链接几乎全是这种写法；之前它落进"绝对路径"分支被拼成
         * "https://www.baidu.com//news.baidu.com"，点新闻直接跳错。 */
        const char *scheme = (strncmp(st->browser_url, "https://", 8) == 0) ? "https:" : "http:";
        uint32_t pos = 0;
        for (const char *p = scheme; *p && pos + 1 < out_cap; p++) out[pos++] = *p;
        for (uint32_t k = 0; href[k] && pos + 1 < out_cap; k++) out[pos++] = href[k];
        out[pos] = 0;
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
    if (st->browser_loading) return;
    if (st->browser_hist_pos <= 0) { st->status = "没有更早的页面"; return; }
    st->browser_hist_pos--;
    strncpy(st->browser_url, st->browser_hist[st->browser_hist_pos], BROWSER_URL_CAP - 1);
    st->browser_url[BROWSER_URL_CAP - 1] = 0;
    st->browser_url_cursor = (uint32_t)strlen(st->browser_url);
    st->browser_loading = 1;
    st->browser_push_hist = 0;
    st->browser_load_tid = (uint32_t)task_create("browser-fetch", browser_fetch_bg, st);
}

static void browser_go_forward(gui_state_t *st) {
    if (st->browser_loading) return;
    if (st->browser_hist_pos + 1 >= st->browser_hist_count) { st->status = "没有更新的页面"; return; }
    st->browser_hist_pos++;
    strncpy(st->browser_url, st->browser_hist[st->browser_hist_pos], BROWSER_URL_CAP - 1);
    st->browser_url[BROWSER_URL_CAP - 1] = 0;
    st->browser_url_cursor = (uint32_t)strlen(st->browser_url);
    st->browser_loading = 1;
    st->browser_push_hist = 0;
    st->browser_load_tid = (uint32_t)task_create("browser-fetch", browser_fetch_bg, st);
}

static void browser_save_page(gui_state_t *st) {
    browser_init(st);
    char path[GUI_PATH_MAX];
    if (gui_path_join(gui_file_path(st), "browser-page.txt", path, sizeof(path)) < 0) {
        br_status(st, "保存路径过长");
        return;
    }
    vfs_node_t *node = vfs_lookup(path);
    if (!node) node = vfs_create(path);
    if (!node || node->type != VFS_NODE_FILE ||
        vfs_truncate(node) < 0 ||
        vfs_write(node, 0, st->browser_page, st->browser_page_len) < 0) {
        br_status(st, "网页保存失败");
        return;
    }
    (void)fs_sync();
    (void)gui_select_path(st, path);
    br_status(st, "网页已保存到文件");
}


/* 链接点击命中区域——每次 draw_rendered_page 重新填充（滚动/窗口大小
 * 变化后旧的矩形就作废了），draw_browser_app 一帧内可能画两遍（滚动钳
 * 位重绘），以最后一次为准，供 hit_browser_link 做命中测试。 */

/* ── 浏览器布局/绘制层转发（实现见 src/gui/gui_browser_layout.c）──
 * 该层不引用 gui_state_t，只按坐标把排版结果画出来。
 * 保留同名 static inline 短名，既有调用点零改动。 */
static inline void browser_add_link_rect(int x, int y, int w, int h, int link_idx) { gb_browser_add_link_rect(x, y, w, h, link_idx); }
static inline int browser_draw_segment(int x, int w, int *cy, int row_unit, int scroll, int max_lines,
                                 int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len,
                                 const browser_style_t *bsp, int link_idx) { return gb_browser_draw_segment(x, w, cy, row_unit, scroll, max_lines, drawn_rows, buf, start, seg_len, bsp, link_idx); }
static inline uint32_t br_read_style(const char *buf, uint32_t len, uint32_t i, int type,
                              browser_style_t *bs, int *link_idx) { return gb_br_read_style(buf, len, i, type, bs, link_idx); }
static inline uint32_t br_utf8_next(const char *s, uint32_t pos, uint32_t end, uint32_t *cp_out) { return gb_br_utf8_next(s, pos, end, cp_out); }
static inline int br_cp_is_cjk(uint32_t cp) { return gb_br_cp_is_cjk(cp); }
static inline void br_draw_tok(int px, int py, int rowh, int rh, const char *tok, int tw,
                        const browser_style_t *bs, int link_idx) { gb_br_draw_tok(px, py, rowh, rh, tok, tw, bs, link_idx); }
static inline int browser_flow_block(int x, int w, int *cy, int row_unit, int scroll, int max_lines,
                              int *drawn_rows, const char *buf, const br_run_t *runs, int nrun) { return gb_browser_flow_block(x, w, cy, row_unit, scroll, max_lines, drawn_rows, buf, runs, nrun); }
static inline uint32_t br_table_cell(const char *buf, uint32_t start, uint32_t seg_len, int col,
                              char *out, uint32_t out_cap) { return gb_br_table_cell(buf, start, seg_len, col, out, out_cap); }
static inline void br_table_scan(const char *buf, uint32_t len, uint32_t pos, int avail_w,
                          int *col_w, int *ncols_out) { gb_br_table_scan(buf, len, pos, avail_w, col_w, ncols_out); }
static inline int br_draw_table_row(int x, int *cy, int row_unit, int scroll, int max_lines,
                             int *drawn_rows, const char *buf, uint32_t start, uint32_t seg_len,
                             const browser_style_t *bs, const int *col_w, int ncols,
                             int last_row) { return gb_br_draw_table_row(x, cy, row_unit, scroll, max_lines, drawn_rows, buf, start, seg_len, bs, col_w, ncols, last_row); }
static inline uint32_t br_card_read(const char *buf, uint32_t len, uint32_t i, br_card_t *c) { return gb_br_card_read(buf, len, i, c); }
static inline void br_card_text_copy(const char *buf, uint32_t start, uint32_t len,
                              char *out, uint32_t cap) { gb_br_card_text_copy(buf, start, len, out, cap); }
static inline int browser_draw_card_grid(int x, int w, int *cy, int row_unit, int scroll,
                                  int max_lines, int *drawn_rows,
                                  const char *buf, const br_card_t *cards, int ncards) { return gb_browser_draw_card_grid(x, w, cy, row_unit, scroll, max_lines, drawn_rows, buf, cards, ncards); }
static inline int browser_draw_hero_card(int x, int w, int *cy, int row_unit, int scroll,
                                  int max_lines, int *drawn_rows,
                                  const char *buf, const br_card_t *card) { return gb_browser_draw_hero_card(x, w, cy, row_unit, scroll, max_lines, drawn_rows, buf, card); }
static inline int draw_rendered_page(int x, int y, int w, int h, const char *buf, uint32_t len, int scroll) { return gb_draw_rendered_page(x, y, w, h, buf, len, scroll); }
static inline void br_icon_arrow(int cx, int cy, int dir, uint32_t col) { gb_br_icon_arrow(cx, cy, dir, col); }
static inline void br_icon_reload(int cx, int cy, uint32_t col, uint32_t bg) { gb_br_icon_reload(cx, cy, col, bg); }

static void draw_browser_app(int tx, int ty, int win_w, int win_h, gui_state_t *st) {
    browser_init(st);
    const hive_browser_backend_t *browser_backend = hive_browser_lite_backend();
    int wx = tx - 29;            /* 窗口内区左边缘（win_x+1） */
    int ww = win_w - 2;          /* 窗口内区宽度 */
    if (ww < 340) ww = 340;
    uint32_t strip_bg = UI_SURFACE;   /* Chrome 深色标签条 */
    uint32_t tool_bg  = UI_SURFACE2;   /* 工具栏 = 活动标签同色 */
    uint32_t pill_bg  = UI_SURFACE2;   /* 地址栏胶囊 */
    uint32_t icon_col = UI_FG;
    uint32_t icon_dis = UI_DIM;

    /* 标签页条 + 活动标签 */
    rect(wx, ty, ww, BR_TAB_H, strip_bg);
    int tab_w = 240;
    if (tab_w > ww - 80) tab_w = ww - 80;
    int tab_x = wx + 10;
    fill_round_rect(tab_x, ty + 4, tab_w, BR_TAB_H - 4, 9, tool_bg, RR_TOP);
    gui_fill_circle(tab_x + 16, ty + 4 + (BR_TAB_H - 4) / 2, 4, UI_ACCENT); /* favicon 占位点 */
    {
        /* 标签标题直接用页面标题（没有就显示"新标签页"） */
        const char *t = g_browser_page_title[0] ? g_browser_page_title : "新标签页";
        text_clipped(tab_x + 30, ty + 9, tab_x + tab_w - 24, t, UI_FG, 1);
    }
    /* 标签上的 × */
    {
        int xx = tab_x + tab_w - 16, xy = ty + 4 + (BR_TAB_H - 4) / 2 - 1;
        for (int i = 0; i < 5; i++) {
            rect(xx + i - 2, xy + i - 2, 1, 1, icon_col);
            rect(xx + 2 - i, xy + i - 2, 1, 1, icon_col);
        }
    }
    /* 新建标签 +（纯装饰，和 Chrome 一样放在标签右侧） */
    rect(tab_x + tab_w + 12, ty + BR_TAB_H / 2, 11, 1, icon_dis);
    rect(tab_x + tab_w + 17, ty + BR_TAB_H / 2 - 5, 1, 11, icon_dis);
    /* 明示当前渲染后端，避免把静态 SSR 摘要误认为 JavaScript/Vue 已运行。 */
    text_clipped(wx + ww - 62, ty + 9, wx + ww - 26,
                 browser_backend->name, UI_FG2, 1);

    /* 工具栏 */
    rect(wx, ty + BR_TAB_H, ww, BR_TOOL_H, tool_bg);
    int can_back = st->browser_hist_pos > 0;
    int can_fwd  = st->browser_hist_pos + 1 < st->browser_hist_count;
    br_icon_arrow(tx + BR_BACK_CX, ty + BR_BTN_CY, -1, can_back ? icon_col : icon_dis);
    br_icon_arrow(tx + BR_FWD_CX, ty + BR_BTN_CY, 1, can_fwd ? icon_col : icon_dis);
    /* 加载中时旋转动画替代重载图标，用8帧弧段表示旋转进度 */
    if (st->browser_loading) {
        static uint32_t spin_tick = 0;
        spin_tick++;
        int frame = (int)(spin_tick / 4) % 8;
        int cx = tx + BR_RELOAD_CX, cy = ty + BR_BTN_CY;
        /* 8个点均匀分布在圆周上，亮度递减 */
        for (int k = 0; k < 8; k++) {
            int angle_k = (k + frame) % 8; /* 相对亮度：angle_k==7最亮 */
            int brightness = 60 + angle_k * 24;
            uint32_t col = rgb(brightness, brightness, brightness);
            /* 用整数近似 sin/cos：8方向 */
            static const int dx8[8] = {0, 5, 7, 5, 0, -5, -7, -5};
            static const int dy8[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
            rect(cx + dx8[k]/2 - 1, cy + dy8[k]/2 - 1, 2, 2, col);
        }
    } else {
        br_icon_reload(tx + BR_RELOAD_CX, ty + BR_BTN_CY, icon_col, tool_bg);
    }
    /* 右侧 ⋮ 菜单点（装饰） */
    for (int i = 0; i < 3; i++)
        rect(wx + ww - 18, ty + BR_TAB_H + BR_TOOL_H / 2 - 5 + i * 4, 2, 2, icon_col);

    /* 胶囊地址栏 */
    int pill_x = tx + BR_RELOAD_CX + 24;
    int pill_r = wx + ww - 32;
    int pill_h = 28;
    int pill_y = ty + BR_TAB_H + (BR_TOOL_H - pill_h) / 2;
    fill_round_rect(pill_x, pill_y, pill_r - pill_x, pill_h, pill_h / 2, pill_bg, RR_ALL);
    /* https 锁 / http 空心圆点 */
    int ind_x = pill_x + 14;
    int ind_cy = pill_y + pill_h / 2;
    if (st->browser_url[0] == 'h' && st->browser_url[4] == 's') {
        rect(ind_x - 3, ind_cy - 1, 8, 6, UI_FG2);      /* 锁体 */
        rect(ind_x - 2, ind_cy - 4, 1, 3, UI_FG2);      /* 锁梁 */
        rect(ind_x + 3, ind_cy - 4, 1, 3, UI_FG2);
        rect(ind_x - 1, ind_cy - 5, 4, 1, UI_FG2);
    } else {
        gui_fill_circle(ind_x, ind_cy, 4, icon_dis);
        gui_fill_circle(ind_x, ind_cy, 2, pill_bg);
    }
    int url_x = pill_x + 28;
    text_clipped(url_x, pill_y + 5, pill_r - 10, st->browser_url, UI_FG, 1);
    /* 闪烁插入光标 */
    {
        uint32_t cur = st->browser_url_cursor;
        uint32_t n = (uint32_t)strlen(st->browser_url);
        if (cur > n) cur = n;
        char prefix[BROWSER_URL_CAP];
        memcpy(prefix, st->browser_url, cur);
        prefix[cur] = 0;
        int caret_x = url_x + text_width(prefix, 1);
        if (caret_x > pill_r - 10) caret_x = pill_r - 10;
        static uint32_t browser_caret_ticks = 0;
        browser_caret_ticks++;
        if ((browser_caret_ticks / 15) % 2)
            rect(caret_x + 1, pill_y + 5, 2, 18, UI_FG);
    }

    /* 页面区：白底（Chrome 默认渲染），铺满窗口剩余高度 */
    int page_y = ty + BR_TAB_H + BR_TOOL_H;
    /* 底部留 32px：那是 draw_app_window_body 之后统一画的窗口状态栏
     * （加载中/出错文本都显示在那里，Chrome 底部状态气泡的等价物）。 */
    int page_h = win_h - 42 - (BR_TAB_H + BR_TOOL_H) - 32;
    if (page_h < 80) page_h = 80;
    uint64_t missing_caps = st->browser_required_caps &
                            ~browser_backend->capabilities;
    if (missing_caps) {
        /* 列出全部缺失能力（用 “ · ” 连接），而不是只显示第一个。
         * 信号给用户：页面渲染到此为止，其余能力需要完整引擎。 */
        char caps_list[192];
        size_t off = 0;
        int sep_pending = 0, truncated = 0;
        for (uint64_t bit = 1; bit; bit <<= 1) {
            if (!(missing_caps & bit)) continue;
            const char *nm = hive_browser_capability_name(bit);
            size_t nm_len = strlen(nm);
            size_t need = (sep_pending ? 4 : 0) + nm_len + 1; /* " · "=4B */
            if (off + need + 5 > sizeof(caps_list)) {         /* 留 “ …” */
                truncated = 1;
                break;
            }
            if (sep_pending) {
                memcpy(caps_list + off, " · ", 4);
                off += 4;
            }
            memcpy(caps_list + off, nm, nm_len + 1);
            off += nm_len;
            sep_pending = 1;
        }
        if (truncated) {
            static const char dots[] = " …"; /* " …" */
            size_t dlen = sizeof(dots) - 1;
            if (off + dlen + 1 <= sizeof(caps_list)) {
                memcpy(caps_list + off, dots, dlen + 1);
                off += dlen;
            } else {
                caps_list[off] = 0;
            }
        }
        rect(wx, page_y, ww, 26, UI_FG);
        text(wx + 12, page_y + 5, "Lite 未执行页面能力:", UI_WARN, 1);
        text_clipped(wx + 160, page_y + 5, wx + ww - 12, caps_list,
                     UI_WARN, 1);
        page_y += 26;
        page_h -= 26;
        if (page_h < 54) page_h = 54;
    }
    int inner_h = page_h - 20;
    int total = 0, max_scroll = 0, visible_rows = 0;
    for (int pass = 0; pass < 2; pass++) {
        rect(wx, page_y, ww, page_h, UI_FG);
        total = draw_rendered_page(wx + 18, page_y + 12, ww - 44, inner_h,
                                   st->browser_render, st->browser_render_len,
                                   st->browser_scroll);
        int rh = gui_font_line_height() + 3;
        visible_rows = inner_h / rh;
        max_scroll = total - visible_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (st->browser_scroll <= max_scroll) break;
        st->browser_scroll = max_scroll;
    }
    /* Chrome 式滚动条：浅色细槽 + 灰滑块，内容装得下就不显示 */
    if (total > visible_rows && visible_rows > 0) {
        int sb_x = wx + ww - 10;
        int sb_y = page_y + 2;
        int sb_h = page_h - 4;
        rect(sb_x, sb_y, 6, sb_h, UI_FG);
        int thumb_h = sb_h * visible_rows / total;
        if (thumb_h < 20) thumb_h = 20;
        if (thumb_h > sb_h) thumb_h = sb_h;
        int thumb_y = sb_y;
        if (max_scroll > 0) thumb_y += (sb_h - thumb_h) * st->browser_scroll / max_scroll;
        fill_round_rect(sb_x, thumb_y, 6, thumb_h, 3, UI_FG2, RR_ALL);
    }
}

static void draw_window_frame(int x, int y, int win_w, int win_h, const char *title, int icon_id, int active, int state, int light) {
    (void)light;
    int R = (state == WM_STATE_MAXIMIZED) ? 0 : 8;
    if (state != WM_STATE_MAXIMIZED) {
        soft_shadow(x, y, win_w, win_h);
        soft_shadow(x + 2, y + 3, win_w - 4, win_h - 4);  // deeper drop shadow
    }

    uint32_t body_bg  = active ? (UI_SURFACE2)
                               : (UI_SURFACE);
    uint32_t title_bg = active ? (UI_SURFACE2)
                               : (UI_SURFACE2);
    uint32_t border_c = active ? UI_ACCENT
                               : (UI_SURFACE3);

    // Rounded Breeze frame: rounded body, rounded-top titlebar, 1px separator,
    // focus border on the straight edges (corners carried by the AA fill).
    fill_round_rect(x, y, win_w, win_h, R, body_bg, RR_ALL);
    fill_round_rect(x, y, win_w, WM_TITLE_H, R, title_bg, RR_TOP);
    rect(x + 1, y + WM_TITLE_H, win_w - 2, 1, UI_SURFACE3);
    rect(x + R, y, win_w - 2 * R, 1, border_c);
    rect(x + R, y + win_h - 1, win_w - 2 * R, 1, border_c);
    rect(x, y + R, 1, win_h - 2 * R, border_c);
    rect(x + win_w - 1, y + R, 1, win_h - 2 * R, border_c);

    // Window controls: flat, close=red, min/max neutral (no accent strip).
    uint32_t nbtn = active ? (UI_SURFACE3)
                           : (UI_SURFACE3);
    int btn_x = x + win_w - WM_BTN_W - 8;
    int btn_y = y + 7;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20,
                     UI_ERR, UI_ERR, UI_ERR, 0);
    draw_window_control_icon(btn_x + 7, btn_y + 6, GUI_CTRL_CLOSE, 0,
                             UI_FG);

    btn_x -= WM_BTN_W + WM_BTN_GAP;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20, nbtn, nbtn,
                     UI_SURFACE3, 0);
    draw_window_control_icon(btn_x + 6, btn_y + 4, GUI_CTRL_MAX,
                             state == WM_STATE_MAXIMIZED,
                             UI_FG);

    btn_x -= WM_BTN_W + WM_BTN_GAP;
    draw_panel_shell(btn_x, btn_y, WM_BTN_W, 20, nbtn, nbtn,
                     UI_SURFACE3, 0);
    draw_window_control_icon(btn_x + 6, btn_y + 4, GUI_CTRL_MIN, 0,
                             UI_FG);

    // Titlebar app icon (real atlas tile; fallback = rounded accent square)
    // + vertically-centered title.
    int ico = 16, icy = y + (WM_TITLE_H - ico) / 2;
    int icon_x = x + 10;
    int tile = 0;
    const uint32_t *src = gui_icon_tile(icon_id, &tile);
    if (src) {
        blit_icon_rgba(icon_x, icy, ico, src, tile);
        if (!active) {
            /* 非活跃窗口：图标叠一层标题栏底色半透明罩，视觉退后 */
            rect_alpha(icon_x - 1, icy - 1, ico + 2, ico + 2,
                       0x78000000u | (uint32_t)title_bg);
        }
    } else {
        uint32_t icon_c = active ? UI_ACCENT
                                 : (UI_DIM);
        fill_round_rect(icon_x, icy, ico, ico, 3, icon_c, RR_ALL);
        rect(icon_x + 4, icy + 5, ico - 8, ico - 9, UI_FG);
    }
    int tty = y + (WM_TITLE_H - gui_font_line_height()) / 2;
    text_clipped(x + 34, tty, x + win_w - WM_BTN_W * 3 - WM_BTN_GAP * 2 - 16,
                 title, active ? UI_FG : (UI_FG2), 1);
}

static void draw_panel_window(int tx, int ty, int win_w, int win_h, int w, int h, gui_state_t *st, int panel) {
    if (panel == PANEL_FILES) draw_files_panel(tx, ty, win_w, win_h, st);
    else if (panel == PANEL_DISK) draw_disk_panel(tx, ty, win_w);
    else if (panel == PANEL_SYS) draw_resource_panel(tx, ty, win_w, w, h);
    else draw_apps_panel(tx, ty, win_w, st);
}

/* 浏览器模块适配：只暴露 draw。
 *
 * 输入（键/鼠标）仍由 gui.c 主循环里的既有分发处理——浏览器的交互逻辑
 * 与窗口命中测试（地址栏、导航按钮、链接热点）深度耦合在那边，硬抽成
 * 回调需要先把它那约 20 个状态字段从 gui_state_t 拆成私有 ctx（P5 的活）。
 * 这里先给出 draw 回调 + 元数据，使浏览器重新出现在应用列表与开始菜单；
 * on_click/on_key 保持为 0，主循环的既有路径继续生效。 */
void browser_module_draw(gui_state_t *st, int tx, int ty, int win_w, int win_h) {
    draw_browser_app(tx, ty, win_w, win_h, st);
}

static void draw_app_window_body(int tx, int ty, int win_w, int win_h, gui_state_t *st, int mode) {
    /* 全部应用（含浏览器）都经注册表分发；此处不再有 mode 特例。 */
    (void)gui_app_draw(st, mode, tx, ty, win_w, win_h);
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
                      win->kind == WM_WIN_PANEL ? panel_icon_id(win->mode)
                                                : app_icon_id(win->mode),
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
                    0xD0000000 | (UI_SURFACE),
                    0xD0000000 | (UI_BG));
    rect(win_x + 1, win_y + win_h - 32, win_w - 2, 1, UI_LINE2);
    rect(win_x + 1, win_y + win_h - 31, win_w - 2, 1, idx == st->wm.active_window ? cyber_neon_pink(st->theme_light) : (UI_SURFACE2));
    rect(win_x + 1, win_y + win_h - 1, win_w - 2, 1, UI_BG);
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
    uint32_t bg = (0xF6u << 24) | (light ? 0xF5F6F8u : 0x202428u);
    fill_round_rect(px, py, BR_POPUP_W, BR_POPUP_H, 10, bg, RR_ALL);
    uint32_t bd = UI_SURFACE3;
    border(px, py, BR_POPUP_W, BR_POPUP_H, bd);

    char line[24];
    uint32_t pos = 0; line[0] = 0;
    append_str(line, sizeof(line), &pos, "亮度  ");
    append_uint(line, sizeof(line), &pos, (uint32_t)st->brightness);
    append_char(line, sizeof(line), &pos, '%');
    text(px + 18, py + 14, line, UI_FG, 1);

    int tx0, tx1, ty;
    brightness_track_xy(px, py, &tx0, &tx1, &ty);
    uint32_t track_bg = UI_SURFACE3;
    fill_round_rect(tx0, ty - 3, tx1 - tx0, 6, 3, track_bg, RR_ALL);
    int fill_w = (tx1 - tx0) * (st->brightness - BR_MIN) / (BR_MAX - BR_MIN);
    if (fill_w > 0)
        fill_round_rect(tx0, ty - 3, fill_w, 6, 3, UI_ACCENT, RR_ALL);
    int thumb_x = tx0 + fill_w;
    gui_fill_circle(thumb_x, ty, 8, UI_FG);
    gui_fill_circle(thumb_x, ty, 6, UI_ACCENT);
}

static void draw_taskbar(int w, int h, const gui_state_t *st) {
    int light = st->theme_light;
    uint32_t accent = UI_ACCENT;
    int wins[WM_MAX_WINDOWS];
    int n = taskbar_windows(st, wins, WM_MAX_WINDOWS);
    int item_count = 1 + TB_PIN_COUNT + n;

    for (int i = 0; i < item_count; i++) {
        int rx, ry;
        taskbar_item_xy(i, item_count, w, h, &rx, &ry);
        if (i == 0) {
            if (st->wm.start_menu_open)
                fill_round_rect(rx, ry, TB_BTN, TB_BTN, UI_RADIUS_SM, (0x44u << 24) | 0xFFFFFFu, RR_ALL);
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
                 act ? accent : (UI_FG2));
        }
    }

    /* 亮度图标：贴在显示桌面竖条左边 */
    int bix, biy;
    brightness_icon_xy(w, h, &bix, &biy);
    if (st->brightness_popup_open)
        fill_round_rect(bix - 4, biy - 4, BR_ICON_SZ + 8, BR_ICON_SZ + 8, UI_RADIUS_SM, (0x44u << 24) | 0xFFFFFFu, RR_ALL);
    draw_brightness_icon(bix, biy, BR_ICON_SZ, light ? cyber_text(1) : UI_FG);

    /* Win11 风格：时间在上、日期在下，各自右对齐，给亮度图标+显示桌面竖条让出空间 */
    char tline[32], dline[40];
    time_line(tline, sizeof(tline), st->taskbar_show_seconds);
    date_line(dline, sizeof(dline));
    uint32_t tcol = light ? cyber_text(1) : UI_FG;
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
         UI_LINE2);
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
    if (!gd_surface_ptr() || !src) return;
    for (int yy = 0; yy < sh; yy++) {
        int py = dy + yy;
        if (py < 0 || py >= gd_surface_h()) continue;
        uint32_t *drow = gd_surface_ptr() + (uint32_t)py * gd_surface_pitch();
        const uint32_t *srow = src + (uint32_t)yy * sw;
        for (int xx = 0; xx < sw; xx++) {
            int px = dx + xx;
            if (px < 0 || px >= gd_surface_w()) continue;
            drow[px] = srow[xx] | 0xFF000000;
        }
    }
}

/* 画一个应用窗口：标题栏（标题 + 关闭按钮）+ 内容表面 + 边框 */
static void draw_one_app_window(winsrv_window_t *win) {
    if (!win) return;
    if (win->win.state == WINSRV_STATE_MINIMIZED) {
        win->win.dirty = 0;
        return;
    }
    int wx = win->win.x, wy = win->win.y, ww = win->win.w, wh = win->win.h;
    int th = APPWIN_TITLE_H;
    soft_shadow(wx - 2, wy - th - 2, ww + 4, wh + th + 6);
    fill_round_rect(wx, wy - th, ww, th, 6, UI_SURFACE, RR_TOP);
    text(wx + 10, wy - th + (th - gui_font_line_height()) / 2,
         win->win.title[0] ? win->win.title : "应用", UI_FG, 1);
    /* 关闭按钮 */
    int cbx = wx + ww - 20;
    text(cbx, wy - th + (th - gui_font_line_height()) / 2, "✕", UI_ERR, 1);
    /* 内容 */
    blit_app_surface(wx, wy, win->win.surface, ww, wh);
    border(wx, wy, ww, wh, UI_SURFACE3);
    win->win.dirty = 0;
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
        /* gd_surface_ptr() alloc failed (OOM): draw_gui_screen drew straight
         * through fb_put_pixel/fb_fill_rect, which now land in the TUI's
         * off-screen back-buffer instead of the real framebuffer (see
         * graphics.c). gui_present_surface() no-ops without a surface, so
         * flush that back-buffer to the screen ourselves. */
        if (!gd_surface_ptr()) console_present();
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
    if (!gd_surface_ptr()) console_present();
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
        text(lx + 1, iy + isz + 6, d->label, UI_BG, 1);          // shadow
        text(lx, iy + isz + 5, d->label, UI_FG, 1);
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
            rect(px + 6, yy, pw - 12, 1, UI_ACCENT);
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

void gui_select_file(gui_state_t *st, int index) {
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
    return gui_app_meta_at((uint32_t)st->selected_app);
}

static void gui_open_selected_app(gui_state_t *st) {
    const gui_app_meta_t *app = selected_gui_app(st);
    if (!app) {
        st->status = "未选择应用";
        return;
    }
    if (gui_open_window(st, WM_WIN_APP, app->mode, 0) < 0) return;
    if (app->mode == GUI_APP_SNAKE) app_snake_reset(st);
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
        app_code_set_path(st, full);
        if (gui_open_window(st, WM_WIN_APP, GUI_APP_CODE, 0) >= 0)
            st->status = "已用代码工作台打开";
        return;
    }
    if (gui_has_suffix(full, ".bmp") || gui_has_suffix(full, ".png") ||
        gui_has_suffix(full, ".jpg") || gui_has_suffix(full, ".jpeg")) {
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
    } else if (st->app_mode == GUI_APP_BROWSER) {
        /* 注意方向和上面几个分支相反（是 -= steps 不是 += steps）：这几个
         * 分支原本的 +steps 语义是"steps 为正=列表下一项/编辑器视图下移"，
         * 用真实滚轮实测（QEMU input-send-event wheel-down/up）验证过，
         * wheel-down（应让网页显示更靠后内容、滚动位置增大）此时 steps
         * 恰好是 -1，wheel-up 是 +1——和其它分支的符号是反的，所以这里
         * 单独取负号，不动 steps 本身（其它分支继续用原来的语义）。 */
        st->browser_scroll -= steps;
        if (st->browser_scroll < 0) st->browser_scroll = 0;
        st->status = "滚动网页";
    }
}

static void handle_action(gui_state_t *st, int action) {
    if (action == 0) app_notes_create(st);
    else if (action == 1) gui_open_selected_file(st);
    else if (action == 2) app_notes_append(st);
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


/* 重新加载（⟳）按钮命中测试：Chrome 式工具栏的第三个圆形按钮，圆心/半径
 * 与 draw_browser_app 的 BR_RELOAD_CX/BR_BTN_CY/BR_BTN_R 保持一致。 */
static int hit_browser_load_button(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_BROWSER) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_w; (void)win_h;
    int cx = win_x + 30 + BR_RELOAD_CX;
    int cy = win_y + 42 + BR_BTN_CY;
    return (mx >= cx - BR_BTN_R && mx < cx + BR_BTN_R &&
            my >= cy - BR_BTN_R && my < cy + BR_BTN_R);
}

/* 0=没点中，1=后退，2=前进；圆心与 draw_browser_app 的 BR_BACK_CX/BR_FWD_CX
 * 保持一致（命中区按外接正方形算，圆形按钮的常规近似）。 */
static int hit_browser_nav_button(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return 0;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_BROWSER) return 0;

    int win_x, win_y, win_w, win_h;
    gui_window_metrics((gui_state_t *)st, w, h, win, st->wm.active_window, &win_x, &win_y, &win_w, &win_h);
    (void)win_w; (void)win_h;
    int cy = win_y + 42 + BR_BTN_CY;
    if (my < cy - BR_BTN_R || my >= cy + BR_BTN_R) return 0;
    int bx = win_x + 30 + BR_BACK_CX;
    int fx = win_x + 30 + BR_FWD_CX;
    if (mx >= bx - BR_BTN_R && mx < bx + BR_BTN_R) return 1;
    if (mx >= fx - BR_BTN_R && mx < fx + BR_BTN_R) return 2;
    return 0;
}

/* 点中某个渲染过的链接就返回它的下标，否则 -1；命中矩形是
 * draw_rendered_page() 上一次绘制时填进 g_browser_link_rects 的。 */
static int hit_browser_link(int w, int h, const gui_state_t *st, int mx, int my) {
    if (st->wm.active_window < 0 || st->wm.active_window >= st->wm.window_count) return -1;
    const wm_window_t *win = wm_get_window((wm_state_t *)&st->wm, st->wm.active_window);
    if (!win || win->kind != WM_WIN_APP || win->mode != GUI_APP_BROWSER) return -1;
    (void)w; (void)h;
    for (int i = 0; i < gb_link_rect_count(); i++) {
        browser_link_rect_t *r = gb_link_rect_at(i);
        if (mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h) return r->link_idx;
    }
    return -1;
}


static void handle_app_key(gui_state_t *st, int key) {
    if (key == KB_KEY_F6) {
        gui_focus_next_window(st, 1);
        if (st->wm.window_count > 1) st->switcher_ticks = 40;
        return;
    }
    gui_sync_focus(st);
    if (gui_app_handle_key(st, key)) return;
    if (st->app_mode == GUI_APP_BROWSER) {
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
        } else if (key == GUI_KEY_DELETE) {
            /* 之前地址栏只认 Backspace，Delete 完全没反应 */
            if (st->browser_url_cursor < n) {
                memmove(st->browser_url + st->browser_url_cursor,
                        st->browser_url + st->browser_url_cursor + 1,
                        n - st->browser_url_cursor);
            }
        } else if (key == GUI_KEY_HOME) {
            st->browser_url_cursor = 0;
        } else if (key == GUI_KEY_END) {
            st->browser_url_cursor = n;
        } else if (key == GUI_KEY_LEFT) {
            if (st->browser_url_cursor > 0) st->browser_url_cursor--;
        } else if (key == GUI_KEY_RIGHT) {
            if (st->browser_url_cursor < n) st->browser_url_cursor++;
        } else if (key == GUI_KEY_UP) {
            if (st->browser_scroll > 0) st->browser_scroll--;
        } else if (key == GUI_KEY_DOWN) {
            st->browser_scroll++;
        } else if (key == GUI_KEY_PGUP) {
            /* 整页滚动：一次滚 10 行（视口高度因窗口而异，取个稳妥值） */
            st->browser_scroll -= 10;
            if (st->browser_scroll < 0) st->browser_scroll = 0;
        } else if (key == GUI_KEY_PGDOWN) {
            st->browser_scroll += 10;  /* 越界由 draw_browser_app 的钳制兜住 */
        } else if (key >= 32 && key <= 126) {
            if (n + 1 < BROWSER_URL_CAP) {
                memmove(st->browser_url + st->browser_url_cursor + 1,
                        st->browser_url + st->browser_url_cursor,
                        n - st->browser_url_cursor + 1);
                st->browser_url[st->browser_url_cursor] = (char)key;
                st->browser_url_cursor++;
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
        app_notes_create(st);
        gui_open_panel_window(st, PANEL_FILES);
    } else if (key == 'a') {
        app_notes_append(st);
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
        uint32_t accent = win->kind == WM_WIN_PANEL ? UI_OK : cyber_neon_cyan(st->theme_light);
        if (active) {
            vgradient(ox + pad, ry, ow - pad * 2, row_h - 4,
                      UI_SURFACE,
                      UI_BG);
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

/* ── 磁盘 .hax 应用（hpt 安装到 /bin，GUI 类）───────────────
 * hpt install 的 HIVE 应用落在 /bin/<name>.hax，不在 hax 表里；
 * 开始菜单打开时扫描 /bin，把带 .haxmeta 且声明 GUI 类的应用列出来，
 * 启动时用 elf64_load_and_spawn 直接加载（应用自身 hax_win_open
 * 连接合成器）。 */
#define GUI_DISK_APP_MAX 16
static char disk_apps[GUI_DISK_APP_MAX][32];
static int disk_app_count;
static uint8_t disk_hax_buf[65536];

static int disk_hax_kind(const uint8_t *data, size_t len,
                         char *name_out, size_t name_cap) {
    if (!data || len < 64 || memcmp(data, "\x7f" "ELF\x02\x01", 6) != 0)
        return 0;
    uint64_t shoff;
    uint16_t shentsize, shentnum, shstrndx;
    __builtin_memcpy(&shoff, data + 0x28, 8);
    __builtin_memcpy(&shentsize, data + 0x3A, 2);
    __builtin_memcpy(&shentnum, data + 0x3C, 2);
    __builtin_memcpy(&shstrndx, data + 0x3E, 2);
    if (shentsize < 64 || shstrndx >= shentnum ||
        shoff + (uint64_t)shentsize * shentnum > len)
        return 0;
    const uint8_t *sh0 = data + shoff;
    uint64_t shstr_off, shstr_size;
    __builtin_memcpy(&shstr_off,
                     sh0 + (uint64_t)shstrndx * shentsize + 0x18, 8);
    __builtin_memcpy(&shstr_size,
                     sh0 + (uint64_t)shstrndx * shentsize + 0x20, 8);
    if (shstr_off + shstr_size > len) return 0;
    for (uint16_t i = 0; i < shentnum; i++) {
        const uint8_t *sh = sh0 + (uint64_t)i * shentsize;
        uint32_t name_off;
        __builtin_memcpy(&name_off, sh, 4);
        if (name_off >= shstr_size) continue;
        const char *secname = (const char *)(data + shstr_off + name_off);
        if (strcmp(secname, ".haxmeta") != 0) continue;
        uint64_t sec_off, sec_size;
        __builtin_memcpy(&sec_off, sh + 0x18, 8);
        __builtin_memcpy(&sec_size, sh + 0x20, 8);
        if (sec_size < 104 || sec_off + sec_size > len) return 0;
        const uint8_t *meta = data + sec_off;
        uint32_t magic, kind;
        __builtin_memcpy(&magic, meta, 4);
        __builtin_memcpy(&kind, meta + 4, 4);
        if (magic != 0x4D584148u || kind == 0) return 0;
        if (name_out && name_cap) {
            const char *nm = (const char *)(meta + 8);
            size_t n = 0;
            while (n + 1 < name_cap && nm[n] && n < 32) { name_out[n] = nm[n]; n++; }
            name_out[n] = 0;
        }
        return (int)kind;
    }
    return 0;
}

static void gui_scan_disk_apps(void) {
    disk_app_count = 0;
    char full[256];
    if (vfs_resolve_path("/", "/bin", full, sizeof(full)) < 0) return;
    for (uint32_t i = 0; disk_app_count < GUI_DISK_APP_MAX; i++) {
        char name[VFS_MAX_NAME];
        uint32_t type;
        if (vfs_readdir_at(full, i, name, &type) != 0) break;
        if (type != VFS_NODE_FILE) continue;
        size_t nl = strlen(name);
        if (nl < 5 || name[nl - 4] != '.' || name[nl - 3] != 'h' ||
            name[nl - 2] != 'a' || name[nl - 1] != 'x')
            continue;
        char path[96];
        size_t pn = nl < 90 ? nl : 90;
        memcpy(path, "/bin/", 5);
        memcpy(path + 5, name, pn);
        path[5 + pn] = 0;
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        size_t total = 0;
        long rn;
        while (total < sizeof(disk_hax_buf) &&
               (rn = read(fd, disk_hax_buf + total,
                          sizeof(disk_hax_buf) - total)) > 0)
            total += (size_t)rn;
        close(fd);
        if (total == 0) continue;
        char app_name[32] = {0};
        int kind = disk_hax_kind(disk_hax_buf, total, app_name,
                                 sizeof(app_name));
        if (!(kind & HAX_KIND_GUI)) continue;
        const char *use = app_name[0] ? app_name : name;
        size_t un = strlen(use);
        if (un >= 32) un = 31;
        memcpy(disk_apps[disk_app_count], use, un);
        disk_apps[disk_app_count][un] = 0;
        disk_app_count++;
    }
}

/* 启动器动态总高度：内置 13 项 + 追加的 .hax 行 */
/* ── 开始菜单：统一应用条目（内置 + .hax + 磁盘 hpt 应用）+ 搜索过滤 ─
 * 键盘是 ASCII，应用名是中文，故每个内置应用附带 ASCII/拼音关键词供搜索。 */
enum { SM_K_PANEL = 0, SM_K_APP = 1, SM_K_HAX = 2, SM_K_DISK = 3 };
typedef struct {
    const char *name; const char *kw; int kind; int mode; int icon;
} sm_entry_t;

/* 开始菜单条目：只保留 4 个"面板"（它们是 GUI 内部模式，不是应用模块）。
 * 应用条目**不再硬编码**——由 sm_entry_at() 动态从模块注册表派生。
 *
 * 重构前这里是第二份应用清单，新增应用必须同步，漏掉就"搜不到"（实测
 * 踩到 UI 组件页）。现在应用的名称/关键词/图标全部来自 gui_app_module_t。 */
static const sm_entry_t sm_builtin[] = {
    {"文件管理器", "file files fm wenjian",    SM_K_PANEL, PANEL_FILES,     ICON_FILES},
    {"磁盘",       "disk cipan",               SM_K_PANEL, PANEL_DISK,      ICON_DISK},
    {"资源",       "resource sys ziyuan",      SM_K_PANEL, PANEL_SYS,       ICON_SYS},
    {"应用",       "app apps yingyong",        SM_K_PANEL, PANEL_APPS,      ICON_APPS},
};
#define SM_BUILTIN_N ((int)(sizeof(sm_builtin) / sizeof(sm_builtin[0])))

static int sm_total_entries(void) {
    return SM_BUILTIN_N + gui_app_module_count() + gui_hax_count() + disk_app_count;
}

static int sm_entry_tail_at(int k, sm_entry_t *out);

static int sm_entry_at(int idx, sm_entry_t *out) {
    if (idx < 0) return 0;
    if (idx < SM_BUILTIN_N) { *out = sm_builtin[idx]; return 1; }

    /* 应用条目从模块注册表派生：name/kw/icon 都在那里，只有一份来源 */
    {
        int k = idx - SM_BUILTIN_N;
        int app_n = gui_app_module_count();
        if (k < app_n) {
            const gui_app_module_t *m = gui_app_module_at(k);
            if (!m) return 0;
            out->name = m->name;
            out->kw   = (m->kw && m->kw[0]) ? m->kw : m->name;
            out->kind = SM_K_APP;
            out->mode = m->mode;
            out->icon = m->icon;
            return 1;
        }
        k -= app_n;
        /* 其余（hax / 磁盘应用）沿用原逻辑 */
        return sm_entry_tail_at(k, out);
    }
}

/** 注册表之后的部分：hax 应用与磁盘应用（原逻辑，抽出来保持可读） */
static int sm_entry_tail_at(int k, sm_entry_t *out) {
    if (k < 0) return 0;
    const hax_app_entry_t *e = gui_hax_at(k);
    if (e) {
        out->name = e->name; out->kw = e->name;
        out->kind = SM_K_HAX; out->mode = k; out->icon = ICON_APPS;
        return 1;
    }
    int d = k - gui_hax_count();
    if (d >= 0 && d < disk_app_count) {
        out->name = disk_apps[d]; out->kw = disk_apps[d];
        out->kind = SM_K_DISK; out->mode = d; out->icon = ICON_APPS;
        return 1;
    }
    return 0;
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
    else if (e->kind == SM_K_DISK) {
        /* hpt 安装到 /bin 的 GUI 应用：从磁盘加载并异步启动（非阻塞，
         * 应用自身 hax_win_open 连接合成器，与 SM_K_HAX 的并发窗口应用
         * 同一套非阻塞语义）。 */
        char path[96];
        size_t dn = strlen(disk_apps[e->mode]);
        if (dn > 90) dn = 90;
        memcpy(path, "/bin/", 5);
        memcpy(path + 5, disk_apps[e->mode], dn);
        path[5 + dn] = 0;
        int fd = open(path, O_RDONLY);
        if (fd < 0) return;
        size_t total = 0;
        long rn;
        while (total < sizeof(disk_hax_buf) &&
               (rn = read(fd, disk_hax_buf + total,
                          sizeof(disk_hax_buf) - total)) > 0)
            total += (size_t)rn;
        close(fd);
        if (total == 0) return;
        char *av[1]; av[0] = disk_apps[e->mode];
        if (elf64_load_and_spawn(disk_hax_buf, total, av, 0, av[0]) >= 0)
            gui_toast(st, "已启动 ", av[0]);
        return;
    } else {
        const hax_app_entry_t *he = gui_hax_at(e->mode);
        if (he) {
            char *av[1]; av[0] = (char *)he->name;
            if (he->kind & HAX_KIND_GUI_WIN) {
                /* 并发窗口应用（hax_win_open）：必须非阻塞启动。
                 * 它靠合成器主循环持续跑帧才能把窗口内容合成到屏幕上，同步
                 * 等待会卡住本函数所在的合成器任务，窗口画面永远显示不出来。 */
                hax_app_spawn(he->name, 1, av);
                gui_toast(st, "已启动 ", he->name);
            } else {
                /* 其余应用（纯控制台或独占画布风格，即使声明了 HAX_KIND_GUI）：
                 * 同步等待退出，而非 fire-and-forget 的
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
    fill_round_rect(ox, oy, mw, mh, UI_RADIUS_SM, (0xF8u << 24) | (light ? 0xF4F6F8u : 0x222A33u), RR_ALL);
    border(ox, oy, mw, mh, UI_SURFACE3);
    for (int i = 0; i < n; i++) {
        int iy = oy + 4 + i * CTX_ITEM_H;
        text(ox + 14, iy + (CTX_ITEM_H - gui_font_line_height()) / 2,
             ctx_item_label(st->ctx_open, i),
             UI_FG, 1);
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

    uint32_t accent = UI_ACCENT;
    uint32_t fg     = UI_FG;
    uint32_t dim    = UI_DIM;
    fill_round_rect(ox, oy, ow, oh, UI_RADIUS, (0xF6u << 24) | (light ? 0xF5F6F8u : 0x202428u), RR_ALL);
    uint32_t bd = UI_SURFACE3;
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
    fill_round_rect(nx_prev, nav_y, btn, btn, 6, UI_SURFACE2, RR_ALL);
    fill_round_rect(nx_next, nav_y, btn, btn, 6, UI_SURFACE2, RR_ALL);
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
                 UI_FG, 1);
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
    uint32_t bg = (0xF6u << 24) | (light ? 0xF5F6F8u : 0x202428u);
    fill_round_rect(ox, oy, SM_W, mh, 12, bg, RR_ALL);
    uint32_t bd = UI_SURFACE3;
    /* top/bottom border lines for rounded rect */
    rect(ox + 12, oy, SM_W - 24, 1, bd);
    rect(ox + 12, oy + mh - 1, SM_W - 24, 1, bd);
    rect(ox, oy + 12, 1, mh - 24, bd);
    rect(ox + SM_W - 1, oy + 12, 1, mh - 24, bd);

    /* ── search bar ── */
    int sx = ox + SM_PAD, sy = oy + SM_SEARCH_TOP;
    int sw = SM_W - 2 * SM_PAD;
    vgradient(sx, sy, sw, SM_SEARCH_H,
              UI_SURFACE,
              UI_SURFACE);
    border(sx, sy, sw, SM_SEARCH_H, UI_SURFACE3);
    if (st->sm_search_len > 0) {
        text(sx + 12, sy + (SM_SEARCH_H - gui_font_line_height()) / 2,
             st->sm_search, UI_FG, 1);
        /* 光标 */
        int cxp = sx + 12 + text_width(st->sm_search, 1) + 1;
        rect(cxp, sy + 8, 1, SM_SEARCH_H - 16, UI_FG);
    } else {
        text(sx + 12, sy + (SM_SEARCH_H - gui_font_line_height()) / 2,
             "搜索应用…（输入名称或拼音，Enter 启动）",
             UI_DIM, 1);
    }
    /* magnifier icon */
    int mg = sy + SM_SEARCH_H / 2;
    int mr = 6;
    /* simple circle + line for magnifier */
    rect(sx + sw - 22 + mr/2 + 2, mg - mr, 1, mr * 2, UI_LINE2);
    rect(sx + sw - 22 - mr, mg - 1, mr * 2, 1, UI_LINE2);

    /* ── "已固定" section label ── */
    int lty = oy + SM_PIN_TOP;
    text(ox + SM_PAD, lty + (SM_PIN_LABEL_H - gui_font_line_height()) / 2,
         "已固定", UI_FG2, 1);
    text(ox + SM_W - SM_PAD - text_width("全部 >", 1),
         lty + (SM_PIN_LABEL_H - gui_font_line_height()) / 2,
         "全部 >", UI_ACCENT, 1);

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
        if (e.kind == SM_K_HAX) {
            /* 自做的 .hax 可内嵌图标（.haxicon 段，构建期由 genhax.py
             * 解码成 64x64 RGBA 瓦片）；有则优先显示，否则回退默认图集图标。 */
            const hax_app_entry_t *he = gui_hax_at(e.mode);
            if (he) {
                int tsz = 0;
                const uint32_t *tic = hax_app_icon_at(
                    (uint32_t)(he - hax_app_table), &tsz);
                if (tic) blit_icon_rgba(ix, iy, isz, tic, tsz);
                else blit_icon(ix, iy, isz, e.icon);
            } else {
                blit_icon(ix, iy, isz, e.icon);
            }
        } else {
            blit_icon(ix, iy, isz, e.icon);
        }
        int tw = text_width(e.name, 1);
        int lx = cx - tw / 2;
        if (lx < ox + SM_PAD) lx = ox + SM_PAD;
        text_clipped(lx, iy + isz + 4, ox + SM_W - SM_PAD,
                     e.name, UI_FG, 1);
    }
    if (vis == 0) {
        text(ox + SM_PAD, oy + SM_GRID_TOP + 24, "无匹配应用",
             UI_FG2, 1);
    }

    /* ── separator（随额外行下移） ── */
    int sep_y = oy + SM_SEP_Y + 2 + extra;
    rect(ox + SM_PAD, sep_y, SM_W - 2 * SM_PAD, 1,
         UI_SURFACE2);

    /* ── bottom bar（随额外行下移） ── */
    int bar_y = oy + SM_BAR_TOP + extra;
    /* left: user icon + name */
    int uisz = ui_s(28);
    int uix = ox + SM_PAD, uiy = bar_y + (SM_BAR_H - uisz) / 2;
    draw_user_avatar(uix, uiy, uisz,
                     UI_ACCENT,
                     UI_DIM);
    text(uix + uisz + 8, bar_y + (SM_BAR_H - gui_font_line_height()) / 2,
         "用户", UI_FG, 1);

    /* right: Shell + Power buttons */
    int btn_w = 88, btn_h = 30, gap = 8;
    int power_x = ox + SM_W - SM_PAD - btn_w;
    int shell_x = power_x - btn_w - gap;
    int btn_y = bar_y + (SM_BAR_H - btn_h) / 2;

    /* Shell button */
    vgradient(shell_x, btn_y, btn_w, btn_h,
              UI_SURFACE2,
              UI_SURFACE2);
    border(shell_x, btn_y, btn_w, btn_h, UI_SURFACE3);
    {
        int tw = text_width("返回Shell", 1);
        text(shell_x + (btn_w - tw) / 2, btn_y + (btn_h - gui_font_line_height()) / 2,
             "返回Shell", UI_FG, 1);
    }

    /* Power button */
    vgradient(power_x, btn_y, btn_w, btn_h,
              UI_ERR,
              UI_SURFACE3);
    border(power_x, btn_y, btn_w, btn_h, UI_SURFACE2);
    {
        /* power symbol: circle arc + vertical bar */
        int pcx = power_x + btn_w / 2;
        int pcy = btn_y + btn_h / 2;
        rect(pcx - 1, pcy - 7, 2, 8, UI_FG);
        /* arc approximation via horizontal lines */
        for (int dy = -5; dy <= 5; dy++) {
            int dx = dy < 0 ? (6 - dy/2) : (6 + dy/2);
            if (dx > 8) dx = 8;
            rect(pcx - dx, pcy + dy, 1, 1, UI_FG);
            rect(pcx + dx, pcy + dy, 1, 1, UI_FG);
        }
        text(power_x + 20, btn_y + (btn_h - gui_font_line_height()) / 2,
             "关机", UI_FG, 1);
    }
}

/* 进入 GUI 的欢迎窗口 —— 与桌面其余部分统一的圆角现代风格（蓝/深主题）。 */
// 与 draw_window_frame 完全一致的 Breeze 窗框配色/圆角/标题栏样式（中性标题
// 栏 + 强调色边框 + 左上角圆角图标），让欢迎窗口看起来就是桌面里的一个普通
// 窗口，而不是一张单独的强调色卡片。
static void draw_splash_window(int w, int h, int ticks, int light) {
    (void)light;
    int sw = ui_s(440), sh = ui_s(208);
    int sx = (w - sw) / 2, sy = (h - sh) / 2;
    int title_h = WM_TITLE_H;
    int R = 8;
    uint32_t accent   = UI_ACCENT;
    uint32_t body     = UI_SURFACE2;
    uint32_t title_bg = UI_SURFACE2;
    uint32_t tcol     = UI_FG;
    uint32_t scol     = UI_FG2;
    int lh = gui_font_line_height();

    soft_shadow(sx, sy, sw, sh);
    soft_shadow(sx + 2, sy + 3, sw - 4, sh - 4);
    fill_round_rect(sx, sy, sw, sh, R, body, RR_ALL);
    fill_round_rect(sx, sy, sw, title_h, R, title_bg, RR_TOP);
    rect(sx + 1, sy + title_h, sw - 2, 1, UI_SURFACE3);
    rect(sx + R, sy, sw - 2 * R, 1, accent);
    rect(sx + R, sy + sh - 1, sw - 2 * R, 1, accent);
    rect(sx, sy + R, 1, sh - 2 * R, accent);
    rect(sx + sw - 1, sy + R, 1, sh - 2 * R, accent);

    /* 标题栏：圆角图标 + 标题 + 右侧版本号（与 draw_window_frame 一致） */
    int ico = 14, icy = sy + (title_h - ico) / 2;
    fill_round_rect(sx + 12, icy, ico, ico, 3, accent, RR_ALL);
    rect(sx + 12 + 4, icy + 5, ico - 8, ico - 9, UI_FG);
    text(sx + 34, sy + (title_h - lh) / 2, "欢迎使用 HBOS", UI_FG, 1);
    {
        const char *v = HBOS_VERSION_TAG;
        int vw = text_width(v, 1);
        text(sx + sw - 16 - vw, sy + (title_h - lh) / 2, v,
             UI_FG2, 1);
    }

    /* 圆角 logo 方块：HBOS "H" 标志 */
    int bx = sx + 18, by = sy + title_h + 20;
    fill_round_rect(bx, by, 48, 48, 8, accent, RR_ALL);
    draw_hbos_logo(bx + 12, by + 12, 24, UI_FG);

    text(sx + 82, sy + title_h + 22, "HIVE 桌面环境", tcol, 1);
    text(sx + 82, sy + title_h + 44, "64 位 x86_64 · BIOS / UEFI 双启动", scol, 1);
    text(sx + 82, sy + title_h + 64, "并发多窗口 · 协作式多任务", scol, 1);

    /* 圆角进度条 */
    int bar_x = sx + 18, bar_w = sw - 36, bar_y = sy + sh - 32, bar_h = 8;
    fill_round_rect(bar_x, bar_y, bar_w, bar_h, 4,
                    UI_SURFACE, RR_ALL);
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
                    light ? UI_SURFACE : UI_SURFACE2, RR_ALL);
    /* 左侧强调色条 */
    fill_round_rect(bx, by, 5, bh, 10, UI_ACCENT, RR_TL | RR_BL);
    text(bx + 16, by + 9,  "通知", UI_ACCENT, 1);
    text(bx + 16, by + 30, st->toast_msg,
         UI_FG, 1);
}

static void cmd_gui(int argc, char **argv) {
    fb_info_t fb;
    if (fb_get_info(&fb) < 0) {
        console_puts("gui: 需要 framebuffer 模式\n");
        return;
    }

    if (!gui_font_init()) {
        console_puts("gui: GUI 字体加载失败\n");
        return;
    }

    /* 主题层：载入内置主题并扫描 /themes 下的 .theme。必须在任何绘制之前调用，
     * 之后所有颜色都来自 ui_theme()。此调用不会失败——外部主题损坏时
     * 逐键降级，内置主题永远兜底。 */
    ui_theme_init();
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
    int appwin_mouse_capture = -1; /* 内容区按下后持续接收移动与松开 */
    int appwin_mouse_hover = -1;   /* 上次收到悬停事件的应用窗口 */
    int appwin_focus = -1;         /* 键盘焦点窗口；不再假定最高槽位 */
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
    st.last_clicked_file = -1;
    st.delete_confirm_index = -1;
    st.taskbar_show_seconds = 1;
    st.brightness = 100;
    gui_set_brightness(st.brightness);
    /* 把 shell 的初始主题发布到 gui_service：HAX 应用窗口跟随的是
     * service，不发布的话它们按 service 默认值（深色）渲染，桌面却是
     * 浅色——"浅色桌面 + 深色应用窗口"的混搭（issue #2 视频里那扇
     * 盖满整屏的深色窗口就是这么来的）。 */
    (void)gui_service_theme_set(st.theme_light);
    st.status = "就绪";
    wm_init(&st.wm, w, h);
    st.console_input[0] = 0;
    st.console_input_len = 0;
    st.console_line_count = 0;
    st.console_cursor = 0;
    st.console_history_idx = -1;
    app_diag_welcome(&st);
    /* 控制台 sink 常驻：异步 spawn 的 .hax 应用输出持续进终端历史，
     * GUI 主循环得以继续重绘（否则同步运行会冻结桌面）。 */
    app_diag_install_sink(&st);
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

    /* 兜底：凡在 gui_apps[] 里登记过、但上面没有显式标题的应用，统一
     * 用登记表里的名字。这样新增应用只需改 gui_apps[] 一处就能拥有正确
     * 标题，不会出现"功能可用但窗口标题是'应用'"的漏配。
     * （上面那些显式设置保留：它们的中文名与菜单 label 不完全一致。） */
    for (uint32_t i = 0; i < gui_app_count(); i++) {
        const char *nm = wm_window_title_by_mode(gui_app_meta_at(i)->mode);
        if (!nm) wm_set_app_title(gui_app_meta_at(i)->mode, gui_app_meta_at(i)->name);
    }

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

    /* gui <url>：跳过桌面直接打开浏览器并加载，方便脚本化验证（截图/
     * 自动化测试），不影响正常无参数启动。 */
    if (argc > 1 && argv[1][0]) {
        if (gui_open_window(&st, WM_WIN_APP, GUI_APP_BROWSER, 0) >= 0) {
            browser_init(&st);
            strncpy(st.browser_url, argv[1], BROWSER_URL_CAP - 1);
            st.browser_url[BROWSER_URL_CAP - 1] = 0;
            st.browser_url_cursor = (uint32_t)strlen(st.browser_url);
            browser_load(&st);
        }
    }

    st.splash_ticks = 90;
    gui_dirty_mark_full();
    draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
    while (1) {
        /* 回收已退出应用留下的窗口 */
        if (winsrv_reap_dead() > 0) gui_dirty_mark_full();

        /* 全屏画布应用（hax_gui_*）拥有屏幕时，合成器让位：不绘制、不读输入，
         * 由该应用独占 gd_surface_ptr() 并自行 present；其退出后收回屏幕。 */
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

        /* 跟随 gui_service 的设置变化（设置应用或其它写方经 service 改
         * 主题时，桌面这里同步应用）。revision 未变时只是一次结构体
         * 拷贝；service 对原值重放不递增 revision，不会引发无谓整帧
         * 重绘（issue #2 的闪烁放大器）。 */
        {
            static uint32_t settings_revision;
            gui_service_settings_t live;
            if (gui_service_settings(0, GUI_SERVICE_SETTINGS_QUERY, &live) == 0 &&
                live.revision != settings_revision) {
                settings_revision = live.revision;
                int light = live.theme == GUI_THEME_LIGHT;
                /* 用"已应用到主题层的值"判断，而不是 st.theme_light：
                 * F4 与设置应用会**先**改 st.theme_light 再写 service，等主
                 * 循环读到时两者已相等，若拿它做条件就永远不进这个分支——
                 * 表现为状态栏提示已切换、实际画面不变（曾复现：任务栏变浅
                 * 但窗口与桌面仍是深色）。这里改成比较真正生效过的值。 */
                if (g_applied_theme_light != light) {
                    gui_apply_theme(light);
                }
                st.theme_light = light;
                if (st.taskbar_show_seconds != live.taskbar_show_seconds)
                    st.taskbar_show_seconds = live.taskbar_show_seconds;
                if (st.brightness != live.brightness) {
                    st.brightness = live.brightness;
                    gui_set_brightness(st.brightness);
                }
                gui_dirty_mark_full();
            }
        }

        int key = key_poll();
        /* F1: toggle the start menu (keyboard-only path to every app --
         * previously mouse-only via the taskbar logo / right-click menu;
         * once open, typing filters and Enter launches, see below). */
        if (key == KB_KEY_F1) {
            wm_toggle_start_menu(&st.wm);
            if (st.wm.start_menu_open) {
                gui_scan_disk_apps();
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
        /* F4: plain F4 toggles light/dark theme; Alt+F4 closes the active
         * application window (standard window-manager behavior). */
        if (key == KB_KEY_F4) {
            if (kb_alt_pressed()) {
                if (st.wm.active_window >= 0)
                    gui_close_window(&st, st.wm.active_window);
            } else {
                st.theme_light = !st.theme_light;
                /* 立即落到主题层，本帧就生效（不必等 service 同步回路） */
                gui_apply_theme(st.theme_light);
                /* shell 与 service 双写同步：HAX 应用窗口跟随 service，
                 * 不同步就会出现桌面浅色、窗口深色的混搭（issue #2）。 */
                (void)gui_service_theme_set(st.theme_light);
                st.status = st.theme_light ? "已切换为浅色主题" : "已切换为深色主题";
            }
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
        /* F9: force-maximize the active window. Debug/testing aid — lets
         * scripted QEMU tests (monitor `sendkey f9`) toggle maximize
         * without relying on flaky QMP mouse double-click coordinates. */
        if (key == KB_KEY_F9) {
            if (st.wm.active_window >= 0) wm_toggle_maximize(&st.wm, st.wm.active_window);
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
            winsrv_window_t *fw = winsrv_get(appwin_focus);
            if (fw && fw->win.state == WINSRV_STATE_MINIMIZED) fw = 0;
            for (int i = WINSRV_MAX - 1; !fw && i >= 0; i--) {
                fw = winsrv_get(i);
                if (fw && fw->win.state == WINSRV_STATE_MINIMIZED) fw = 0;
                else if (fw) appwin_focus = i;
            }
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

            /* HIVE 窗口鼠标语义：除了按下，还发送移动、拖动和松开。按下后由
             * 内容窗口捕获指针，即使移出窗口也能收到松开，控件才不会卡在
             * pressed 状态。离开窗口时用 (-1,-1) 通知旧悬停目标。 */
            int appwin_left_pressed = left_down && !(last_buttons & 1);
            if (!appwin_left_pressed) {
                int target = -1;
                winsrv_window_t *target_win = 0;
                if (appwin_mouse_capture >= 0) {
                    target_win = winsrv_get(appwin_mouse_capture);
                    if (target_win) target = appwin_mouse_capture;
                    else appwin_mouse_capture = -1;
                }
                if (target < 0) {
                    for (int i = WINSRV_MAX - 1; i >= 0; i--) {
                        winsrv_window_t *candidate = winsrv_get(i);
                        if (!candidate ||
                            candidate->win.state == WINSRV_STATE_MINIMIZED) continue;
                        if (mx >= candidate->win.x && mx < candidate->win.x + candidate->win.w &&
                            my >= candidate->win.y && my < candidate->win.y + candidate->win.h) {
                            target = i;
                            target_win = candidate;
                            break;
                        }
                    }
                }
                if (appwin_mouse_hover >= 0 && appwin_mouse_hover != target) {
                    winsrv_window_t *old_hover = winsrv_get(appwin_mouse_hover);
                    if (old_hover)
                        winsrv_push_event(old_hover, WINEV_MOUSE, -1, -1, st.buttons);
                }
                if (target_win) {
                    winsrv_push_event(target_win, WINEV_MOUSE,
                                      mx - target_win->win.x, my - target_win->win.y,
                                      st.buttons);
                }
                appwin_mouse_hover = target;
                if (!left_down) appwin_mouse_capture = -1;
            }

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
                    if (app_code_hit_editor(w, h, &st, mx, my, &off)) {
                        app_code_set_cursor(&st, off, 1);
                        app_code_ensure_visible(&st);
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
                    if (app_notes_hit_editor(w, h, &st, mx, my, &off)) {
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
                    win->win.x = mx - appwin_drag_dx;
                    win->win.y = my - appwin_drag_dy;
                    if (win->win.y < APPWIN_TITLE_H) win->win.y = APPWIN_TITLE_H;
                    if (win->win.x < 0) win->win.x = 0;
                    if (win->win.x > w - 40) win->win.x = w - 40;
                    redraw = 1;
                } else {
                    winsrv_window_t *ended = winsrv_get(appwin_drag);
                    if (ended)
                        winsrv_push_event(ended, WINEV_MOVE,
                                          ended->win.x, ended->win.y, 0);
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
                if ((st.app_mode == GUI_APP_NOTES && app_notes_hit_editor(w, h, &st, mx, my, &hover_off)) ||
                    (st.app_mode == GUI_APP_CODE && app_code_hit_editor(w, h, &st, mx, my, &hover_off)))
                    cursor_edge = CURSOR_HOVER_TEXT;
            }
            /* 浏览器链接悬停：手型光标 + 状态栏预览目标 URL（Chrome 把目标
             * 地址显示在左下角，这里等价物是窗口底部状态栏）。 */
            {
                static int br_hover_idx = -1;
                static const char *br_saved_status = 0;
                static char br_hover_buf[120];
                int hl = -1;
                if (cursor_edge == WM_EDGE_NONE && st.app_mode == GUI_APP_BROWSER)
                    hl = hit_browser_link(w, h, &st, mx, my);
                if (hl >= 0) cursor_edge = CURSOR_HOVER_LINK;
                if (hl != br_hover_idx) {
                    br_hover_idx = hl;
                    if (hl >= 0 && hl < st.browser_link_count) {
                        if (st.status != br_hover_buf) br_saved_status = st.status;
                        /* 预览解析后的完整地址（协议相对/相对路径都补全），
                         * 和点击后真正要去的地方一致 */
                        char resolved[BROWSER_URL_CAP];
                        const char *show = st.browser_link_href[hl];
                        if (browser_resolve_href(&st, show, resolved, sizeof(resolved)) == 0)
                            show = resolved;
                        uint32_t bp = 0;
                        br_hover_buf[0] = 0;
                        append_str(br_hover_buf, sizeof(br_hover_buf), &bp, "打开 ");
                        append_str(br_hover_buf, sizeof(br_hover_buf), &bp, show);
                        st.status = br_hover_buf;
                    } else if (st.status == br_hover_buf) {
                        st.status = br_saved_status ? br_saved_status : "就绪";
                    }
                    /* 直接 gui_damage_region 不行：紧接着的鼠标移动分支会
                     * gui_dirty_reset() 把这条脏矩形吞掉、只重画光标区域，
                     * 状态栏像素一直是旧的。悬停进出是单次事件，整帧重绘
                     * 一次的代价可以接受。 */
                    redraw = 1;
                }
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
                /* 弹层（右键菜单/亮度/日历/开始菜单）画在所有窗口之上
                 * （draw_gui_screen 的绘制顺序），打开期间应用窗口必须
                 * 让出点击：否则压在弹层下面的窗口会抢走点击（点击穿透
                 * 到下层窗口），弹层又永远等不到"点外部关闭"，像卡死一
                 * 样留在原地；之后光标附近的每次点击都在拿球——误触
                 * "切换主题"/"最大化" 就是 issue #2 的主题闪烁。 */
                int popup_open = st.ctx_open || st.brightness_popup_open ||
                                 st.cal_open || st.wm.start_menu_open;
                int appwin_hit = 0;
                for (int i = WINSRV_MAX - 1; i >= 0 && !appwin_hit && !popup_open; i--) {
                    winsrv_window_t *win = winsrv_get(i);
                    if (!win || win->win.state == WINSRV_STATE_MINIMIZED) continue;
                    int th = APPWIN_TITLE_H;
                    int cbx = win->win.x + win->win.w - 22;
                    if (mx >= cbx && mx < win->win.x + win->win.w &&
                        my >= win->win.y - th && my < win->win.y) {            /* 关闭按钮 */
                        win->win.want_close = 1;
                        winsrv_push_event(win, WINEV_CLOSE, 0, 0, 0);
                        appwin_hit = 1;
                    } else if (mx >= win->win.x && mx < win->win.x + win->win.w &&
                               my >= win->win.y - th && my < win->win.y) {     /* 标题栏→拖动 */
                        appwin_drag = i;
                        appwin_drag_dx = mx - win->win.x;
                        appwin_drag_dy = my - win->win.y;
                        appwin_hit = 1;
                    } else if (mx >= win->win.x && mx < win->win.x + win->win.w &&
                               my >= win->win.y && my < win->win.y + win->win.h) { /* 内容→鼠标事件 */
                        appwin_mouse_capture = i;
                        appwin_mouse_hover = i;
                        winsrv_push_event(win, WINEV_MOUSE,
                                          mx - win->win.x, my - win->win.y, st.buttons);
                        appwin_hit = 1;
                    }
                    if (appwin_hit && appwin_focus != i) {
                        winsrv_window_t *old_focus = winsrv_get(appwin_focus);
                        if (old_focus) {
                            old_focus->win.focused = 0;
                            winsrv_push_event(old_focus, WINEV_FOCUS, 0, 0, 0);
                        }
                        appwin_focus = i;
                        win->win.focused = 1;
                        winsrv_push_event(win, WINEV_FOCUS, 1, 0, 0);
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
                            else if (ci == 1) {
                                st.theme_light = !st.theme_light;
                                (void)gui_service_theme_set(st.theme_light);
                            }
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
                            } else {
                                /* Client-area clicks participate in normal window
                                 * activation too.  The old dispatch keyed only on
                                 * st.app_mode, so it kept sending a click to the
                                 * previously active app unless the user clicked a
                                 * titlebar first.  Resolve the topmost window under
                                 * the pointer before interpreting any client widget. */
                                int content_idx = gui_window_at(&st, w, h, mx, my);
                                if (content_idx >= 0 && content_idx != st.wm.active_window)
                                    gui_focus_window(&st, content_idx);

                                if (content_idx >= 0 && st.app_mode != GUI_APP_NONE) {
                                int code_cmd = app_code_hit_command(w, h, &st, mx, my);
                                if (code_cmd == 4 /* CODE_CMD_GUI_RUN */) {
                                    app_code_gui_run(&st, &fb);
                                } else if (code_cmd) {
                                    app_code_command(&st, code_cmd);
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
                                    int note_file = app_notes_hit_file(w, h, &st, mx, my);
                                    if (note_file >= 0) {
                                        gui_select_file(&st, note_file);
                                        st.status = "已切换编辑文件";
                                    } else {
                                        int code_file = app_code_hit_file(w, h, &st, mx, my);
                                        if (code_file >= 0) {
                                            if (st.last_clicked_file == code_file && st.selected_file == code_file) {
                                                app_code_open_selected(&st);
                                                st.last_clicked_file = -1;
                                            } else {
                                                gui_select_file(&st, code_file);
                                                st.last_clicked_file = code_file;
                                                st.status = "已选择代码文件";
                                            }
                                        } else {
                                            uint32_t code_off = 0, note_off = 0;
                                            if (app_code_hit_editor(w, h, &st, mx, my, &code_off)) {
                                                app_code_begin_selection(&st, code_off);
                                                code_text_dragging = 1;
                                                app_code_ensure_visible(&st);
                                                st.status = "已移动代码光标";
                                            } else if (app_notes_hit_editor(w, h, &st, mx, my, &note_off)) {
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
                                } else if (content_idx >= 0) {
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
                                } else {
                                    /* No visible window owns this point: it is a
                                     * desktop click even while another window is
                                     * active.  This keeps exposed shortcuts usable
                                     * and prevents clicks through an overlapping
                                     * window. */
                                    int dico = hit_desktop_icon(mx, my);
                                    if (dico >= 0) {
                                        const desktop_icon_t *d = &g_desktop_icons[dico];
                                        if (d->kind == WM_WIN_PANEL)
                                            gui_open_panel_window(&st, d->mode);
                                        else
                                            gui_open_window(&st, WM_WIN_APP, d->mode, 0);
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
        if (gui_app_tick(&st)) {
            gui_dirty_mark_full();
            draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
        }
        /* 有并发应用窗口时，每帧重绘以反映它们的实时刷新 */
        if (winsrv_has_dirty()) {
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
        /* 后台浏览器加载：任务结束后触发一帧重绘（让新内容显示出来） */
        if (st.browser_loading) {
            const task_t *bt = task_get_by_id(st.browser_load_tid);
            if (!bt || bt->state == TASK_TERMINATED) {
                if (st.browser_loading) st.browser_loading = 0; /* 防止 task_create 失败时的 loading */
                gui_dirty_mark_full();
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            } else {
                /* 加载中：每帧刷新旋转动画，然后让出 CPU */
                gui_dirty_mark_full();
                draw_gui_frame(&fb, w, h, &st, mx, my, cursor_edge);
            }
        }
        task_yield();
    }

    gui_set_surface(0, 0, 0, 0);
    if (surface_phys) pmm_free_blocks(surface_phys, surface_pages);
    mouse_shutdown();
    /* 退出 GUI 后必须解除常驻控制台 sink：否则 shell 后续所有输出仍会被
     * 吞进已退出的 GUI 终端历史，真终端什么都不显示，看起来像卡死。 */
    app_diag_remove_sink();
    console_reset_terminal();
    console_puts("HIVE: 已返回 shell\n");
}

void tool_gui_init(void) {
    static const command_t cmds[] = {
        {"hive", CMD_GROUP_GRAPHICS, "Start HIVE desktop", "hive", cmd_gui},
        {"gui", CMD_GROUP_GRAPHICS, "Start HIVE desktop", "gui", cmd_gui},
        {"startx", CMD_GROUP_GRAPHICS, "Start HIVE desktop", "startx", cmd_gui},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}

uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b) { return rgb(r, g, b); }
void gui_rect(int x, int y, int w, int h, uint32_t color) { rect(x, y, w, h, color); }
void gui_rect_alpha(int x, int y, int w, int h, uint32_t color) { rect_alpha(x, y, w, h, color); }
void gui_blit_rgb888(int x, int y, const uint8_t *rgbdata, int img_w, int img_h,
                      int clip_x, int clip_y, int clip_w, int clip_h) {
    if (!gd_surface_ptr() || !rgbdata || img_w <= 0 || img_h <= 0 || clip_w <= 0 || clip_h <= 0) return;
    int x0 = x, y0 = y, x1 = x + img_w, y1 = y + img_h;
    if (x0 < clip_x) x0 = clip_x;
    if (y0 < clip_y) y0 = clip_y;
    if (x1 > clip_x + clip_w) x1 = clip_x + clip_w;
    if (y1 > clip_y + clip_h) y1 = clip_y + clip_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > gd_surface_w()) x1 = gd_surface_w();
    if (y1 > gd_surface_h()) y1 = gd_surface_h();
    for (int py = y0; py < y1; py++) {
        int src_y = py - y;
        uint32_t *drow = gd_surface_ptr() + (uint32_t)py * gd_surface_pitch();
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
int gui_draw_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale) {
    return draw_text_codepoint(x, y, cp, color, scale);
}
int gui_text_mono(int x, int y, int max_x, const char *s, uint32_t color) {
    return text_mono(x, y, max_x, s, color);
}
void gui_draw_small_button(int x, int y, int w, const char *label, uint32_t color) {
    draw_small_button(x, y, w, label, color);
}
int gui_ui_scale_v(int v) { return ui_s(v); }
int gui_draw_mono_char(int x, int y, char c, uint32_t color) { return draw_mono_char(x, y, c, color); }
int gui_key_poll(void) { return key_poll(); }
int gui_surface_width(void) { return gd_surface_w(); }
int gui_surface_height(void) { return gd_surface_h(); }
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
