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

void gui_dirty_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_full) return;
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
