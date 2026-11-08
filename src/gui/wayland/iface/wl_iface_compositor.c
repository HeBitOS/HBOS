/**
 * @file    wl_iface_compositor.c
 * @brief   wl_compositor + wl_surface + wl_region
 *
 * surface 的 commit 是整条链路的枢轴：
 *   attach(buffer) → damage(rect)* → commit()
 *
 * commit 时做三件事：
 *   1. 把 attached 提升为 current，旧 buffer 发 release 还给客户端；
 *   2. 标记 damaged，让 backend 下一轮把这块像素合成上屏；
 *   3. configured 兜底 —— 正常的 configure 已在 xdg get_toplevel 时
 *      主动发过（见 wl_iface_xdg.c），这里只补没走 xdg 的纯 surface。
 *      「等 commit 才发 configure」会与 toolkit 形成互等死锁。
 */

#include "wl_iface.h"
#include "../backend/wl_backend.h"

void wl_compositor_request(wl_client_t *client, wl_object_t *object,
                           uint16_t opcode, wl_args_t *args) {
    (void)object;
    switch (opcode) {
    case WL_COMPOSITOR_CREATE_SURFACE: {
        uint32_t surface_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !surface_id) {
            wl_dbg_note("surf.args_rejected", (int)surface_id, 0, 0);
            return;
        }
        wl_surface_state_t *state = wl_surface_alloc();
        if (!state) {
            wl_dbg_note("surf.alloc_failed", (int)surface_id, 0, 0);
            return;
        }
        if (!wl_client_adopt_id(client, surface_id, WL_OBJ_SURFACE, state)) {
            wl_dbg_note("surf.adopt_failed", (int)surface_id, 0, 0);
            wl_surface_free(state);
            return;
        }
        wl_dbg_note("surf.ok", (int)surface_id, 0, 0);
        break;
    }
    case WL_COMPOSITOR_CREATE_REGION: {
        uint32_t region_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !region_id) return;
        /* region 用 NULL data 占位即可：wl_region_request 只认 destroy。 */
        wl_client_adopt_id(client, region_id, WL_OBJ_REGION, NULL);
        break;
    }
    default:
        break;
    }
}

void wl_region_request(wl_client_t *client, wl_object_t *object,
                       uint16_t opcode, wl_args_t *args) {
    (void)client;
    (void)args;
    /* wl_region 只影响裁剪，MVP 阶段整块贴图用不到；但 destroy 要认，
     * 否则客户端反复建/删 region 会把对象表塞满。 */
    if (opcode == 0 /* destroy */) {
        object->kind = WL_OBJ_FREE;
        object->id = 0;
        object->data = NULL;
    }
}

void wl_surface_request(wl_client_t *client, wl_object_t *object,
                        uint16_t opcode, wl_args_t *args) {
    wl_surface_state_t *surface = (wl_surface_state_t *)object->data;
    if (!surface) return;

    switch (opcode) {
    case WL_SURFACE_ATTACH: {
        uint32_t buffer_id = wl_arg_u32(args);
        (void)wl_arg_i32(args);  /* x, y：Wayland 已废弃，忽略 */
        (void)wl_arg_i32(args);
        if (wl_args_failed(args)) return;
        if (!buffer_id) {
            /* attach(NULL) = 解除映射，窗口隐藏。 */
            surface->attached = NULL;
            surface->width = 0;
            surface->height = 0;
            return;
        }
        wl_object_t *buffer_object = wl_client_object(client, buffer_id);
        if (!buffer_object || buffer_object->kind != WL_OBJ_BUFFER) return;
        surface->attached = (wl_shm_buffer_t *)buffer_object->data;
        if (surface->attached) {
            surface->width = surface->attached->width;
            surface->height = surface->attached->height;
        }
        break;
    }
    case WL_SURFACE_DAMAGE:
    case WL_SURFACE_DAMAGE_BUFFER: {
        int32_t x = wl_arg_i32(args);
        int32_t y = wl_arg_i32(args);
        int32_t w = wl_arg_i32(args);
        int32_t h = wl_arg_i32(args);
        if (wl_args_failed(args)) return;
        /* 只记包围盒：backend 是整块贴图，精确裁剪收益很小。 */
        if (!surface->damaged) {
            surface->damage_x = x;
            surface->damage_y = y;
            surface->damage_w = w;
            surface->damage_h = h;
        } else {
            int32_t right = surface->damage_x + surface->damage_w;
            int32_t bottom = surface->damage_y + surface->damage_h;
            if (x < surface->damage_x) surface->damage_x = x;
            if (y < surface->damage_y) surface->damage_y = y;
            if (x + w > right) right = x + w;
            if (y + h > bottom) bottom = y + h;
            surface->damage_w = right - surface->damage_x;
            surface->damage_h = bottom - surface->damage_y;
        }
        surface->damaged = 1;
        break;
    }
    case WL_SURFACE_FRAME: {
        uint32_t callback_id = wl_arg_u32(args);
        if (wl_args_failed(args) || !callback_id) return;
        if (!wl_client_adopt_id(client, callback_id, WL_OBJ_CALLBACK, NULL))
            return;
        /* 帧回调立刻 done：没有 vsync 源，等下一轮合成反而会让客户端
         * 在最慢的一帧上卡住。由 backend 决定什么时候真正呈现。 */
        wl_event_begin(client, callback_id, WL_CALLBACK_E_DONE);
        wl_event_u32(client, client->serial++);
        wl_event_end(client);
        break;
    }
    case WL_SURFACE_COMMIT: {
        if (wl_args_failed(args)) return;
        surface->client = client;

        if (surface->attached) {
            if (surface->current && surface->current != surface->attached) {
                /* 旧 buffer 还给客户端：它收到 release 才能复用那块内存。 */
                uint32_t old_id =
                    wl_client_find_id(client, WL_OBJ_BUFFER, surface->current);
                if (old_id) {
                    wl_event_begin(client, old_id, WL_BUFFER_E_RELEASE);
                    wl_event_end(client);
                }
                surface->current->busy = 0;
            }
            surface->current = surface->attached;
            surface->current->busy = 1;
            surface->attached = NULL;
            surface->mapped = 1;
        }
        surface->commit_serial = client->serial++;

        /* 兜底：客户端没走 xdg_toplevel（例如纯 wl_surface 的光标表面）时
         * 也别让 configured 永远为 0；实际 configure 已在 get_toplevel
         * 里主动发过，这里只在状态位要求时补一次。 */
        if (!surface->configured || surface->pending_configure)
            wl_surface_send_configure(client, surface);

        /* 交给 backend：建窗口 / 贴图 / 请求重绘。 */
        wl_backend_surface_committed(client, surface);
        break;
    }
    case WL_SURFACE_DESTROY: {
        uint32_t id = object->id;
        wl_backend_surface_destroyed(client, surface);
        wl_client_destroy(client, id);
        wl_surface_free(surface);
        object->data = NULL;
        break;
    }
    default:
        /* set_buffer_scale / set_buffer_transform / set_opaque_region /
         * set_input_region：对整块 blit 的合成器没有影响。 */
        break;
    }
}
