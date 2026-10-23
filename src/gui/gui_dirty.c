#include "gui_dirty.h"

static int g_full;
static int g_dirty_n;
static int g_dx[GUI_DIRTY_MAX];
static int g_dy[GUI_DIRTY_MAX];
static int g_dw[GUI_DIRTY_MAX];
static int g_dh[GUI_DIRTY_MAX];

static int g_clip_on;
static int g_clip_x, g_clip_y, g_clip_w, g_clip_h;

static int rect_intersect(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh,
                          int *ox, int *oy, int *ow, int *oh) {
    int x1 = ax > bx ? ax : bx;
    int y1 = ay > by ? ay : by;
    int x2 = ax + aw;
    if (x2 > bx + bw) x2 = bx + bw;
    int y2 = ay + ah;
    if (y2 > by + bh) y2 = by + bh;
    *ox = x1;
    *oy = y1;
    *ow = x2 - x1;
    *oh = y2 - y1;
    return *ow > 0 && *oh > 0;
}

/* 相交或**相邻**都合并：相邻矩形拼起来能少一次重绘，且不会露缝。 */
static int rect_touch(int ax, int ay, int aw, int ah,
                      int bx, int by, int bw, int bh) {
    int ox = (ax + aw < bx + bw ? ax + aw : bx + bw) - (ax > bx ? ax : bx);
    int oy = (ay + ah < by + bh ? ay + ah : by + bh) - (ay > by ? ay : by);
    return ox >= 0 && oy >= 0;
}

void gui_dirty_reset(void) {
    g_full = 0;
    g_dirty_n = 0;
}

void gui_dirty_mark_full(void) {
    g_full = 1;
    g_dirty_n = 0;
}

int gui_dirty_is_full(void) {
    return g_full;
}

/* 把新矩形并入已有脏区：相交就取并集，并集可能又碰到别的，循环到稳定。
 * 好处：光标移动这类大量小矩形会被并成一块，不会很快撞上 GUI_DIRTY_MAX
 * 而退化成整屏重绘（那是最贵的一条路径）。 */
void gui_dirty_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_full) return;

    int merged = 1;
    while (merged) {
        merged = 0;
        for (int i = 0; i < g_dirty_n; i++) {
            if (!rect_touch(x, y, w, h, g_dx[i], g_dy[i], g_dw[i], g_dh[i]))
                continue;
            int x1 = x < g_dx[i] ? x : g_dx[i];
            int y1 = y < g_dy[i] ? y : g_dy[i];
            int x2 = x + w > g_dx[i] + g_dw[i] ? x + w : g_dx[i] + g_dw[i];
            int y2 = y + h > g_dy[i] + g_dh[i] ? y + h : g_dy[i] + g_dh[i];
            x = x1; y = y1; w = x2 - x1; h = y2 - y1;
            /* 移除 i：用最后一个补齐 */
            g_dirty_n--;
            g_dx[i] = g_dx[g_dirty_n];
            g_dy[i] = g_dy[g_dirty_n];
            g_dw[i] = g_dw[g_dirty_n];
            g_dh[i] = g_dh[g_dirty_n];
            merged = 1;
            break;
        }
    }

    if (g_dirty_n >= GUI_DIRTY_MAX) {
        gui_dirty_mark_full();
        return;
    }
    g_dx[g_dirty_n] = x;
    g_dy[g_dirty_n] = y;
    g_dw[g_dirty_n] = w;
    g_dh[g_dirty_n] = h;
    g_dirty_n++;
}

int gui_dirty_count(void) {
    return g_dirty_n;
}

int gui_dirty_get(int idx, int *x, int *y, int *w, int *h) {
    if (idx < 0 || idx >= g_dirty_n || !x || !y || !w || !h) return -1;
    *x = g_dx[idx];
    *y = g_dy[idx];
    *w = g_dw[idx];
    *h = g_dh[idx];
    return 0;
}

void gui_clip_clear(void) {
    g_clip_on = 0;
}

void gui_clip_set(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        g_clip_on = 0;
        return;
    }
    g_clip_on = 1;
    g_clip_x = x;
    g_clip_y = y;
    g_clip_w = w;
    g_clip_h = h;
}

int gui_clip_active(void) {
    return g_clip_on;
}

int gui_clip_intersect(int *x, int *y, int *w, int *h) {
    if (!x || !y || !w || !h) return 0;
    if (*w <= 0 || *h <= 0) return 0;
    if (!g_clip_on) return 1;
    return rect_intersect(*x, *y, *w, *h, g_clip_x, g_clip_y, g_clip_w, g_clip_h, x, y, w, h);
}

void gui_dirty_expand(int *x, int *y, int *w, int *h, int pad, int max_w, int max_h) {
    if (!x || !y || !w || !h) return;
    *x -= pad;
    *y -= pad;
    *w += pad * 2;
    *h += pad * 2;
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > max_w) *w = max_w - *x;
    if (*y + *h > max_h) *h = max_h - *y;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

/* ── 裁剪区栈 ───────────────────────────────────────────────
 * gui_clip_set 是替换语义；窗口内容需要"在脏矩形裁剪内再收缩到客户区"，
 * 因此提供 push/pop。深度小（嵌套只发生在窗口内容一层），定长数组足够。 */
#define GUI_CLIP_STACK_MAX 8
static struct { int on, x, y, w, h; } g_clip_stack[GUI_CLIP_STACK_MAX];
static int g_clip_depth;

int gui_clip_push(int x, int y, int w, int h) {
    if (g_clip_depth >= GUI_CLIP_STACK_MAX) return -1;
    g_clip_stack[g_clip_depth].on = g_clip_on;
    g_clip_stack[g_clip_depth].x = g_clip_x;
    g_clip_stack[g_clip_depth].y = g_clip_y;
    g_clip_stack[g_clip_depth].w = g_clip_w;
    g_clip_stack[g_clip_depth].h = g_clip_h;
    g_clip_depth++;

    /* 与现有裁剪区求交集：现有区关闭时直接用新矩形 */
    if (g_clip_on) {
        int nx = x > g_clip_x ? x : g_clip_x;
        int ny = y > g_clip_y ? y : g_clip_y;
        int rx = (x + w) < (g_clip_x + g_clip_w) ? (x + w) : (g_clip_x + g_clip_w);
        int ry = (y + h) < (g_clip_y + g_clip_h) ? (y + h) : (g_clip_y + g_clip_h);
        gui_clip_set(nx, ny, rx - nx, ry - ny);
    } else {
        gui_clip_set(x, y, w, h);
    }
    return g_clip_depth;
}

void gui_clip_pop(void) {
    if (g_clip_depth <= 0) return;
    g_clip_depth--;
    if (g_clip_stack[g_clip_depth].on)
        gui_clip_set(g_clip_stack[g_clip_depth].x, g_clip_stack[g_clip_depth].y,
                     g_clip_stack[g_clip_depth].w, g_clip_stack[g_clip_depth].h);
    else
        gui_clip_clear();
}

int gui_dirty_add_regions(const gui_region_t *regions, int n,
                          int screen_w, int screen_h) {
    if (!regions || n <= 0 || screen_w <= 0 || screen_h <= 0) return -1;
    int added = 0;
    for (int i = 0; i < n; i++) {
        int x = regions[i].x, y = regions[i].y;
        int w = regions[i].w, h = regions[i].h;
        if (w <= 0 || h <= 0) continue;
        /* 裁剪到屏幕 */
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > screen_w) w = screen_w - x;
        if (y + h > screen_h) h = screen_h - y;
        if (w <= 0 || h <= 0) continue;
        gui_dirty_add(x, y, w, h);
        added++;
    }
    return added;
}
