/**
 * @file    wl_iface_seat.c
 * @brief   wl_seat + wl_pointer + wl_keyboard
 *
 * 这一层只处理"客户端对输入设备的请求"（建设备对象、设光标）。
 * 真正的输入事件由 backend 从 HIVE 收键鼠后主动推下去
 * （见 wl_backend_send_pointer_* / wl_backend_send_keyboard_*）。
 */

#include "wl_iface.h"

void wl_seat_request(wl_client_t *client, wl_object_t *object,
                     uint16_t opcode, wl_args_t *args) {
    (void)object;
    switch (opcode) {
    case WL_SEAT_GET_POINTER: {
        uint32_t new_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !new_id) return;
        wl_client_adopt_id(client, new_id, WL_OBJ_POINTER, NULL);
        break;
    }
    case WL_SEAT_GET_KEYBOARD: {
        uint32_t new_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !new_id) return;
        wl_client_adopt_id(client, new_id, WL_OBJ_KEYBOARD, NULL);
        /* 必须紧跟一条 keymap，否则客户端会一直等键位表：Qt Wayland 与
         * Chromium Ozone 都是"收到 keymap 才建映射"，在此之前送来的 key
         * 事件被静默丢弃 —— 表现为窗口正常但键盘完全没反应。
         *
         * format 用 NO_KEYMAP：HBOS 没有 xkbcommon 数据文件，发 xkb_v1
         * 只能是伪造的映射，客户端解析失败会直接断开连接，比不发更糟。
         * fd 传 -1（wl_event_i32 写的是 fd 占位，服务端不发 scm_rights
         * 时客户端按 -1 处理，语义即"没有 keymap 文件"）。 */
        wl_event_begin(client, new_id, WL_KEYBOARD_E_KEYMAP);
        wl_event_u32(client, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP);
        wl_event_i32(client, -1);
        wl_event_u32(client, 0);   /* size：没有 keymap 文件，长度为 0 */
        wl_event_end(client);
        break;
    }
    case WL_SEAT_GET_TOUCH: {
        /* 没有触摸屏：仍然接受 new_id，这样客户端不会因为对象表缺号崩溃，
         * 但它永远收不到 touch 事件。 */
        uint32_t new_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !new_id) return;
        wl_client_adopt_id(client, new_id, WL_OBJ_POINTER, NULL);
        break;
    }
    case WL_SEAT_RELEASE:
        object->kind = WL_OBJ_FREE;
        object->id = 0;
        break;
    default:
        break;
    }
}

void wl_input_device_request(wl_client_t *client, wl_object_t *object,
                             uint16_t opcode, wl_args_t *args) {
    (void)client;
    (void)args;
    /* wl_pointer 只有一个请求 set_cursor(serial, surface, hotspot_x, y)。
     * 我们没有硬件光标平面，忽略即可 —— 客户端会用软件画光标进自己的
     * buffer，效果一样。 */
    if (object->kind == WL_OBJ_POINTER && opcode == 0) return;
    /* wl_keyboard 没有请求。 */
}
