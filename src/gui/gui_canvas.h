/**
 * @file    gui_canvas.h
 * @brief   HAX GUI 应用画布接口 —— 内核 syscall 层与 GUI 绘制层之间的桥
 *
 * 这些函数在 src/tools/gui.c 中实现，直接操作 GUI 的离屏表面。用户态 .hax
 * 应用通过 HBOS_SYS_GUI_* 系统调用间接调用它们，从而在窗口/全屏画布上绘图。
 * 当 GUI 未运行（g_gui_surface 为 NULL）时，gui_app_info 返回 0。
 */
#ifndef HBOS_GUI_CANVAS_H
#define HBOS_GUI_CANVAS_H

#include <stdint.h>

/** 查询画布尺寸；GUI 可用返回 1 并写入 w、h，否则返回 0 */
int  gui_app_info(int *w, int *h);

/** 用指定颜色填满整个画布 */
void gui_app_clear(uint32_t color);

/** 在画布上填充矩形 */
void gui_app_rect(int x, int y, int w, int h, uint32_t color);

/** 在画布上绘制文本（UTF-8，scale 为整数放大倍数，>=1） */
void gui_app_text(int x, int y, const char *s, uint32_t color, int scale);

/** 把画布内容提交到真实帧缓冲（显示到屏幕） */
void gui_app_present(void);

/** 轮询一个按键，返回键值；无按键返回 -1 */
int  gui_app_pollkey(void);

/** 轮询鼠标；写入绝对坐标 x、y，返回按键位掩码（bit0=左键） */
int  gui_app_pollmouse(int *x, int *y);

/* ── 绘制图元（v0.1-beta4 扩充）────────────────────────────────
 * 之前画布只有 clear/rect/text 三件套，画不了线、圆、渐变和图标，
 * .hax 应用只能拿矩形拼图案。下面这些直接映射到内核绘图原语，
 * 全部以画布左上角为原点，越界由内核裁剪。 */

/** 直线（Bresenham，1px） */
void gui_app_line(int x0, int y0, int x1, int y1, uint32_t color);

/** 粗直线（thickness >= 1） */
void gui_app_thick_line(int x0, int y0, int x1, int y1, int thickness, uint32_t color);

/** 圆轮廓（1px） */
void gui_app_circle(int cx, int cy, int r, uint32_t color);

/** 实心圆 */
void gui_app_fill_circle(int cx, int cy, int r, uint32_t color);

/** 圆角矩形；corners 位掩码 bit0..3 = 左上/右上/左下/右下 */
void gui_app_round_rect(int x, int y, int w, int h, int r, uint32_t color, int corners);

/** 垂直渐变（top → bottom） */
void gui_app_gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);

/** 半透明矩形（color 最高字节为 alpha，0..255） */
void gui_app_alpha_rect(int x, int y, int w, int h, uint32_t color);

/** 贴一张 RGBA 图标（src 为 w*h 个 0xAARRGGBB 像素，行优先） */
void gui_app_blit(int x, int y, int w, int h, const uint32_t *src);

/** 画内置图标集中的一张（icon_id 见 gui_icons.h） */
void gui_app_icon(int x, int y, int icon_id, uint32_t tint);

/** 文本像素宽度/行高（用于居中排版） */
int  gui_app_text_width(const char *s, int scale);
int  gui_app_text_height(int scale);

#endif /* HBOS_GUI_CANVAS_H */
