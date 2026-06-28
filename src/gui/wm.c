#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "wm.h"
#include "../string.h"

static int g_titles_ready = 0;

#define WM_APP_TITLE_COUNT 8
#define WM_START_MENU_ROWS 13

static const char *panel_titles[4];
static const char *app_titles[WM_APP_TITLE_COUNT];

void wm_set_panel_title(int panel, const char *title) {
    if (panel >= 0 && panel < 4) panel_titles[panel] = title;
}

void wm_set_app_title(int mode, const char *title) {
    if (mode >= 0 && mode < WM_APP_TITLE_COUNT) app_titles[mode] = title;
    g_titles_ready = 1;
}

const char *wm_window_title(wm_window_t *win) {
    if (!win || !win->used) return "窗口";
    if (win->kind == WM_WIN_PANEL) {
        if (win->mode >= 0 && win->mode < 4 && panel_titles[win->mode])
            return panel_titles[win->mode];
        return "面板";
    }
    if (win->mode >= 0 && win->mode < WM_APP_TITLE_COUNT && app_titles[win->mode])
        return app_titles[win->mode];
    return "应用";
}

void wm_init(wm_state_t *wm, int desk_w, int desk_h) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm->windows[i].used = 0;
        wm->z_order[i] = i;
    }
    wm->window_count = 0;
    wm->active_window = -1;
    wm->start_menu_open = 0;
    wm->desk_w = desk_w;
    wm->desk_h = desk_h;
}

static void wm_bring_to_top(wm_state_t *wm, int idx) {
    int pos = -1;
    for (int i = 0; i < wm->window_count; i++) {
        if (wm->z_order[i] == idx) {
            pos = i;
            break;
        }
    }
    if (pos >= 0) {
        for (int i = pos; i + 1 < wm->window_count; i++) {
            wm->z_order[i] = wm->z_order[i + 1];
        }
        wm->z_order[wm->window_count - 1] = idx;
    }
}

int wm_open_window(wm_state_t *wm, int kind, int mode, int unique) {
    if (unique) {
        for (int i = 0; i < wm->window_count; i++) {
            if (wm->windows[i].used && wm->windows[i].kind == kind &&
                wm->windows[i].mode == mode) {
                if (wm->windows[i].state == WM_STATE_MINIMIZED)
                    wm_restore_window(wm, i);
                wm_focus_window(wm, i);
                return i;
            }
        }
    }
    if (wm->window_count >= WM_MAX_WINDOWS) return -1;
    int idx = wm->window_count++;
    wm_window_t *win = &wm->windows[idx];
    win->used = 1;
    win->kind = kind;
    win->mode = mode;
    win->state = WM_STATE_NORMAL;
    win->x = 130 + (idx % 5) * 28;
    win->y = 62 + (idx % 5) * 22;
    win->w = 0;
    win->h = 0;
    win->anim_type = WM_ANIM_OPEN;  /* 打开时淡入 */
    win->anim_frame = 0;
    win->anim_total = 10;
    win->opacity = 40;   /* 起始淡显，update 会逐帧拉满 */
    wm->z_order[idx] = idx;
    wm_focus_window(wm, idx);
    return idx;
}

void wm_close_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;

    int z_pos = -1;
    for (int i = 0; i < wm->window_count; i++) {
        if (wm->z_order[i] == idx) {
            z_pos = i;
            break;
        }
    }
    if (z_pos >= 0) {
        for (int i = z_pos; i + 1 < wm->window_count; i++) {
            wm->z_order[i] = wm->z_order[i + 1];
        }
    }
    for (int i = 0; i < wm->window_count - 1; i++) {
        if (wm->z_order[i] > idx) {
            wm->z_order[i]--;
        }
    }

    for (int i = idx; i + 1 < wm->window_count; i++) {
        wm->windows[i] = wm->windows[i + 1];
    }
    wm->window_count--;

    int next = -1;
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int w_idx = wm->z_order[i];
        if (wm->windows[w_idx].used && wm->windows[w_idx].state != WM_STATE_MINIMIZED) {
            next = w_idx;
            break;
        }
    }
    if (next >= 0) {
        wm_focus_window(wm, next);
    } else {
        wm->active_window = -1;
    }
}

void wm_focus_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;
    if (!wm->windows[idx].used) return;
    if (wm->windows[idx].state == WM_STATE_MINIMIZED)
        wm_restore_window(wm, idx);
    wm->active_window = idx;
    wm_bring_to_top(wm, idx);
}

void wm_focus_next(wm_state_t *wm, int dir) {
    if (wm->window_count <= 0) return;
    int idx = wm->active_window;
    if (idx < 0) idx = 0;
    else idx = (idx + dir + wm->window_count) % wm->window_count;
    wm_focus_window(wm, idx);
}

void wm_move_window(wm_state_t *wm, int idx, int x, int y) {
    if (idx < 0 || idx >= wm->window_count) return;
    wm_window_t *win = &wm->windows[idx];
    if (!win->used || win->state == WM_STATE_MAXIMIZED) return;
    win->x = x;
    win->y = y;
}

void wm_resize_window(wm_state_t *wm, int idx, int w, int h) {
    if (idx < 0 || idx >= wm->window_count) return;
    wm_window_t *win = &wm->windows[idx];
    if (!win->used || win->state == WM_STATE_MAXIMIZED) return;
    if (w < 200) w = 200;
    if (h < 120) h = 120;
    win->w = w;
    win->h = h;
}

void wm_minimize_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;
    wm_window_t *win = &wm->windows[idx];
    if (!win->used) return;
    if (win->state == WM_STATE_NORMAL) {
        win->prev_x = win->x;
        win->prev_y = win->y;
        win->prev_w = win->w;
        win->prev_h = win->h;
    }
    
    /* 启动最小化动画 */
    win->anim_type = WM_ANIM_MINIMIZE;
    win->anim_frame = 0;
    win->anim_total = 10;  /* 10帧动画 */
    win->anim_start_x = win->x;
    win->anim_start_y = win->y;
    win->anim_start_w = win->w;
    win->anim_start_h = win->h;
    
    win->state = WM_STATE_MINIMIZED;

    int next = -1;
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int w_idx = wm->z_order[i];
        if (w_idx != idx && wm->windows[w_idx].used &&
            wm->windows[w_idx].state != WM_STATE_MINIMIZED) {
            next = w_idx;
            break;
        }
    }
    if (next >= 0) {
        wm_focus_window(wm, next);
    } else {
        wm->active_window = -1;
    }
}

void wm_maximize_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;
    wm_window_t *win = &wm->windows[idx];
    if (!win->used) return;
    if (win->state == WM_STATE_NORMAL) {
        win->prev_x = win->x;
        win->prev_y = win->y;
        win->prev_w = win->w;
        win->prev_h = win->h;
    }
    
    /* 启动最大化动画 */
    win->anim_type = WM_ANIM_MAXIMIZE;
    win->anim_frame = 0;
    win->anim_total = 8;  /* 8帧动画 */
    win->anim_start_x = win->x;
    win->anim_start_y = win->y;
    win->anim_start_w = win->w;
    win->anim_start_h = win->h;
    
    win->state = WM_STATE_MAXIMIZED;
    win->x = 0;
    win->y = 0;
    win->w = wm->desk_w;
    win->h = wm->desk_h - WM_TASKBAR_H;
}

void wm_restore_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;
    wm_window_t *win = &wm->windows[idx];
    if (!win->used) return;
    if (win->state == WM_STATE_NORMAL) return;
    
    /* 启动还原动画 */
    win->anim_type = WM_ANIM_RESTORE;
    win->anim_frame = 0;
    win->anim_total = 8;  /* 8帧动画 */
    win->anim_start_x = win->x;
    win->anim_start_y = win->y;
    win->anim_start_w = win->w;
    win->anim_start_h = win->h;
    
    win->state = WM_STATE_NORMAL;
    if (win->prev_w > 0 && win->prev_h > 0) {
        win->x = win->prev_x;
        win->y = win->prev_y;
        win->w = win->prev_w;
        win->h = win->prev_h;
    }
}

// 双击标题栏：在最大化与还原之间切换
void wm_toggle_maximize(wm_state_t *wm, int idx) {
    wm_window_t *win = wm_get_window(wm, idx);
    if (!win) return;
    if (win->state == WM_STATE_MAXIMIZED) wm_restore_window(wm, idx);
    else wm_maximize_window(wm, idx);
}

// 边缘吸附：左半屏 / 右半屏 / 顶部最大化
void wm_snap_window(wm_state_t *wm, int idx, int side) {
    wm_window_t *win = wm_get_window(wm, idx);
    if (!win || side == WM_SNAP_NONE) return;
    if (side == WM_SNAP_TOP) {
        wm_maximize_window(wm, idx);
        return;
    }
    // 记录还原位置（仅在从普通态吸附时）
    if (win->state == WM_STATE_NORMAL) {
        win->prev_x = win->x;
        win->prev_y = win->y;
        win->prev_w = win->w;
        win->prev_h = win->h;
    }
    win->state = WM_STATE_NORMAL;
    int half = wm->desk_w / 2;
    win->y = 0;
    win->h = wm->desk_h - WM_TASKBAR_H;
    win->w = half;
    win->x = (side == WM_SNAP_LEFT) ? 0 : (wm->desk_w - half);
}

void wm_toggle_start_menu(wm_state_t *wm) {
    wm->start_menu_open = !wm->start_menu_open;
    if (wm->start_menu_open) {
        /* Size only; the GUI repositions x/y to sit above the centered Start
         * button (header 44 + 32px rows, kept in sync with draw_start_menu /
         * wm_hit_start_menu). */
        wm->menu_w = 240;
        wm->menu_h = 44 + WM_START_MENU_ROWS * 32 + 8;
        wm->menu_x = 10;
        wm->menu_y = wm->desk_h - 50 - wm->menu_h;
        if (wm->menu_y < 10) wm->menu_y = 10;
    }
}

void wm_close_start_menu(wm_state_t *wm) {
    wm->start_menu_open = 0;
}

wm_window_t *wm_get_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return NULL;
    if (!wm->windows[idx].used) return NULL;
    return &wm->windows[idx];
}

wm_window_t *wm_get_active(wm_state_t *wm) {
    return wm_get_window(wm, wm->active_window);
}

void wm_get_window_rect(wm_state_t *wm, int idx, int *x, int *y, int *w, int *h) {
    wm_window_t *win = wm_get_window(wm, idx);
    if (!win) {
        *x = *y = *w = *h = 0;
        return;
    }
    int def_w = wm->desk_w > 920 ? 790 : wm->desk_w - 120;
    int def_h = wm->desk_h > 620 ? 430 : wm->desk_h - 140;
    if (win->w > 0) def_w = win->w;
    if (win->h > 0) def_h = win->h;
    int def_x = wm->desk_w > 900 ? 132 : 110;
    if (def_x + def_w > wm->desk_w - 28) def_x = wm->desk_w - def_w - 28;
    if (def_x < 100) def_x = 100;
    int def_y = 42;

    if (win->state == WM_STATE_MAXIMIZED && win->anim_type == WM_ANIM_NONE) {
        *x = 0;
        *y = 0;
        *w = wm->desk_w;
        *h = wm->desk_h - WM_TASKBAR_H;
        return;
    }

    if (win->anim_type == WM_ANIM_MAXIMIZE || win->anim_type == WM_ANIM_RESTORE ||
        win->anim_type == WM_ANIM_MINIMIZE) {
        *x = win->x ? win->x : def_x + idx * 24;
        *y = win->y ? win->y : def_y + idx * 18;
        *w = win->w > 0 ? win->w : def_w;
        *h = win->h > 0 ? win->h : def_h;
        if (*x + *w > wm->desk_w - 4) *x = wm->desk_w - *w - 4;
        if (*y + *h > wm->desk_h - WM_TASKBAR_H - 4) *y = wm->desk_h - WM_TASKBAR_H - *h - 4;
        if (*x < 4) *x = 4;
        if (*y < 4) *y = 4;
        return;
    }

    *x = win->x ? win->x : def_x + idx * 24;
    *y = win->y ? win->y : def_y + idx * 18;
    *w = def_w;
    *h = def_h;

    if (*x + *w > wm->desk_w - 4) *x = wm->desk_w - *w - 4;
    if (*y + *h > wm->desk_h - WM_TASKBAR_H - 4) *y = wm->desk_h - WM_TASKBAR_H - *h - 4;
    if (*x < 4) *x = 4;
    if (*y < 4) *y = 4;
}

int wm_hit_titlebar(wm_state_t *wm, int mx, int my) {
    if (wm->start_menu_open && wm_hit_start_menu(wm, mx, my) >= 0) return -1;
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int idx = wm->z_order[i];
        if (wm->windows[idx].state == WM_STATE_MINIMIZED) continue;
        int wx, wy, ww, wh;
        wm_get_window_rect(wm, idx, &wx, &wy, &ww, &wh);
        if (mx >= wx && mx < wx + ww - 80 &&
            my >= wy && my < wy + WM_TITLE_H) return idx;
    }
    return -1;
}

int wm_hit_close(wm_state_t *wm, int mx, int my) {
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int idx = wm->z_order[i];
        if (wm->windows[idx].state == WM_STATE_MINIMIZED) continue;
        int wx, wy, ww, wh;
        wm_get_window_rect(wm, idx, &wx, &wy, &ww, &wh);
        (void)wh;
        if (mx >= wx + ww - WM_BTN_W - 8 && mx < wx + ww - 8 &&
            my >= wy + 4 && my < wy + WM_TITLE_H - 4) return idx;
    }
    return -1;
}

int wm_hit_minimize(wm_state_t *wm, int mx, int my) {
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int idx = wm->z_order[i];
        if (wm->windows[idx].state == WM_STATE_MINIMIZED) continue;
        int wx, wy, ww, wh;
        wm_get_window_rect(wm, idx, &wx, &wy, &ww, &wh);
        (void)wh;
        if (mx >= wx + ww - WM_BTN_W * 3 - WM_BTN_GAP * 2 - 8 &&
            mx <  wx + ww - WM_BTN_W * 2 - WM_BTN_GAP * 2 - 8 &&
            my >= wy + 4 && my < wy + WM_TITLE_H - 4) return idx;
    }
    return -1;
}

int wm_hit_maximize(wm_state_t *wm, int mx, int my) {
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int idx = wm->z_order[i];
        if (wm->windows[idx].state == WM_STATE_MINIMIZED) continue;
        int wx, wy, ww, wh;
        wm_get_window_rect(wm, idx, &wx, &wy, &ww, &wh);
        (void)wh;
        if (mx >= wx + ww - WM_BTN_W * 2 - WM_BTN_GAP - 8 &&
            mx <  wx + ww - WM_BTN_W - WM_BTN_GAP - 8 &&
            my >= wy + 4 && my < wy + WM_TITLE_H - 4) return idx;
    }
    return -1;
}

int wm_hit_border(wm_state_t *wm, int mx, int my, int *edge) {
    *edge = WM_EDGE_NONE;
    int bw = WM_BORDER_W + 4;
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int idx = wm->z_order[i];
        if (wm->windows[idx].state != WM_STATE_NORMAL) continue;
        int wx, wy, ww, wh;
        wm_get_window_rect(wm, idx, &wx, &wy, &ww, &wh);
        if (mx < wx - bw || mx > wx + ww + bw ||
            my < wy - bw || my > wy + wh + bw) continue;

        int on_n = (my >= wy - bw && my < wy + bw);
        int on_s = (my > wy + wh - bw && my <= wy + wh + bw);
        int on_w = (mx >= wx - bw && mx < wx + bw);
        int on_e = (mx > wx + ww - bw && mx <= wx + ww + bw);

        if (on_n && on_w) *edge = WM_EDGE_NW;
        else if (on_n && on_e) *edge = WM_EDGE_NE;
        else if (on_s && on_w) *edge = WM_EDGE_SW;
        else if (on_s && on_e) *edge = WM_EDGE_SE;
        else if (on_n) *edge = WM_EDGE_N;
        else if (on_s) *edge = WM_EDGE_S;
        else if (on_w) *edge = WM_EDGE_W;
        else if (on_e) *edge = WM_EDGE_E;

        if (*edge != WM_EDGE_NONE) return idx;
    }
    return -1;
}

int wm_hit_start_menu(wm_state_t *wm, int mx, int my) {
    if (!wm->start_menu_open) return -1;
    if (mx >= wm->menu_x && mx < wm->menu_x + wm->menu_w &&
        my >= wm->menu_y && my < wm->menu_y + wm->menu_h) {
        int row = (my - wm->menu_y - 44) / 32;
        if (row >= 0 && row < WM_START_MENU_ROWS) return row;
    }
    return -1;
}

/* ================================================================
 * wm_update_animations — 更新所有窗口动画
 * ================================================================ */

void wm_update_animations(wm_state_t *wm) {
    for (int i = 0; i < wm->window_count; i++) {
        wm_window_t *win = &wm->windows[i];
        if (!win->used || win->anim_type == WM_ANIM_NONE) continue;
        
        win->anim_frame++;
        
        if (win->anim_frame >= win->anim_total) {
            /* 动画完成 */
            win->anim_type = WM_ANIM_NONE;
            win->anim_frame = 0;
            win->opacity = 255;  /* 确保完全不透明 */
        } else {
            /* 使用固定步长和整数运算计算动画进度与缓动 */
            int total = win->anim_total > 0 ? win->anim_total : 8;
            int frame = win->anim_frame;
            if (frame > total) frame = total;
            
            int F = total - frame;
            /* eased_scaled ranges from 0 to 512, where 512 is 100% */
            int eased_scaled = 512 - ((F * F * F) * 512) / (total * total * total);

            switch (win->anim_type) {
                case WM_ANIM_MINIMIZE:
                    /* 最小化：淡出 */
                    win->opacity = (uint8_t)(255 * (total - frame) / total);
                    break;

                case WM_ANIM_OPEN:
                    /* 打开：缓动淡入 (40 -> 255) */
                    win->opacity = (uint8_t)(40 + (215 * eased_scaled) / 512);
                    break;
                    
                case WM_ANIM_MAXIMIZE:
                case WM_ANIM_RESTORE:
                    /* 最大化/还原：平滑过渡位置和大小 */
                    {
                        int target_x = win->x;
                        int target_y = win->y;
                        int target_w = win->w;
                        int target_h = win->h;
                        
                        /* 临时设置动画中的位置（用于渲染） */
                        win->x = win->anim_start_x + ((target_x - win->anim_start_x) * eased_scaled) / 512;
                        win->y = win->anim_start_y + ((target_y - win->anim_start_y) * eased_scaled) / 512;
                        win->w = win->anim_start_w + ((target_w - win->anim_start_w) * eased_scaled) / 512;
                        win->h = win->anim_start_h + ((target_h - win->anim_start_h) * eased_scaled) / 512;
                    }
                    break;
            }
        }
    }
}

/* ================================================================
 * wm_has_active_animations — 检查是否有活动的动画
 * ================================================================ */

int wm_has_active_animations(wm_state_t *wm) {
    for (int i = 0; i < wm->window_count; i++) {
        if (wm->windows[i].used && wm->windows[i].anim_type != WM_ANIM_NONE) {
            return 1;
        }
    }
    return 0;
}
