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
#define HBOS_GUI_SERVICE_ABI_MINOR 1u

enum {
    GUI_SERVICE_V2_QUERY = 0,
    GUI_SERVICE_V2_CREATE,
    GUI_SERVICE_V2_GET_STATE,
    GUI_SERVICE_V2_SET_TITLE,
    GUI_SERVICE_V2_SET_GEOMETRY,
    GUI_SERVICE_V2_SET_WINDOW_STATE,
    GUI_SERVICE_V2_DRAW_BATCH,
    GUI_SERVICE_V2_PRESENT,
    GUI_SERVICE_V2_POLL,
    GUI_SERVICE_V2_CLOSE,
};

enum {
    GUI_SERVICE_CAP_MULTI_WINDOW = 1u << 0,
    GUI_SERVICE_CAP_RESIZE       = 1u << 1,
    GUI_SERVICE_CAP_ARGB         = 1u << 2,
    GUI_SERVICE_CAP_DIRTY_RECT   = 1u << 3,
    GUI_SERVICE_CAP_BATCH_DRAW   = 1u << 4,
    GUI_SERVICE_CAP_FOCUS_EVENTS = 1u << 5,
};

enum {
    GUI_SERVICE_DRAW_CLEAR = 1,
    GUI_SERVICE_DRAW_FILL,
    GUI_SERVICE_DRAW_TEXT,
    GUI_SERVICE_DRAW_ARGB,
};

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    uint32_t capabilities;
    uint32_t max_windows;
    uint32_t event_queue_size;
    uint32_t reserved[3];
} gui_service_caps_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    const char *title;
    int32_t width, height;
    uint32_t flags;
    uint32_t reserved[3];
} gui_service_create_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    int32_t x, y, width, height;
    int32_t window_state;
    uint32_t flags;
    uint32_t reserved[2];
} gui_service_state_t;

typedef struct {
    int32_t x, y, width, height;
} gui_service_rect_t;

typedef struct {
    uint32_t type;
    int32_t x, y, width, height;
    uint32_t color;
    const void *data;  /**< TEXT: UTF-8 char*；ARGB: uint32_t* */
    int32_t stride;    /**< ARGB 每行像素数；0 表示 width */
    uint32_t reserved;
} gui_service_draw_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    const gui_service_draw_t *commands;
    uint32_t count;
    uint32_t reserved[3];
} gui_service_batch_t;

typedef struct {
    uint32_t struct_size;
    uint16_t abi_major, abi_minor;
    uint32_t type;
    int32_t a, b, c;
    uint32_t window;
    uint32_t reserved[2];
} gui_service_event_t;

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
void gui_service_window_blit(uint32_t owner_task, int x, int y, int w, int h,
                             const uint32_t *pixels, int stride);
void gui_service_window_present(uint32_t owner_task);
int  gui_service_window_poll(uint32_t owner_task, int *event4);
void gui_service_window_close(uint32_t owner_task);

/** HIVE 窗口 ABI v2：显式句柄、多窗口、批量绘制和版本化结构体。 */
int gui_service_window_v2(uint32_t owner_task, uint32_t operation,
                          int handle, void *data);

#endif /* HBOS_API_GUI_SERVICE_H */
