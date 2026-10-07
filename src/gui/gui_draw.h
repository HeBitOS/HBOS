#ifndef HBOS_GUI_DRAW_H
#define HBOS_GUI_DRAW_H

#include <stdint.h>
#include "../graphics/gui_font.h"

#include "../graphics/graphics.h"

uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b);

void gui_set_layer_opacity(uint8_t opacity);
uint8_t gui_get_layer_opacity(void);

void gui_set_surface(uint32_t *surface, int w, int h, uint32_t pitch_px);
void gui_present_surface(const fb_info_t *fb);
void gui_present_rect(const fb_info_t *fb, int x, int y, int w, int h);

void gui_rect(int x, int y, int w, int h, uint32_t color);
void gui_rect_alpha(int x, int y, int w, int h, uint32_t color);
/* 一次性把一块 RGB888（每像素 3 字节，行主序，从上到下）位图拷进当前
 * surface，裁剪到 (clip_x,clip_y,clip_w,clip_h) 和屏幕范围——比逐像素
 * gui_rect(px,py,1,1,color) 快得多，供图片查看器这类需要整块贴图的场景
 * 使用。 */
void gui_blit_rgb888(int x, int y, const uint8_t *rgb, int img_w, int img_h,
                      int clip_x, int clip_y, int clip_w, int clip_h);
void gui_border(int x, int y, int w, int h, uint32_t color);
void gui_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gui_soft_shadow(int x, int y, int w, int h);
void gui_draw_panel_shell(int x, int y, int w, int h, uint32_t top, uint32_t bottom,
                          uint32_t border_c, uint32_t accent);
void gui_text(int x, int y, const char *s, uint32_t color, int scale);
void gui_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale);
int  gui_text_width(const char *s, int scale);

/* 逐码点绘制（共享给需要自行排版 UTF-8 的模块：记事本正文循环、
 * 代码高亮逐 span 绘制等）。gui_cp_advance 返回该码点的步进宽度，
 * gui_draw_codepoint 画出字形并返回步进；查不到字形时按 '?' 兜底。 */
int  gui_cp_advance(uint32_t cp, int scale);
int  gui_draw_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale);

/* 终端风格文本：ASCII 用 8x16 控制台位图字体（固定 8px cell，与真 TUI
 * 一致），CJK 回退到比例 GUI 字体。y 是 cell 顶部，返回结束笔位 x。
 * 控制台终端与 C 语法高亮共用。 */
int  gui_text_mono(int x, int y, int max_x, const char *s, uint32_t color);
#define GUI_MONO_GLYPH_W 8
#define GUI_MONO_GLYPH_H 16

/* Flat Breeze 风格小按钮（代码工作台工具行用） */
void gui_draw_small_button(int x, int y, int w, const char *label, uint32_t color);
/* 8x16 控制台位图字体的单字符绘制（C 语法高亮 span 用），y 为 cell 顶 */
int  gui_draw_mono_char(int x, int y, char c, uint32_t color);
/* GUI 主循环按键轮询（含 GUI_KEY_* 归一化），C 脚本 GUI 的 wait_key 用 */
int  gui_key_poll(void);
/* 离屏表面尺寸（GUI 未运行时为 0） */
int  gui_surface_width(void);
int  gui_surface_height(void);
/* DPI 缩放（ui_s 的导出形式；g_ui_scale 由 GUI 启动时按分辨率设置） */
int  gui_ui_scale_v(int v);

void gui_append_char(char *buf, uint32_t cap, uint32_t *pos, char c);
void gui_append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s);
void gui_append_int(char *buf, uint32_t cap, uint32_t *pos, int v);
void gui_append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v);
void gui_append_ll(char *buf, uint32_t cap, uint32_t *pos, long long v);
/* Scientific notation ("D.DDDDDDDDe+NN") for values too large for a plain
 * long long -- see app_calc.c's calc_to_sci for how mant/exp are derived. */
void gui_append_sci(char *buf, uint32_t cap, uint32_t *pos, long long mant, int exp);

void gui_draw_line(int x0, int y0, int x1, int y1, uint32_t color);
void gui_draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint32_t color);
void gui_fill_circle(int cx, int cy, int r, uint32_t color);
void gui_draw_circle(int cx, int cy, int r, uint32_t color);
void gui_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color);


/* ── 渲染层实现（src/gui/gui_draw_impl.c）─────────────────────────
 * 绘制原语的**真实实现**。gui.c 用同名 static inline 短名转发，使既有
 * 300+ 处调用点零改动；其它模块（控件库等）应直接用 gd_ 版本。 */
void gui_set_brightness(int pct);
int  gui_get_brightness(void);
uint32_t *gd_surface_ptr(void);
int gd_surface_w(void);
int gd_surface_h(void);
uint32_t gd_surface_pitch(void);
int  gd_draw_text_codepoint(int x, int y, uint32_t cp, uint32_t color, int scale);
void gd_vgradient_alpha(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gd_draw_window_control_icon(int x, int y, int kind, int restore, uint32_t color);

/* 窗口控制按钮类型（gd_draw_window_control_icon 的 kind） */
enum { GUI_CTRL_CLOSE = 0, GUI_CTRL_MAX = 1, GUI_CTRL_MIN = 2 };

void gd_rect(int x, int y, int w, int h, uint32_t color);
void gd_rect_alpha(int x, int y, int w, int h, uint32_t color);
void gd_border(int x, int y, int w, int h, uint32_t color);
void gd_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gd_hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right);
void gd_soft_shadow(int x, int y, int w, int h);
/* 圆角掩码：gd_fill_round_rect 的 corners 参数。
 * 放在公共头是因为调用方（gui.c 的面板/按钮、控件库）都要用它拼圆角，
 * 而它们不该各自重复定义一份。 */
enum {
    RR_TL = 1, RR_TR = 2, RR_BL = 4, RR_BR = 8,
    RR_ALL = 15, RR_TOP = 3, RR_BOT = 12
};

void gd_fill_round_rect(int x, int y, int w, int h, int r, uint32_t color, int corners);
void gd_draw_panel_shell(int x, int y, int w, int h, uint32_t top, uint32_t bottom,
                         uint32_t border_c, uint32_t accent);
void gd_draw_inset_shell(int x, int y, int w, int h, uint32_t border_c);
void gd_blit_glyph(int x, int y, const gui_glyph_t *g, uint32_t color, int scale);
void gd_blit_mono_glyph(int x, int y, const uint8_t *glyph, uint32_t color, int scale);
void gd_blit_icon_rgba(int x, int y, int sz, const uint32_t *src, int tile);
void gd_blit_icon(int x, int y, int sz, int icon_id);
int  gd_draw_mono_char(int x, int y, char c, uint32_t color);
int  gd_text_width(const char *s, int scale);
void gd_text(int x, int y, const char *s, uint32_t color, int scale);
void gd_text_clipped(int x, int y, int max_x, const char *s, uint32_t color, int scale);
int  gd_text_mono(int x, int y, int max_x, const char *s, uint32_t color);
uint32_t gd_rgb_mix(uint32_t c1, uint32_t c2, int t);
uint32_t gd_rgb_lift(uint32_t c, int d);
uint32_t gd_apply_layer_opacity(uint32_t color);
void gd_round_corner(int bx, int by, int r, int cxc, int cyc, uint32_t color);
void gd_append_ll(char *buf, uint32_t cap, uint32_t *pos, long long v);
void gd_append_sci(char *buf, uint32_t cap, uint32_t *pos, long long mant, int exp);
void gd_append_char(char *buf, uint32_t cap, uint32_t *pos, char c);
void gd_append_str(char *buf, uint32_t cap, uint32_t *pos, const char *s);
void gd_append_uint(char *buf, uint32_t cap, uint32_t *pos, uint32_t v);
void gd_append_int(char *buf, uint32_t cap, uint32_t *pos, int v);

#endif
