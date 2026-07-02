#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "wm.h"
#include "../string.h"

static int g_titles_ready = 0;

#define WM_APP_TITLE_COUNT 16

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
    /* 边界钳制：保证标题栏始终可达，否则窗口拖出屏幕/塞进任务栏底下
     * 就再也抓不回来了。横向两侧各留 80px 可见；纵向不许高于屏幕顶、
     * 不许把标题栏藏进任务栏。 */
    int est_w = win->w > 0 ? win->w : 640;   /* 未定尺寸窗口的保守估计 */
    int keep = ui_s(80);
    if (x < keep - est_w) x = keep - est_w;
    if (x > wm->desk_w - keep) x = wm->desk_w - keep;
    if (y < 0) y = 0;
    int max_y = wm->desk_h - WM_TASKBAR_H - WM_TITLE_H;
    if (y > max_y) y = max_y;
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
    
    /* 瞬时最小化。 */
    win->anim_type = WM_ANIM_NONE;
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
    /* 瞬时最大化（几何动画反复出 bug，改为直接铺满）。 */
    win->anim_type = WM_ANIM_NONE;
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
    /* 瞬时还原。 */
    win->anim_type = WM_ANIM_NONE;
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
        wm->menu_w = SM_W;
        wm->menu_h = SM_H;
        /* center horizontally above the taskbar */
        wm->menu_x = (wm->desk_w - SM_W) / 2;
        if (wm->menu_x < 8) wm->menu_x = 8;
        wm->menu_y = wm->desk_h - WM_TASKBAR_H - SM_H - 8;
        if (wm->menu_y < 8) wm->menu_y = 8;
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

    if (win->anim_type == WM_ANIM_MAXIMIZE || win->anim_type == WM_ANIM_RESTORE) {
        /* Ease (cubic ease-out) from the captured start rect toward the target
         * rect (win->x/y/w/h), non-destructively — win->* keep the final values. */
        int total = win->anim_total > 0 ? win->anim_total : 8;
        int frame = win->anim_frame; if (frame > total) frame = total;
        int F = total - frame;
        int e = 512 - ((F * F * F) * 512) / (total * total * total);  /* 0..512 */
        int tx = win->x, ty = win->y, tw = win->w > 0 ? win->w : def_w,
            th = win->h > 0 ? win->h : def_h;
        *x = win->anim_start_x + ((tx - win->anim_start_x) * e) / 512;
        *y = win->anim_start_y + ((ty - win->anim_start_y) * e) / 512;
        *w = win->anim_start_w + ((tw - win->anim_start_w) * e) / 512;
        *h = win->anim_start_h + ((th - win->anim_start_h) * e) / 512;
        if (*w < 80) *w = 80;
        if (*h < 60) *h = 60;
        return;
    }
    if (win->anim_type == WM_ANIM_MINIMIZE) {
        /* Minimize is an opacity fade; geometry stays put. */
        *x = (win->x || win->w > 0) ? win->x : def_x + idx * 24;
        *y = (win->y || win->h > 0) ? win->y : def_y + idx * 18;
        *w = win->w > 0 ? win->w : def_w;
        *h = win->h > 0 ? win->h : def_h;
        return;
    }

    *x = (win->x || win->w > 0) ? win->x : def_x + idx * 24;
    *y = (win->y || win->h > 0) ? win->y : def_y + idx * 18;
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
    int ox = wm->menu_x, oy = wm->menu_y;
    if (mx < ox || mx >= ox + SM_W || my < oy || my >= oy + wm->menu_h) return -1;
    int lx = mx - ox, ly = my - oy;

    /* 搜索框：保持菜单打开，由 gui.c 处理 */
    if (ly >= SM_SEARCH_TOP && ly < SM_SEARCH_TOP + SM_SEARCH_H)
        return SM_SEARCH_ITEM;

    /* 动态额外高度（gui.c 开菜单时按应用数写入 menu_h） */
    int extra = wm->menu_h - SM_H;
    if (extra < 0) extra = 0;
    int grid_h = SM_GRID_H + extra;

    /* 应用网格：返回可见单元格序号（gui.c 据搜索过滤映射为实际应用） */
    if (ly >= SM_GRID_TOP && ly < SM_GRID_TOP + grid_h) {
        int col = (lx - SM_PAD) * SM_GRID_COLS / (SM_W - 2 * SM_PAD);
        int row = (ly - SM_GRID_TOP) / SM_CELL_H;
        if (col < 0) col = 0;
        if (col >= SM_GRID_COLS) col = SM_GRID_COLS - 1;
        int cell = row * SM_GRID_COLS + col;
        return SM_CELL_BASE + cell;
    }

    /* bottom bar — Shell and Power buttons（随额外高度下移） */
    if (ly >= SM_BAR_TOP + extra) {
        /* Shell button is in the right half, Power is far right */
        int btn_w = 88, gap = 8;
        int power_x = SM_W - SM_PAD - btn_w;
        int shell_x = power_x - btn_w - gap;
        if (lx >= power_x && lx < power_x + btn_w) return SM_POWER_ITEM;
        if (lx >= shell_x && lx < shell_x + btn_w) return SM_SHELL_ITEM;
    }
    return -1;  /* click inside menu but not on an item */
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
                    /* 几何过渡在 wm_get_window_rect 里按 anim_frame 插值计算，
                     * 这里绝不能改 win->x/y/w/h —— 它们保存的是最终目标，之前
                     * 在此自引用地覆盖目标，导致永远收敛不到全屏并产生拖影。 */
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
