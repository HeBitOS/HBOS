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

/* ── 生命周期 ───────────────────────────────────────────────── */

int winsrv_create(uint32_t owner_task, const char *title, int w, int h) {
    if (w < WINSRV_MIN_W) w = WINSRV_MIN_W;
    if (h < WINSRV_MIN_H) h = WINSRV_MIN_H;
    if (w > WINSRV_MAX_W) w = WINSRV_MAX_W;
    if (h > WINSRV_MAX_H) h = WINSRV_MAX_H;

    /* 每个任务只允许一个窗口：已存在则复用 */
    winsrv_window_t *existing = winsrv_for_task(owner_task);
    if (existing) return (int)(existing - g_wins);

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

void winsrv_destroy(int id) {
    if (id < 0 || id >= WINSRV_MAX) return;
    winsrv_window_t *win = &g_wins[id];
    if (!win->used) return;
    if (win->surface_phys && win->surface_pages > 0)
        pmm_free_blocks(win->surface_phys, (size_t)win->surface_pages);
    memset(win, 0, sizeof(*win));
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

int winsrv_count(void) {
    int c = 0;
    for (int i = 0; i < WINSRV_MAX; i++) if (g_wins[i].used) c++;
    return c;
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
}

/* ── 事件队列（单生产者 GUI / 单消费者 app，环形缓冲） ───────── */

void winsrv_push_event(winsrv_window_t *win, int type, int a, int b, int c) {
    if (!win) return;
    int nt = (win->ev_tail + 1) % WINSRV_EVQ;
    if (nt == win->ev_head) return;   /* 满则丢弃最旧之外的新事件 */
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
