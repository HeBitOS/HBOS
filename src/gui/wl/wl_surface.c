/**
 * @file    wl_surface.c
 * @brief   Wayland 风格 surface 状态机
 */

#include "wl_surface.h"

#define WL_SURFACE_POOL 16
static wl_surface_t g_pool[WL_SURFACE_POOL];
static uint8_t g_used[WL_SURFACE_POOL];

static void buffer_release(wl_buffer_t *b) {
    if (!b) return;
    b->busy = 0;
    if (b->release) b->release(b, b->release_ctx);
}

wl_surface_t *wl_surface_create(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    for (int i = 0; i < WL_SURFACE_POOL; i++) {
        if (g_used[i]) continue;
        g_used[i] = 1;
        wl_surface_t *s = &g_pool[i];
        s->w = w;
        s->h = h;
        s->pending = 0;
        s->current = 0;
        s->damage_count = 0;
        s->full_damage = 0;
        s->mapped = 0;
        s->serial = 0;
        s->on_commit = 0;
        s->commit_ctx = 0;
        return s;
    }
    return 0;
}

void wl_surface_destroy(wl_surface_t *s) {
    if (!s) return;
    if (s->current) buffer_release(s->current);
    s->current = 0;
    s->pending = 0;
    s->on_commit = 0;
    s->mapped = 0;
    int idx = (int)(s - g_pool);
    if (idx >= 0 && idx < WL_SURFACE_POOL) g_used[idx] = 0;
}

void wl_surface_set_commit_listener(wl_surface_t *s, wl_commit_fn fn, void *ctx) {
    if (!s) return;
    s->on_commit = fn;
    s->commit_ctx = ctx;
}

int wl_surface_attach(wl_surface_t *s, wl_buffer_t *buf) {
    if (!s) return -1;
    if (buf && (buf->w != s->w || buf->h != s->h)) return -1;
    s->pending = buf;
    if (buf) buf->busy = 1;
    return 0;
}

void wl_surface_damage(wl_surface_t *s, int x, int y, int w, int h) {
    if (!s || w <= 0 || h <= 0) return;

    /* 裁剪到 surface */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > s->w) x1 = s->w;
    int y1 = y + h; if (y1 > s->h) y1 = s->h;
    if (x1 <= x0 || y1 <= y0) return;

    if (s->full_damage) return;
    if (s->damage_count >= WL_SURFACE_MAX_DAMAGE) {
        s->full_damage = 1;
        s->damage_count = 0;
        return;
    }
    wl_rect_t *r = &s->damage[s->damage_count++];
    r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0;
}

int wl_surface_commit(wl_surface_t *s) {
    if (!s) return -1;

    /* 没有 pending buffer：仅推进 serial（只提交状态，不换帧） */
    if (!s->pending) {
        s->serial++;
        return 0;
    }

    wl_buffer_t *old = s->current;
    wl_buffer_t *next = s->pending;
    s->pending = 0;

    s->current = next;
    s->mapped = 1;
    s->serial++;

    if (next && s->on_commit)
        s->on_commit(s, next, s->damage, s->damage_count, s->full_damage,
                     s->commit_ctx);

    if (old && old != next) buffer_release(old);

    s->damage_count = 0;
    s->full_damage = 0;
    return 0;
}

wl_buffer_t *wl_surface_current(const wl_surface_t *s) { return s ? s->current : 0; }
int wl_surface_is_mapped(const wl_surface_t *s) { return s ? s->mapped : 0; }
uint32_t wl_surface_serial(const wl_surface_t *s) { return s ? s->serial : 0; }
