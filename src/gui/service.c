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

void gui_service_window_blit(uint32_t owner_task, int x, int y, int w, int h,
                             const uint32_t *pixels, int stride) {
    winsrv_blit_argb(winsrv_for_task(owner_task), x, y, w, h, pixels, stride);
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

static int gui_service_v2_header(uint32_t size, uint16_t major,
                                 uint32_t minimum) {
    return size >= minimum && major == HBOS_GUI_SERVICE_ABI_MAJOR;
}

int gui_service_window_v2(uint32_t owner_task, uint32_t operation,
                          int handle, void *data) {
    if (operation == GUI_SERVICE_V2_QUERY) {
        gui_service_caps_t *caps = (gui_service_caps_t *)data;
        if (!caps || caps->struct_size < sizeof(*caps)) return -1;
        caps->abi_major = HBOS_GUI_SERVICE_ABI_MAJOR;
        caps->abi_minor = HBOS_GUI_SERVICE_ABI_MINOR;
        caps->capabilities =
            GUI_SERVICE_CAP_MULTI_WINDOW | GUI_SERVICE_CAP_RESIZE |
            GUI_SERVICE_CAP_ARGB | GUI_SERVICE_CAP_DIRTY_RECT |
            GUI_SERVICE_CAP_BATCH_DRAW | GUI_SERVICE_CAP_FOCUS_EVENTS;
        caps->max_windows = WINSRV_MAX;
        caps->event_queue_size = WINSRV_EVQ;
        return 0;
    }

    if (operation == GUI_SERVICE_V2_CREATE) {
        gui_service_create_t *create = (gui_service_create_t *)data;
        if (!create ||
            !gui_service_v2_header(create->struct_size, create->abi_major,
                                   sizeof(*create)))
            return -1;
        return winsrv_create_handle(owner_task, create->title,
                                    create->width, create->height);
    }

    winsrv_window_t *win = winsrv_for_handle(owner_task, handle);
    if (!win) return -1;

    switch (operation) {
        case GUI_SERVICE_V2_GET_STATE: {
            gui_service_state_t *state = (gui_service_state_t *)data;
            if (!state ||
                !gui_service_v2_header(state->struct_size, state->abi_major,
                                       sizeof(*state)))
                return -1;
            state->abi_minor = HBOS_GUI_SERVICE_ABI_MINOR;
            state->x = win->x; state->y = win->y;
            state->width = win->w; state->height = win->h;
            state->window_state = win->state;
            state->flags = (win->focused ? 1u : 0u) |
                           (win->want_close ? 2u : 0u);
            return 0;
        }
        case GUI_SERVICE_V2_SET_TITLE:
            if (!data) return -1;
            winsrv_set_title(win, (const char *)data);
            return 0;
        case GUI_SERVICE_V2_SET_GEOMETRY: {
            gui_service_rect_t *rect = (gui_service_rect_t *)data;
            if (!rect) return -1;
            int moved = rect->x != win->x || rect->y != win->y;
            win->x = rect->x; win->y = rect->y;
            if (winsrv_resize(win, rect->width, rect->height) < 0) return -1;
            if (moved) {
                win->dirty = 1;
                winsrv_push_event(win, WINEV_MOVE, win->x, win->y, 0);
            }
            return 0;
        }
        case GUI_SERVICE_V2_SET_WINDOW_STATE: {
            if (!data) return -1;
            int state = *(const int *)data;
            if (state < WINSRV_STATE_NORMAL ||
                state > WINSRV_STATE_MAXIMIZED) return -1;
            if (state != win->state) {
                win->state = state;
                win->dirty = 1;
                winsrv_push_event(win, WINEV_STATE, state, 0, 0);
            }
            return 0;
        }
        case GUI_SERVICE_V2_DRAW_BATCH: {
            gui_service_batch_t *batch = (gui_service_batch_t *)data;
            if (!batch || !batch->commands || batch->count > 1024 ||
                !gui_service_v2_header(batch->struct_size, batch->abi_major,
                                       sizeof(*batch)))
                return -1;
            for (uint32_t i = 0; i < batch->count; i++) {
                const gui_service_draw_t *cmd = &batch->commands[i];
                switch (cmd->type) {
                    case GUI_SERVICE_DRAW_CLEAR:
                        winsrv_clear(win, cmd->color);
                        break;
                    case GUI_SERVICE_DRAW_FILL:
                        winsrv_fill(win, cmd->x, cmd->y, cmd->width,
                                    cmd->height, cmd->color);
                        break;
                    case GUI_SERVICE_DRAW_TEXT:
                        if (!cmd->data) return -1;
                        winsrv_text(win, cmd->x, cmd->y,
                                    (const char *)cmd->data, cmd->color);
                        break;
                    case GUI_SERVICE_DRAW_ARGB:
                        if (!cmd->data) return -1;
                        winsrv_blit_argb(win, cmd->x, cmd->y, cmd->width,
                                         cmd->height,
                                         (const uint32_t *)cmd->data,
                                         cmd->stride);
                        break;
                    default:
                        return -1;
                }
            }
            return 0;
        }
        case GUI_SERVICE_V2_PRESENT: {
            gui_service_rect_t *rect = (gui_service_rect_t *)data;
            if (rect)
                winsrv_mark_dirty(win, rect->x, rect->y,
                                  rect->width, rect->height);
            task_yield();
            return 0;
        }
        case GUI_SERVICE_V2_POLL: {
            gui_service_event_t *out = (gui_service_event_t *)data;
            if (!out || out->struct_size < sizeof(*out)) return -1;
            winsrv_event_t ev;
            if (!winsrv_pop_event(win, &ev)) return 0;
            out->abi_major = HBOS_GUI_SERVICE_ABI_MAJOR;
            out->abi_minor = HBOS_GUI_SERVICE_ABI_MINOR;
            out->type = (uint32_t)ev.type;
            out->a = ev.a; out->b = ev.b; out->c = ev.c;
            out->window = (uint32_t)handle;
            return ev.type;
        }
        case GUI_SERVICE_V2_CLOSE:
            winsrv_destroy((int)((uint32_t)handle & 0xFFu));
            return 0;
        default:
            return -1;
    }
}
