/**
 * @file    wl_iface_display.c
 * @brief   wl_display + wl_registry
 *
 * 这两个接口是整个协议的入口：客户端连上之后第一件事就是
 * get_registry 拿到全局对象列表，再按名字 bind 出自己需要的东西。
 */

#include "wl_iface.h"
#include "../../../string.h"

/* 屏幕尺寸：wl_output 的 mode 事件要报真实分辨率。GUI 没起来时用兜底值，
 * 免得客户端拿到 0x0 的窗口尺寸。 */
extern int gui_surface_width(void);
extern int gui_surface_height(void);

#define WL_OUTPUT_FALLBACK_W 1024
#define WL_OUTPUT_FALLBACK_H 768

/** bind wl_output 之后立刻把屏幕信息发一遍（geometry → mode → scale → done）。 */
static void wl_output_send_info(wl_client_t *client, uint32_t output_id) {
    int width = gui_surface_width();
    int height = gui_surface_height();
    if (width <= 0) width = WL_OUTPUT_FALLBACK_W;
    if (height <= 0) height = WL_OUTPUT_FALLBACK_H;

    /* geometry(x, y, phys_w, phys_h, subpixel, make, model, transform) */
    wl_event_begin(client, output_id, WL_OUTPUT_E_GEOMETRY);
    wl_event_i32(client, 0);                          /* x */
    wl_event_i32(client, 0);                          /* y */
    wl_event_i32(client, width * 264 / 1000);         /* 物理宽 mm，按 ~96dpi 估 */
    wl_event_i32(client, height * 264 / 1000);
    wl_event_u32(client, WL_OUTPUT_SUBPIXEL_UNKNOWN);
    wl_event_string(client, "HBOS");
    wl_event_string(client, "Virtual Display");
    wl_event_u32(client, WL_OUTPUT_TRANSFORM_NORMAL);
    wl_event_end(client);

    /* mode(flags, width, height, refresh_mhz)：CURRENT|PREFERRED，60Hz */
    wl_event_begin(client, output_id, WL_OUTPUT_E_MODE);
    wl_event_u32(client, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED);
    wl_event_i32(client, width);
    wl_event_i32(client, height);
    wl_event_i32(client, 60000);
    wl_event_end(client);

    /* scale(factor)：1 = 无缩放 */
    wl_event_begin(client, output_id, WL_OUTPUT_E_SCALE);
    wl_event_i32(client, 1);
    wl_event_end(client);

    /* done()：告诉客户端"这一批属性发完了，可以用" */
    wl_event_begin(client, output_id, WL_OUTPUT_E_DONE);
    wl_event_end(client);
}

void wl_display_request(wl_client_t *client, wl_object_t *object,
                        uint16_t opcode, wl_args_t *args) {
    (void)object;
    switch (opcode) {
    case WL_DISPLAY_SYNC: {
        /* sync(new_id callback)：立刻回 done。客户端靠它判断"服务端已经
         * 处理完我前面发的一切"，所以必须排在前面所有事件之后。 */
        uint32_t callback_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !callback_id) return;
        if (!wl_client_adopt_id(client, callback_id, WL_OBJ_CALLBACK, NULL))
            return;
        wl_event_begin(client, callback_id, WL_CALLBACK_E_DONE);
        wl_event_u32(client, client->serial++);
        wl_event_end(client);
        break;
    }
    case WL_DISPLAY_GET_REGISTRY: {
        uint32_t registry_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !registry_id) return;
        if (!wl_client_adopt_id(client, registry_id, WL_OBJ_REGISTRY, NULL))
            return;
        int count = 0;
        const wl_global_t *globals = wl_server_global_list(&count);
        for (int i = 0; i < count; i++) {
            wl_event_begin(client, registry_id, WL_REGISTRY_E_GLOBAL);
            wl_event_u32(client, globals[i].name);
            wl_event_string(client, globals[i].interface);
            wl_event_u32(client, globals[i].version);
            wl_event_end(client);
        }
        break;
    }
    default:
        break;
    }
}

void wl_registry_request(wl_client_t *client, wl_object_t *object,
                         uint16_t opcode, wl_args_t *args) {
    (void)object;
    if (opcode != WL_REGISTRY_BIND) return;
    uint32_t name = wl_arg_u32(args);
    char interface[64];
    if (!wl_arg_string(args, interface, sizeof(interface))) return;
    uint32_t version = wl_arg_u32(args);
    uint32_t new_id = wl_arg_u32(args);
    if (wl_args_failed(args) || !new_id) return;
    wl_dbg_bind(interface, name, version, new_id);

    wl_object_t *bound = wl_client_adopt_id(client, new_id, WL_OBJ_FREE, NULL);
    if (!bound) return;
    bound->global_name = name;
    bound->version = version;

    if (strcmp(interface, WL_IFACE_COMPOSITOR) == 0) {
        bound->kind = WL_OBJ_COMPOSITOR;
    } else if (strcmp(interface, WL_IFACE_SHM) == 0) {
        bound->kind = WL_OBJ_SHM;
        /* 声明支持的像素格式。只报这两种 —— 报了却不支持会导致花屏。 */
        wl_event_begin(client, new_id, WL_SHM_E_FORMAT);
        wl_event_u32(client, WL_SHM_FORMAT_ARGB8888);
        wl_event_end(client);
        wl_event_begin(client, new_id, WL_SHM_E_FORMAT);
        wl_event_u32(client, WL_SHM_FORMAT_XRGB8888);
        wl_event_end(client);
    } else if (strcmp(interface, WL_IFACE_SEAT) == 0) {
        bound->kind = WL_OBJ_SEAT;
        wl_event_begin(client, new_id, WL_SEAT_E_CAPABILITIES);
        wl_event_u32(client, WL_SEAT_CAP_POINTER | WL_SEAT_CAP_KEYBOARD);
        wl_event_end(client);
        wl_event_begin(client, new_id, WL_SEAT_E_NAME);
        wl_event_string(client, "seat0");
        wl_event_end(client);
    } else if (strcmp(interface, WL_IFACE_XDG_WM_BASE) == 0) {
        bound->kind = WL_OBJ_XDG_WM_BASE;
        /* xdg-shell 的心跳：服务端 ping，客户端必须 pong。Chromium 的
         * WaylandXdgShell 在 bind 时就装好 ping listener，第一轮
         * roundtrip 会把 pong 一起带回；缺 ping 时它仍会把这条握手算作
         * 未完成，窗口创建流程就不再往前走。 */
        wl_event_begin(client, new_id, XDG_WM_BASE_E_PING);
        wl_event_u32(client, client->serial++);
        wl_event_end(client);
    } else if (strcmp(interface, WL_IFACE_OUTPUT) == 0) {
        bound->kind = WL_OBJ_OUTPUT;
        /* 屏幕信息在 bind 的那一刻就发：客户端拿它算窗口大小，
         * 晚发一步它就会在初始化里干等。 */
        wl_output_send_info(client, new_id);
    } else if (strcmp(interface, WL_IFACE_DATA_DEVICE_MANAGER) == 0) {
        bound->kind = WL_OBJ_DATA_DEVICE_MANAGER;
    } else if (strcmp(interface, WL_IFACE_SUBCOMPOSITOR) == 0) {
        bound->kind = WL_OBJ_SUBCOMPOSITOR;
    } else {
        /* 不认识的接口：把 id 还回去，让客户端知道 bind 失败。 */
        bound->kind = WL_OBJ_FREE;
        bound->id = 0;
    }
}
