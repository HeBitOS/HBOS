/**
 * @file    wl_seat.h
 * @brief   Wayland 风格输入席位（① 的地基）
 *
 * 把"焦点 + 事件队列 + 分发"从 gui.c 里独立出来：
 *   - 只有持有指针焦点的 surface 才收到 motion/button/axis；
 *   - 焦点变化会生成 ENTER / LEAVE 事件；
 *   - 键盘同理，焦点单独维护。
 * 事件入队后由 wl_seat_dispatch() 统一派发，合成器不再直接读鼠标。
 */
#ifndef HBOS_WL_SEAT_H
#define HBOS_WL_SEAT_H

#include <stdint.h>

typedef enum {
    WL_EV_POINTER_ENTER = 1,
    WL_EV_POINTER_LEAVE,
    WL_EV_POINTER_MOTION,
    WL_EV_POINTER_BUTTON,
    WL_EV_POINTER_AXIS,
    WL_EV_KEY,
    WL_EV_KEYBOARD_FOCUS
} wl_event_type_t;

typedef struct {
    wl_event_type_t type;
    uint32_t serial;
    int surface_id;     /**< ENTER/LEAVE/FOCUS 时的目标 surface */
    int x, y;           /**< MOTION 坐标（surface 局部坐标） */
    int button;         /**< 1=左 2=右 3=中 */
    int pressed;
    int axis;           /**< 120 = 一格滚轮 */
    uint32_t key;
} wl_event_t;

#define WL_SEAT_QUEUE 32

typedef struct wl_seat wl_seat_t;
typedef void (*wl_event_fn)(void *ctx, const wl_event_t *ev);

struct wl_seat {
    wl_event_t queue[WL_SEAT_QUEUE];
    int head, tail;
    uint32_t dropped;
    uint32_t serial;
    int pointer_focus;
    int keyboard_focus;
    wl_event_fn listener;
    void *listener_ctx;
};

wl_seat_t *wl_seat_create(void);
void wl_seat_destroy(wl_seat_t *s);
void wl_seat_add_listener(wl_seat_t *s, wl_event_fn fn, void *ctx);

/** 设置指针焦点（-1/0 表示无）。会生成 LEAVE(old) + ENTER(new)。 */
void wl_seat_set_pointer_focus(wl_seat_t *s, int surface_id);
/** 设置键盘焦点。会生成 KEYBOARD_FOCUS 事件。 */
void wl_seat_set_keyboard_focus(wl_seat_t *s, int surface_id);

int wl_seat_pointer_motion(wl_seat_t *s, int x, int y);
int wl_seat_pointer_button(wl_seat_t *s, int button, int pressed);
int wl_seat_pointer_axis(wl_seat_t *s, int axis);
int wl_seat_key(wl_seat_t *s, uint32_t key, int pressed);

/** 派发最多 max 个事件，返回实际派发数。 */
int wl_seat_dispatch(wl_seat_t *s, int max_events);
int wl_seat_pending(const wl_seat_t *s);
uint32_t wl_seat_dropped(const wl_seat_t *s);

#endif /* HBOS_WL_SEAT_H */
