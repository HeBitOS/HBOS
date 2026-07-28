/**
 * @file gui_service.h
 * @brief 内核与可选 GUI 组件之间的稳定服务边界。
 *
 * syscall 层只依赖本接口，不包含窗口管理器、合成器或应用模块私有结构。
 * GUI 构建链接 src/gui/service.c；no-GUI 构建链接 src/gui/nogui.c。
 */
#ifndef HBOS_API_GUI_SERVICE_H
#define HBOS_API_GUI_SERVICE_H

#include <stdint.h>

#define HBOS_GUI_SERVICE_ABI_MAJOR 1u
#define HBOS_GUI_SERVICE_ABI_MINOR 0u

int  gui_service_canvas_info(int *w, int *h);
void gui_service_canvas_clear(uint32_t color);
void gui_service_canvas_rect(int x, int y, int w, int h, uint32_t color);
void gui_service_canvas_text(int x, int y, const char *s, uint32_t color, int scale);
void gui_service_canvas_present(void);
int  gui_service_canvas_pollkey(void);
int  gui_service_canvas_pollmouse(int *x, int *y);

int  gui_service_window_open(uint32_t owner_task, const char *title, int w, int h);
int  gui_service_window_info(uint32_t owner_task, int *w, int *h);
void gui_service_window_clear(uint32_t owner_task, uint32_t color);
void gui_service_window_fill(uint32_t owner_task, int x, int y, int w, int h,
                             uint32_t color);
void gui_service_window_text(uint32_t owner_task, int x, int y, const char *s,
                             uint32_t color);
void gui_service_window_present(uint32_t owner_task);
int  gui_service_window_poll(uint32_t owner_task, int *event4);
void gui_service_window_close(uint32_t owner_task);

#endif /* HBOS_API_GUI_SERVICE_H */
