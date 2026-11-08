/**
 * @file    gui_draw.h
 * @brief   导出 84 个接口（0 类型 / 2 宏 / 82 函数）
 *
 * @layer   gui
 * @owner   src/gui/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏     GUI_MONO_GLYPH_W                控制台等宽字形宽（8）
 *   宏     GUI_MONO_GLYPH_H                控制台等宽字形高（16）
 *   函数   gui_rgb                         打包 8:8:8 像素值
 *   函数   gui_set_layer_opacity           设置整层合成不透明度
 *   函数   gui_get_layer_opacity           读取整层合成不透明度
 *   函数   gui_set_surface                 指定绘制目标画布及其尺寸
 *   函数   gui_target_begin                把后续绘制重定向到独立缓冲
 *   函数   gui_target_end                  结束离屏目标，恢复上一层
 *   函数   gui_target_active               查询当前是否在离屏目标内
 *   函数   gd_target_origin                离屏目标的逻辑原点偏移
 *   函数   gui_wl_init                     把桌面画布包成 Wayland root surface
 *   函数   gui_wl_active                   GUI 是否处于 Wayland 合成模式
 *   函数   gui_present_surface             把离屏目标整块上屏
 *   函数   gui_present_rect                把离屏目标的一块矩形上屏
 *   函数   gui_rect                        填充实心矩形
 *   函数   gui_rect_alpha                  按 alpha 混合填充矩形
 *   函数   gui_blit_rgb888                 贴一张 RGB888 位图
 *   函数   gui_blit_argb8888               贴一张 ARGB8888 位图（带混合）
 *   函数   gui_border                      画一像素矩形边框
 *   函数   gui_vgradient                   纵向渐变填充
 *   函数   gui_soft_shadow                 圆角柔化阴影
 *   函数   gui_draw_panel_shell            绘制面板外壳（圆角＋描边＋阴影）
 *   函数   gui_text                        在指定位置绘制 UTF-8 文本
 *   函数   gui_text_clipped                绘制文本并按裁剪区截断
 *   函数   gui_text_width                  测量 UTF-8 文本像素宽
 *   函数   gui_cp_advance                  逐码点绘制（供自行排版的调用方）
 *   函数   gui_draw_codepoint              逐码点绘制（供自行排版的调用方）
 *   函数   gui_text_mono                   等宽文本：ASCII 走 8x16 位图
 *   函数   gui_draw_small_button           Flat Breeze 风格小按钮
 *   函数   gui_draw_mono_char              8x16 位图字体单字符绘制
 *   函数   gui_key_poll                    GUI 主循环按键轮询
 *   函数   gui_surface_width               离屏表面宽（GUI 未运行为 0）
 *   函数   gui_surface_height              离屏表面高（GUI 未运行为 0）
 *   函数   gui_ui_scale_v                  DPI 缩放系数
 *   函数   gui_append_char                 向绘制缓冲追加一个字符
 *   函数   gui_append_str                  向绘制缓冲追加字符串
 *   函数   gui_append_int                  向绘制缓冲追加有符号整数
 *   函数   gui_append_uint                 向绘制缓冲追加无符号整数
 *   函数   gui_append_ll                   向绘制缓冲追加 64 位整数
 *   函数   gui_append_sci                  向绘制缓冲追加科学计数法
 *   函数   gui_draw_line                   画一像素直线
 *   函数   gui_draw_thick_line             画指定线宽的直线
 *   函数   gui_fill_circle                 填充实心圆
 *   函数   gui_draw_circle                 画圆环轮廓
 *   函数   gui_fill_round_rect             填充圆角矩形
 *   函数   gui_set_brightness              设置全局背光亮度
 *   函数   gui_get_brightness              读取全局背光亮度
 *   函数   gd_surface_ptr                  当前绘制目标像素缓冲
 *   函数   gd_surface_w                    当前绘制目标宽
 *   函数   gd_surface_h                    当前绘制目标高
 *   函数   gd_surface_pitch                当前绘制目标行跨度（像素）
 *   函数   gd_draw_text_codepoint          单码点绘制（内部实现）
 *   函数   gd_vgradient_alpha              带 alpha 的纵向渐变
 *   函数   gd_draw_window_control_icon     绘制窗口控制按钮图标
 *   函数   gd_text_italic_ptr              按斜体指针绘制文本
 *   函数   gd_rect                         填充实心矩形（内部实现）
 *   函数   gd_rect_alpha                   alpha 混合矩形（内部实现）
 *   函数   gd_border                       矩形边框（内部实现）
 *   函数   gd_vgradient                    纵向渐变（内部实现）
 *   函数   gd_hgradient                    横向渐变（内部实现）
 *   函数   gd_soft_shadow                  圆角柔化阴影（内部实现）
 *   函数   gd_fill_round_rect              圆角矩形（内部实现）
 *   函数   gd_round_border                 圆角矩形边框
 *   函数   gd_draw_panel_shell             面板外壳（内部实现）
 *   函数   gd_draw_inset_shell             绘制内嵌凹陷外壳
 *   函数   gd_blit_glyph                   贴一张字形位图
 *   函数   gd_blit_mono_glyph              贴一张等宽字形位图
 *   函数   gd_blit_icon_rgba               贴一张 RGBA 图标
 *   函数   gd_blit_icon                    贴一张图标（按当前色调制）
 *   函数   gd_draw_mono_char               等宽单字符绘制（内部实现）
 *   函数   gd_text_width                   测量文本像素宽（内部实现）
 *   函数   gd_text                         绘制文本（内部实现）
 *   函数   gd_text_clipped                 裁剪绘制文本（内部实现）
 *   函数   gd_text_mono                    等宽文本绘制（内部实现）
 *   函数   gd_rgb_mix                      按比例混合两种颜色
 *   函数   gd_rgb_lift                     按比例提亮颜色
 *   函数   gd_apply_layer_opacity          把层不透明度应用到像素
 *   函数   gd_round_corner                 单个圆角的覆盖判定
 *   函数   gd_append_ll                    追加 64 位整数（内部实现）
 *   函数   gd_append_sci                   追加科学计数法（内部实现）
 *   函数   gd_append_char                  追加字符（内部实现）
 *   函数   gd_append_str                   追加字符串（内部实现）
 *   函数   gd_append_uint                  追加无符号整数（内部实现）
 *   函数   gd_append_int                   追加有符号整数（内部实现）
 */
#ifndef HBOS_GUI_DRAW_H
#define HBOS_GUI_DRAW_H

#include <stdint.h>
#include "../graphics/gui_font.h"

#include "../graphics/graphics.h"

uint32_t gui_rgb(uint8_t r, uint8_t g, uint8_t b);

void gui_set_layer_opacity(uint8_t opacity);
uint8_t gui_get_layer_opacity(void);

void gui_set_surface(uint32_t *surface, int w, int h, uint32_t pitch_px);

/* ① P3：把后续绘制重定向到一块独立 buffer（每窗口 buffer）。
 * 逻辑坐标仍是屏幕绝对坐标，绘制层内部减去 (origin_x, origin_y) 落到
 * buffer 上；裁剪区语义不变。只能单层嵌套（内层请求会被忽略）。 */
void gui_target_begin(uint32_t *px, int w, int h, uint32_t pitch_px,
                      int origin_x, int origin_y);
void gui_target_end(void);
int  gui_target_active(void);
void gd_target_origin(int *ox, int *oy);

/* ① P1：把桌面画布包成一个 root surface，呈现改走 attach/damage/commit。
 * 必须在 gui_set_surface() 之后调用；失败时自动退回直贴路径。 */
void gui_wl_init(uint32_t *pixels, int w, int h, uint32_t pitch_px);
int  gui_wl_active(void);
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
/* 整块贴一张 32 位 ARGB8888 位图（Wayland wl_shm 的原生格式）。
 * 与 gui_blit_rgb888 的区别：源已经是目标像素格式，不需要逐像素转换，
 * 一个 32 位拷贝就完事。alpha=0xFF 的像素走不透明路径，其余做混合。 */
void gui_blit_argb8888(int x, int y, const uint8_t *argb, int img_w, int img_h,
                       int src_stride, int clip_x, int clip_y, int clip_w,
                       int clip_h);
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

int *gd_text_italic_ptr(void);
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
void gd_round_border(int x, int y, int w, int h, int r, uint32_t border_c, uint32_t body_c);
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
