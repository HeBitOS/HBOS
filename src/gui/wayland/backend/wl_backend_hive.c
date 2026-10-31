/**
 * @file    wl_backend_hive.c
 * @brief   Wayland → HIVE 的渲染与输入后端
 *
 * 渲染路径（每帧）：
 *   surface.damaged
 *     → wl_buffer_read() 从 memfd 按行取像素到内核缓冲
 *       → gui_blit_argb8888() 整块贴上当前 GUI 表面
 *         → gui_present_rect() 刷到帧缓冲
 *
 * 为什么中间要过一块内核缓冲：客户端像素在它自己的 memfd 物理页里，
 * blit 函数要求线性地址。按行 read_at 是"每行一次页内 memcpy"，比给
 * 整块建映射简单，也不会因为客户端 unmap 而悬空。
 *
 * 输入路径是反的：HIVE 收键鼠 → 这里找焦点 surface → 组 wl_pointer /
 * wl_keyboard 事件发回去。
 */

#include "wl_backend.h"

#include "../../../core/heap.h"
#include "../../../string.h"
#include "../../gui_draw.h"

extern void console_puts(const char *text);

/** 每个 surface 一块暂存像素。尺寸变了就重分配。 */
typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int capacity_bytes;
} wl_backend_scratch_t;

static wl_backend_scratch_t wl_scratch[WL_MAX_SURFACES];

/* 焦点：谁最后被点到 / 谁的窗口在最上面。0 表示没有。 */
static wl_surface_state_t *wl_focus_surface;
static int wl_pointer_x = -1;
static int wl_pointer_y = -1;
static int wl_pointer_inside;
static int wl_input_probe;
static int wl_input_probe_done;

void wl_backend_set_input_probe(int enable) {
    wl_input_probe = enable;
    wl_input_probe_done = 0;
}

void wl_backend_init(void) {
    memset(wl_scratch, 0, sizeof(wl_scratch));
    wl_focus_surface = NULL;
    wl_pointer_x = -1;
    wl_pointer_y = -1;
    wl_pointer_inside = 0;
}

/* ── 暂存缓冲 ───────────────────────────────────────────────── */

static uint32_t *wl_scratch_for(int index, int width, int height) {
    if (index < 0 || index >= WL_MAX_SURFACES) return NULL;
    wl_backend_scratch_t *scratch = &wl_scratch[index];
    int needed = width * height * 4;
    if (needed <= 0) return NULL;
    if (scratch->pixels && scratch->capacity_bytes >= needed &&
        scratch->width == width && scratch->height == height)
        return scratch->pixels;
    if (scratch->pixels) {
        kfree(scratch->pixels);
        scratch->pixels = NULL;
        scratch->capacity_bytes = 0;
    }
    uint32_t *pixels = (uint32_t *)kmalloc((size_t)needed);
    if (!pixels) return NULL;
    scratch->pixels = pixels;
    scratch->width = width;
    scratch->height = height;
    scratch->capacity_bytes = needed;
    return pixels;
}

/* ── 合成 ───────────────────────────────────────────────────── */

/** surface 在屏幕上的左上角。MVP 阶段全部贴 (0,0)，窗口管理交给 HIVE 之后
 *  再按 window_id 查真实位置。 */
static void wl_surface_origin(const wl_surface_state_t *surface, int *x, int *y) {
    (void)surface;
    if (x) *x = 0;
    if (y) *y = 0;
}

static int wl_composite_surface(wl_surface_state_t *surface, int index) {
    wl_shm_buffer_t *buffer = surface->current;
    if (!buffer || !buffer->used) return 0;
    if (surface->width <= 0 || surface->height <= 0) return 0;

    uint32_t *scratch = wl_scratch_for(index, surface->width, surface->height);
    if (!scratch) return 0;

    if (wl_buffer_read(buffer, scratch, surface->width * 4,
                       0, 0, surface->width, surface->height) != 0) {
        /* 读不出来（fd 关了 / 尺寸不对）：这一帧跳过，不要画垃圾。 */
        return 0;
    }

    int origin_x = 0, origin_y = 0;
    wl_surface_origin(surface, &origin_x, &origin_y);
    gui_blit_argb8888(origin_x, origin_y, (const uint8_t *)scratch,
                      surface->width, surface->height, surface->width * 4,
                      origin_x, origin_y, surface->width, surface->height);

    /* 合成完就把 buffer 还给客户端。
     *
     * 真实合成器会等这一帧真正上屏再 release；我们的 blit 是同步的，
     * 返回时像素已经在屏幕缓冲里，所以立刻归还最安全 —— 客户端拿到
     * release 才能复用这块内存继续画下一帧。不还的话单缓冲客户端在
     * 第二帧就卡死（它没有第二块 buffer 可 attach）。
     *
     * 注意：只还 current，不还 attached —— attached 是客户端刚提交、
     * 还没被合成的那块。 */
    if (buffer && surface->client) {
        uint32_t buffer_id = wl_client_find_id(surface->client, WL_OBJ_BUFFER,
                                               buffer);
        if (buffer_id) {
            wl_event_begin(surface->client, buffer_id, WL_BUFFER_E_RELEASE);
            wl_event_end(surface->client);
            wl_client_flush(surface->client);
        }
        buffer->busy = 0;
    }
    return 1;
}

void wl_backend_pump(void) {
    int composited = 0;
    int total = wl_iface_surface_count();
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        wl_surface_state_t *surface = wl_iface_surface_at(i);
        if (!surface || !surface->damaged) continue;
        if (!surface->mapped) {
            surface->damaged = 0;
            continue;
        }
        if (wl_composite_surface(surface, i)) {
            composited++;
            /* 自检：第一次成功上屏后注入一轮输入，验证回传通路。
             * 只在开关打开时做一次，避免干扰正常使用。 */
            if (wl_input_probe && !wl_input_probe_done) {
                wl_input_probe_done = 1;
                if (surface->width > 4 && surface->height > 4) {
                    wl_backend_pointer_motion(surface->width / 2,
                                              surface->height / 2);
                    wl_backend_pointer_motion(surface->width / 2 + 1,
                                              surface->height / 2 + 1);
                    wl_backend_pointer_button(surface->width / 2,
                                              surface->height / 2, 0x110, 1);
                    wl_backend_keyboard_key(30 /* KEY_A */, 1);
                }
            }
        }
        surface->damaged = 0;
    }
    (void)composited;
    (void)total;
}

void wl_backend_surface_committed(wl_client_t *client,
                                  wl_surface_state_t *surface) {
    (void)client;
    if (!surface) return;
    /* commit 已经把 damaged 置好了；这里只负责把焦点给新映射的窗口，
     * 让键盘输入有个默认落点。 */
    if (surface->mapped && !wl_focus_surface) wl_focus_surface = surface;
}

void wl_backend_surface_destroyed(wl_client_t *client,
                                  wl_surface_state_t *surface) {
    (void)client;
    if (!surface) return;
    if (wl_focus_surface == surface) wl_focus_surface = NULL;
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (wl_iface_surface_at(i) != surface) continue;
        if (wl_scratch[i].pixels) {
            kfree(wl_scratch[i].pixels);
            wl_scratch[i].pixels = NULL;
            wl_scratch[i].capacity_bytes = 0;
        }
        break;
    }
}

/* ── 焦点与坐标 ─────────────────────────────────────────────── */

static wl_surface_state_t *wl_surface_at_point(int x, int y) {
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        wl_surface_state_t *surface = wl_iface_surface_at(i);
        if (!surface || !surface->mapped) continue;
        if (surface->width <= 0 || surface->height <= 0) continue;
        int origin_x = 0, origin_y = 0;
        wl_surface_origin(surface, &origin_x, &origin_y);
        if (x >= origin_x && x < origin_x + surface->width &&
            y >= origin_y && y < origin_y + surface->height)
            return surface;
    }
    return NULL;
}

/** 找到客户端里某个 surface 对应的 wl_pointer / wl_keyboard 对象。 */
static wl_client_t *wl_client_of_surface(wl_surface_state_t *surface) {
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        wl_client_t *client = wl_server_client_at(i);
        if (!client) continue;
        for (uint32_t id = 1; id < WL_MAX_OBJECTS; id++) {
            if (!client->objects[id].id) continue;
            if (client->objects[id].data == surface) return client;
        }
    }
    return NULL;
}

int wl_backend_has_keyboard_focus(void) {
    return wl_focus_surface != NULL;
}

/* ── 指针事件 ───────────────────────────────────────────────── */

int wl_backend_pointer_motion(int x, int y) {
    wl_surface_state_t *target = wl_surface_at_point(x, y);
    wl_client_t *client = target ? wl_client_of_surface(target) : NULL;

    /* 离开原来的 surface */
    if (wl_pointer_inside && (!target || target != wl_focus_surface)) {
        wl_client_t *old_client = wl_focus_surface ?
            wl_client_of_surface(wl_focus_surface) : NULL;
        if (old_client) {
            uint32_t pointer_id =
                wl_client_find_id(old_client, WL_OBJ_POINTER, NULL);
            uint32_t old_surface_id = wl_focus_surface ?
                wl_client_find_id(old_client, WL_OBJ_SURFACE,
                                  wl_focus_surface) : 0;
            if (pointer_id) {
                /* leave(serial, surface) —— wl_pointer 的事件号是
                 * 0=enter 1=leave 2=motion 3=button 4=axis，
                 * 别把 motion 的 1 当成 leave。 */
                wl_event_begin(old_client, pointer_id, 1 /* leave */);
                wl_event_u32(old_client, old_client->serial++);
                wl_event_u32(old_client, old_surface_id);
                wl_event_end(old_client);
            }
            wl_client_flush(old_client);
        }
        wl_pointer_inside = 0;
    }

    if (!target || !client) {
        wl_pointer_x = x;
        wl_pointer_y = y;
        return 0;
    }

    int origin_x = 0, origin_y = 0;
    wl_surface_origin(target, &origin_x, &origin_y);
    int local_x = x - origin_x;
    int local_y = y - origin_y;
    uint32_t pointer_id = wl_client_find_id(client, WL_OBJ_POINTER, NULL);
    if (!pointer_id) {
        wl_pointer_x = x;
        wl_pointer_y = y;
        return 0;
    }

    if (!wl_pointer_inside) {
        /* enter(serial, surface, x, y)：surface 参数用的是 wl_surface 的
         * 对象 id，客户端据此知道是哪块表面被进入。 */
        uint32_t surface_id = wl_client_find_id(client, WL_OBJ_SURFACE, target);
        wl_event_begin(client, pointer_id, 0 /* enter */);
        wl_event_u32(client, client->serial++);
        wl_event_u32(client, surface_id);
        wl_event_i32(client, local_x);
        wl_event_i32(client, local_y);
        wl_event_end(client);
        wl_pointer_inside = 1;
        wl_focus_surface = target;
    } else {
        wl_event_begin(client, pointer_id, 2 /* motion */);
        wl_event_i32(client, local_x);
        wl_event_i32(client, local_y);
        wl_event_end(client);
    }
    wl_client_flush(client);
    wl_pointer_x = x;
    wl_pointer_y = y;
    return 1;
}

int wl_backend_pointer_button(int x, int y, uint32_t button, int pressed) {
    wl_surface_state_t *target = wl_surface_at_point(x, y);
    if (!target) target = wl_focus_surface;
    wl_client_t *client = target ? wl_client_of_surface(target) : NULL;
    if (!client) return 0;

    /* 点击决定了键盘焦点：点谁谁接管输入。 */
    if (pressed) wl_focus_surface = target;

    uint32_t pointer_id = wl_client_find_id(client, WL_OBJ_POINTER, NULL);
    if (!pointer_id) return 0;
    int origin_x = 0, origin_y = 0;
    wl_surface_origin(target, &origin_x, &origin_y);
    wl_event_begin(client, pointer_id, 3 /* button */);
    wl_event_u32(client, client->serial++);
    wl_event_i32(client, x - origin_x);
    wl_event_i32(client, y - origin_y);
    wl_event_u32(client, button);
    wl_event_u32(client, pressed ? 1 : 0);   /* WL_POINTER_BUTTON_STATE_* */
    wl_event_end(client);
    wl_client_flush(client);
    return 1;
}

int wl_backend_pointer_axis(int x, int y, int32_t axis, int32_t value) {
    wl_surface_state_t *target = wl_surface_at_point(x, y);
    if (!target) target = wl_focus_surface;
    wl_client_t *client = target ? wl_client_of_surface(target) : NULL;
    if (!client) return 0;
    uint32_t pointer_id = wl_client_find_id(client, WL_OBJ_POINTER, NULL);
    if (!pointer_id) return 0;
    wl_event_begin(client, pointer_id, 4 /* axis */);
    wl_event_u32(client, client->serial++);
    wl_event_u32(client, (uint32_t)axis);   /* 0=vertical 1=horizontal */
    wl_event_u32(client, (uint32_t)value);  /* wl_fixed_t */
    wl_event_end(client);
    wl_client_flush(client);
    return 1;
}

/* ── 键盘事件 ───────────────────────────────────────────────── */

int wl_backend_keyboard_key(uint32_t keycode, int pressed) {
    wl_client_t *client = wl_focus_surface ?
        wl_client_of_surface(wl_focus_surface) : NULL;
    if (!client) return 0;
    uint32_t keyboard_id = wl_client_find_id(client, WL_OBJ_KEYBOARD, NULL);
    if (!keyboard_id) return 0;

    /* wl_keyboard.key(time, key, state)
     * key 是 evdev 键码（不是 ascii），Linux evdev 的键码 = X 键码 - 8，
     * HBOS 的 gui 层给的本来就是 evdev 码，直接透传。 */
    wl_event_begin(client, keyboard_id, 3 /* key */);
    wl_event_u32(client, client->serial++);
    wl_event_u32(client, 0);              /* time（毫秒，暂时给 0） */
    wl_event_u32(client, keycode);
    wl_event_u32(client, pressed ? 1 : 0); /* WL_KEYBOARD_KEY_STATE_* */
    wl_event_end(client);
    wl_client_flush(client);
    return 1;
}

int wl_backend_keyboard_modifiers(uint32_t depressed, uint32_t latched,
                                  uint32_t locked, uint32_t group) {
    wl_client_t *client = wl_focus_surface ?
        wl_client_of_surface(wl_focus_surface) : NULL;
    if (!client) return 0;
    uint32_t keyboard_id = wl_client_find_id(client, WL_OBJ_KEYBOARD, NULL);
    if (!keyboard_id) return 0;
    wl_event_begin(client, keyboard_id, 4 /* modifiers */);
    wl_event_u32(client, client->serial++);
    wl_event_u32(client, depressed);
    wl_event_u32(client, latched);
    wl_event_u32(client, locked);
    wl_event_u32(client, group);
    wl_event_end(client);
    wl_client_flush(client);
    return 1;
}
