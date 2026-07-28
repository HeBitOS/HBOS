/**
 * @file nogui.c
 * @brief 无桌面构建的 GUI 边界空实现。
 *
 * no-GUI 内核保留公开系统调用编号，保证 ABI 不重排；所有图形入口明确报告
 * 不可用，不分配窗口或表面。
 */
#include "../api/gui_service.h"

int gui_service_canvas_info(int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}

void gui_service_canvas_clear(uint32_t color) {
    (void)color;
}

void gui_service_canvas_rect(int x, int y, int w, int h, uint32_t color) {
    (void)x; (void)y; (void)w; (void)h; (void)color;
}

void gui_service_canvas_text(int x, int y, const char *s, uint32_t color, int scale) {
    (void)x; (void)y; (void)s; (void)color; (void)scale;
}

void gui_service_canvas_present(void) {
}

int gui_service_canvas_pollkey(void) {
    return -1;
}

int gui_service_canvas_pollmouse(int *x, int *y) {
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

int gui_service_window_open(uint32_t owner_task, const char *title, int w, int h) {
    (void)owner_task; (void)title; (void)w; (void)h;
    return -1;
}

int gui_service_window_info(uint32_t owner_task, int *w, int *h) {
    (void)owner_task;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}

void gui_service_window_clear(uint32_t owner_task, uint32_t color) {
    (void)owner_task; (void)color;
}

void gui_service_window_fill(uint32_t owner_task, int x, int y, int w, int h,
                             uint32_t color) {
    (void)owner_task; (void)x; (void)y; (void)w; (void)h; (void)color;
}

void gui_service_window_text(uint32_t owner_task, int x, int y, const char *s,
                             uint32_t color) {
    (void)owner_task; (void)x; (void)y; (void)s; (void)color;
}

void gui_service_window_blit(uint32_t owner_task, int x, int y, int w, int h,
                             const uint32_t *pixels, int stride) {
    (void)owner_task; (void)x; (void)y; (void)w; (void)h;
    (void)pixels; (void)stride;
}

void gui_service_window_present(uint32_t owner_task) {
    (void)owner_task;
}

int gui_service_window_poll(uint32_t owner_task, int *event4) {
    (void)owner_task;
    if (event4) {
        event4[0] = 0; event4[1] = 0; event4[2] = 0; event4[3] = 0;
    }
    return 0;
}

void gui_service_window_close(uint32_t owner_task) {
    (void)owner_task;
}

int gui_service_window_v2(uint32_t owner_task, uint32_t operation,
                          int handle, void *data) {
    (void)owner_task;
    (void)handle;
    if (operation == GUI_SERVICE_V2_QUERY && data) {
        gui_service_caps_t *caps = (gui_service_caps_t *)data;
        if (caps->struct_size < sizeof(*caps)) return -1;
        caps->abi_major = HBOS_GUI_SERVICE_ABI_MAJOR;
        caps->abi_minor = HBOS_GUI_SERVICE_ABI_MINOR;
        caps->capabilities = 0;
        caps->max_windows = 0;
        caps->event_queue_size = 0;
        return 0;
    }
    return -1;
}
