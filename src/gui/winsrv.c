/**
 * @file    winsrv.c
 * @brief   并发窗口服务实现（见 winsrv.h）
 */
#include "winsrv.h"
#include "../core/pmm.h"
#include "../core/task.h"
#include "../string.h"
#include "../graphics/gui_font.h"

#define WS_PAGE 4096u

static winsrv_window_t g_wins[WINSRV_MAX];
static uint32_t g_next_generation = 1;
static int g_scene_dirty;

/* ── 生命周期 ───────────────────────────────────────────────── */

static int winsrv_create_common(uint32_t owner_task, const char *title,
                                int w, int h, int reuse_owner) {
    if (w < WINSRV_MIN_W) w = WINSRV_MIN_W;
    if (h < WINSRV_MIN_H) h = WINSRV_MIN_H;
    if (w > WINSRV_MAX_W) w = WINSRV_MAX_W;
    if (h > WINSRV_MAX_H) h = WINSRV_MAX_H;

    if (reuse_owner) {
        winsrv_window_t *existing = winsrv_for_task(owner_task);
        if (existing) return (int)(existing - g_wins);
    }

    int id = -1;
    for (int i = 0; i < WINSRV_MAX; i++) {
        if (!g_wins[i].used) { id = i; break; }
    }
    if (id < 0) return -1;

    size_t bytes = (size_t)w * (size_t)h * 4u;
    size_t pages = (bytes + WS_PAGE - 1) / WS_PAGE;
    uint64_t phys = pmm_alloc_blocks(pages);
    if (!phys) return -1;

    winsrv_window_t *win = &g_wins[id];
    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->generation = g_next_generation++;
    if (g_next_generation == 0 || g_next_generation > 0x007FFFFFu)
        g_next_generation = 1;
    win->owner_task = owner_task;
    win->w = w;
    win->h = h;
    /* 简单层叠初始位置 */
    win->x = 90 + id * 36;
    win->y = 70 + id * 30;
    win->surface = (uint32_t *)(uintptr_t)phys;
    win->surface_phys = phys;
    win->surface_pages = (int)pages;
    win->want_close = 0;
    win->state = WINSRV_STATE_NORMAL;
    win->dirty = 1;
    win->dirty_w = w;
    win->dirty_h = h;
    win->ev_head = win->ev_tail = 0;

    if (title) {
        int n = 0;
        while (title[n] && n < WINSRV_TITLE - 1) { win->title[n] = title[n]; n++; }
        win->title[n] = 0;
    }

    /* 默认深色背景 */
    winsrv_clear(win, 0xFF1A1F26);
    return id;
}

int winsrv_create(uint32_t owner_task, const char *title, int w, int h) {
    return winsrv_create_common(owner_task, title, w, h, 1);
}

int winsrv_create_handle(uint32_t owner_task, const char *title, int w, int h) {
    int id = winsrv_create_common(owner_task, title, w, h, 0);
    return id < 0 ? -1 : winsrv_handle(&g_wins[id]);
}

void winsrv_destroy(int id) {
    if (id < 0 || id >= WINSRV_MAX) return;
    winsrv_window_t *win = &g_wins[id];
    if (!win->used) return;
    if (win->surface_phys && win->surface_pages > 0)
        pmm_free_blocks(win->surface_phys, (size_t)win->surface_pages);
    memset(win, 0, sizeof(*win));
    g_scene_dirty = 1;
}

void winsrv_close_for_task(uint32_t owner_task) {
    for (int i = 0; i < WINSRV_MAX; i++)
        if (g_wins[i].used && g_wins[i].owner_task == owner_task)
            { winsrv_destroy(i); return; }
}

winsrv_window_t *winsrv_get(int id) {
    if (id < 0 || id >= WINSRV_MAX) return 0;
    return g_wins[id].used ? &g_wins[id] : 0;
}

winsrv_window_t *winsrv_for_task(uint32_t owner_task) {
    for (int i = 0; i < WINSRV_MAX; i++)
        if (g_wins[i].used && g_wins[i].owner_task == owner_task)
            return &g_wins[i];
    return 0;
}

int winsrv_handle(const winsrv_window_t *win) {
    if (!win || !win->used || win < g_wins || win >= g_wins + WINSRV_MAX)
        return -1;
    uint32_t id = (uint32_t)(win - g_wins);
    return (int)((win->generation << 8) | id);
}

winsrv_window_t *winsrv_for_handle(uint32_t owner_task, int handle) {
    if (handle <= 0) return 0;
    uint32_t raw = (uint32_t)handle;
    uint32_t id = raw & 0xFFu;
    uint32_t generation = raw >> 8;
    if (id >= WINSRV_MAX) return 0;
    winsrv_window_t *win = &g_wins[id];
    if (!win->used || win->owner_task != owner_task ||
        win->generation != generation) return 0;
    return win;
}

int winsrv_count(void) {
    int c = 0;
    for (int i = 0; i < WINSRV_MAX; i++) if (g_wins[i].used) c++;
    return c;
}

int winsrv_has_dirty(void) {
    if (g_scene_dirty) {
        g_scene_dirty = 0;
        return 1;
    }
    for (int i = 0; i < WINSRV_MAX; i++)
        if (g_wins[i].used && g_wins[i].dirty)
            return 1;
    return 0;
}

int winsrv_reap_dead(void) {
    int reaped = 0;
    for (int i = 0; i < WINSRV_MAX; i++) {
        if (!g_wins[i].used) continue;
        const task_t *t = task_get_by_id(g_wins[i].owner_task);
        if (!t || t->state == TASK_TERMINATED) {
            winsrv_destroy(i);
            reaped++;
        }
    }
    return reaped;
}

/* ── 表面绘制原语 ───────────────────────────────────────────── */

void winsrv_clear(winsrv_window_t *win, uint32_t color) {
    if (!win || !win->surface) return;
    int n = win->w * win->h;
    uint32_t *p = win->surface;
    for (int i = 0; i < n; i++) p[i] = color;
    winsrv_mark_dirty(win, 0, 0, win->w, win->h);
}

void winsrv_fill(winsrv_window_t *win, int x, int y, int w, int h, uint32_t color) {
    if (!win || !win->surface) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win->w) w = win->w - x;
    if (y + h > win->h) h = win->h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = win->surface + (uint32_t)(y + yy) * win->w + (uint32_t)x;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
    winsrv_mark_dirty(win, x, y, w, h);
}

/* 把一个字形按 alpha 覆盖混合进窗口表面（无全局状态，抢占安全） */
static void ws_blit_glyph(winsrv_window_t *win, int x, int y,
                          const gui_glyph_t *g, uint32_t color) {
    if (!g->coverage || g->width == 0 || g->height == 0) return;
    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    for (int yy = 0; yy < (int)g->height; yy++) {
        int py = y + yy;
        if (py < 0 || py >= win->h) continue;
        const uint8_t *cov = g->coverage + (uint32_t)yy * g->width;
        uint32_t *row = win->surface + (uint32_t)py * win->w;
        for (int xx = 0; xx < (int)g->width; xx++) {
            int px = x + xx;
            if (px < 0 || px >= win->w) continue;
            uint32_t a = cov[xx];
            if (a == 0) continue;
            uint32_t dst = row[px];
            uint32_t inv = 255 - a;
            uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
            uint32_t orr = (sr * a + dr * inv) / 255;
            uint32_t og  = (sg * a + dg * inv) / 255;
            uint32_t ob  = (sb * a + db * inv) / 255;
            row[px] = 0xFF000000 | (orr << 16) | (og << 8) | ob;
        }
    }
}

void winsrv_text(winsrv_window_t *win, int x, int y, const char *s, uint32_t color) {
    if (!win || !win->surface || !s) return;
    int start_x = x;
    int ascent = gui_font_ascent_n(0);
    while (*s) {
        /* 简化 UTF-8 解码（与字体 lookup 配合，足够窗口标题/正文） */
        uint32_t cp = (uint8_t)*s;
        int len = 1;
        if (cp >= 0xF0)      { cp &= 0x07; len = 4; }
        else if (cp >= 0xE0) { cp &= 0x0F; len = 3; }
        else if (cp >= 0xC0) { cp &= 0x1F; len = 2; }
        for (int i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) { len = i; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        s += len;

        gui_glyph_t g;
        if (!gui_font_lookup(cp, &g)) {
            if (!gui_font_lookup('?', &g)) { x += 6; continue; }
        }
        int gx = x + (int)g.bearing_x;
        int gy = y + ascent - (int)g.bearing_y;
        ws_blit_glyph(win, gx, gy, &g, color);
        int adv = (int)g.advance;
        if (adv <= 0) adv = g.width ? g.width + 1 : 6;
        x += adv;
    }
    winsrv_mark_dirty(win, start_x, y, x - start_x, ascent + 6);
}

void winsrv_mark_dirty(winsrv_window_t *win, int x, int y, int w, int h) {
    if (!win || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > win->w) w = win->w - x;
    if (y + h > win->h) h = win->h - y;
    if (w <= 0 || h <= 0) return;
    if (!win->dirty) {
        win->dirty_x = x; win->dirty_y = y;
        win->dirty_w = w; win->dirty_h = h;
        win->dirty = 1;
        return;
    }
    int x0 = x < win->dirty_x ? x : win->dirty_x;
    int y0 = y < win->dirty_y ? y : win->dirty_y;
    int x1 = x + w > win->dirty_x + win->dirty_w
                 ? x + w : win->dirty_x + win->dirty_w;
    int y1 = y + h > win->dirty_y + win->dirty_h
                 ? y + h : win->dirty_y + win->dirty_h;
    win->dirty_x = x0; win->dirty_y = y0;
    win->dirty_w = x1 - x0; win->dirty_h = y1 - y0;
}

void winsrv_blit_argb(winsrv_window_t *win, int x, int y, int w, int h,
                      const uint32_t *pixels, int stride) {
    if (!win || !win->surface || !pixels || w <= 0 || h <= 0) return;
    if (stride <= 0) stride = w;
    int src_x = 0, src_y = 0;
    if (x < 0) { src_x = -x; w += x; x = 0; }
    if (y < 0) { src_y = -y; h += y; y = 0; }
    if (x + w > win->w) w = win->w - x;
    if (y + h > win->h) h = win->h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        const uint32_t *src = pixels + (src_y + yy) * stride + src_x;
        uint32_t *dst = win->surface + (y + yy) * win->w + x;
        for (int xx = 0; xx < w; xx++) {
            uint32_t s = src[xx];
            uint32_t a = s >> 24;
            if (a == 255) {
                dst[xx] = s;
            } else if (a != 0) {
                uint32_t d = dst[xx], inv = 255 - a;
                uint32_t r = (((s >> 16) & 255) * a +
                              ((d >> 16) & 255) * inv) / 255;
                uint32_t g = (((s >> 8) & 255) * a +
                              ((d >> 8) & 255) * inv) / 255;
                uint32_t b = ((s & 255) * a + (d & 255) * inv) / 255;
                dst[xx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
    winsrv_mark_dirty(win, x, y, w, h);
}

int winsrv_resize(winsrv_window_t *win, int w, int h) {
    if (!win || !win->surface) return -1;
    if (w < WINSRV_MIN_W) w = WINSRV_MIN_W;
    if (h < WINSRV_MIN_H) h = WINSRV_MIN_H;
    if (w > WINSRV_MAX_W) w = WINSRV_MAX_W;
    if (h > WINSRV_MAX_H) h = WINSRV_MAX_H;
    if (w == win->w && h == win->h) return 0;
    size_t bytes = (size_t)w * (size_t)h * 4u;
    size_t pages = (bytes + WS_PAGE - 1) / WS_PAGE;
    uint64_t phys = pmm_alloc_blocks(pages);
    if (!phys) return -1;
    uint32_t *surface = (uint32_t *)(uintptr_t)phys;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++)
        surface[i] = 0xFF1A1F26;
    int copy_w = w < win->w ? w : win->w;
    int copy_h = h < win->h ? h : win->h;
    for (int yy = 0; yy < copy_h; yy++)
        memcpy(surface + yy * w, win->surface + yy * win->w,
               (size_t)copy_w * sizeof(uint32_t));
    pmm_free_blocks(win->surface_phys, (size_t)win->surface_pages);
    win->surface = surface;
    win->surface_phys = phys;
    win->surface_pages = (int)pages;
    win->w = w; win->h = h;
    win->dirty = 0;
    winsrv_mark_dirty(win, 0, 0, w, h);
    winsrv_push_event(win, WINEV_RESIZE, w, h, 0);
    return 0;
}

void winsrv_set_title(winsrv_window_t *win, const char *title) {
    if (!win) return;
    int n = 0;
    if (title)
        while (title[n] && n < WINSRV_TITLE - 1) {
            win->title[n] = title[n];
            n++;
        }
    win->title[n] = 0;
    win->dirty = 1;
}

/* ── 事件队列（单生产者 GUI / 单消费者 app，环形缓冲） ───────── */

void winsrv_push_event(winsrv_window_t *win, int type, int a, int b, int c) {
    if (!win) return;
    /* 连续移动只保留最新坐标，但按钮状态变化必须保留。这样拖动/悬停不会
     * 填满 32 项队列，也不会因丢失 release 让控件永久停在 pressed。 */
    if (type == WINEV_MOUSE && win->ev_head != win->ev_tail) {
        int last = (win->ev_tail + WINSRV_EVQ - 1) % WINSRV_EVQ;
        if (win->evq[last].type == WINEV_MOUSE && win->evq[last].c == c) {
            win->evq[last].a = a;
            win->evq[last].b = b;
            return;
        }
    }
    int nt = (win->ev_tail + 1) % WINSRV_EVQ;
    if (nt == win->ev_head) {
        /* 同按钮状态的纯移动已在上方合并；能到这里的鼠标事件是按钮状态
         * 变化。淘汰最旧事件，保证 release、按键和 close 一定能入队。 */
        win->ev_head = (win->ev_head + 1) % WINSRV_EVQ;
    }
    win->evq[win->ev_tail].type = type;
    win->evq[win->ev_tail].a = a;
    win->evq[win->ev_tail].b = b;
    win->evq[win->ev_tail].c = c;
    win->ev_tail = nt;
}

int winsrv_pop_event(winsrv_window_t *win, winsrv_event_t *ev) {
    if (!win || win->ev_head == win->ev_tail) return 0;
    if (ev) *ev = win->evq[win->ev_head];
    win->ev_head = (win->ev_head + 1) % WINSRV_EVQ;
    return 1;
}
