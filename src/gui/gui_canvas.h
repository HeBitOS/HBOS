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

#endif /* HBOS_GUI_CANVAS_H */
