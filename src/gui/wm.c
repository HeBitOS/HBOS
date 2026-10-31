#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "wm.h"
#include "gui_effects.h"
#include "gui_dirty.h"
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

/** 查询某 mode 是否已显式设置过标题（未设置返回 NULL）。
 * 供桌面初始化时"只给缺标题的应用补默认值"，避免覆盖精心设定的中文名。 */
const char *wm_window_title_by_mode(int mode);

/* 记录"还原几何"（定义在最大化那一节）。这里提前声明，因为
 * wm_minimize_window 也要用同一套规则拍快照。 */
static void wm_save_restore(wm_window_t *win);

const char *wm_window_title_by_mode(int mode) {
    if (mode < 0 || mode >= WM_APP_TITLE_COUNT) return 0;
    return app_titles[mode];
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
    wm->top_bar_h = 0;
    wm->layer_seq = 0;
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

/* ── layer → z_order ────────────────────────────────────────────────
 * 绘制（draw_gui_frame 按 z_order 从底到顶画）与命中测试（从顶往底找）
 * 都只认 z_order；layer 原先只是个"摆设数字"——所以「降维打击」改完
 * layer 之后画面纹丝不动（实测就是完全不工作）。
 *
 * 现在反过来：layer 是唯一真源，z_order 每次层级变化后由它重建。
 * 窗口数 ≤ 16，且绝大多数调用时已经有序，插入排序实际接近 O(n)。 */
static void wm_sync_z(wm_state_t *wm) {
    int n = 0;
    for (int i = 0; i < wm->window_count && i < WM_MAX_WINDOWS; i++) {
        if (wm->windows[i].used) wm->z_order[n++] = i;
    }
    for (int i = 1; i < n; i++) {
        int v = wm->z_order[i];
        uint32_t lv = wm->windows[v].layer;
        int j = i - 1;
        while (j >= 0 && wm->windows[wm->z_order[j]].layer > lv) {
            wm->z_order[j + 1] = wm->z_order[j];
            j--;
        }
        wm->z_order[j + 1] = v;
    }
    /* 未使用的槽位填进自身索引，保持数组"无重复且下标合法"
     * （有些老代码仍会无脑遍历整张表）。 */
    for (int i = n; i < WM_MAX_WINDOWS; i++) wm->z_order[i] = i;
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
    win->snap_side = WM_SNAP_NONE;
    win->x = 130 + (idx % 5) * 28;
    win->y = 62 + (idx % 5) * 22;
    win->w = 0;
    win->h = 0;
    win->layer = 0;
    win->knock_cd = 0;
    win->knock_flash = 0;
    if (gui_effects_enabled()) {
        /* 打开时淡入。帧数由策略层给：100Hz 下 10 帧 = 100ms，
         * 符合笔记「时长 ≤180ms」。 */
        win->anim_type = WM_ANIM_OPEN;
        win->anim_frame = 0;
        win->anim_total = gui_effects_max_frames();
        if (win->anim_total <= 0) win->anim_total = 1;
        win->anim_ms = (uint32_t)gui_effects_duration_ms();
        win->anim_t0 = gui_effects_now_ticks();
        win->anim_scale = 92;
        win->opacity = 40;   /* 起始淡显，update 会逐帧拉满 */
    } else {
        /* 动效关闭：绝不以半透明创建。若仍然设 opacity=40，创建帧会以
         * 40% 透明度上屏，下一帧虽然收敛成 255，但窗口已不在动效脏区
         * 列表里，不会再重绘 —— 窗口就永远透明（用户实测 bug）。 */
        win->anim_type = WM_ANIM_NONE;
        win->anim_frame = 0;
        win->anim_total = 1;
        win->opacity = 255;
    }
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
    /* 关窗会整体前移索引，靠手写的 z_order 修补很容易漏一格；
     * 直接按 layer 重建，顺带把"未使用槽位"复位。 */
    wm_sync_z(wm);
}

void wm_focus_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;
    if (!wm->windows[idx].used) return;
    if (wm->windows[idx].state == WM_STATE_MINIMIZED)
        wm_restore_window(wm, idx);
    wm->active_window = idx;
    wm_bring_to_top(wm, idx);
    /* 聚焦 = 提到最前一层（"维度"提升）。层级是 32 位单调序号，笔记要求
     * 上限 2^32-1；快撞顶时整体重标定，避免回绕后新旧层反转。 */
    wm_window_layer(wm, idx);   /* 内部处理重标定 */
    wm->windows[idx].layer = ++wm->layer_seq;
    /* layer 变了就要立刻落到 z_order 上，否则绘制顺序还是旧的——
     * 焦点窗口会"看起来没被提到最前"（点任务栏按钮后窗口还在下面）。 */
    wm_sync_z(wm);
}

/* ── 「降维打击」层级 ──────────────────────────────────────────────
 * 手稿②：拖动窗口时把之前打开的窗口按层级渲染成 3D；被拖窗口撞到某个
 * 窗口时，把它打到下一层级。
 *
 * 实现要点：
 *  - 层级是 uint32 单调序号，上限 UINT32_MAX（2^32-1）；
 *  - 序号逼近上限时把全场最小层级平移到 0 附近重标定，保持相对次序；
 *  - 降层不会低于 0，也不会跨过撞击者（撞击者永远在它上面）。 */

static void wm_renormalize_layers(wm_state_t *wm) {
    uint32_t min_layer = WM_LAYER_MAX;
    for (int i = 0; i < wm->window_count; i++) {
        if (!wm->windows[i].used) continue;
        if (wm->windows[i].layer < min_layer) min_layer = wm->windows[i].layer;
    }
    if (min_layer == WM_LAYER_MAX || min_layer == 0) return;
    for (int i = 0; i < wm->window_count; i++) {
        if (wm->windows[i].used) wm->windows[i].layer -= min_layer;
    }
    wm->layer_seq = wm->layer_seq > min_layer ? wm->layer_seq - min_layer : 0;
}

uint32_t wm_window_layer(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return 0;
    if (!wm->windows[idx].used) return 0;
    if (wm->layer_seq > WM_LAYER_MAX - 64u) wm_renormalize_layers(wm);
    return wm->windows[idx].layer;
}

uint32_t wm_knock_down(wm_state_t *wm, int idx, uint32_t target_layer) {
    if (idx < 0 || idx >= wm->window_count) return 0;
    if (!wm->windows[idx].used) return 0;
    /* 已经比撞击者低了：撞击对"更低一层"没有意义，原样返回。
     * （旧实现里这里算的是 target_layer-1 的"空位"，而撞击者永远是最高层，
     * 于是被撞窗口反而被抬到次高层——方向整个反了。） */
    if (wm->windows[idx].layer < target_layer) return wm->windows[idx].layer;
    wm_sink_one(wm, idx);
    return wm->windows[idx].layer;
}

int wm_sink_one(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count || !wm->windows[idx].used) return 0;
    uint32_t self = wm->windows[idx].layer;
    int below = -1;
    uint32_t best = 0;
    for (int i = 0; i < wm->window_count; i++) {
        if (i == idx || !wm->windows[i].used) continue;
        uint32_t l = wm->windows[i].layer;
        if (l >= self) continue;
        if (below < 0 || l > best) { below = i; best = l; }
    }
    if (below < 0) return 0;          /* 已经在最底层，沉无可沉 */
    wm->windows[idx].layer = best;
    wm->windows[below].layer = self;
    wm_sync_z(wm);
    return 1;
}

void wm_tick_knock(wm_state_t *wm) {
    for (int i = 0; i < wm->window_count; i++) {
        wm_window_t *w = &wm->windows[i];
        if (!w->used) continue;
        if (w->knock_cd > 0) w->knock_cd--;
        if (w->knock_flash > 0) w->knock_flash--;
    }
}

int wm_windows_collide(wm_state_t *wm, int a, int b) {
    if (a == b) return 0;
    if (a < 0 || b < 0 || a >= wm->window_count || b >= wm->window_count) return 0;
    wm_window_t *wa = &wm->windows[a], *wb = &wm->windows[b];
    if (!wa->used || !wb->used) return 0;
    if (wa->state == WM_STATE_MINIMIZED || wb->state == WM_STATE_MINIMIZED) return 0;
    int aw = wa->w > 0 ? wa->w : 1, ah = wa->h > 0 ? wa->h : 1;
    int bw = wb->w > 0 ? wb->w : 1, bh = wb->h > 0 ? wb->h : 1;
    return wa->x < wb->x + bw && wb->x < wa->x + aw &&
           wa->y < wb->y + bh && wb->y < wa->y + ah;
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
    if (y < wm->top_bar_h) y = wm->top_bar_h;
    int bot_margin = wm->top_bar_h > 0 ? 0 : WM_TASKBAR_H;
    int max_y = wm->desk_h - bot_margin - WM_TITLE_H;
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
    if (win->state == WM_STATE_MINIMIZED) return;
    /* 只从"真正的正常态"拍快照：吸附态（半屏）下的几何是临时的，
     * 记进去会让恢复后的窗口永远卡在半屏。最大化态更不必记 ——
     * 它自己的 restore 里就存着最大化前的尺寸。
     * 以前这里只看 state == NORMAL，而吸附态的 state 恰好也是 NORMAL。 */
    if (win->state == WM_STATE_NORMAL && win->snap_side == WM_SNAP_NONE) {
        wm_save_restore(win);
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

/* 记录"还原几何"。调用方必须保证此刻窗口就是它的正常/吸附前尺寸。 */
static void wm_save_restore(wm_window_t *win) {
    if (win->w <= 0 || win->h <= 0) return;   /* 未定尺寸的窗口不覆盖旧快照 */
    win->restore.x = win->x;
    win->restore.y = win->y;
    win->restore.w = win->w;
    win->restore.h = win->h;
}

/* 几何过渡时刻的"当前可见矩形"：优先取过渡插值中的实时值，
 * 其次才退回 win->x/y/w/h。否则在开口动画/上一次最大化过渡还没收敛时
 * 再触发一次，anim_from 会取到"上一段过渡的起点"，窗口看起来先跳一下。 */
static void wm_current_rect(wm_state_t *wm, wm_window_t *win, int idx,
                            int *x, int *y, int *w, int *h) {
    if (win->anim_type == WM_ANIM_MAXIMIZE || win->anim_type == WM_ANIM_RESTORE) {
        wm_get_window_rect(wm, idx, x, y, w, h);
        return;
    }
    int def_w = wm->desk_w > 920 ? 790 : wm->desk_w - 120;
    int def_h = wm->desk_h > 620 ? 430 : wm->desk_h - 140;
    if (win->anim_type == WM_ANIM_MINIMIZE) {
        wm_get_window_rect(wm, idx, x, y, w, h);  /* 最小化中是中心缩放后的矩形 */
        return;
    }
    *x = win->x;
    *y = win->y;
    *w = win->w > 0 ? win->w : def_w;
    *h = win->h > 0 ? win->h : def_h;
}

void wm_maximize_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;
    wm_window_t *win = &wm->windows[idx];
    if (!win->used) return;
    /* 已经是最大化就别再"最大化"一次：以前这里会重拍一次 restore 快照，
     * 把快照刷成全屏尺寸 —— 于是"还原"回来的还是满屏（用户实测 bug）。 */
    if (win->state == WM_STATE_MAXIMIZED) return;

    int from_x, from_y, from_w, from_h;
    if (win->snap_side != WM_SNAP_NONE) {
        /* 从半屏吸附直接最大化：restore 里已经存着吸附前的正常尺寸，
         * 不能再用半屏几何覆盖它。过渡起点用当前可见的半屏矩形。 */
        wm_current_rect(wm, win, idx, &from_x, &from_y, &from_w, &from_h);
    } else {
        if (win->state == WM_STATE_NORMAL) {
            from_x = win->x;
            from_y = win->y;
            from_w = win->w > 0 ? win->w : (wm->desk_w > 920 ? 790 : wm->desk_w - 120);
            from_h = win->h > 0 ? win->h : (wm->desk_h > 620 ? 430 : wm->desk_h - 140);
            wm_save_restore(win);
        } else {
            /* 最小化状态被直接最大化：保留它最小化前存的 restore。 */
            wm_current_rect(wm, win, idx, &from_x, &from_y, &from_w, &from_h);
        }
    }
    win->snap_side = WM_SNAP_NONE;

    /* 几何过渡：只重排单个窗口，普通权限也放行（见 gui_effects_allowed_geometry）。
     * 过渡期间 win->x/y/w/h 存的是最终目标，起点单独存在 anim_from，
     * 避免自引用——之前几何动画"永远收敛不到全屏并产生拖影"就是这个。 */
    if (gui_effects_allowed_geometry() && from_w > 0 && from_h > 0) {
        win->anim_from.x = from_x;
        win->anim_from.y = from_y;
        win->anim_from.w = from_w;
        win->anim_from.h = from_h;
        win->anim_frame = 0;
        win->anim_total = gui_effects_max_frames();
        if (win->anim_total <= 0) win->anim_total = 1;
        win->anim_ms = (uint32_t)gui_effects_duration_ms();
        win->anim_t0 = gui_effects_now_ticks();
        win->anim_scale = 100;
        win->anim_type = WM_ANIM_MAXIMIZE;
    } else {
        win->anim_type = WM_ANIM_NONE;
    }
    win->state = WM_STATE_MAXIMIZED;
    win->x = 0;
    win->y = wm->top_bar_h;
    win->w = wm->desk_w;
    win->h = wm->desk_h - (wm->top_bar_h > 0 ? wm->top_bar_h : WM_TASKBAR_H);
}

void wm_restore_window(wm_state_t *wm, int idx) {
    if (idx < 0 || idx >= wm->window_count) return;
    wm_window_t *win = &wm->windows[idx];
    if (!win->used) return;
    if (win->state == WM_STATE_NORMAL) return;

    /* 还原目标：restore 里存的是"最大化/吸附/最小化之前"的正常几何。
     * 以前这里有个致命 bug —— 目标写在函数尾部，而过渡起点用
     * wm_get_window_rect() 取，此刻 state 还是 MAXIMIZED，取到的也是
     * 全屏，于是 anim_from == 目标，要么原地不动、要么收敛后仍是满屏。 */
    int def_w = wm->desk_w > 920 ? 790 : wm->desk_w - 120;
    int def_h = wm->desk_h > 620 ? 430 : wm->desk_h - 140;
    int tx, ty, tw, th;
    if (win->restore.w > 0 && win->restore.h > 0) {
        tx = win->restore.x;
        ty = win->restore.y;
        tw = win->restore.w;
        th = win->restore.h;
    } else {
        /* 从未有过正常几何快照：给一个居中的默认尺寸，绝不能停在全屏。 */
        tw = def_w;
        th = def_h;
        tx = (wm->desk_w - tw) / 2;
        ty = wm->top_bar_h + (wm->desk_h - wm->top_bar_h - th) / 2;
    }

    /* 不再要求 anim_type == NONE：最大化过渡没走完就点还原（双击标题栏
     * 连按、或按钮连点）时，旧逻辑会落到瞬时分支，窗口"啪"地跳回正常态。
     * 这里改成"从当前可见矩形重新起一段"——CSS transition 中途反向的语义。 */
    int fx_ok = gui_effects_allowed_geometry() && tw > 0 && th > 0;
    int cx, cy, cw, ch;
    if (fx_ok) {
        wm_get_window_rect(wm, idx, &cx, &cy, &cw, &ch);
        win->anim_from.x = cx;
        win->anim_from.y = cy;
        win->anim_from.w = cw;
        win->anim_from.h = ch;
        win->anim_frame = 0;
        win->anim_total = gui_effects_max_frames();
        if (win->anim_total <= 0) win->anim_total = 1;
        win->anim_ms = (uint32_t)gui_effects_duration_ms();
        win->anim_t0 = gui_effects_now_ticks();
        win->anim_scale = 100;
        win->anim_type = WM_ANIM_RESTORE;
    } else {
        win->anim_type = WM_ANIM_NONE;
    }

    win->state = WM_STATE_NORMAL;
    win->snap_side = WM_SNAP_NONE;
    /* 先落地目标几何，再记快照：否则下一次最大化会把"全屏尺寸"当成
     * 正常尺寸存进 restore，还原就再也回不到原样了。 */
    win->x = tx;
    win->y = ty;
    win->w = tw;
    win->h = th;
}

/* 目标矩形（不改变状态）：给"拖动最大化窗口标题栏先还原再用"，
 * 让 UI 层能在一次调用里拿到还原后的窗口矩形，不必二次读状态。 */
void wm_restore_target(const wm_state_t *wm, int idx, int *x, int *y, int *w, int *h) {
    const wm_window_t *win = (idx >= 0 && idx < wm->window_count && wm->windows[idx].used)
                             ? &wm->windows[idx] : NULL;
    int def_w = wm->desk_w > 920 ? 790 : wm->desk_w - 120;
    int def_h = wm->desk_h > 620 ? 430 : wm->desk_h - 140;
    if (!win) { *x = *y = *w = *h = 0; return; }
    if (win->restore.w > 0 && win->restore.h > 0) {
        *x = win->restore.x;
        *y = win->restore.y;
        *w = win->restore.w;
        *h = win->restore.h;
    } else {
        *w = def_w;
        *h = def_h;
        *x = (wm->desk_w - *w) / 2;
        *y = wm->top_bar_h + (wm->desk_h - wm->top_bar_h - *h) / 2;
    }
    /* 夹到桌面内，避免还原到一个被顶栏吃掉的 y。 */
    int min_y = wm->top_bar_h > 0 ? wm->top_bar_h : 0;
    if (*y < min_y) *y = min_y;
    if (*x < 0) *x = 0;
    if (*x + *w > wm->desk_w) *x = wm->desk_w - *w;
    if (*x < 0) { *x = 0; *w = wm->desk_w; }
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
    // 记录还原位置：只在"真正从正常态开始吸附"时拍快照。
    // 已经吸附过、或从最大化直接吸附的，restore 里存的才是用户想要的
    // 正常尺寸，不能被半屏几何覆盖。
    if (win->state == WM_STATE_NORMAL && win->snap_side == WM_SNAP_NONE &&
        win->anim_type != WM_ANIM_MAXIMIZE && win->anim_type != WM_ANIM_RESTORE) {
        wm_save_restore(win);
    }
    win->anim_type = WM_ANIM_NONE;   /* 吸附走瞬时，不再叠几何过渡 */
    win->state = WM_STATE_NORMAL;
    win->snap_side = side;
    int half = wm->desk_w / 2;
    win->y = wm->top_bar_h;
    win->h = wm->desk_h - (wm->top_bar_h > 0 ? wm->top_bar_h : WM_TASKBAR_H);
    win->w = half;
    win->x = (side == WM_SNAP_LEFT) ? 0 : (wm->desk_w - half);
}

/* 从吸附态退出。吸附时 state 仍是 NORMAL，所以只能靠 snap_side 判断；
 * 不还原的话窗口会永远停在半屏尺寸（issue #8）。 */
int wm_unsnap_window(wm_state_t *wm, int idx) {
    wm_window_t *win = wm_get_window(wm, idx);
    if (!win || win->snap_side == WM_SNAP_NONE) return 0;
    win->snap_side = WM_SNAP_NONE;
    if (win->restore.w > 0 && win->restore.h > 0) {
        win->x = win->restore.x;
        win->y = win->restore.y;
        win->w = win->restore.w;
        win->h = win->restore.h;
    }
    return 1;
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

/* 以矩形中心为原点按 pct% 缩放（100 = 原尺寸）。用于打开/最小化的
 * "浮起/收起"手感：缩放围绕中心进行，窗口看起来是从中间长出来的，
 * 而不是从左上角拉伸。 */
static void wm_apply_center_scale(int *x, int *y, int *w, int *h, int pct);

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
    int def_y = wm->top_bar_h > 0 ? wm->top_bar_h + 10 : 42;

    if (win->state == WM_STATE_MAXIMIZED && win->anim_type == WM_ANIM_NONE) {
        *x = 0;
        *y = wm->top_bar_h;
        *w = wm->desk_w;
        *h = wm->desk_h - (wm->top_bar_h > 0 ? wm->top_bar_h : WM_TASKBAR_H);
        return;
    }

    if (win->anim_type == WM_ANIM_MAXIMIZE || win->anim_type == WM_ANIM_RESTORE) {
        /* 与 wm_update_animations 用同一套基于时间的 Ease-Out Cubic，
         * 保证淡入/几何位移两条曲线同源同步（早期按帧数插值，帧率一抖
         * 几何和透明度就走偏，看起来像"撕裂"）。
         * win->x/y/w/h 始终保存最终目标，起点单独存在 anim_from。 */
        int e = gui_effects_ease_progress(win->anim_t0, win->anim_ms);
        int tx = win->x, ty = win->y, tw = win->w > 0 ? win->w : def_w,
            th = win->h > 0 ? win->h : def_h;
        *x = win->anim_from.x + ((tx - win->anim_from.x) * e) / 512;
        *y = win->anim_from.y + ((ty - win->anim_from.y) * e) / 512;
        *w = win->anim_from.w + ((tw - win->anim_from.w) * e) / 512;
        *h = win->anim_from.h + ((th - win->anim_from.h) * e) / 512;
        if (*w < 80) *w = 80;
        if (*h < 60) *h = 60;
        return;
    }
    if (win->anim_type == WM_ANIM_MINIMIZE) {
        /* 最小化：几何按 anim_scale 以中心收缩，配合淡出。 */
        *x = (win->x || win->w > 0) ? win->x : def_x + idx * 24;
        *y = (win->y || win->h > 0) ? win->y : def_y + idx * 18;
        *w = win->w > 0 ? win->w : def_w;
        *h = win->h > 0 ? win->h : def_h;
        wm_apply_center_scale(x, y, w, h, win->anim_scale);
        return;
    }

    *x = (win->x || win->w > 0) ? win->x : def_x + idx * 24;
    *y = (win->y || win->h > 0) ? win->y : def_y + idx * 18;
    *w = def_w;
    *h = def_h;

    int bot_margin = wm->top_bar_h > 0 ? 0 : WM_TASKBAR_H;
    if (*x + *w > wm->desk_w - 4) *x = wm->desk_w - *w - 4;
    if (*y + *h > wm->desk_h - bot_margin - 4) *y = wm->desk_h - bot_margin - *h - 4;
    if (*x < 4) *x = 4;
    if (*y < wm->top_bar_h + 4) *y = wm->top_bar_h + 4;

    /* 打开动画期间同样按 anim_scale 缩放（居中原点），收敛时正好是
     * anim_scale == 100，与上面的正常路径无缝衔接。 */
    if (win->anim_type == WM_ANIM_OPEN)
        wm_apply_center_scale(x, y, w, h, win->anim_scale);
}

/* 以矩形中心为原点按 pct% 缩放（100 = 原尺寸）。用于打开/最小化的
 * "浮起/收起"手感：缩放围绕中心进行，窗口看起来是从中间长出来的，
 * 而不是从左上角拉伸。 */
static void wm_apply_center_scale(int *x, int *y, int *w, int *h, int pct) {
    if (pct <= 0 || pct == 100) return;
    int cx = *x + *w / 2;
    int cy = *y + *h / 2;
    int nw = (int)((int64_t)*w * pct / 100);
    int nh = (int)((int64_t)*h * pct / 100);
    if (nw < 40) nw = 40;
    if (nh < 30) nh = 30;
    *w = nw;
    *h = nh;
    *x = cx - nw / 2;
    *y = cy - nh / 2;
}

int wm_hit_titlebar(wm_state_t *wm, int mx, int my) {
    if (wm->start_menu_open && wm_hit_start_menu(wm, mx, my) >= 0) return -1;
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int idx = wm->z_order[i];
        if (wm->windows[idx].state == WM_STATE_MINIMIZED) continue;
        int wx, wy, ww, wh;
        wm_get_window_rect(wm, idx, &wx, &wy, &ww, &wh);
        /* 右侧三个按钮属于按钮层，不算拖动区。以前固定让出 80px，窗口变窄
         * 或按钮布局变了就会"点最小化变成拖窗口"。现在按实际按钮宽度算。 */
        int btns_w = WM_BTN_W * 3 + WM_BTN_GAP * 2 + 16;
        if (mx >= wx && mx < wx + ww - btns_w &&
            my >= wy && my < wy + WM_TITLE_H) return idx;
    }
    return -1;
}

/* 标题栏按钮的命中测试。
 *
 * 旧实现只从底到顶扫"窗口最上面那一行"，而且前提是 state == NORMAL ——
 * 于是窗口一旦最大化（state == MAXIMIZED），关闭/最小化/最大化三个按钮
 * 全都点不到，"点还原按钮没反应"就是这么来的（用户实测 bug）。
 * 现在：最大化窗口的按钮依旧可点；按钮区域整体让位给标题栏拖动判定
 * （wm_hit_titlebar 会排除这块），所以点击不会既拖动又弹窗。 */
static int wm_hit_title_button(wm_state_t *wm, int mx, int my, int slot) {
    for (int i = wm->window_count - 1; i >= 0; i--) {
        int idx = wm->z_order[i];
        const wm_window_t *win = &wm->windows[idx];
        if (!win->used) continue;
        if (win->state == WM_STATE_MINIMIZED) continue;
        int wx, wy, ww, wh;
        wm_get_window_rect(wm, idx, &wx, &wy, &ww, &wh);
        (void)wh;
        /* slot: 0 = 关闭（最右）、1 = 最小化、2 = 最大化/还原 */
        int right = wx + ww - 8 - WM_BTN_W * slot - WM_BTN_GAP * slot;
        int left  = right - WM_BTN_W;
        if (mx >= left && mx < right &&
            my >= wy + 4 && my < wy + WM_TITLE_H - 4) return idx;
    }
    return -1;
}

int wm_hit_close(wm_state_t *wm, int mx, int my) {
    return wm_hit_title_button(wm, mx, my, 0);
}

int wm_hit_minimize(wm_state_t *wm, int mx, int my) {
    return wm_hit_title_button(wm, mx, my, 1);
}

int wm_hit_maximize(wm_state_t *wm, int mx, int my) {
    return wm_hit_title_button(wm, mx, my, 2);
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
    /* 动效总开关（③）：关闭时所有动画立即收敛，不占帧。
     * 策略层在 gui_effects.c，可被设置应用 / 命令修改。 */
    int fx = gui_effects_enabled();
    wm_tick_knock(wm);
    for (int i = 0; i < wm->window_count; i++) {
        wm_window_t *win = &wm->windows[i];
        if (!win->used || win->anim_type == WM_ANIM_NONE) continue;

        if (!fx) {
            /* 动效被中途关闭：立即收敛。若 opacity 原本 < 255，屏幕上
             * 还留着半透明的一帧，必须标记整屏重绘把它刷掉，否则窗口
             * 不在动效脏区列表里、永远停在半透明状态。 */
            if (win->opacity != 255) gui_dirty_mark_full();
            win->anim_type = WM_ANIM_NONE;
            win->anim_frame = 0;
            win->opacity = 255;
            continue;
        }

        /* 按真实时间推进，而不是按帧计数。
         *
         * 帧计数的问题：主循环一帧可能是 2ms 也可能 40ms（有窗口在渲染
         * 内容时），10 帧的淡入于是有时 20ms 结束、有时 400ms 拖成卡顿，
         * 同一条动画曲线在不同负载下走得完全不一样 —— 这就是"不够顺滑"
         * 的根源。改成 t0 + 时长之后，曲线只取决于时间，与帧率解耦。 */
        int eased_scaled = gui_effects_ease_progress(win->anim_t0, win->anim_ms);
        /* anim_frame 仅作"已走到的帧号"给兼容代码看，不再驱动进度 */
        win->anim_frame = (int)((uint64_t)eased_scaled * (uint32_t)(win->anim_total > 0 ? win->anim_total : 1) / 512u);

        if (eased_scaled >= 512) {
            /* 动画完成 */
            win->anim_type = WM_ANIM_NONE;
            win->anim_frame = 0;
            win->opacity = 255;  /* 确保完全不透明 */
            continue;
        }

        switch (win->anim_type) {
            case WM_ANIM_NONE:
                break;   /* 无动画：不该走到这里，显式处理以消除 -Wswitch */

            case WM_ANIM_MINIMIZE: {
                /* 最小化：淡出 + 轻微缩小（缩到 88%，比纯淡出有"收起来"的
                 * 方向感，也更接近参考视频里的观感）。几何在
                 * wm_get_window_rect 里按 anim_scale 插值。 */
                win->opacity = (uint8_t)(255 - (255 * eased_scaled) / 512);
                win->anim_scale = 100 - (12 * eased_scaled) / 512;   /* 100 → 88 */
                break;
            }

            case WM_ANIM_OPEN:
                /* 打开：淡入 + 从 92% 放大到 100%。缩放是"现代桌面"手感
                 * 的关键——只有透明度时窗口像贴纸，有了缩放才有纵深。 */
                win->opacity = (uint8_t)(40 + (215 * eased_scaled) / 512);
                win->anim_scale = 92 + (8 * eased_scaled) / 512;     /* 92 → 100 */
                break;

            case WM_ANIM_MAXIMIZE:
            case WM_ANIM_RESTORE:
                /* 几何过渡在 wm_get_window_rect 里按同一套缓动插值计算，
                 * 这里绝不能改 win->x/y/w/h —— 它们保存的是最终目标，之前
                 * 在此自引用地覆盖目标，导致永远收敛不到全屏并产生拖影。 */
                win->anim_scale = 100;
                break;
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
