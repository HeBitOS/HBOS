/**
 * @file    wl_seat.c
 * @brief   输入席位实现
 */

#include "wl_seat.h"

#define SEAT_POOL 4
static wl_seat_t g_seats[SEAT_POOL];
static uint8_t g_seat_used[SEAT_POOL];

static int q_push(wl_seat_t *s, const wl_event_t *ev) {
    int next = (s->tail + 1) % WL_SEAT_QUEUE;
    if (next == s->head) {          /* 满：丢弃新事件并计数 */
        s->dropped++;
        return -1;
    }
    s->queue[s->tail] = *ev;
    s->queue[s->tail].serial = ++s->serial;
    s->tail = next;
    return 0;
}

static int q_pop(wl_seat_t *s, wl_event_t *out) {
    if (s->head == s->tail) return -1;
    *out = s->queue[s->head];
    s->head = (s->head + 1) % WL_SEAT_QUEUE;
    return 0;
}

wl_seat_t *wl_seat_create(void) {
    for (int i = 0; i < SEAT_POOL; i++) {
        if (g_seat_used[i]) continue;
        g_seat_used[i] = 1;
        wl_seat_t *s = &g_seats[i];
        s->head = s->tail = 0;
        s->dropped = 0;
        s->serial = 0;
        s->pointer_focus = 0;
        s->keyboard_focus = 0;
        s->listener = 0;
        s->listener_ctx = 0;
        return s;
    }
    return 0;
}

void wl_seat_destroy(wl_seat_t *s) {
    if (!s) return;
    int idx = (int)(s - g_seats);
    if (idx >= 0 && idx < SEAT_POOL) g_seat_used[idx] = 0;
}

void wl_seat_add_listener(wl_seat_t *s, wl_event_fn fn, void *ctx) {
    if (!s) return;
    s->listener = fn;
    s->listener_ctx = ctx;
}

void wl_seat_set_pointer_focus(wl_seat_t *s, int surface_id) {
    if (!s || s->pointer_focus == surface_id) return;
    if (s->pointer_focus) {
        wl_event_t ev = {0};
        ev.type = WL_EV_POINTER_LEAVE;
        ev.surface_id = s->pointer_focus;
        (void)q_push(s, &ev);
    }
    s->pointer_focus = surface_id;
    if (surface_id) {
        wl_event_t ev = {0};
        ev.type = WL_EV_POINTER_ENTER;
        ev.surface_id = surface_id;
        (void)q_push(s, &ev);
    }
}

void wl_seat_set_keyboard_focus(wl_seat_t *s, int surface_id) {
    if (!s || s->keyboard_focus == surface_id) return;
    s->keyboard_focus = surface_id;
    wl_event_t ev = {0};
    ev.type = WL_EV_KEYBOARD_FOCUS;
    ev.surface_id = surface_id;
    (void)q_push(s, &ev);
}

int wl_seat_pointer_motion(wl_seat_t *s, int x, int y) {
    if (!s || !s->pointer_focus) return -1;
    wl_event_t ev = {0};
    ev.type = WL_EV_POINTER_MOTION;
    ev.surface_id = s->pointer_focus;
    ev.x = x;
    ev.y = y;
    return q_push(s, &ev);
}

int wl_seat_pointer_button(wl_seat_t *s, int button, int pressed) {
    if (!s || !s->pointer_focus) return -1;
    wl_event_t ev = {0};
    ev.type = WL_EV_POINTER_BUTTON;
    ev.surface_id = s->pointer_focus;
    ev.button = button;
    ev.pressed = pressed ? 1 : 0;
    return q_push(s, &ev);
}

int wl_seat_pointer_axis(wl_seat_t *s, int axis) {
    if (!s || !s->pointer_focus) return -1;
    wl_event_t ev = {0};
    ev.type = WL_EV_POINTER_AXIS;
    ev.surface_id = s->pointer_focus;
    ev.axis = axis;
    return q_push(s, &ev);
}

int wl_seat_key(wl_seat_t *s, uint32_t key, int pressed) {
    if (!s || !s->keyboard_focus) return -1;
    wl_event_t ev = {0};
    ev.type = WL_EV_KEY;
    ev.surface_id = s->keyboard_focus;
    ev.key = key;
    ev.pressed = pressed ? 1 : 0;
    return q_push(s, &ev);
}

int wl_seat_dispatch(wl_seat_t *s, int max_events) {
    if (!s) return 0;
    int n = 0;
    wl_event_t ev;
    while (n < max_events && q_pop(s, &ev) == 0) {
        if (s->listener) s->listener(s->listener_ctx, &ev);
        n++;
    }
    return n;
}

int wl_seat_pending(const wl_seat_t *s) {
    if (!s) return 0;
    return (s->tail - s->head + WL_SEAT_QUEUE) % WL_SEAT_QUEUE;
}

uint32_t wl_seat_dropped(const wl_seat_t *s) { return s ? s->dropped : 0; }
