/**
 * @file service.c
 * @brief 完整 GUI 对内核 gui_service ABI 的适配实现。
 */
#include "../api/gui_service.h"
#include "../core/task.h"
#include "gui_canvas.h"
#include "winsrv.h"

int gui_service_canvas_info(int *w, int *h) {
    return gui_app_info(w, h);
}

void gui_service_canvas_clear(uint32_t color) {
    gui_app_clear(color);
}

void gui_service_canvas_rect(int x, int y, int w, int h, uint32_t color) {
    gui_app_rect(x, y, w, h, color);
}

void gui_service_canvas_text(int x, int y, const char *s, uint32_t color, int scale) {
    gui_app_text(x, y, s, color, scale);
}

void gui_service_canvas_present(void) {
    gui_app_present();
}

int gui_service_canvas_pollkey(void) {
    return gui_app_pollkey();
}

int gui_service_canvas_pollmouse(int *x, int *y) {
    return gui_app_pollmouse(x, y);
}

int gui_service_window_open(uint32_t owner_task, const char *title, int w, int h) {
    return winsrv_create(owner_task, title, w, h);
}

int gui_service_window_info(uint32_t owner_task, int *w, int *h) {
    winsrv_window_t *win = winsrv_for_task(owner_task);
    if (!win || win->want_close) return 0;
    if (w) *w = win->w;
    if (h) *h = win->h;
    return 1;
}

void gui_service_window_clear(uint32_t owner_task, uint32_t color) {
    winsrv_clear(winsrv_for_task(owner_task), color);
}

void gui_service_window_fill(uint32_t owner_task, int x, int y, int w, int h,
                             uint32_t color) {
    winsrv_fill(winsrv_for_task(owner_task), x, y, w, h, color);
}

void gui_service_window_text(uint32_t owner_task, int x, int y, const char *s,
                             uint32_t color) {
    winsrv_text(winsrv_for_task(owner_task), x, y, s, color);
}

void gui_service_window_present(uint32_t owner_task) {
    (void)owner_task;
    task_yield();
}

int gui_service_window_poll(uint32_t owner_task, int *event4) {
    winsrv_event_t ev;
    winsrv_window_t *win = winsrv_for_task(owner_task);
    if (!win || !winsrv_pop_event(win, &ev)) return 0;
    if (event4) {
        event4[0] = ev.type;
        event4[1] = ev.a;
        event4[2] = ev.b;
        event4[3] = ev.c;
    }
    return ev.type;
}

void gui_service_window_close(uint32_t owner_task) {
    winsrv_close_for_task(owner_task);
}
