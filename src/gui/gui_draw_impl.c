/**
 * @file    gui_draw_impl.c
 * @brief   GUI 渲染层实现：绘制原语、字形位图、表面管理、亮度。
 *
 * 从 src/tools/gui.c（原 8697 行）拆出的第一块，也是耦合最低的一块——
 * 本文件**完全不引用 gui_state_t**（窗口/应用/焦点状态），只按坐标往
 * surface 上画像素。因此它可以被控件库复用，也能在宿主单测里替换。
 *
 * 命名约定：对外符号统一 `gd_` 前缀（gui-draw），避免与 gui.c 里大量
 * 同义的短名冲突。gui.c 保留一组 static inline 转发，于是 300+ 处调用点
 * 无需改动（机械改 300 处的风险远大于多一层 inline）。
 *
 * 依赖：fb_*（帧缓冲）、gui_font_*（字形）、gui_clip_*（裁剪）、主题令牌。
 */
#include "gui_draw.h"
#include "wl/wl_surface.h"
#include "wl/wl_compositor.h"
#include "gui_dirty.h"
#include "../ui/theme.h"
#include "../ui/ui_tokens.h"
#include "../graphics/gui_font.h"
#include "../graphics/gui_icons.h"
#include "../graphics/font_cjk.h"
#include "../fb.h"
#include "../string.h"

/* 前置声明：块内存在"先调用后定义"（gd_rect 等） */
void gd_rect(int x, int y, int w, int h, uint32_t color);
void gd_rect_alpha(int x, int y, int w, int h, uint32_t color);
void gd_border(int x, int y, int w, int h, uint32_t color);
uint32_t gd_rgb_mix(uint32_t c1, uint32_t c2, int t);
uint32_t gd_rgb_lift(uint32_t c, int d);
void gd_soft_shadow(int x, int y, int w, int h);

/* ── 表面访问器 ──────────────────────────────────────────────
 * 表面指针与尺寸随本块迁入，但 gui.c 的合成器仍需要直接读它们
 * （逐像素混合、present）。导出只读访问器，而不是把全局变量 extern
 * 出去——后者会让"谁都能改表面指针"这种隐患扩散。 */
uint32_t *gd_surface_ptr(void);
int gd_surface_w(void);
int gd_surface_h(void);
uint32_t gd_surface_pitch(void);

/* 颜色打包与逐通道钳制：本文件多处使用，放最前。
 * （原先定义在 gui.c 的 rgb()，随本块一起迁入。） */
static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

uint32_t gd_rgb_mix(uint32_t c1, uint32_t c2, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t r = (uint8_t)(r1 + ((int)r2 - (int)r1) * t / 255);
    uint8_t g = (uint8_t)(g1 + ((int)g2 - (int)g1) * t / 255);
    uint8_t b = (uint8_t)(b1 + ((int)b2 - (int)b1) * t / 255);
    return rgb(r, g, b);
}

uint32_t gd_rgb_lift(uint32_t c, int d) {
    int r = (int)((c >> 16) & 0xFF) + d;
    int g = (int)((c >> 8) & 0xFF) + d;
    int b = (int)(c & 0xFF) + d;
    return rgb(clamp8(r), clamp8(g), clamp8(b));
}

void gd_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) {
        gd_rect(x, y + i, w, 1, gd_rgb_mix(top, bottom, (i * 255) / (h > 1 ? h - 1 : 1)));
    }
}

void gd_hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < w; i++) {
        gd_rect(x + i, y, 1, h, gd_rgb_mix(left, right, (i * 255) / (w > 1 ? w - 1 : 1)));
    }
}

void gd_soft_shadow(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    gd_rect(x + 3, y + h, w - 1, 2, UI_BG);
    gd_rect(x + 6, y + h + 2, w - 5, 2, UI_BG);
    gd_rect(x + 10, y + h + 4, w - 12, 1, UI_BG);
    gd_rect(x + w, y + 3, 2, h - 1, UI_BG);
    gd_rect(x + w + 2, y + 6, 2, h - 5, UI_BG);
    gd_rect(x + w + 4, y + 10, 1, h - 12, UI_BG);
}

static uint32_t *g_gui_surface = 0;
static uint32_t g_gui_surface_pitch = 0;
static int g_gui_surface_w = 0;
static int g_gui_surface_h = 0;
static uint8_t g_layer_opacity = 255;
/* 假斜体：没有真正的斜体字形数据，靠 blit_glyph 逐行加一点递减的水平位移
 * 模拟倾斜（顶部偏右、底部不动），跟粗体的"描边两次"是同一类"没有字形
 * 数据就用像素技巧凑"的思路。只在 <em>/<i> 或 font-style:italic 命中时
 * 临时置 1，画完立刻复位，不影响其余所有 gd_text() 调用者。 */
int g_text_italic = 0;   /* 供 gui.c 的斜体渲染开关直接读写 */

void gui_set_layer_opacity(uint8_t opacity) {
    g_layer_opacity = opacity;
}

uint8_t gui_get_layer_opacity(void) {
    return g_layer_opacity;
}

uint32_t gd_apply_layer_opacity(uint32_t color) {
    if (g_layer_opacity >= 255) return color;
    if (g_layer_opacity == 0) return color & 0x00FFFFFF;
    uint8_t a = (color >> 24) & 0xFF;
    if (a == 0) a = 255;
    a = (uint8_t)((a * g_layer_opacity) / 255);
    return ((uint32_t)a << 24) | (color & 0x00FFFFFF);
}

// 屏幕亮度：没有真实背光可调，用"呈现到硬件帧缓冲时按比例压暗每个像素的
// RGB 通道"模拟。20..100，floor 在 20 避免完全变黑（那样用户没法再把它调回来）。
static int g_brightness = 100;

void gui_set_brightness(int pct) {
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    g_brightness = pct;
}

int gui_get_brightness(void) { return g_brightness; }

static inline uint32_t apply_brightness_px(uint32_t px) {
    if (g_brightness >= 100) return px;
    uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
    r = r * (uint32_t)g_brightness / 100;
    g = g * (uint32_t)g_brightness / 100;
    b = b * (uint32_t)g_brightness / 100;
    return (px & 0xFF000000) | (r << 16) | (g << 8) | b;
}

void gui_set_surface(uint32_t *surface, int w, int h, uint32_t pitch_px) {
    g_gui_surface = surface;
    g_gui_surface_w = surface ? w : 0;
    g_gui_surface_h = surface ? h : 0;
    g_gui_surface_pitch = surface ? pitch_px : 0;
}

static void present_full_raw(const fb_info_t *fb) {
    if (!fb || !g_gui_surface) return;
    uint32_t fb_pitch = (uint32_t)(fb->pitch / 4);
    for (int y = 0; y < g_gui_surface_h; y++) {
        uint32_t *dst = fb->addr + (uint32_t)y * fb_pitch;
        uint32_t *src = g_gui_surface + (uint32_t)y * g_gui_surface_pitch;
        for (int x = 0; x < g_gui_surface_w; x++) dst[x] = apply_brightness_px(src[x]);
    }
}

/* ── Wayland 风格提交路径（① P1）────────────────────────────────
 * 桌面画进 g_gui_surface（就是 root surface 的 buffer），提交走
 * attach → damage → commit；commit 回调再由合成器贴到真实帧缓冲。
 * 没有初始化（或分配失败）时退回原来的直贴路径。 */
static wl_surface_t *g_root;
static wl_buffer_t g_root_buf;
static wl_compositor_t *g_comp;
static const fb_info_t *g_present_fb;

static void present_rect_raw(const fb_info_t *fb, int x, int y, int w, int h);

static void wl_on_blit(void *ctx, int dx, int dy, wl_buffer_t *b,
                       const wl_rect_t *damage, int count, int full) {
    (void)ctx; (void)b;
    if (!g_present_fb) return;
    if (full || count <= 0) {
        present_full_raw(g_present_fb);
        return;
    }
    for (int i = 0; i < count; i++)
        present_rect_raw(g_present_fb, dx + damage[i].x, dy + damage[i].y,
                         damage[i].w, damage[i].h);
}

void gui_wl_init(uint32_t *pixels, int w, int h, uint32_t pitch_px) {
    if (!pixels || w <= 0 || h <= 0) return;
    g_root_buf.w = w;
    g_root_buf.h = h;
    g_root_buf.stride = (int)(pitch_px * 4);
    g_root_buf.pixels = pixels;
    g_root_buf.release = 0;
    g_root_buf.release_ctx = 0;
    g_root_buf.busy = 0;

    g_root = wl_surface_create(w, h);
    if (!g_root) return;
    (void)wl_surface_attach(g_root, &g_root_buf);
    (void)wl_surface_commit(g_root);

    g_comp = wl_comp_create(wl_on_blit, 0);
    if (g_comp) (void)wl_comp_add(g_comp, g_root, 0, 0, 0);
}

int gui_wl_active(void) { return g_root != 0; }

void gui_present_surface(const fb_info_t *fb) {
    if (!fb || !g_gui_surface) return;
    g_present_fb = fb;
    if (g_root) {
        wl_surface_damage(g_root, 0, 0, g_gui_surface_w, g_gui_surface_h);
        (void)wl_surface_attach(g_root, &g_root_buf);
        (void)wl_surface_commit(g_root);
        if (g_comp) (void)wl_comp_paint(g_comp);
        return;
    }
    present_full_raw(fb);
}

void gui_present_rect(const fb_info_t *fb, int x, int y, int w, int h) {
    g_present_fb = fb;
    if (g_root) {
        wl_surface_damage(g_root, x, y, w, h);
        (void)wl_surface_attach(g_root, &g_root_buf);
        (void)wl_surface_commit(g_root);
        if (g_comp) (void)wl_comp_paint(g_comp);
        return;
    }
    present_rect_raw(fb, x, y, w, h);
}

static void present_rect_raw(const fb_info_t *fb, int x, int y, int w, int h) {
    if (!fb || !g_gui_surface || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
    if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;
    if (w <= 0 || h <= 0) return;
    uint32_t fb_pitch = (uint32_t)(fb->pitch / 4);
    for (int yy = 0; yy < h; yy++) {
        uint32_t *dst = fb->addr + (uint32_t)(y + yy) * fb_pitch + (uint32_t)x;
        uint32_t *src = g_gui_surface + (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
        for (int xx = 0; xx < w; xx++) dst[xx] = apply_brightness_px(src[xx]);
    }
}

void gd_rect(int x, int y, int w, int h, uint32_t color);

void gd_rect_alpha(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (!gui_clip_intersect(&x, &y, &w, &h)) return;
    color = gd_apply_layer_opacity(color);
    uint8_t alpha = (color >> 24) & 0xFF;
    if (alpha == 0) return;
    if (alpha == 0xFF) {
        gd_rect(x, y, w, h, color);
        return;
    }
    if (g_gui_surface) {
        if (x >= g_gui_surface_w || y >= g_gui_surface_h) return;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (w <= 0 || h <= 0) return;
        if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
        if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;

        uint32_t src_r = (color >> 16) & 0xFF;
        uint32_t src_g = (color >> 8) & 0xFF;
        uint32_t src_b = color & 0xFF;
        uint32_t inv_alpha = 255 - alpha;

        for (int yy = 0; yy < h; yy++) {
            uint32_t *row = g_gui_surface + (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
            for (int xx = 0; xx < w; xx++) {
                uint32_t dst = row[xx];
                uint32_t dst_r = (dst >> 16) & 0xFF;
                uint32_t dst_g = (dst >> 8) & 0xFF;
                uint32_t dst_b = dst & 0xFF;

                uint32_t out_r = (src_r * alpha + dst_r * inv_alpha) / 255;
                uint32_t out_g = (src_g * alpha + dst_g * inv_alpha) / 255;
                uint32_t out_b = (src_b * alpha + dst_b * inv_alpha) / 255;

                row[xx] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
        return;
    }
    fb_fill_rect((uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}

static uint32_t rgb_mix_alpha(uint32_t c1, uint32_t c2, int t) {
    if (t < 0) t = 0;
    if (t > 255) t = 255;
    uint8_t a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t a = (uint8_t)(a1 + ((int)a2 - (int)a1) * t / 255);
    uint8_t r = (uint8_t)(r1 + ((int)r2 - (int)r1) * t / 255);
    uint8_t g = (uint8_t)(g1 + ((int)g2 - (int)g1) * t / 255);
    uint8_t b = (uint8_t)(b1 + ((int)b2 - (int)b1) * t / 255);
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void gd_vgradient_alpha(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) {
        gd_rect_alpha(x, y + i, w, 1, rgb_mix_alpha(top, bottom, (i * 255) / (h > 1 ? h - 1 : 1)));
    }
}

void gd_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (!gui_clip_intersect(&x, &y, &w, &h)) return;
    color = gd_apply_layer_opacity(color);
    uint8_t layer_a = (color >> 24) & 0xFF;
    if (layer_a > 0 && layer_a < 255) {
        gd_rect_alpha(x, y, w, h, color);
        return;
    }
    if (g_gui_surface) {
        if (x >= g_gui_surface_w || y >= g_gui_surface_h) return;
        if (x < 0) {
            w += x;
            x = 0;
        }
        if (y < 0) {
            h += y;
            y = 0;
        }
        if (w <= 0 || h <= 0) return;
        if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
        if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;
        for (int yy = 0; yy < h; yy++) {
            uint32_t *row = g_gui_surface + (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
            for (int xx = 0; xx < w; xx++) row[xx] = color;
        }
        return;
    }
    fb_fill_rect((uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h, color);
}

void gd_border(int x, int y, int w, int h, uint32_t color) {
    gd_rect(x, y, w, 1, color);
    gd_rect(x, y + h - 1, w, 1, color);
    gd_rect(x, y, 1, h, color);
    gd_rect(x + w - 1, y, 1, h, color);
}


// Fill one r×r corner box, anti-aliasing a quarter-circle (center cxc,cyc) of
// `color` over whatever is already in the surface — so rounded windows blend
// cleanly against the wallpaper/desktop drawn beneath them.
void gd_round_corner(int bx, int by, int r, int cxc, int cyc, uint32_t color) {
    if (!g_gui_surface) { return; }
    int x = bx, y = by, w = r, h = r;
    if (!gui_clip_intersect(&x, &y, &w, &h)) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= g_gui_surface_w || y >= g_gui_surface_h) return;
    if (x + w > g_gui_surface_w) w = g_gui_surface_w - x;
    if (y + h > g_gui_surface_h) h = g_gui_surface_h - y;
    uint32_t op = g_layer_opacity;
    uint32_t base_a = (color >> 24) & 0xFF;
    if (base_a == 0) base_a = 255;
    uint32_t fa = base_a * op / 255;   // corner honors the color's own alpha too
    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    int rin = (r - 1) * (r - 1), rout = r * r;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *row = g_gui_surface +
                        (uint32_t)(y + yy) * g_gui_surface_pitch + (uint32_t)x;
        for (int xx = 0; xx < w; xx++) {
            int dx = (x + xx) - cxc, dy = (y + yy) - cyc;
            int d2 = dx * dx + dy * dy;
            uint32_t cov;
            if (d2 <= rin) cov = 255;
            else if (d2 >= rout) cov = 0;
            else cov = (uint32_t)((rout - d2) * 255 / (rout - rin));
            if (cov == 0) continue;
            uint32_t a = cov * fa / 255;
            if (a == 0) continue;
            uint32_t d = row[xx];
            uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t inv = 255 - a;
            row[xx] = 0xFF000000 |
                      (((sr * a + dr * inv) / 255) << 16) |
                      (((sg * a + dg * inv) / 255) << 8) |
                      ((sb * a + db * inv) / 255);
        }
    }
}

// Filled rectangle with selectively rounded corners (RR_* mask).
void gd_fill_round_rect(int x, int y, int w, int h, int r,
                            uint32_t color, int corners) {
    if (w <= 0 || h <= 0) return;
    if (r < 1) { gd_rect(x, y, w, h, color); return; }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    gd_rect(x, y + r, w, h - 2 * r, color);          // middle band, full width
    gd_rect(x + r, y, w - 2 * r, r, color);          // top band between corners
    gd_rect(x + r, y + h - r, w - 2 * r, r, color);  // bottom band between corners
    if (corners & RR_TL) gd_round_corner(x, y, r, x + r, y + r, color);
    else gd_rect(x, y, r, r, color);
    if (corners & RR_TR) gd_round_corner(x + w - r, y, r, x + w - r, y + r, color);
    else gd_rect(x + w - r, y, r, r, color);
    if (corners & RR_BL) gd_round_corner(x, y + h - r, r, x + r, y + h - r, color);
    else gd_rect(x, y + h - r, r, r, color);
    if (corners & RR_BR) gd_round_corner(x + w - r, y + h - r, r, x + w - r, y + h - r, color);
    else gd_rect(x + w - r, y + h - r, r, r, color);
}

/* 1px 圆角边框：外层圆角填边框色，内层圆角填内容色。
 * 圆角不能靠 gd_border 画直线段实现——四角会露方角。 */
static void gd_round_border(int x, int y, int w, int h, int r,
                            uint32_t border_c, uint32_t body_c) {
    if (r < 1) r = 1;
    if (r * 2 > w || r * 2 > h) r = (w < h ? w : h) / 2;
    gd_fill_round_rect(x, y, w, h, r, border_c, RR_ALL);
    gd_fill_round_rect(x + 1, y + 1, w - 2, h - 2, r - 1, body_c, RR_ALL);
}

void gd_draw_panel_shell(int x, int y, int w, int h, uint32_t top,
                             uint32_t bottom, uint32_t border_c, uint32_t accent) {
    if (w <= 0 || h <= 0) return;
    // Breeze-flat: near-solid fill with only a whisper of top sheen, a single
    // 1px rounded border, and a slim accent edge.
    int r = 6;
    if (r * 2 > w || r * 2 > h) r = (w < h ? w : h) / 2;
    gd_fill_round_rect(x, y, w, h, r, gd_rgb_lift(top, 4), RR_ALL);
    gd_vgradient(x + r, y, w - 2 * r, h, gd_rgb_lift(top, 4), bottom);
    if (accent) {
        gd_rect(x, y + 1, 3, h - 2, accent);
    }
    gd_round_border(x, y, w, h, r, border_c, gd_rgb_lift(top, 4));
}

void gd_draw_inset_shell(int x, int y, int w, int h, uint32_t border_c) {
    if (w <= 0 || h <= 0) return;
    int r = 5;
    if (r * 2 > w || r * 2 > h) r = (w < h ? w : h) / 2;
    gd_fill_round_rect(x, y, w, h, r, UI_BG, RR_ALL);
    gd_vgradient(x + r, y, w - 2 * r, h, UI_BG, UI_BG);
    gd_rect(x + r, y, w - 2 * r, 1, gd_rgb_lift(border_c, -28));
    gd_rect(x + r, y + h - 1, w - 2 * r, 1, gd_rgb_lift(border_c, 12));
    gd_round_border(x, y, w, h, r, border_c, UI_BG);
}


void gd_draw_window_control_icon(int x, int y, int kind, int restore, uint32_t color) {
    if (kind == GUI_CTRL_CLOSE) {
        for (int i = 0; i < 8; i++) {
            gd_rect(x + i, y + i, 2, 2, color);
            gd_rect(x + 7 - i, y + i, 2, 2, color);
        }
    } else if (kind == GUI_CTRL_MAX) {
        if (restore) {
            gd_rect(x + 4, y + 1, 9, 1, color);
            gd_rect(x + 12, y + 1, 1, 8, color);
            gd_rect(x + 4, y + 8, 5, 1, color);
            gd_rect(x + 4, y + 1, 1, 4, color);
        }
        gd_rect(x, y + 4, 10, 1, color);
        gd_rect(x, y + 4, 1, 9, color);
        gd_rect(x + 9, y + 4, 1, 9, color);
        gd_rect(x, y + 12, 10, 1, color);
    } else {
        gd_rect(x, y + 10, 11, 2, color);
    }
}

// Bilinearly sample the coverage map at the fractional source position that
// dest offset `d` maps to under `scale` (8.8 fixed point). Used for scale>1 so
// upscaled gd_text (titles, clock) gets smooth anti-aliased edges instead of the
// blocky staircase nearest-neighbour produces.
static inline uint32_t cov_bilinear(const uint8_t *cov, int w, int h,
                                    int dx, int dy, int scale) {
    int fx = (dx * 256) / scale, fy = (dy * 256) / scale;
    int sx0 = fx >> 8, sy0 = fy >> 8;
    if (sx0 < 0) sx0 = 0;
    if (sx0 > w - 1) sx0 = w - 1;
    if (sy0 < 0) sy0 = 0;
    if (sy0 > h - 1) sy0 = h - 1;
    int sx1 = sx0 + 1 < w ? sx0 + 1 : sx0;
    int sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
    int tx = fx & 255, ty = fy & 255;
    const uint8_t *r0 = cov + (uint32_t)sy0 * w;
    const uint8_t *r1 = cov + (uint32_t)sy1 * w;
    int top = r0[sx0] + (((int)r0[sx1] - (int)r0[sx0]) * tx >> 8);
    int bot = r1[sx0] + (((int)r1[sx1] - (int)r1[sx0]) * tx >> 8);
    int v = top + ((bot - top) * ty >> 8);
    return v < 0 ? 0 : (v > 255 ? 255 : (uint32_t)v);
}

// Alpha-blend an 8-bit coverage glyph onto the GUI surface at (x, y) (the
// glyph's top-left). `scale` 1 samples 1:1; scale>1 bilinearly upscales for
// crisp large text. Foreground is `color` (0x00RRGGBB), modulated per pixel by
// the glyph coverage and the active layer opacity.
void gd_blit_glyph(int x, int y, const gui_glyph_t *g, uint32_t color, int scale) {
    if (!g->coverage || g->width == 0 || g->height == 0) return;
    if (scale < 1) scale = 1;
    int dw = (int)g->width * scale;
    int dh = (int)g->height * scale;
    int cx = x, cy = y, cw = dw, ch = dh;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    int off_x = cx - x, off_y = cy - y;

    uint32_t base_a = (color >> 24) & 0xFF;
    if (base_a == 0) base_a = 255;
    uint32_t fg_a = base_a * g_layer_opacity / 255;
    if (fg_a == 0) return;
    uint32_t src_r = (color >> 16) & 0xFF;
    uint32_t src_g = (color >> 8) & 0xFF;
    uint32_t src_b = color & 0xFF;
    int width = g->width;
    int smooth = scale > 1;

    if (g_gui_surface) {
        if (cx < 0) { cw += cx; off_x -= cx; cx = 0; }
        if (cy < 0) { ch += cy; off_y -= cy; cy = 0; }
        if (cx >= g_gui_surface_w || cy >= g_gui_surface_h) return;
        if (cx + cw > g_gui_surface_w) cw = g_gui_surface_w - cx;
        if (cy + ch > g_gui_surface_h) ch = g_gui_surface_h - cy;
        int italic_max = (g_text_italic && dh > 1) ? scale : 0;
        for (int yy = 0; yy < ch; yy++) {
            int sy = (off_y + yy) / scale;
            const uint8_t *cov_row = g->coverage + (uint32_t)sy * width;
            uint32_t *row = g_gui_surface +
                            (uint32_t)(cy + yy) * g_gui_surface_pitch + (uint32_t)cx;
            /* 顶部行（yy 小）位移最大，底部行位移为 0——字形看起来往右上方倾斜。 */
            int shift = italic_max ? (int)((int64_t)(dh - 1 - (off_y + yy)) * italic_max / (dh - 1)) : 0;
            for (int xx = 0; xx < cw; xx++) {
                uint32_t cov = smooth
                    ? cov_bilinear(g->coverage, width, g->height,
                                   off_x + xx, off_y + yy, scale)
                    : cov_row[(off_x + xx) / scale];
                if (cov == 0) continue;
                uint32_t a = cov * fg_a / 255;
                if (a == 0) continue;
                int dxx = xx + shift;
                if (dxx < 0 || cx + dxx >= g_gui_surface_w) continue;
                uint32_t dst = row[dxx];
                uint32_t inv = 255 - a;
                uint32_t dr = (dst >> 16) & 0xFF;
                uint32_t dg = (dst >> 8) & 0xFF;
                uint32_t db = dst & 0xFF;
                uint32_t out_r = (src_r * a + dr * inv) / 255;
                uint32_t out_g = (src_g * a + dg * inv) / 255;
                uint32_t out_b = (src_b * a + db * inv) / 255;
                row[dxx] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
        return;
    }
    // Framebuffer fallback (no compositor surface): threshold the coverage.
    for (int yy = 0; yy < ch; yy++) {
        int sy = (off_y + yy) / scale;
        const uint8_t *cov_row = g->coverage + (uint32_t)sy * width;
        for (int xx = 0; xx < cw; xx++) {
            if (cov_row[(off_x + xx) / scale] >= 128)
                fb_put_pixel((uint64_t)(cx + xx), (uint64_t)(cy + yy), color);
        }
    }
}

// Crisp 1-bit monospace glyph from the classic 8x16 VGA console font (the same
// font the TUI uses). No anti-aliasing — solid foreground pixels — so the code
// editor reads like a real terminal instead of the soft proportional GUI font.
extern const uint8_t *fb_console_glyph(uint32_t cp);
void gd_blit_mono_glyph(int x, int y, const uint8_t *glyph, uint32_t color, int scale) {
    if (!glyph) return;
    if (scale < 1) scale = 1;
    int dw = GUI_MONO_GLYPH_W * scale, dh = GUI_MONO_GLYPH_H * scale;
    int cx = x, cy = y, cw = dw, ch = dh;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    int off_x = cx - x, off_y = cy - y;

    uint32_t base_a = (color >> 24) & 0xFF;
    if (base_a == 0) base_a = 255;
    uint32_t fg_a = base_a * g_layer_opacity / 255;
    if (fg_a == 0) return;
    uint32_t src_r = (color >> 16) & 0xFF;
    uint32_t src_g = (color >> 8) & 0xFF;
    uint32_t src_b = color & 0xFF;

    if (!g_gui_surface) {
        for (int yy = 0; yy < ch; yy++) {
            uint8_t bits = glyph[(off_y + yy) / scale];
            for (int xx = 0; xx < cw; xx++)
                if (bits & (0x80 >> ((off_x + xx) / scale)))
                    fb_put_pixel((uint64_t)(cx + xx), (uint64_t)(cy + yy), color);
        }
        return;
    }
    if (cx < 0) { cw += cx; off_x -= cx; cx = 0; }
    if (cy < 0) { ch += cy; off_y -= cy; cy = 0; }
    if (cx >= g_gui_surface_w || cy >= g_gui_surface_h) return;
    if (cx + cw > g_gui_surface_w) cw = g_gui_surface_w - cx;
    if (cy + ch > g_gui_surface_h) ch = g_gui_surface_h - cy;
    for (int yy = 0; yy < ch; yy++) {
        uint8_t bits = glyph[(off_y + yy) / scale];
        uint32_t *row = g_gui_surface +
                        (uint32_t)(cy + yy) * g_gui_surface_pitch + (uint32_t)cx;
        for (int xx = 0; xx < cw; xx++) {
            if (!(bits & (0x80 >> ((off_x + xx) / scale)))) continue;
            if (fg_a >= 255) { row[xx] = 0xFF000000 | (color & 0xFFFFFF); continue; }
            uint32_t dst = row[xx], inv = 255 - fg_a;
            uint32_t out_r = (src_r * fg_a + ((dst >> 16) & 0xFF) * inv) / 255;
            uint32_t out_g = (src_g * fg_a + ((dst >> 8) & 0xFF) * inv) / 255;
            uint32_t out_b = (src_b * fg_a + (dst & 0xFF) * inv) / 255;
            row[xx] = 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
        }
    }
}

// Blit a flat icon tile (RGBA), bilinearly scaled to sz×sz and alpha-blended
// onto the surface. Premultiplied-alpha bilinear keeps the edges smooth when
// downscaling the 64px master to taskbar/launcher sizes. Works for both atlas
// tiles and per-app embedded HAX icon tiles (any square source size). Replaces
// the old hand-drawn rect motifs + their colored backing square.
void gd_blit_icon_rgba(int x, int y, int sz, const uint32_t *src, int tile) {
    if (!src || tile <= 0 || sz <= 0 || !g_gui_surface) return;
    int cx = x, cy = y, cw = sz, ch = sz;
    if (!gui_clip_intersect(&cx, &cy, &cw, &ch)) return;
    int off_x = cx - x, off_y = cy - y;
    if (cx + cw > g_gui_surface_w) cw = g_gui_surface_w - cx;
    if (cy + ch > g_gui_surface_h) ch = g_gui_surface_h - cy;

    // 16.16 source coordinate of each dest pixel center: (d+0.5)*tile/sz - 0.5.
    uint32_t scale = ((uint32_t)tile << 16) / (uint32_t)sz;
    int bias = (int)(scale >> 1) - 32768;          // +0.5 dst, -0.5 src
    int maxc = (tile - 1) << 16;

    for (int yy = 0; yy < ch; yy++) {
        int fy = (off_y + yy) * (int)scale + bias;
        if (fy < 0) fy = 0;
        else if (fy > maxc) fy = maxc;
        int sy0 = fy >> 16, sy1 = sy0 + 1;
        if (sy1 >= tile) sy1 = tile - 1;
        uint32_t wy = (uint32_t)(fy & 0xFFFF);
        const uint32_t *r0 = src + (uint32_t)sy0 * tile;
        const uint32_t *r1 = src + (uint32_t)sy1 * tile;
        uint32_t *drow = g_gui_surface +
                         (uint32_t)(cy + yy) * g_gui_surface_pitch + (uint32_t)cx;
        for (int xx = 0; xx < cw; xx++) {
            int fx = (off_x + xx) * (int)scale + bias;
            if (fx < 0) fx = 0;
            else if (fx > maxc) fx = maxc;
            int sx0 = fx >> 16, sx1 = sx0 + 1;
            if (sx1 >= tile) sx1 = tile - 1;
            uint32_t wx = (uint32_t)(fx & 0xFFFF);

            // Bilerp in premultiplied space. Weights are .14 fixed (each axis
            // weight >>9 = .7, product = .14, sum of the 4 ≈ 16384) — chosen so
            // rgb*a*weight stays inside uint32.
            uint32_t pr = 0, pg = 0, pb = 0, pa = 0;
            const uint32_t *rows[2] = { r0, r1 };
            int sxs[2] = { sx0, sx1 };
            uint32_t wxs[2] = { (65536 - wx) >> 9, wx >> 9 };
            uint32_t wys[2] = { (65536 - wy) >> 9, wy >> 9 };
            for (int j = 0; j < 2; j++) {
                for (int i = 0; i < 2; i++) {
                    uint32_t px = rows[j][sxs[i]];
                    uint32_t a = (px >> 24) & 0xFF;
                    uint32_t wgt = wxs[i] * wys[j];           // .14
                    uint32_t am = a * wgt;                    // <= 255*16384
                    pa += am;
                    pr += ((px >> 16) & 0xFF) * am / 255;
                    pg += ((px >> 8) & 0xFF) * am / 255;
                    pb += (px & 0xFF) * am / 255;
                }
            }
            uint32_t a = pa >> 14;                            // back to 0..255
            a = a * g_layer_opacity / 255;
            if (a == 0) continue;
            if (a > 255) a = 255;
            // premultiplied rgb (already * alpha) for over-opaque compositing:
            uint32_t sr = pr >> 14, sg = pg >> 14, sb = pb >> 14;
            uint32_t dst = drow[xx], inv = 255 - a;
            uint32_t orr = sr + ((dst >> 16) & 0xFF) * inv / 255;
            uint32_t og  = sg + ((dst >> 8) & 0xFF) * inv / 255;
            uint32_t ob  = sb + (dst & 0xFF) * inv / 255;
            if (orr > 255) orr = 255;
            if (og > 255) og = 255;
            if (ob > 255) ob = 255;
            drow[xx] = 0xFF000000 | (orr << 16) | (og << 8) | ob;
        }
    }
}

// Blit an atlas icon tile by id (see blit_icon_rgba for the shared renderer).
void gd_blit_icon(int x, int y, int sz, int id) {
    int tile = 0;
    const uint32_t *src = gui_icon_tile(id, &tile);
    gd_blit_icon_rgba(x, y, sz, src, tile);
}

// Map an app mode / panel id to its atlas icon id.

// Draw one printable ASCII char in the mono console font; returns the cell width.
int gd_draw_mono_char(int x, int y, char c, uint32_t color) {
    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F)
        gd_blit_mono_glyph(x, y, fb_console_glyph((uint32_t)(unsigned char)c), color, 1);
    return GUI_MONO_GLYPH_W;
}

// Draw one codepoint with its top at `y`, returning the pen advance. Latin and
// CJK share a baseline (y + ascent), so mixed text aligns naturally.
// Resolve a codepoint+scale to a concrete glyph. Prefer the native large size
// (idx 1 = 2x base px) for even scales when the glyph exists, so titles and big
// digits render crisply instead of being upscaled from the 16px base. *out_idx
// is the chosen size, *out_sub the residual draw scale. Because the large size
// is exactly 2x the base, baselines/advances line up with the base path, so a
// string mixing both sizes (e.g. a scaled title with a rare uncovered glyph)
// still aligns on one baseline.
int gd_gui_resolve_glyph(uint32_t cp, int scale, gui_glyph_t *g,
                             int *out_idx, int *out_sub) {
    int base_idx  = gui_font_active_base_idx();
    int large_idx = gui_font_active_large_idx();
    if (scale >= 2 && large_idx >= 0) {
        int base = gui_font_size_px(base_idx), large = gui_font_size_px(large_idx);
        if (base > 0 && large > 0 && (scale * base) % large == 0) {
            int sub = scale * base / large;
            if (sub >= 1 && gui_font_lookup_n(cp, large_idx, g)) {
                *out_idx = large_idx; *out_sub = sub; return 1;
            }
        }
    }
    if (gui_font_lookup_n(cp, base_idx, g)) {
        *out_idx = base_idx; *out_sub = scale; return 1;
    }
    return 0;
}

int gd_gui_cp_advance(uint32_t cp, int scale) {
    gui_glyph_t g;
    int idx, sub;
    if (!gd_gui_resolve_glyph(cp, scale, &g, &idx, &sub)) return 6 * scale;
    int adv = (int)g.advance * sub;
    if (adv <= 0) adv = (g.width ? (g.width + 1) : 4) * sub;
    return adv;
}

int gd_draw_text_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale) {
    if (scale < 1) scale = 1;
    gui_glyph_t g;
    int idx, sub;
    if (!gd_gui_resolve_glyph(cp, scale, &g, &idx, &sub)) {
        if (!gd_gui_resolve_glyph('?', scale, &g, &idx, &sub)) return 6 * scale;
    }
    int baseline = y + gui_font_ascent_n(idx) * sub;
    int gx = x + (int)g.bearing_x * sub;
    int gy = baseline - (int)g.bearing_y * sub;
    gd_blit_glyph(gx, gy, &g, color, sub);
    int adv = (int)g.advance * sub;
    if (adv <= 0) adv = (g.width ? (g.width + 1) : 4) * sub;
    return adv;
}

// Total pen advance of a UTF-8 string (no drawing). Used for centering/layout.
int gd_text_width(const char *s, int scale) {
    if (!s) return 0;
    if (scale < 1) scale = 1;
    int w = 0;
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';
        w += gd_gui_cp_advance(cp, scale);
    }
    return w;
}

int gui_text_width(const char *s, int scale) { return gd_text_width(s, scale); }

void gd_text(int x, int y, const char *s, uint32_t color, int scale) {
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        x += gd_draw_text_codepoint(x, y, cp, color, scale);
    }
}

// Terminal-style text: ASCII in the crisp 8x16 console bitmap font (fixed 8px
// cells, like the real TUI), any CJK falls back to the proportional GUI font so
// shell output containing Chinese still renders. y is the TOP of the cell.
// Returns the end pen x.
int gd_text_mono(int x, int y, int max_x, const char *s, uint32_t color) {
    if (!s) return x;
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';
        if (cp < 0x80) {
            if (x + GUI_MONO_GLYPH_W > max_x) break;
            gd_draw_mono_char(x, y, (char)cp, color);
            x += GUI_MONO_GLYPH_W;
        } else {
            int adv = gd_gui_cp_advance(cp, 1);
            if (x + adv > max_x) break;
            // Align the proportional baseline near the bottom of the mono cell.
            gd_draw_text_codepoint(x, y + GUI_MONO_GLYPH_H - gui_font_ascent() - 2, cp, color, 1);
            x += adv;
        }
    }
    return x;
}

void gd_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale) {
    if (!s || max_x <= x) return;
    utf8_state_t utf8;
    utf8_init(&utf8);
    while (*s) {
        uint32_t cp = 0;
        int ok = utf8_feed(&utf8, (uint8_t)*s++, &cp);
        if (ok < 0) continue;
        if (ok == 0) cp = '?';

        int advance = gd_gui_cp_advance(cp, scale);
        if (x + advance > max_x) break;
        gd_draw_text_codepoint(x, y, cp, color, scale);
        x += advance;
    }
}

void gd_append_char(char *buf, uint32_t cap, uint32_t *pos, char c) {
    if (*pos + 1 >= cap) return;
    buf[(*pos)++] = c;
    buf[*pos] = 0;
}

void gd_append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s) {
    while (s && *s && *pos + 1 < cap) gd_append_char(buf, cap, pos, *s++);
}

void gd_append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v) {
    char tmp[16];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));
    while (n) gd_append_char(buf, cap, pos, tmp[--n]);
}

void gd_append_int(char *buf, uint32_t cap, uint32_t *pos, int v) {
    if (v < 0) gd_append_char(buf, cap, pos, '-');
    gd_append_uint(buf, cap, pos, (uint32_t)(v < 0 ? -v : v));
}

void gd_append_uint64(char *buf, uint32_t cap, uint32_t *pos, unsigned long long v) {
    char tmp[24];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < sizeof(tmp));
    while (n) gd_append_char(buf, cap, pos, tmp[--n]);
}

void gd_append_ll(char *buf, uint32_t cap, uint32_t *pos, long long v) {
    /* -v on LLONG_MIN overflows signed long long -- negate in unsigned
     * space instead, which wraps around to the correct magnitude. */
    if (v < 0) {
        gd_append_char(buf, cap, pos, '-');
        gd_append_uint64(buf, cap, pos, 0ULL - (unsigned long long)v);
    } else {
        gd_append_uint64(buf, cap, pos, (unsigned long long)v);
    }
}

/* Scientific-notation formatting for calculator results that overflow
 * long long (see app_calc.c's calc_to_sci) -- pure integer arithmetic
 * only, no float/double, since the kernel is built with SSE/x87 disabled
 * (see Makefile's CFLAGS: -mno-80387 -mno-mmx -mno-sse -mno-sse2). mant is
 * a <=9-significant-digit integer (any sign); the true value is
 * approximately mant * 10^exp. Renders as "D.DDDDDDDDe+NN". */
void gd_append_sci(char *buf, uint32_t cap, uint32_t *pos, long long mant, int exp) {
    if (mant < 0) gd_append_char(buf, cap, pos, '-');
    unsigned long long m = mant < 0 ? (0ULL - (unsigned long long)mant) : (unsigned long long)mant;
    char digits[20];
    int nd = 0;
    if (m == 0) digits[nd++] = '0';
    while (m) { digits[nd++] = (char)('0' + (m % 10)); m /= 10; }
    int total_exp = exp + (nd - 1);
    gd_append_char(buf, cap, pos, digits[nd - 1]);
    gd_append_char(buf, cap, pos, '.');
    if (nd > 1) {
        for (int i = nd - 2; i >= 0; i--) gd_append_char(buf, cap, pos, digits[i]);
    } else {
        gd_append_char(buf, cap, pos, '0');
    }
    gd_append_char(buf, cap, pos, 'e');
    gd_append_char(buf, cap, pos, total_exp < 0 ? '-' : '+');
    int e = total_exp < 0 ? -total_exp : total_exp;
    if (e < 10) gd_append_char(buf, cap, pos, '0');
    gd_append_uint(buf, cap, pos, (uint32_t)e);
}

uint32_t *gd_surface_ptr(void) { return g_gui_surface; }
int gd_surface_w(void) { return g_gui_surface_w; }
int gd_surface_h(void) { return g_gui_surface_h; }
uint32_t gd_surface_pitch(void) { return g_gui_surface_pitch; }

int *gd_text_italic_ptr(void) { return &g_text_italic; }

/* ── 公共 API 别名 ────────────────────────────────────────────
 * 这几个函数原本就是 gui_draw.h 里的公开接口（其它模块与 hax 应用按
 * gui_* 名字调用）。拆分时它们随实现改成了 gd_ 前缀，这里补回公开名字，
 * 保持既有调用方（app_notes.c、浏览器引擎等）不需改动。 */
int gui_cp_advance(uint32_t cp, int scale) { return gd_gui_cp_advance(cp, scale); }