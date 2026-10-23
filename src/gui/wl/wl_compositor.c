/**
 * @file    wl_compositor.c
 * @brief   合成器视图管理
 */

#include "wl_compositor.h"

static wl_compositor_t g_comp;
static int g_comp_ready;

wl_compositor_t *wl_comp_create(wl_blit_fn blit, void *ctx) {
    if (g_comp_ready) return &g_comp;
    g_comp.count = 0;
    g_comp.blit = blit;
    g_comp.blit_ctx = ctx;
    g_comp_ready = 1;
    return &g_comp;
}

static int find_view(const wl_compositor_t *c, const wl_surface_t *s) {
    for (int i = 0; i < c->count; i++)
        if (c->views[i].surface == s) return i;
    return -1;
}

int wl_comp_add(wl_compositor_t *c, wl_surface_t *s, int x, int y, int z) {
    if (!c || !s || c->count >= WL_COMP_MAX_VIEWS) return -1;
    if (find_view(c, s) >= 0) return -1;
    wl_view_t *v = &c->views[c->count++];
    v->surface = s;
    v->x = x;
    v->y = y;
    v->z = z;
    v->visible = 1;
    return 0;
}

int wl_comp_remove(wl_compositor_t *c, wl_surface_t *s) {
    if (!c) return -1;
    int idx = find_view(c, s);
    if (idx < 0) return -1;
    for (int i = idx; i < c->count - 1; i++) c->views[i] = c->views[i + 1];
    c->count--;
    return 0;
}

int wl_comp_move(wl_compositor_t *c, wl_surface_t *s, int x, int y) {
    if (!c) return -1;
    int idx = find_view(c, s);
    if (idx < 0) return -1;
    c->views[idx].x = x;
    c->views[idx].y = y;
    return 0;
}

int wl_comp_set_visible(wl_compositor_t *c, wl_surface_t *s, int visible) {
    if (!c) return -1;
    int idx = find_view(c, s);
    if (idx < 0) return -1;
    c->views[idx].visible = visible ? 1 : 0;
    return 0;
}

int wl_comp_raise(wl_compositor_t *c, wl_surface_t *s) {
    if (!c) return -1;
    int idx = find_view(c, s);
    if (idx < 0) return -1;
    int maxz = 0;
    for (int i = 0; i < c->count; i++) if (c->views[i].z > maxz) maxz = c->views[i].z;
    c->views[idx].z = maxz + 1;
    return 0;
}

wl_surface_t *wl_comp_hit(const wl_compositor_t *c, int x, int y) {
    if (!c) return 0;
    wl_surface_t *best = 0;
    int best_z = -0x7FFFFFFF;
    for (int i = 0; i < c->count; i++) {
        const wl_view_t *v = &c->views[i];
        if (!v->visible || !v->surface) continue;
        if (!v->surface->mapped) continue;
        if (x < v->x || y < v->y ||
            x >= v->x + v->surface->w || y >= v->y + v->surface->h) continue;
        if (v->z > best_z) {
            best_z = v->z;
            best = v->surface;
        }
    }
    return best;
}

int wl_comp_paint(wl_compositor_t *c) {
    if (!c) return 0;
    int painted = 0;
    /* 简单选择排序：从最低 z 开始画。视图上限 16，常数级。 */
    int done[WL_COMP_MAX_VIEWS] = {0};
    for (int round = 0; round < c->count; round++) {
        int pick = -1, pick_z = 0x7FFFFFFF;
        for (int i = 0; i < c->count; i++) {
            if (done[i]) continue;
            if (pick < 0 || c->views[i].z < pick_z) {
                pick = i;
                pick_z = c->views[i].z;
            }
        }
        if (pick < 0) break;
        done[pick] = 1;

        wl_view_t *v = &c->views[pick];
        if (!v->visible || !v->surface || !v->surface->mapped) continue;
        wl_buffer_t *buf = wl_surface_current(v->surface);
        if (!buf) continue;
        if (c->blit) c->blit(c->blit_ctx, v->x, v->y, buf, 0, 0, 1);
        painted++;
    }
    return painted;
}

int wl_comp_count(const wl_compositor_t *c) { return c ? c->count : 0; }
