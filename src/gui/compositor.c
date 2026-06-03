/**
 * @file compositor.c
 * @brief 窗口合成器实现
 *
 * 提供离屏渲染 + 双缓冲 + 脏区域跟踪的简单合成器。
 * 窗口绘制到离屏缓冲区，仅脏区域被刷新到帧缓冲。
 */

#include "gui/compositor.h"
#include "gpu.h"
#include "string.h"
#include "core/heap.h"

/* ================================================================
 * compositor_init — 分配离屏缓冲区并初始化
 * ================================================================ */

int compositor_init(compositor_t *c) {
    if (!c) return -1;

    gpu_device_t *gpu = gpu_primary();
    if (!gpu) return -1;

    memset(c, 0, sizeof(*c));

    c->width  = gpu->fb_width;
    c->height = gpu->fb_height;
    c->pitch  = gpu->fb_width;  /* 离屏缓冲每行像素数 = 宽度 */

    uint64_t buf_size = c->width * c->height * sizeof(uint32_t);

    c->buf[0] = (uint32_t *)kmalloc(buf_size);
    c->buf[1] = (uint32_t *)kmalloc(buf_size);

    if (!c->buf[0] || !c->buf[1]) {
        if (c->buf[0]) kfree(c->buf[0]);
        if (c->buf[1]) kfree(c->buf[1]);
        memset(c, 0, sizeof(*c));
        return -1;
    }

    /* 用黑色填充两个缓冲区 */
    memset(c->buf[0], 0, buf_size);
    memset(c->buf[1], 0, buf_size);

    c->buf_front   = 0;
    c->dirty_count = 0;
    c->frame_count = 0;

    return 0;
}

/* ================================================================
 * compositor_begin_frame — 开始新帧，清除脏区域
 * ================================================================ */

void compositor_begin_frame(compositor_t *c) {
    if (!c) return;
    c->dirty_count = 0;
}

/* ================================================================
 * compositor_end_frame — 结束帧，刷新脏区域并交换缓冲
 * ================================================================ */

void compositor_end_frame(compositor_t *c) {
    if (!c || !c->buf[0]) return;

    /* 交换双缓冲：前缓冲变为后缓冲，后缓冲变为前缓冲 */
    int back = c->buf_front ^ 1;
    c->buf_front = back;

    /* 将脏区域从（新的）前缓冲刷新到帧缓冲 */
    uint32_t *front = c->buf[back];
    for (int i = 0; i < c->dirty_count; i++) {
        compositor_dirty_t *d = &c->dirty[i];
        gpu_bitblt(front + d->y * c->pitch + d->x,
                   c->pitch, d->x, d->y, d->w, d->h);
    }

    /* 将当前前缓冲内容复制到后缓冲（保持两个缓冲同步） */
    if (c->dirty_count > 0) {
        uint32_t *back_buf = c->buf[back ^ 1];
        for (int i = 0; i < c->dirty_count; i++) {
            compositor_dirty_t *d = &c->dirty[i];
            for (uint64_t yy = 0; yy < d->h; yy++) {
                uint32_t *src = front + (d->y + yy) * c->pitch + d->x;
                uint32_t *dst = back_buf + (d->y + yy) * c->pitch + d->x;
                for (uint64_t xx = 0; xx < d->w; xx++) dst[xx] = src[xx];
            }
        }
    }

    c->frame_count++;
}

/* ================================================================
 * compositor_damage_rect — 标记脏区域
 * ================================================================ */

void compositor_damage_rect(compositor_t *c, uint64_t x, uint64_t y,
                            uint64_t w, uint64_t h) {
    if (!c || w == 0 || h == 0) return;

    /* 裁剪到帧缓冲范围 */
    if (x >= c->width || y >= c->height) return;
    if (x + w > c->width)  w = c->width - x;
    if (y + h > c->height) h = c->height - y;

    /* 尝试与已有脏区域合并 */
    for (int i = 0; i < c->dirty_count; i++) {
        compositor_dirty_t *d = &c->dirty[i];
        if (x >= d->x && y >= d->y &&
            x + w <= d->x + d->w && y + h <= d->y + d->h) {
            return; /* 已包含在现有脏区域中 */
        }
        /* 简单合并：扩展已有区域 */
        if (x <= d->x + d->w && x + w >= d->x &&
            y <= d->y + d->h && y + h >= d->y) {
            uint64_t nx = (x < d->x) ? x : d->x;
            uint64_t ny = (y < d->y) ? y : d->y;
            uint64_t nr = (x + w > d->x + d->w) ? (x + w) : (d->x + d->w);
            uint64_t nb = (y + h > d->y + d->h) ? (y + h) : (d->y + d->h);
            d->x = nx;
            d->y = ny;
            d->w = nr - nx;
            d->h = nb - ny;
            return;
        }
    }

    /* 添加新的脏区域 */
    if (c->dirty_count < COMPOSITOR_MAX_DIRTY) {
        compositor_dirty_t *d = &c->dirty[c->dirty_count++];
        d->x = x; d->y = y; d->w = w; d->h = h;
    } else {
        /* 超出上限：标记整个屏幕为脏 */
        c->dirty[0].x = 0;
        c->dirty[0].y = 0;
        c->dirty[0].w = c->width;
        c->dirty[0].h = c->height;
        c->dirty_count = 1;
    }
}

/* ================================================================
 * compositor_get_buf — 获取当前前缓冲指针
 * ================================================================ */

uint32_t *compositor_get_buf(compositor_t *c) {
    if (!c) return NULL;
    return c->buf[c->buf_front];
}

/* ================================================================
 * compositor_get_width / compositor_get_height
 * ================================================================ */

uint64_t compositor_get_width(compositor_t *c) {
    return c ? c->width : 0;
}

uint64_t compositor_get_height(compositor_t *c) {
    return c ? c->height : 0;
}