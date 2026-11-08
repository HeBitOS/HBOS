/**
 * @file    wl_iface_shm.c
 * @brief   wl_shm + wl_shm_pool + wl_buffer
 *
 * 像素通路的关键在于 fd：客户端 create_pool(fd, size) 把 memfd 交给我们，
 * 之后 create_buffer(buffer_id, offset, w, h, stride, format) 只是在这个
 * pool 里划一块区域，不复制数据。真正要画的时候由 backend 按需从 memfd
 * 读（见 wl_buffer_read）。
 *
 * fd 通过 SCM_RIGHTS 附着在请求消息上，分派层先把它们收进 pending_fds，
 * 这里按顺序取走 —— 一条消息带多个 fd 时顺序就是参数里的索引顺序。
 */

#include "wl_iface.h"
#include "../../../linux_compat.h"

void wl_shm_request(wl_client_t *client, wl_object_t *object,
                    uint16_t opcode, wl_args_t *args) {
    (void)object;
    if (opcode != WL_SHM_CREATE_POOL) return;
    uint32_t pool_id = wl_arg_u32(args);
    /* create_pool 的签名是 "nhi"：new_id、fd 索引、size。fd 本身走
     * SCM_RIGHTS，线上那 4 字节只是索引。但实测 Chromium 156 发出来的
     * 消息体只有 8 字节（new_id + size），索引那 4 字节根本不写 —— 于是
     * 严格按 "nhi" 解析会把 size 读成 0 并判定参数越界，create_pool 全部
     * 被丢掉，客户端随后在"永远拿不到 pool"的路径上崩掉。
     * 这里按消息体长度两种形态都收：12 字节走标准 "nhi"，8 字节把第二个
     * 字当 size，fd 从 pending_fds 里按顺序取。 */
    uint32_t size;
    if (args->length >= 12) {
        (void)wl_arg_u32(args);              /* fd 索引 */
        size = wl_arg_u32(args);
    } else {
        size = wl_arg_u32(args);
    }
    wl_dbg_note("shm.create_pool", (int)size, (int)args->length,
                (int)args->offset);
    if (wl_args_failed(args) || !pool_id || !size) {
        wl_dbg_note("pool.args_rejected", (int)pool_id, (int)size,
                    (int)wl_args_failed(args));
        return;
    }

    if (!wl_client_adopt_id(client, pool_id, WL_OBJ_SHM_POOL, NULL)) {
        wl_dbg_note("pool.adopt_failed", (int)pool_id, 0, 0);
        return;
    }
    wl_shm_pool_state_t *pool = wl_pool_alloc();
    if (!pool) {
        wl_dbg_note("pool.alloc_failed", (int)pool_id, 0, 0);
        wl_client_destroy(client, pool_id);
        return;
    }

    int pool_fd = -1;
    if (client->pending_fd_count > 0) {
        pool_fd = client->pending_fds[0];
        for (int i = 1; i < client->pending_fd_count; i++)
            client->pending_fds[i - 1] = client->pending_fds[i];
        client->pending_fd_count--;
    }
    if (pool_fd < 0) {
        /* 没拿到 fd：pool 建了也没用，直接回收。 */
        wl_dbg_note("pool.no_fd", (int)pool_id, (int)size, 0);
        wl_pool_free(pool);
        wl_client_destroy(client, pool_id);
        return;
    }
    wl_dbg_note("pool.ok", (int)pool_id, (int)pool_fd, (int)size);
    pool->fd = pool_fd;
    pool->size = size;

    wl_object_t *pool_object = wl_client_object(client, pool_id);
    if (pool_object) pool_object->data = pool;
}

void wl_shm_pool_request(wl_client_t *client, wl_object_t *object,
                         uint16_t opcode, wl_args_t *args) {
    wl_shm_pool_state_t *pool = (wl_shm_pool_state_t *)object->data;
    if (!pool) return;

    switch (opcode) {
    case WL_SHM_POOL_CREATE_BUFFER: {
        uint32_t buffer_id = wl_arg_u32(args);
        int32_t offset = wl_arg_i32(args);
        int32_t width = wl_arg_i32(args);
        int32_t height = wl_arg_i32(args);
        int32_t stride = wl_arg_i32(args);
        uint32_t format = wl_arg_u32(args);
        if (wl_args_failed(args) || !buffer_id) return;
        if (offset < 0 || width <= 0 || height <= 0 || stride <= 0) return;
        if ((uint64_t)offset + (uint64_t)stride * (uint64_t)height >
            (uint64_t)pool->size)
            return;

        if (!wl_client_adopt_id(client, buffer_id, WL_OBJ_BUFFER, NULL)) return;
        wl_shm_buffer_t *buffer = wl_buffer_alloc();
        if (!buffer) {
            wl_client_destroy(client, buffer_id);
            return;
        }
        buffer->width = width;
        buffer->height = height;
        buffer->stride = stride;
        buffer->format = (int)format;
        buffer->pool_fd = pool->fd;
        buffer->pool_offset = (uint32_t)offset;
        buffer->size = (uint32_t)((uint32_t)stride * (uint32_t)height);
        buffer->busy = 0;

        wl_object_t *buffer_object = wl_client_object(client, buffer_id);
        if (buffer_object) buffer_object->data = buffer;
        break;
    }
    case WL_SHM_POOL_DESTROY: {
        uint32_t id = object->id;
        /* 注意：pool 销毁不代表 buffer 失效 —— 客户端可能已经把 buffer
         * 交给 compositor 用了。fd 由 buffer 自己 retains 一份引用，
         * 这里只是不再接新的 create_buffer。 */
        if (pool->fd >= 0) (void)linux_compat_close(pool->fd);
        wl_pool_free(pool);
        wl_client_destroy(client, id);
        object->data = NULL;
        break;
    }
    case WL_SHM_POOL_RESIZE: {
        uint32_t size = wl_arg_u32(args);
        if (wl_args_failed(args)) return;
        pool->size = size;
        break;
    }
    default:
        break;
    }
}

void wl_buffer_request(wl_client_t *client, wl_object_t *object,
                       uint16_t opcode, wl_args_t *args) {
    (void)client;
    (void)args;
    if (opcode != WL_BUFFER_DESTROY) return;
    wl_shm_buffer_t *buffer = (wl_shm_buffer_t *)object->data;
    if (buffer && !buffer->busy) wl_buffer_free(buffer);
    object->data = NULL;
    object->kind = WL_OBJ_FREE;
    object->id = 0;
}
