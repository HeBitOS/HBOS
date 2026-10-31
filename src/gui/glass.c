/**
 * @file    glass.c
 * @brief   「动态玻璃」：降采样 + 盒式模糊 + 双线性回填的磨砂背景。
 *
 * 设计取舍见 glass.h 的注释。这里补三个实现细节：
 *
 * 1) 为什么用盒式模糊而不是真高斯：三次盒式模糊（box blur）在视觉上
 *    与同半径高斯几乎不可分辨，而滚动和算法每像素的代价是 O(1)——不管
 *    半径多大。配合 1/4 降采样，实际工作量只有原图的 1/16。
 *
 * 2) 为什么只在屏幕表面上做：绘制窗口内容时会切到 per-window 的离屏
 *    缓冲（gui_target_begin），那里面没有"身后"的画面，模糊出来的是窗口
 *    自己的旧像素，比不模糊更难看。所以这里显式拒绝 target 模式。
 *
 * 3) 为什么不用浮点：内核态没有 SSE 上下文保存，浮点在中断/任务切换下
 *    不安全；全部用 8.8 定点完成。
 */
#include <stdint.h>
#include <stddef.h>

#include "glass.h"
#include "gui_draw.h"
#include "gui_dirty.h"
#include "../core/pmm.h"

/* 降采样缓冲上限：2560×1440 的 1/4 是 640×360，留些余量。
 * 一张 736×480×4 = 1.35MB，只在初始化时申请一次。 */
#define GLASS_MAX_W 736
#define GLASS_MAX_H 480

static uint32_t *g_small;              /* 降采样 + 模糊的工作缓冲 */
#define GLASS_LINE_MAX (GLASS_MAX_W > GLASS_MAX_H ? GLASS_MAX_W : GLASS_MAX_H)
static uint32_t  g_line[GLASS_LINE_MAX]; /* 一行/一列的原始副本（模糊需要读旧值） */

void glass_init(void) {
    if (g_small) return;
    size_t bytes = (size_t)GLASS_MAX_W * GLASS_MAX_H * sizeof(uint32_t);
    size_t pages = (bytes + 4095u) / 4096u;
    uint64_t phys = pmm_alloc_blocks(pages);
    if (!phys) return;
    g_small = (uint32_t *)(uintptr_t)phys;
}

int glass_available(void) { return g_small != 0; }

/* ── 逐通道混合 ───────────────────────────────────────────────── */
static inline uint32_t mix_rgb(uint32_t a, uint32_t b, int t /*0..255*/) {
    int it = 255 - t;
    int r = (((a >> 16) & 0xFF) * it + ((b >> 16) & 0xFF) * t) >> 8;
    int g = (((a >> 8) & 0xFF) * it + ((b >> 8) & 0xFF) * t) >> 8;
    int bl = ((a & 0xFF) * it + (b & 0xFF) * t) >> 8;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* 一遍水平盒式模糊。
 *
 * 这里不能"原地读-改-写"：滚动窗口要读 x±r，而那些位置可能已经被本次
 * 遍历覆盖过（x' 前进后 x'-r 就落在已写区域），会得到越糊越烂的链式
 * 结果。所以先把整行拷进 g_line，再从 g_line 求值、写回 buf。 */
static void box_blur_h(uint32_t *buf, int w, int h, int r) {
    if (r <= 0 || w > GLASS_LINE_MAX) return;
    int win = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        uint32_t *row = buf + (size_t)y * w;
        for (int i = 0; i < w; i++) g_line[i] = row[i];
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            int xx = k < 0 ? 0 : (k >= w ? w - 1 : k);
            sr += (g_line[xx] >> 16) & 0xFF;
            sg += (g_line[xx] >> 8) & 0xFF;
            sb += g_line[xx] & 0xFF;
        }
        for (int x = 0; x < w; x++) {
            row[x] = (((sr / (uint32_t)win) & 0xFF) << 16) |
                     (((sg / (uint32_t)win) & 0xFF) << 8) |
                      ((sb / (uint32_t)win) & 0xFF);
            int xa = x - r;
            xa = xa < 0 ? 0 : (xa >= w ? w - 1 : xa);
            sr -= (g_line[xa] >> 16) & 0xFF;
            sg -= (g_line[xa] >> 8) & 0xFF;
            sb -= g_line[xa] & 0xFF;
            int xb = x + r + 1;
            xb = xb < 0 ? 0 : (xb >= w ? w - 1 : xb);
            sr += (g_line[xb] >> 16) & 0xFF;
            sg += (g_line[xb] >> 8) & 0xFF;
            sb += g_line[xb] & 0xFF;
        }
    }
}

/* 一遍垂直盒式模糊（同样需要先拷出一列旧值）。 */
static void box_blur_v(uint32_t *buf, int w, int h, int r) {
    if (r <= 0 || h > GLASS_LINE_MAX) return;
    int win = 2 * r + 1;
    for (int x = 0; x < w; x++) {
        for (int i = 0; i < h; i++) g_line[i] = buf[(size_t)i * w + x];
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            int yy = k < 0 ? 0 : (k >= h ? h - 1 : k);
            sr += (g_line[yy] >> 16) & 0xFF;
            sg += (g_line[yy] >> 8) & 0xFF;
            sb += g_line[yy] & 0xFF;
        }
        for (int y = 0; y < h; y++) {
            buf[(size_t)y * w + x] =
                (((sr / (uint32_t)win) & 0xFF) << 16) |
                (((sg / (uint32_t)win) & 0xFF) << 8) |
                 ((sb / (uint32_t)win) & 0xFF);
            int ya = y - r;
            ya = ya < 0 ? 0 : (ya >= h ? h - 1 : ya);
            sr -= (g_line[ya] >> 16) & 0xFF;
            sg -= (g_line[ya] >> 8) & 0xFF;
            sb -= g_line[ya] & 0xFF;
            int yb = y + r + 1;
            yb = yb < 0 ? 0 : (yb >= h ? h - 1 : yb);
            sr += (g_line[yb] >> 16) & 0xFF;
            sg += (g_line[yb] >> 8) & 0xFF;
            sb += g_line[yb] & 0xFF;
        }
    }
}

/* 圆角覆盖：把 (px,py) 相对矩形的位置判成"在内/在外"。
 * corners 是 1=左上 2=右上 4=左下 8=右下的位掩码，与 RR_* 一致：
 * 标题栏只需要上圆角，状态栏只需要下圆角。
 * 窗口/面板自己会在上面再画一圈圆角边框，玻璃层不需要亚像素精度。 */
static inline int in_round_rect(int px, int py, int w, int h, int r, int corners) {
    if (r <= 0 || corners == 0) return 1;
    if (px < 0 || py < 0 || px >= w || py >= h) return 0;
    int cx, cy;
    if (px < r) { if (!(corners & 1)) return 1; cx = r; }
    else if (px >= w - r) { if (!(corners & 2)) return 1; cx = w - r; }
    else return 1;
    if (py < r) { if (!(corners & 1)) return 1; cy = r; }
    else if (py >= h - r) { if (!(corners & 4)) return 1; cy = h - r; }
    else return 1;
    int dx = px - cx, dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

static int g_want_full;

int glass_full_requested(void) { return g_want_full; }
void glass_clear_full_request(void) { g_want_full = 0; }

void glass_rect(int x, int y, int w, int h, int radius, int corners,
                uint32_t tint, int tint_a, int strength) {
    if (!g_small || w <= 0 || h <= 0) return;
    /* 增量帧：surface 里的"身后"是上一帧的模糊输出（不是干净背景），
     * 再采样会迭代糊化、把光标/悬停残影拖进玻璃。跳过本帧并请求全帧。 */
    if (!gui_dirty_is_full()) { g_want_full = 1; return; }
    if (gui_target_active()) return;          /* 离屏缓冲里没有"身后" */
    uint32_t *dst = gd_surface_ptr();
    if (!dst) return;
    int dw = gd_surface_w(), dh = gd_surface_h();
    uint32_t pitch = gd_surface_pitch();
    if (dw <= 0 || dh <= 0) return;

    /* 裁到屏幕内，并记录原始左上角，保证圆角判断用的是原矩形坐标 */
    int ox = x, oy = y;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dw) x1 = dw;
    if (y1 > dh) y1 = dh;
    if (x1 <= x0 || y1 <= y0) return;

    /* 强度 → 降采样倍率 / 模糊遍数 / 半径 */
    int ds, passes, br;
    switch (strength) {
    case 0:  ds = 5; passes = 1; br = 2; break;
    case 2:  ds = 3; passes = 3; br = 4; break;
    default: ds = 4; passes = 2; br = 3; break;
    }
    int sw = (w + ds - 1) / ds;
    int sh = (h + ds - 1) / ds;
    while ((sw > GLASS_MAX_W || sh > GLASS_MAX_H) && ds < 16) {
        ds++;
        sw = (w + ds - 1) / ds;
        sh = (h + ds - 1) / ds;
    }
    if (sw <= 0 || sh <= 0 || sw > GLASS_MAX_W || sh > GLASS_MAX_H) return;

    /* ① 盒式平均降采样（顺带低通，避免模糊后仍看到原图的硬边） */
    for (int sy = 0; sy < sh; sy++) {
        int ys = y + sy * ds;
        int ye = ys + ds;
        if (ye > y + h) ye = y + h;
        if (ys < y0) ys = y0;
        if (ye > y1) ye = y1;
        for (int sx = 0; sx < sw; sx++) {
            int xs = x + sx * ds;
            int xe = xs + ds;
            if (xe > x + w) xe = x + w;
            if (xs < x0) xs = x0;
            if (xe > x1) xe = x1;
            if (xs >= xe || ys >= ye) { g_small[(size_t)sy * sw + sx] = 0; continue; }
            uint32_t sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int yy = ys; yy < ye; yy++) {
                const uint32_t *row = dst + (size_t)yy * pitch;
                for (int xx = xs; xx < xe; xx++) {
                    uint32_t p = row[xx];
                    sr += (p >> 16) & 0xFF;
                    sg += (p >> 8) & 0xFF;
                    sb += p & 0xFF;
                    cnt++;
                }
            }
            if (!cnt) cnt = 1;
            g_small[(size_t)sy * sw + sx] =
                (((sr / cnt) & 0xFF) << 16) | (((sg / cnt) & 0xFF) << 8) | ((sb / cnt) & 0xFF);
        }
    }

    /* ② 可分离盒式模糊（多遍逼近高斯） */
    for (int p = 0; p < passes; p++) {
        box_blur_h(g_small, sw, sh, br);
        box_blur_v(g_small, sw, sh, br);
    }

    /* ③ 双线性回填 + 着色 + 圆角遮罩 */
    int ia = tint_a;
    if (ia < 0) ia = 0;
    if (ia > 255) ia = 255;
    for (int py = y0; py < y1; py++) {
        int fy = ((py - y) * 256) / ds;              /* 8.8 定点 */
        int sy0 = fy >> 8;
        int wy = fy & 0xFF;
        int sy1 = sy0 + 1;
        if (sy0 >= sh) sy0 = sh - 1;
        if (sy1 >= sh) sy1 = sh - 1;
        uint32_t *drow = dst + (size_t)py * pitch;
        const uint32_t *r0 = g_small + (size_t)sy0 * sw;
        const uint32_t *r1 = g_small + (size_t)sy1 * sw;
        for (int px = x0; px < x1; px++) {
            if (!in_round_rect(px - ox, py - oy, w, h, radius, corners)) continue;
            int fx = ((px - x) * 256) / ds;
            int sx0 = fx >> 8;
            int wx = fx & 0xFF;
            int sx1 = sx0 + 1;
            if (sx0 >= sw) sx0 = sw - 1;
            if (sx1 >= sw) sx1 = sw - 1;
            uint32_t top = mix_rgb(r0[sx0], r0[sx1], wx);
            uint32_t bot = mix_rgb(r1[sx0], r1[sx1], wx);
            uint32_t blur = mix_rgb(top, bot, wy);
            if (ia > 0) blur = mix_rgb(blur, tint, ia);
            drow[px] = blur;
        }
    }
}
