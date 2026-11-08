/**
 * @file    wl_compositor.h
 * @brief   极简合成器：surface 列表 + z-order + 命中 + 绘制顺序（P1）
 *
 * 只管"谁在什么位置、谁盖住谁、从下到上怎么画"，不碰像素格式：
 * 实际 blit 由回调完成（内核里接 gd_*，宿主机测试里接记录器）。
 *
 * @layer   gui
 * @owner   src/gui/wl/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  wl_view_t        结构体 / 类型别名
 *         wl_compositor_t  结构体 / 类型别名
 *   宏    WL_COMP_MAX_VIEWS  常量 / 特性宏
 *   函数  wl_comp_create       （待补）
 *         wl_comp_add          加入视图（z 越大越靠上）
 *         wl_comp_remove       （待补）
 *         wl_comp_move         （待补）
 *         wl_comp_set_visible  （待补）
 *         wl_comp_raise        （待补）
 *         wl_comp_hit          命中测试：返回最上层、且该点落在 surf…
 *         wl_comp_paint        从下到上绘制所有可见 surface
 *         wl_comp_count        （待补）
 */
#ifndef HBOS_WL_COMPOSITOR_H
#define HBOS_WL_COMPOSITOR_H

#include "wl_surface.h"

#define WL_COMP_MAX_VIEWS 16

/** 绘制回调：把 buf 画到 (dst_x,dst_y)，只需处理 damage 区域。 */
typedef void (*wl_blit_fn)(void *ctx, int dst_x, int dst_y, wl_buffer_t *buf,
                           const wl_rect_t *damage, int count, int full_damage);

typedef struct {
    wl_surface_t *surface;
    int x, y;
    int z;          /**< 越大越靠上 */
    int visible;
} wl_view_t;

typedef struct {
    wl_view_t views[WL_COMP_MAX_VIEWS];
    int count;
    wl_blit_fn blit;
    void *blit_ctx;
} wl_compositor_t;

wl_compositor_t *wl_comp_create(wl_blit_fn blit, void *ctx);

/** 加入视图（z 越大越靠上）；返回 0 成功。 */
int wl_comp_add(wl_compositor_t *c, wl_surface_t *s, int x, int y, int z);
int wl_comp_remove(wl_compositor_t *c, wl_surface_t *s);
int wl_comp_move(wl_compositor_t *c, wl_surface_t *s, int x, int y);
int wl_comp_set_visible(wl_compositor_t *c, wl_surface_t *s, int visible);
int wl_comp_raise(wl_compositor_t *c, wl_surface_t *s);

/** 命中测试：返回最上层、且该点落在 surface 矩形内的 surface。 */
wl_surface_t *wl_comp_hit(const wl_compositor_t *c, int x, int y);

/** 从下到上绘制所有可见 surface；返回绘制的个数。 */
int wl_comp_paint(wl_compositor_t *c);

int wl_comp_count(const wl_compositor_t *c);

#endif /* HBOS_WL_COMPOSITOR_H */
