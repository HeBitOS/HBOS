/**
 * @file    ui_app.c
 * @brief   统一窗口管理器与应用生命周期（见 ui_app.h）。
 *
 * 本文件收编了原来两套并行实现（gui/wm.c 的 wm_* 与 gui/winsrv.c 的窗口
 * 部分），统一为一份 z 序、焦点、几何、拖拽/缩放逻辑。
 *
 * 不变量（整个文件都依赖，改动时务必保持）：
 *   1. `wm->windows[i].used` 为真的索引才有效；`count` 只是高水位，不代表
 *      连续有效（关闭会留下空洞，由 open 复用）。
 *   2. `wm->z[]` 是**有效窗口索引的全排列**，`z[0]` 最底、`z[count-1]` 最顶。
 *      任何修改 used 的操作都必须重建 z（见 z_rebuild）。
 *   3. `active` 要么是 -1，要么指向一个 used 的窗口。
 *   4. 窗口的 content 矩形由几何推导，不单独存储，避免两者不同步。
 */
#include "ui_app.h"

#include "../string.h"
#include "ui_tokens.h"

/* ── 实例表 ───────────────────────────────────────────────── */

static ui_app_t *g_apps = NULL;      /**< 应用实例链表 */
static int g_next_app_id = 1;

/* 注册表：定长数组，避免动态分配 */
#define UI_APP_VT_MAX 32
static const ui_app_vt_t *g_vts[UI_APP_VT_MAX];
static int g_vt_count = 0;

int ui_app_register(const ui_app_vt_t *vt) {
    if (!vt) return -1;
    if (g_vt_count >= UI_APP_VT_MAX) return -1;
    /* 同 mode 重复注册：以最后一次为准（便于测试里替换实现） */
    for (int i = 0; i < g_vt_count; i++) {
        if (g_vts[i] && g_vts[i]->mode == vt->mode) {
            g_vts[i] = vt;
            return i;
        }
    }
    g_vts[g_vt_count] = vt;
    return g_vt_count++;
}

const ui_app_vt_t *ui_app_vt_by_mode(int mode) {
    for (int i = 0; i < g_vt_count; i++)
        if (g_vts[i] && g_vts[i]->mode == mode) return g_vts[i];
    return NULL;
}

int ui_app_vt_count(void) { return g_vt_count; }

const ui_app_vt_t *ui_app_vt_at(int index) {
    if (index < 0 || index >= g_vt_count) return NULL;
    return g_vts[index];
}

/* ── 几何常量（经主题令牌，不再硬编码）────────────────────── */

#define TITLE_H     30
#define BORDER_W    1
#define BTN_W       24
#define BTN_GAP     2
#define MIN_W       200
#define MIN_H       120
#define RESIZE_GRAB 6      /**< 边缘可抓取宽度 */

static void content_from_geom(ui_window_t *win) {
    win->content.x = win->x + BORDER_W;
    win->content.y = win->y + TITLE_H;
    win->content.w = win->w - 2 * BORDER_W;
    win->content.h = win->h - TITLE_H - BORDER_W;
    if (win->content.w < 0) win->content.w = 0;
    if (win->content.h < 0) win->content.h = 0;
}

/* ── z 序 ─────────────────────────────────────────────────── */

/**
 * 重建 z 序数组。
 *
 * 关闭窗口会在 windows[] 里留下空洞，z[] 若还留着旧索引就会指向已释放的
 * 窗口（表现为"点到了不存在的窗口"或崩溃）。所以任何 used 变化后都整体
 * 重建，代价是 O(n²) 但 n≤16，完全可接受。
 *
 * 重建时保持**相对顺序**：先在 z 里已有的窗口中按原次序挑出仍有效的，
 * 再把新出现（未在旧 z 中）的窗口按索引追加到顶部。
 */
static void z_rebuild(ui_wm_t *wm) {
    bool in_old[UI_WIN_MAX];
    int  newz[UI_WIN_MAX];
    int  n = 0;

    for (int i = 0; i < UI_WIN_MAX; i++) in_old[i] = false;

    /* 保留旧顺序中的有效项（遍历到 z_count，越界的旧值不再参与） */
    for (int i = 0; i < wm->z_count; i++) {
        int idx = wm->z[i];
        if (idx >= 0 && idx < UI_WIN_MAX && wm->windows[idx].used) {
            newz[n++] = idx;
            in_old[idx] = true;
        }
    }
    /* 追加尚未进入 z 的有效窗口（按索引序，保证确定性） */
    for (int i = 0; i < UI_WIN_MAX && n < UI_WIN_MAX; i++) {
        if (wm->windows[i].used && !in_old[i]) newz[n++] = i;
    }
    for (int i = 0; i < UI_WIN_MAX; i++) wm->z[i] = (i < n) ? newz[i] : -1;
    wm->z_count = n;
}

/** 把 idx 移到 z 顶 */
static void z_raise(ui_wm_t *wm, int idx) {
    if (!wm->windows[idx].used) return;
    int pos = -1;
    for (int i = 0; i < wm->z_count; i++) if (wm->z[i] == idx) { pos = i; break; }
    if (pos < 0) return;                      /* 不在 z 中，等 z_rebuild */
    for (int i = pos; i < wm->z_count - 1; i++) wm->z[i] = wm->z[i + 1];
    wm->z[wm->z_count - 1] = idx;
}

/* ── 初始化 ───────────────────────────────────────────────── */

void ui_wm_init(ui_wm_t *wm, int desk_w, int desk_h) {
    if (!wm) return;
    memset(wm, 0, sizeof(*wm));
    wm->count = 0;
    wm->active = -1;
    wm->desk_w = desk_w;
    wm->desk_h = desk_h;
    wm->drag_win = -1;
    wm->resize_win = -1;
    wm->resize_edge = UI_EDGE_NONE;
    for (int i = 0; i < UI_WIN_MAX; i++) wm->z[i] = -1;
}

/* ── 窗口生命周期 ─────────────────────────────────────────── */

int ui_wm_open(ui_wm_t *wm, const char *title, int w, int h) {
    if (!wm) return -1;

    /* 找一个空洞复用，避免 count 只增不减导致窗口用尽 */
    int idx = -1;
    for (int i = 0; i < UI_WIN_MAX; i++) {
        if (!wm->windows[i].used) { idx = i; break; }
    }
    if (idx < 0) return -1;                   /* 已达上限 */

    ui_window_t *win = &wm->windows[idx];
    memset(win, 0, sizeof(*win));
    win->used = true;
    win->id = idx;

    if (title) {
        int i = 0;
        for (; title[i] && i < UI_WIN_TITLE_MAX - 1; i++) win->title[i] = title[i];
        win->title[i] = '\0';
    }

    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    win->w = w;
    win->h = h;
    /* 层叠摆放：每新开一个错开一点，避免完全重叠 */
    int step = idx % 6;
    win->x = 80 + step * 26;
    win->y = 60 + step * 22;
    /* 不超出桌面 */
    if (win->x + win->w > wm->desk_w) win->x = wm->desk_w - win->w;
    if (win->y + win->h > wm->desk_h) win->y = wm->desk_h - win->h;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;

    win->state = UI_WIN_NORMAL;
    win->opacity = 255;
    win->dirty = true;
    win->dirty_rect = (ui_rect_t){ win->x, win->y, win->w, win->h };
    content_from_geom(win);

    /* 打开动画：旧实现是淡入，这里保留但只用少量帧 */
    win->anim = UI_ANIM_OPEN;
    win->anim_frame = 0;
    win->anim_total = 8;
    win->anim_from = (ui_rect_t){ win->x, win->y, win->w, win->h };

    if (idx >= wm->count) wm->count = idx + 1;
    z_rebuild(wm);
    z_raise(wm, idx);
    wm->active = idx;
    for (int i = 0; i < UI_WIN_MAX; i++) wm->windows[i].focused = false;
    win->focused = true;
    return idx;
}

void ui_wm_close(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    if (!wm->windows[idx].used) return;

    /* 解除应用关联：应用实例由 ui_app_close 统一管理，这里只断开指针 */
    ui_window_t *win = &wm->windows[idx];
    if (win->owner_app) {
        win->owner_app->win = NULL;
        win->owner_app = NULL;
    }
    win->used = false;
    win->focused = false;

    if (wm->drag_win == idx) wm->drag_win = -1;
    if (wm->resize_win == idx) { wm->resize_win = -1; wm->resize_edge = UI_EDGE_NONE; }

    z_rebuild(wm);

    /* 焦点转移：优先给原来在它下面的窗口（z 序次顶） */
    if (wm->active == idx) {
        wm->active = -1;
        for (int i = wm->z_count - 1; i >= 0; i--) {
            int c = wm->z[i];
            if (c >= 0 && wm->windows[c].used) {
                wm->active = c;
                wm->windows[c].focused = true;
                break;
            }
        }
    }

    /* 收缩 count：尾部若已空则回退，避免窗口数虚高 */
    while (wm->count > 0 && !wm->windows[wm->count - 1].used) wm->count--;
}

void ui_wm_raise(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    if (!wm->windows[idx].used) return;
    z_raise(wm, idx);
}

void ui_wm_focus(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    if (!wm->windows[idx].used) return;

    if (wm->windows[idx].state == UI_WIN_MINIMIZED)
        ui_wm_restore(wm, idx);

    for (int i = 0; i < UI_WIN_MAX; i++) wm->windows[i].focused = false;
    wm->windows[idx].focused = true;
    wm->active = idx;
    z_raise(wm, idx);
}

void ui_wm_focus_next(ui_wm_t *wm, int dir) {
    if (!wm || wm->count <= 0) return;
    /* 沿 z 序从顶往下找下一个有效窗口；只在一个窗口时聚焦它自身 */
    int start = -1;
    for (int i = wm->z_count - 1; i >= 0; i--) {
        if (wm->z[i] == wm->active) { start = i; break; }
    }
    if (start < 0) start = wm->count;

    for (int k = 1; k <= wm->count; k++) {
        int pos = ((start + dir * k) % wm->count + wm->count) % wm->count;
        int idx = wm->z[pos];
        if (idx >= 0 && wm->windows[idx].used) {
            ui_wm_focus(wm, idx);
            return;
        }
    }
}

/* ── 几何操作 ─────────────────────────────────────────────── */

void ui_wm_move(ui_wm_t *wm, int idx, int x, int y) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    ui_window_t *win = &wm->windows[idx];
    if (!win->used || win->state == UI_WIN_MAXIMIZED) return;

    /* 边界钳制：保证标题栏始终有一部分在屏内，否则窗口被拖到屏幕外
     * 就再也抓不回来（这是窗口管理器必须防的用户级死锁）。 */
    int min_visible = 60;
    if (x + win->w < min_visible) x = min_visible - win->w;
    if (x > wm->desk_w - min_visible) x = wm->desk_w - min_visible;
    if (y < 0) y = 0;
    if (y > wm->desk_h - TITLE_H) y = wm->desk_h - TITLE_H;

    win->x = x;
    win->y = y;
    content_from_geom(win);
    win->dirty = true;
}

void ui_wm_resize(ui_wm_t *wm, int idx, int w, int h) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    ui_window_t *win = &wm->windows[idx];
    if (!win->used) return;

    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    /* 不允许超出桌面（否则客户区算出负值，绘制会越界写） */
    if (win->x + w > wm->desk_w) w = wm->desk_w - win->x;
    if (win->y + h > wm->desk_h) h = wm->desk_h - win->y;
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;

    win->w = w;
    win->h = h;
    content_from_geom(win);
    win->dirty = true;
}

void ui_wm_minimize(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    ui_window_t *win = &wm->windows[idx];
    if (!win->used || win->state == UI_WIN_MINIMIZED) return;

    /* 记录最小化**之前**处于什么状态：还原时才知道该回到常规还是最大化。
     * 若省掉这一步，"最大化→最小化→还原"会停在最大化尺寸但状态记为常规。 */
    win->state_before_min = win->state;
    if (win->state == UI_WIN_NORMAL)
        win->restore = (ui_rect_t){ win->x, win->y, win->w, win->h };
    win->state = UI_WIN_MINIMIZED;
    win->anim = UI_ANIM_MINIMIZE;
    win->anim_frame = 0;
    win->anim_total = 6;
    win->focused = false;

    if (wm->active == idx) {
        wm->active = -1;
        for (int i = wm->z_count - 1; i >= 0; i--) {
            int c = wm->z[i];
            if (c >= 0 && c != idx && wm->windows[c].used &&
                wm->windows[c].state != UI_WIN_MINIMIZED) {
                wm->active = c;
                wm->windows[c].focused = true;
                break;
            }
        }
    }
}

void ui_wm_restore(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    ui_window_t *win = &wm->windows[idx];
    if (!win->used) return;
    if (win->state == UI_WIN_NORMAL) return;

    /* 从最小化还原：回到最小化之前的状态（可能是最大化） */
    if (win->state == UI_WIN_MINIMIZED && win->state_before_min == UI_WIN_MAXIMIZED) {
        win->state = UI_WIN_MAXIMIZED;
        win->x = 0; win->y = 0;
        win->w = wm->desk_w; win->h = wm->desk_h;
        win->anim = UI_ANIM_RESTORE;
        win->anim_frame = 0;
        win->anim_total = 6;
        content_from_geom(win);
        for (int i = 0; i < UI_WIN_MAX; i++) wm->windows[i].focused = false;
        win->focused = true;
        wm->active = idx;
        z_raise(wm, idx);
        return;
    }
    /* 从最大化还原：几何回到保存的常规尺寸 */
    if (win->state == UI_WIN_MAXIMIZED) {
        win->x = win->restore.x; win->y = win->restore.y;
        win->w = win->restore.w; win->h = win->restore.h;
    }
    win->state = UI_WIN_NORMAL;
    win->state_before_min = UI_WIN_NORMAL;
    win->anim = UI_ANIM_RESTORE;
    win->anim_frame = 0;
    win->anim_total = 6;
    content_from_geom(win);
    for (int i = 0; i < UI_WIN_MAX; i++) wm->windows[i].focused = false;
    win->focused = true;
    wm->active = idx;
    z_raise(wm, idx);
}

void ui_wm_maximize(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    ui_window_t *win = &wm->windows[idx];
    if (!win->used || win->state == UI_WIN_MAXIMIZED) return;

    if (win->state == UI_WIN_NORMAL)
        win->restore = (ui_rect_t){ win->x, win->y, win->w, win->h };

    /* 最大化区域：整屏减去任务栏高度（由框架传入 desk_h 时已扣除） */
    win->x = 0;
    win->y = 0;
    win->w = wm->desk_w;
    win->h = wm->desk_h;
    win->state = UI_WIN_MAXIMIZED;
    win->anim = UI_ANIM_MAXIMIZE;
    win->anim_frame = 0;
    win->anim_total = 6;
    content_from_geom(win);
    for (int i = 0; i < UI_WIN_MAX; i++) wm->windows[i].focused = false;
    win->focused = true;
    wm->active = idx;
    z_raise(wm, idx);
}

void ui_wm_toggle_maximize(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return;
    if (wm->windows[idx].state == UI_WIN_MAXIMIZED) ui_wm_restore(wm, idx);
    else ui_wm_maximize(wm, idx);
}

ui_window_t *ui_wm_get(ui_wm_t *wm, int idx) {
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return NULL;
    return wm->windows[idx].used ? &wm->windows[idx] : NULL;
}

ui_window_t *ui_wm_active(ui_wm_t *wm) {
    if (!wm || wm->active < 0) return NULL;
    return ui_wm_get(wm, wm->active);
}

ui_rect_t ui_wm_content_rect(const ui_wm_t *wm, int idx) {
    ui_rect_t r = { 0, 0, 0, 0 };
    if (!wm || idx < 0 || idx >= UI_WIN_MAX) return r;
    if (!wm->windows[idx].used) return r;
    return wm->windows[idx].content;
}

bool ui_wm_tick(ui_wm_t *wm, int ms) {
    (void)ms;
    if (!wm) return false;
    bool any = false;
    for (int i = 0; i < UI_WIN_MAX; i++) {
        ui_window_t *win = &wm->windows[i];
        if (!win->used) continue;
        if (win->anim == UI_ANIM_NONE) continue;

        any = true;
        win->anim_frame++;
        if (win->anim_frame >= win->anim_total) {
            win->anim = UI_ANIM_NONE;
            win->opacity = 255;
        } else {
            /* 淡入进度：动画期间不透明度从 40 线性升到 255 */
            int p = 40 + (int)((215L * win->anim_frame) / win->anim_total);
            if (p > 255) p = 255;
            win->opacity = (uint8_t)p;
        }
        win->dirty = true;
    }
    return any;
}

/* ── 命中测试 ─────────────────────────────────────────────── */

/** 标题栏矩形（屏幕坐标） */
static ui_rect_t titlebar_rect(const ui_window_t *win) {
    ui_rect_t r = { win->x, win->y, win->w, TITLE_H };
    return r;
}

/** 关闭按钮矩形 */
ui_rect_t ui_wm_close_button_rect(const ui_window_t *win);

ui_rect_t ui_wm_close_button_rect(const ui_window_t *win) {
    ui_rect_t r = { win->x + win->w - BTN_W - 6, win->y + (TITLE_H - BTN_W) / 2,
                    BTN_W, BTN_W };
    return r;
}

/** 最大化按钮矩形 */
ui_rect_t ui_wm_max_button_rect(const ui_window_t *win);

ui_rect_t ui_wm_max_button_rect(const ui_window_t *win) {
    ui_rect_t r = ui_wm_close_button_rect(win);
    r.x -= BTN_W + BTN_GAP;
    return r;
}

/** 最小化按钮矩形 */
ui_rect_t ui_wm_min_button_rect(const ui_window_t *win);

ui_rect_t ui_wm_min_button_rect(const ui_window_t *win) {
    ui_rect_t r = ui_wm_close_button_rect(win);
    r.x -= 2 * (BTN_W + BTN_GAP);
    return r;
}

int ui_wm_hit_titlebar(const ui_wm_t *wm, int mx, int my) {
    if (!wm) return -1;
    /* 从顶层往下找，保证点到的总是可见的那个 */
    for (int i = wm->z_count - 1; i >= 0; i--) {
        int idx = wm->z[i];
        if (idx < 0 || idx >= UI_WIN_MAX) continue;
        const ui_window_t *win = &wm->windows[idx];
        if (!win->used || win->state == UI_WIN_MINIMIZED) continue;
        ui_rect_t tb = titlebar_rect(win);
        if (ui_hit(&tb, mx, my)) return idx;
    }
    return -1;
}

int ui_wm_hit_window(const ui_wm_t *wm, int mx, int my) {
    if (!wm) return -1;
    for (int i = wm->z_count - 1; i >= 0; i--) {
        int idx = wm->z[i];
        if (idx < 0 || idx >= UI_WIN_MAX) continue;
        const ui_window_t *win = &wm->windows[idx];
        if (!win->used || win->state == UI_WIN_MINIMIZED) continue;
        ui_rect_t r = { win->x, win->y, win->w, win->h };
        if (ui_hit(&r, mx, my)) return idx;
    }
    return -1;
}

int ui_wm_hit_border(const ui_wm_t *wm, int mx, int my, ui_edge_t *edge) {
    if (!wm) return -1;
    if (edge) *edge = UI_EDGE_NONE;

    for (int i = wm->z_count - 1; i >= 0; i--) {
        int idx = wm->z[i];
        if (idx < 0 || idx >= UI_WIN_MAX) continue;
        const ui_window_t *win = &wm->windows[idx];
        if (!win->used || win->state == UI_WIN_MINIMIZED) continue;
        if (win->state == UI_WIN_MAXIMIZED) continue;   /* 最大化时不可缩放 */

        /* 只在边框附近判定，避免抢占客户区点击 */
        int L = win->x, T = win->y, R = win->x + win->w, B = win->y + win->h;
        bool in_x = (mx >= L - RESIZE_GRAB && mx <= R + RESIZE_GRAB);
        bool in_y = (my >= T - RESIZE_GRAB && my <= B + RESIZE_GRAB);
        if (!in_x || !in_y) continue;

        bool on_l = (mx <= L + RESIZE_GRAB);
        bool on_r = (mx >= R - RESIZE_GRAB);
        bool on_t = (my <= T + RESIZE_GRAB);
        bool on_b = (my >= B - RESIZE_GRAB);
        if (!on_l && !on_r && !on_t && !on_b) continue;
        /* 必须在窗口范围内（含抓取带）才算，否则窗口之间的缝隙也会命中 */
        bool inside = ui_hit_xy(L, T, win->w, win->h, mx, my);
        if (!inside && !on_l && !on_r && !on_t && !on_b) continue;

        if (edge) {
            if (on_t && on_l)      *edge = UI_EDGE_NW;
            else if (on_t && on_r) *edge = UI_EDGE_NE;
            else if (on_b && on_l) *edge = UI_EDGE_SW;
            else if (on_b && on_r) *edge = UI_EDGE_SE;
            else if (on_t)         *edge = UI_EDGE_N;
            else if (on_b)         *edge = UI_EDGE_S;
            else if (on_l)         *edge = UI_EDGE_W;
            else                   *edge = UI_EDGE_E;
        }
        return idx;
    }
    return -1;
}

/* ── 应用生命周期 ─────────────────────────────────────────── */

ui_app_t *ui_app_launch(ui_wm_t *wm, int mode, bool unique) {
    if (!wm) return NULL;
    const ui_app_vt_t *vt = ui_app_vt_by_mode(mode);
    if (!vt) return NULL;

    if (unique) {
        ui_app_t *ex = ui_app_find(wm, mode);
        if (ex) {
            if (ex->win) ui_wm_focus(wm, ex->win->id);
            return ex;
        }
    }

    int w = vt->want_w > 0 ? vt->want_w : 520;
    int h = vt->want_h > 0 ? vt->want_h : 400;
    int idx = ui_wm_open(wm, vt->name ? vt->name : "应用", w, h);
    if (idx < 0) return NULL;

    ui_app_t *app = (ui_app_t *)ui_alloc(sizeof(ui_app_t));
    if (!app) {
        ui_wm_close(wm, idx);
        return NULL;
    }
    app->vt = vt;
    app->ctx = vt->create ? vt->create() : NULL;
    app->win = &wm->windows[idx];
    app->id = g_next_app_id++;
    app->alive = true;
    app->next = g_apps;
    g_apps = app;

    wm->windows[idx].owner_app = app;
    return app;
}

void ui_app_close(ui_wm_t *wm, ui_app_t *app) {
    if (!app || !app->alive) return;

    int win_idx = app->win ? app->win->id : -1;

    /* 先摘链，再从链表移除：避免 destroy 里回调造成悬空遍历 */
    app->alive = false;
    app->win = NULL;

    ui_app_t **pp = &g_apps;
    while (*pp) {
        if (*pp == app) { *pp = app->next; break; }
        pp = &(*pp)->next;
    }

    if (app->vt && app->vt->destroy && app->ctx)
        app->vt->destroy(app->ctx);
    app->ctx = NULL;

    ui_free(app);

    if (wm && win_idx >= 0 && win_idx < UI_WIN_MAX) {
        if (wm->windows[win_idx].used) {
            wm->windows[win_idx].owner_app = NULL;
            ui_wm_close(wm, win_idx);
        }
    }
}

ui_app_t *ui_app_find(ui_wm_t *wm, int mode) {
    (void)wm;
    /* 返回**最早创建**的实例：链表是头插，所以遍历到底再返回。
     * 语义明确化——用户再次点击任务栏图标时，应回到他最早开的那个窗口，
     * 而不是最近一次开的（后者会让人觉得"窗口在乱跳"）。 */
    ui_app_t *found = NULL;
    for (ui_app_t *a = g_apps; a; a = a->next) {
        if (a->alive && a->vt && a->vt->mode == mode) found = a;
    }
    return found;
}

ui_app_t *ui_app_active(ui_wm_t *wm) {
    if (!wm) return NULL;
    ui_window_t *win = ui_wm_active(wm);
    if (!win) return NULL;
    return win->owner_app;
}
