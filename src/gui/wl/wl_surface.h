/**
 * @file    wl_surface.h
 * @brief   Wayland 风格 surface/buffer 模型（① 的地基）
 *
 * 只实现"状态机 + 双缓冲 + 脏区"，不含任何绘制：
 *   attach(buffer) → damage(rect)* → commit()
 * commit 时把 pending 换成 current，并通过回调把 (buffer, damage) 交给合成器。
 *
 * 与现有 gui.c 解耦，因此可以在宿主机上单测；接进 gd_* 只是实现一个
 * commit 监听器的事。
 */
#ifndef HBOS_WL_SURFACE_H
#define HBOS_WL_SURFACE_H

#include <stdint.h>

typedef struct wl_buffer {
    int w, h, stride;
    void *pixels;                          /**< 由调用方管理的内存 */
    void (*release)(struct wl_buffer *b, void *ctx);
    void *release_ctx;
    int busy;                              /**< 1=合成器正在使用 */
} wl_buffer_t;

typedef struct { int x, y, w, h; } wl_rect_t;

#define WL_SURFACE_MAX_DAMAGE 8

typedef struct wl_surface wl_surface_t;

/** commit 监听器：合成器在这里把 buffer 画到屏幕上。 */
typedef void (*wl_commit_fn)(wl_surface_t *s, wl_buffer_t *buf,
                             const wl_rect_t *damage, int count,
                             int full_damage, void *ctx);

struct wl_surface {
    int w, h;
    wl_buffer_t *pending;
    wl_buffer_t *current;
    wl_rect_t damage[WL_SURFACE_MAX_DAMAGE];
    int damage_count;
    int full_damage;
    int mapped;
    uint32_t serial;
    wl_commit_fn on_commit;
    void *commit_ctx;
};

wl_surface_t *wl_surface_create(int w, int h);
void wl_surface_destroy(wl_surface_t *s);

void wl_surface_set_commit_listener(wl_surface_t *s, wl_commit_fn fn, void *ctx);

/** 附加缓冲区（NULL 表示解除映射）。不触发提交。 */
int wl_surface_attach(wl_surface_t *s, wl_buffer_t *buf);

/** 标记脏区（自动裁剪到 surface 范围；溢出后退化为整屏脏）。 */
void wl_surface_damage(wl_surface_t *s, int x, int y, int w, int h);

/** 提交：pending→current，旧 buffer 触发 release，脏区清零。 */
int wl_surface_commit(wl_surface_t *s);

wl_buffer_t *wl_surface_current(const wl_surface_t *s);
int wl_surface_is_mapped(const wl_surface_t *s);
uint32_t wl_surface_serial(const wl_surface_t *s);

#endif /* HBOS_WL_SURFACE_H */
