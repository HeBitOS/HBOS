/**
 * @file    wl_iface_state.c
 * @brief   iface 层共享的对象池与像素读取
 *
 * 所有 surface / buffer / pool 的生命周期都跟连接走，数量上限很清楚，
 * 所以用静态池而不是 kmalloc —— 避免碎片，也让"用完没还"这类错误能
 * 通过计数看出来。
 */

#include "wl_iface.h"

#include "../../../linux_compat.h"
#include "../../../string.h"

static wl_surface_state_t wl_surfaces[WL_MAX_SURFACES];
static wl_shm_buffer_t wl_buffers[WL_MAX_BUFFERS];
static wl_shm_pool_state_t wl_pools[WL_MAX_POOLS];

void wl_iface_init(void) {
    memset(wl_surfaces, 0, sizeof(wl_surfaces));
    memset(wl_buffers, 0, sizeof(wl_buffers));
    memset(wl_pools, 0, sizeof(wl_pools));
    for (int i = 0; i < WL_MAX_POOLS; i++) wl_pools[i].fd = -1;
}

wl_surface_state_t *wl_surface_alloc(void) {
    for (int i = 0; i < WL_MAX_SURFACES; i++) {
        if (wl_surfaces[i].used) continue;
        memset(&wl_surfaces[i], 0, sizeof(wl_surfaces[i]));
        wl_surfaces[i].used = 1;
        return &wl_surfaces[i];
    }
    return NULL;
}

void wl_surface_free(wl_surface_state_t *surface) {
    if (!surface) return;
    memset(surface, 0, sizeof(*surface));
}

wl_shm_buffer_t *wl_buffer_alloc(void) {
    for (int i = 0; i < WL_MAX_BUFFERS; i++) {
        if (wl_buffers[i].used) continue;
        memset(&wl_buffers[i], 0, sizeof(wl_buffers[i]));
        wl_buffers[i].used = 1;
        wl_buffers[i].pool_fd = -1;
        return &wl_buffers[i];
    }
    return NULL;
}

void wl_buffer_free(wl_shm_buffer_t *buffer) {
    if (!buffer) return;
    memset(buffer, 0, sizeof(*buffer));
    buffer->pool_fd = -1;
}

wl_shm_pool_state_t *wl_pool_alloc(void) {
    for (int i = 0; i < WL_MAX_POOLS; i++) {
        if (wl_pools[i].used) continue;
        memset(&wl_pools[i], 0, sizeof(wl_pools[i]));
        wl_pools[i].used = 1;
        wl_pools[i].fd = -1;
        return &wl_pools[i];
    }
    return NULL;
}

void wl_pool_free(wl_shm_pool_state_t *pool) {
    if (!pool) return;
    memset(pool, 0, sizeof(*pool));
    pool->fd = -1;
}

int wl_iface_surface_count(void) {
    int count = 0;
    for (int i = 0; i < WL_MAX_SURFACES; i++)
        if (wl_surfaces[i].used) count++;
    return count;
}

wl_surface_state_t *wl_iface_surface_at(int index) {
    if (index < 0 || index >= WL_MAX_SURFACES) return NULL;
    return wl_surfaces[index].used ? &wl_surfaces[index] : NULL;
}

/**
 * 把 buffer 的一段矩形按行读出来。
 *
 * 像素本身不在内核里 —— 客户端往自己的 mmap 里写，我们靠
 * linux_compat_memfd_read_at() 从 memfd 的物理页里按页边界分段取。
 * 这是"每帧拷贝一次"，比直接拿物理页慢一点，但不需要给 memfd 建立
 * 内核映射，也不会因为客户端 unmap 而出悬空指针。
 */
int wl_buffer_read(const wl_shm_buffer_t *buffer, void *dst,
                   int dst_stride, int x, int y, int w, int h) {
    if (!buffer || !buffer->used || !dst) return -1;
    if (buffer->pool_fd < 0) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return -1;
    if (x + w > buffer->width || y + h > buffer->height) return -1;

    const int bytes_per_pixel = 4;

    /* 快速路径：整宽、行距和目标一致时，整块就是一段连续内存 ——
     * 一次 read_at 就够，不用按行切。
     *
     * 合成器实际就是这么调的（dst_stride == buffer->stride，x=0，
     * w=width），所以这条路径每帧都命中。原来逐行调用意味着 1080 次
     * 函数调用 + 每次重新算页边界，纯属白费。 */
    if (x == 0 && w == buffer->width && dst_stride == buffer->stride) {
        uint64_t offset = buffer->pool_offset +
                          (uint64_t)y * (uint64_t)buffer->stride;
        uint64_t length = (uint64_t)buffer->stride * (uint64_t)h;
        if (offset + length > buffer->pool_offset + buffer->size) return -1;
        return linux_compat_memfd_read_at(buffer->pool_fd, offset, dst,
                                          (size_t)length);
    }

    uint8_t *out = (uint8_t *)dst;
    for (int row = 0; row < h; row++) {
        uint64_t offset = buffer->pool_offset +
                          (uint64_t)(y + row) * (uint64_t)buffer->stride +
                          (uint64_t)x * (uint64_t)bytes_per_pixel;
        uint64_t length = (uint64_t)w * (uint64_t)bytes_per_pixel;
        if (offset + length > buffer->pool_offset + buffer->size) return -1;
        int result = linux_compat_memfd_read_at(
            buffer->pool_fd, offset, out + (size_t)row * (size_t)dst_stride,
            (size_t)length);
        if (result < 0) return -1;
    }
    return 0;
}
