#ifndef HBOS_WM_H
#define HBOS_WM_H

#include <stdint.h>

#define WM_MAX_WINDOWS 16
#define WM_TASKBAR_H   44
#define WM_TITLE_H     42
#define WM_BORDER_W    4
#define WM_BTN_W       28
#define WM_BTN_GAP     4

enum {
    WM_WIN_PANEL = 0,
    WM_WIN_APP   = 1,
};

enum {
    WM_STATE_NORMAL    = 0,
    WM_STATE_MINIMIZED = 1,
    WM_STATE_MAXIMIZED = 2,
};

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
    WM_SNAP_NONE  = 0,
    WM_SNAP_LEFT  = 1,
    WM_SNAP_RIGHT = 2,
    WM_SNAP_TOP   = 3,
};

enum {
    WM_ANIM_NONE     = 0,
    WM_ANIM_MINIMIZE = 1,
    WM_ANIM_MAXIMIZE = 2,
    WM_ANIM_RESTORE  = 3,
    WM_ANIM_OPEN     = 4,  /**< 新窗口淡入 */
};

typedef struct {
    int used;
    int kind;
    int mode;
    int state;
    int x, y;
    int w, h;
    int prev_x, prev_y, prev_w, prev_h;
    int anim_type;         /**< 当前动画类型 */
    int anim_frame;        /**< 动画帧计数 */
    int anim_total;        /**< 动画总帧数 */
    int anim_start_x;      /**< 动画起始X */
    int anim_start_y;      /**< 动画起始Y */
    int anim_start_w;      /**< 动画起始宽度 */
    int anim_start_h;      /**< 动画起始高度 */
    uint8_t opacity;       /**< 窗口不透明度 (0-255) */
} wm_window_t;

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