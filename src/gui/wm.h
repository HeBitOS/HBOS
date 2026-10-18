#ifndef HBOS_WM_H
#define HBOS_WM_H

#include <stdint.h>
#include "../ui/ui_app.h"   /* ui_window_t：统一后的窗口类型 */

/* ── DPI 缩放 ───────────────────────────────────────────────────────
 * 所有像素尺寸常量都经 ui_s() 按屏幕分辨率缩放（基准 1080p = 100%），
 * 故下面这些 #define 在使用处自动变为缩放后的运行时值，无需改调用点。
 * g_ui_scale 在 GUI 启动时按 fb 高度设置（见 gui.c）。计数/索引不缩放。 */
extern int g_ui_scale;          /* 百分比，100 = 基准 */
int ui_s(int v);                /* 返回 v 缩放后的像素值 */

#define WM_MAX_WINDOWS 16
#define WM_TASKBAR_H   ui_s(44)
#define WM_TITLE_H     ui_s(38)
#define WM_BORDER_W    ui_s(4)
#define WM_BTN_W       ui_s(26)
#define WM_BTN_GAP     ui_s(4)

/* ── Win11-style start-menu layout (shared by wm.c draw and hit-test) ─── */
#define SM_W            ui_s(540)   /* total panel width  */
#define SM_H            ui_s(368)   /* total panel height */
#define SM_PAD          ui_s(16)    /* horizontal padding */
#define SM_SEARCH_TOP   ui_s(8)     /* search bar top */
#define SM_SEARCH_H     ui_s(36)    /* search bar height */
#define SM_PIN_TOP      ui_s(52)    /* "已固定" label top */
#define SM_PIN_LABEL_H  ui_s(22)    /* section label height */
#define SM_GRID_TOP     ui_s(76)    /* grid cells top */
#define SM_GRID_COLS      5   /* columns（计数，不缩放） */
#define SM_GRID_ROWS      3   /* rows（计数，不缩放） */
#define SM_CELL_W       ((SM_W - 2*SM_PAD) / SM_GRID_COLS)
#define SM_CELL_H       ui_s(76)    /* cell height */
#define SM_GRID_H       (SM_GRID_ROWS * SM_CELL_H)
#define SM_SEP_Y        (SM_GRID_TOP + SM_GRID_H)
#define SM_BAR_TOP      (SM_SEP_Y + ui_s(8))
#define SM_BAR_H        (SM_H - SM_BAR_TOP)
#define SM_PINNED_COUNT  12   /* items 0..11 = pinned apps */
/* items 13 = 返回Shell, 14 = 关机; hit-tested in bottom bar */
#define SM_SHELL_ITEM   13
#define SM_POWER_ITEM   14
/* 动态追加的 .hax GUI 应用：网格命中返回 SM_HAX_BASE+k（避开 13/14） */
#define SM_HAX_BASE    100
#define SM_HAX_MAX       7   /* 启动器最多展示的 .hax GUI 应用数（限面板高度） */
/* 网格命中返回 SM_CELL_BASE+可见单元格序号；gui.c 据搜索过滤映射为实际应用 */
#define SM_CELL_BASE   200
#define SM_SEARCH_ITEM  15   /* 点中搜索框（保持菜单打开） */

enum {
    WM_WIN_PANEL = 0,
    WM_WIN_APP   = 1,
};

/* 状态常量：直接别名到统一枚举 ui_win_state_t。
 * 原先这里另定义一份匿名 enum，导致与 ui_win_state_t 混用时出现
 * -Wenum-compare / -Wenum-conversion 告警（两边是不同类型）。别名后
 * 类型一致，既有 WM_STATE_* 调用点仍可用。 */
#define WM_STATE_NORMAL    UI_WIN_NORMAL
#define WM_STATE_MINIMIZED UI_WIN_MINIMIZED
#define WM_STATE_MAXIMIZED UI_WIN_MAXIMIZED

enum {
    WM_EDGE_NONE  = -1,
    WM_EDGE_N     = 0,
    WM_EDGE_S     = 1,
    WM_EDGE_E     = 2,
    WM_EDGE_W     = 3,
    WM_EDGE_NE    = 4,
    WM_EDGE_NW    = 5,
    WM_EDGE_SE    = 6,
    WM_EDGE_SW    = 7,
};

enum {
    WM_SNAP_NONE   = 0,
    WM_SNAP_LEFT   = 1,
    WM_SNAP_RIGHT  = 2,
    WM_SNAP_TOP    = 3,   // 顶部：最大化
    WM_SNAP_BOTTOM = 4,   // 底部：最小化
};

/* 动画类型：别名到统一枚举 ui_anim_t（理由同 WM_STATE_*） */
#define WM_ANIM_NONE     UI_ANIM_NONE
#define WM_ANIM_MINIMIZE UI_ANIM_MINIMIZE
#define WM_ANIM_MAXIMIZE UI_ANIM_MAXIMIZE
#define WM_ANIM_RESTORE  UI_ANIM_RESTORE
#define WM_ANIM_OPEN     UI_ANIM_OPEN

/* 窗口结构已统一到 src/ui/ui_app.h 的 ui_window_t。
 *
 * 原先这里另有一份 wm_window_t（内置应用用）与 winsrv 的 winsrv_window_t
 * （hax 应用用）两套定义，字段高度重复、行为不一致，同一 bug 要修两遍。
 * 现在统一为一个类型；`wm_window_t` 保留为别名，使既有调用点零改动。 */
typedef ui_window_t wm_window_t;

typedef struct {
    wm_window_t windows[WM_MAX_WINDOWS];
    int window_count;
    int active_window;
    int z_order[WM_MAX_WINDOWS];
    int start_menu_open;
    int menu_x, menu_y, menu_w, menu_h;
    int desk_w, desk_h;
} wm_state_t;

void wm_init(wm_state_t *wm, int desk_w, int desk_h);
void wm_set_panel_title(int panel, const char *title);
void wm_set_app_title(int mode, const char *title);
const char *wm_window_title_by_mode(int mode);
int  wm_open_window(wm_state_t *wm, int kind, int mode, int unique);
void wm_close_window(wm_state_t *wm, int idx);
void wm_focus_window(wm_state_t *wm, int idx);
void wm_focus_next(wm_state_t *wm, int dir);
void wm_move_window(wm_state_t *wm, int idx, int x, int y);
void wm_resize_window(wm_state_t *wm, int idx, int w, int h);
void wm_minimize_window(wm_state_t *wm, int idx);
void wm_maximize_window(wm_state_t *wm, int idx);
void wm_restore_window(wm_state_t *wm, int idx);
void wm_toggle_maximize(wm_state_t *wm, int idx);
void wm_snap_window(wm_state_t *wm, int idx, int side);
/* 从吸附态退出：恢复到吸附前的几何。返回 1 表示确实做了还原。 */
int  wm_unsnap_window(wm_state_t *wm, int idx);
void wm_toggle_start_menu(wm_state_t *wm);
void wm_close_start_menu(wm_state_t *wm);

wm_window_t *wm_get_window(wm_state_t *wm, int idx);
wm_window_t *wm_get_active(wm_state_t *wm);
const char  *wm_window_title(wm_window_t *win);
void         wm_get_window_rect(wm_state_t *wm, int idx, int *x, int *y, int *w, int *h);
int          wm_hit_titlebar(wm_state_t *wm, int mx, int my);
int          wm_hit_close(wm_state_t *wm, int mx, int my);
int          wm_hit_minimize(wm_state_t *wm, int mx, int my);
int          wm_hit_maximize(wm_state_t *wm, int mx, int my);
int          wm_hit_border(wm_state_t *wm, int mx, int my, int *edge);
int          wm_hit_start_menu(wm_state_t *wm, int mx, int my);
void         wm_update_animations(wm_state_t *wm);
int          wm_has_active_animations(wm_state_t *wm);

#endif