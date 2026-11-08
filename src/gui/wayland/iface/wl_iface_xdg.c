/**
 * @file    wl_iface_xdg.c
 * @brief   xdg_wm_base + xdg_surface + xdg_toplevel + xdg_popup
 *
 * xdg_shell 是窗口语义所在：客户端先 get_xdg_surface 把 wl_surface 包装成
 * 「窗口」，再 get_toplevel 声明它是顶层窗。真正的显示要等一轮握手：
 *
 *   get_toplevel → 服务端主动发 xdg_toplevel.configure(w,h,states)
 *                 + xdg_surface.configure(serial)
 *     → 客户端 ack_configure(serial)
 *       → 客户端 attach(buffer) + commit  ← 这时才真正出画面
 *
 * configure 必须由服务端先发。反过来「等客户端先 commit 再发 configure」
 * 会让 Chromium 这类 toolkit 与服务端互等：它拿不到 configure 就不
 * commit，服务端等不到 commit 就不发 configure，窗口永远建不起来。
 */

#include "wl_iface.h"
#include "../../../string.h"

/** 发一轮 configure。width/height 为 0 表示"你自己定"。 */
void wl_surface_send_configure(wl_client_t *client,
                               wl_surface_state_t *surface) {
    if (!client || !surface) return;

    uint32_t serial = client->serial++;
    surface->configured_serial = serial;
    /* 「已发过 configure」这个状态只该有一个归属。原来由 commit 分支单独
     * 置位，结果 get_toplevel 主动发 configure 时没置，客户端第一次 commit
     * 又会收到第二份 configure —— 同一个 serial 被 ack 两次会让 toolkit
     * 迷糊。统一在这里收口。 */
    surface->configured = 1;
    surface->pending_configure = 0;

    /* xdg_toplevel.configure(width, height, states)
     * states 是 array：uint32 字节数 + 内容（0 个元素就是 0）。 */
    uint32_t toplevel_id =
        wl_client_find_id(client, WL_OBJ_XDG_TOPLEVEL, surface);
    if (toplevel_id) {
        wl_event_begin(client, toplevel_id, XDG_TOPLEVEL_E_CONFIGURE);
        wl_event_i32(client, surface->width);
        wl_event_i32(client, surface->height);
        wl_event_u32(client, 0);   /* 空 states */
        wl_event_end(client);
    }

    /* xdg_surface.configure(serial)：客户端拿它去 ack。 */
    uint32_t xdg_surface_id =
        wl_client_find_id(client, WL_OBJ_XDG_SURFACE, surface);
    if (xdg_surface_id) {
        wl_event_begin(client, xdg_surface_id, XDG_SURFACE_E_CONFIGURE);
        wl_event_u32(client, serial);
        wl_event_end(client);
    }
}

void wl_xdg_wm_base_request(wl_client_t *client, wl_object_t *object,
                            uint16_t opcode, wl_args_t *args) {
    (void)object;
    switch (opcode) {
    case XDG_WM_BASE_GET_XDG_SURFACE: {
        uint32_t xdg_surface_id = wl_arg_u32(args);
        uint32_t surface_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !xdg_surface_id) return;
        wl_object_t *surface = wl_client_object(client, surface_id);
        if (!surface || surface->kind != WL_OBJ_SURFACE) return;
        /* xdg_surface 和 wl_surface 共享同一份状态：标题、尺寸、谁 attach
         * 了哪块 buffer 都在一处，避免两份状态不同步。 */
        wl_client_adopt_id(client, xdg_surface_id, WL_OBJ_XDG_SURFACE,
                           surface->data);
        break;
    }
    case XDG_WM_BASE_CREATE_POSITIONER: {
        uint32_t positioner_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !positioner_id) return;
        /* 定位器：MVP 阶段当空对象存着，set_* 全部忽略。 */
        wl_client_adopt_id(client, positioner_id, WL_OBJ_FREE, NULL);
        break;
    }
    case XDG_WM_BASE_PONG:
        (void)wl_arg_u32(args);
        break;
    case XDG_WM_BASE_DESTROY:
        object->kind = WL_OBJ_FREE;
        object->id = 0;
        break;
    default:
        break;
    }
}

void wl_xdg_surface_request(wl_client_t *client, wl_object_t *object,
                            uint16_t opcode, wl_args_t *args) {
    wl_surface_state_t *surface = (wl_surface_state_t *)object->data;
    switch (opcode) {
    case XDG_SURFACE_GET_TOPLEVEL: {
        uint32_t toplevel_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !toplevel_id) return;
        wl_client_adopt_id(client, toplevel_id, WL_OBJ_XDG_TOPLEVEL, surface);
        /* configure 不能拖到第一次 commit 才发。
         *
         * xdg-shell 的握手方向是「服务端 configure → 客户端 ack_configure
         * → 客户端 attach(buffer) + commit」。旧实现把 configure 塞在
         * WL_SURFACE_COMMIT 的 !configured 分支里，而 Chromium 在拿到
         * configure 之前根本不会 commit —— 两边互等，实测表现就是
         * create_surface 之后再无任何请求、永远没有 attach/commit。
         *
         * 尺寸给具体值而不是 0x0：协议里 0x0 表示「客户端自定」，
         * Chromium 会先建一个无缓冲的初始 toplevel，首帧迟迟不出；
         * 给 800x600 它直接按这个尺寸建 shm buffer 并 attach。 */
        if (surface) {
            if (surface->width <= 0)  surface->width  = 800;
            if (surface->height <= 0) surface->height = 600;
            wl_surface_send_configure(client, surface);
        }
        break;
    }
    case XDG_SURFACE_GET_POPUP: {
        uint32_t popup_id = wl_arg_u32(args);
        (void)wl_arg_u32(args);   /* parent */
        (void)wl_arg_u32(args);   /* positioner */
        if (wl_args_failed(args) || !popup_id) return;
        wl_client_adopt_id(client, popup_id, WL_OBJ_XDG_POPUP, surface);
        break;
    }
    case XDG_SURFACE_ACK_CONFIGURE: {
        uint32_t serial = wl_arg_u32(args);
        if (surface) surface->configured_serial = serial;
        break;
    }
    case XDG_SURFACE_SET_WINDOW_GEOMETRY:
        wl_arg_skip_u32(args, 4);
        break;
    case XDG_SURFACE_DESTROY:
        object->kind = WL_OBJ_FREE;
        object->id = 0;
        object->data = NULL;
        break;
    default:
        break;
    }
}

void wl_xdg_toplevel_request(wl_client_t *client, wl_object_t *object,
                             uint16_t opcode, wl_args_t *args) {
    (void)client;
    wl_surface_state_t *surface = (wl_surface_state_t *)object->data;
    switch (opcode) {
    case XDG_TOPLEVEL_SET_TITLE: {
        char text[WL_TITLE_MAX];
        if (!wl_arg_string(args, text, sizeof(text))) return;
        if (!surface) return;
        size_t i = 0;
        while (text[i] && i + 1 < sizeof(surface->title)) {
            surface->title[i] = text[i];
            i++;
        }
        surface->title[i] = '\0';
        break;
    }
    case XDG_TOPLEVEL_SET_APP_ID: {
        char text[64];
        if (!wl_arg_string(args, text, sizeof(text))) return;
        if (!surface) return;
        size_t i = 0;
        while (text[i] && i + 1 < sizeof(surface->app_id)) {
            surface->app_id[i] = text[i];
            i++;
        }
        surface->app_id[i] = '\0';
        break;
    }
    case XDG_TOPLEVEL_DESTROY:
        object->kind = WL_OBJ_FREE;
        object->id = 0;
        object->data = NULL;
        break;
    case XDG_TOPLEVEL_SET_MIN_SIZE:
    case XDG_TOPLEVEL_SET_MAX_SIZE:
        wl_arg_skip_u32(args, 2);
        break;
    case XDG_TOPLEVEL_MOVE:
    case XDG_TOPLEVEL_RESIZE:
        wl_arg_skip_u32(args, 3);   /* seat, serial, edges */
        break;
    case XDG_TOPLEVEL_SHOW_WINDOW_MENU:
        wl_arg_skip_u32(args, 2);
        break;
    case XDG_TOPLEVEL_SET_PARENT:
        (void)wl_arg_u32(args);
        break;
    case XDG_TOPLEVEL_SET_MAXIMIZED:
    case XDG_TOPLEVEL_UNSET_MAXIMIZED:
    case XDG_TOPLEVEL_SET_FULLSCREEN:
    case XDG_TOPLEVEL_UNSET_FULLSCREEN:
    case XDG_TOPLEVEL_SET_MINIMIZED:
        /* 窗口状态由 HIVE 的窗口管理器决定最终几何，这里只标一下
         * 下次 commit 要重发 configure。 */
        if (surface) surface->pending_configure = 1;
        break;
    default:
        break;
    }
}

void wl_xdg_popup_request(wl_client_t *client, wl_object_t *object,
                          uint16_t opcode, wl_args_t *args) {
    (void)client;
    (void)args;
    if (opcode == 0 /* destroy */) {
        object->kind = WL_OBJ_FREE;
        object->id = 0;
        object->data = NULL;
    }
}
