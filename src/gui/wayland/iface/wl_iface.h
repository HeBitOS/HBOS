/**
 * @file    wl_iface.h
 * @brief   Wayland 接口实现层：共享状态 + 各接口 handler 声明
 *
 * 这一层只维护协议状态（哪个 id 对应哪个 surface、surface 上挂的是哪块
 * buffer、窗口标题是什么），不碰像素怎么上屏 —— 那是 backend/ 的事。
 *
 * 拆成多个文件是为了让"协议语义"和"渲染后端"能分别读：
 *   wl_iface_display.c    wl_display + wl_registry
 *   wl_iface_compositor.c wl_compositor + wl_surface + wl_region
 *   wl_iface_shm.c        wl_shm + wl_shm_pool + wl_buffer
 *   wl_iface_seat.c       wl_seat + wl_pointer + wl_keyboard
 *   wl_iface_xdg.c        xdg_wm_base + xdg_surface + xdg_toplevel + xdg_popup
 *
 * @layer   gui
 * @owner   src/gui/wayland/iface/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  wl_shm_buffer_t      最近一次 attach 的 buffer
 *         wl_shm_pool_state_t  结构体 / 类型别名
 *         wl_surface_state_t   结构体 / 类型别名
 *   宏    WL_MAX_BUFFERS   常量 / 特性宏
 *         WL_MAX_SURFACES  常量 / 特性宏
 *         WL_MAX_POOLS     常量 / 特性宏
 *         WL_TITLE_MAX     常量 / 特性宏
 *   函数  wl_iface_init           （待补）
 *         wl_surface_alloc        （待补）
 *         wl_surface_free         （待补）
 *         wl_buffer_alloc         （待补）
 *         wl_buffer_free          （待补）
 *         wl_pool_alloc           （待补）
 *         wl_pool_free            （待补）
 *         wl_iface_surface_count  （待补）
 *         wl_iface_surface_at     （待补）
 */
#ifndef HBOS_WL_IFACE_H
#define HBOS_WL_IFACE_H

#include "../core/wl_server.h"

#define WL_MAX_BUFFERS   32
#define WL_MAX_SURFACES  16
#define WL_MAX_POOLS     8
#define WL_TITLE_MAX     128

/** 一块 shm buffer 的元数据。像素不在内核里存，按需从 pool 的 fd 读。 */
typedef struct {
    int used;
    int width;
    int height;
    int stride;
    int format;              /**< WL_SHM_FORMAT_* */
    int pool_fd;             /**< 客户端给的 memfd，读像素用 */
    uint32_t pool_offset;    /**< buffer 在 pool 内的偏移 */
    uint32_t size;           /**< stride * height */
    int busy;                /**< 合成器正在用，暂不 release */
} wl_shm_buffer_t;

typedef struct {
    int used;
    int fd;
    uint32_t size;
} wl_shm_pool_state_t;

/** 一个窗口的表面状态。xdg_surface / xdg_toplevel 共享同一份。 */
typedef struct {
    int used;
    int width;
    int height;
    wl_shm_buffer_t *attached;    /**< 最近一次 attach 的 buffer */
    wl_shm_buffer_t *current;     /**< commit 之后正在显示的那个 */
    wl_client_t *client;          /**< 所属连接（合成完要往这里发 release） */
    int damage_x, damage_y, damage_w, damage_h;
    int damaged;                  /**< 有新内容待合成 */
    uint32_t commit_serial;
    uint32_t configured_serial;   /**< 最近一次 configure 的 serial */
    int configured;               /**< 是否已发过 configure */
    int pending_configure;        /**< 需要发 configure（首帧/尺寸变化） */
    char title[WL_TITLE_MAX];
    char app_id[64];
    int mapped;
    /* backend 侧的窗口句柄（HIVE 窗口 id），0 表示还没建窗口 */
    int window_id;
} wl_surface_state_t;

void wl_iface_init(void);

/* ── 共享池（surface/buffer/pool 生命周期都跟着连接走）────────── */

wl_surface_state_t *wl_surface_alloc(void);
void wl_surface_free(wl_surface_state_t *surface);
wl_shm_buffer_t *wl_buffer_alloc(void);
void wl_buffer_free(wl_shm_buffer_t *buffer);
wl_shm_pool_state_t *wl_pool_alloc(void);
void wl_pool_free(wl_shm_pool_state_t *pool);

/** 把 buffer 的像素按行拷到 dst（dst_stride 单位字节）。
 *  返回 0 成功；非 0 表示这块 buffer 读不出来（fd 无效/越界）。 */
int wl_buffer_read(const wl_shm_buffer_t *buffer, void *dst,
                   int dst_stride, int x, int y, int w, int h);

/* ── 供 backend 枚举所有 surface（合成器要遍历它们上屏）──────── */

int wl_iface_surface_count(void);
wl_surface_state_t *wl_iface_surface_at(int index);

/* ── 各接口 handler ─────────────────────────────────────────── */

void wl_display_request(wl_client_t *client, wl_object_t *object,
                        uint16_t opcode, wl_args_t *args);
void wl_registry_request(wl_client_t *client, wl_object_t *object,
                         uint16_t opcode, wl_args_t *args);
void wl_compositor_request(wl_client_t *client, wl_object_t *object,
                           uint16_t opcode, wl_args_t *args);
void wl_surface_request(wl_client_t *client, wl_object_t *object,
                        uint16_t opcode, wl_args_t *args);
void wl_region_request(wl_client_t *client, wl_object_t *object,
                       uint16_t opcode, wl_args_t *args);
void wl_shm_request(wl_client_t *client, wl_object_t *object,
                    uint16_t opcode, wl_args_t *args);
void wl_shm_pool_request(wl_client_t *client, wl_object_t *object,
                         uint16_t opcode, wl_args_t *args);
void wl_buffer_request(wl_client_t *client, wl_object_t *object,
                       uint16_t opcode, wl_args_t *args);
void wl_seat_request(wl_client_t *client, wl_object_t *object,
                     uint16_t opcode, wl_args_t *args);
void wl_input_device_request(wl_client_t *client, wl_object_t *object,
                             uint16_t opcode, wl_args_t *args);
void wl_xdg_wm_base_request(wl_client_t *client, wl_object_t *object,
                            uint16_t opcode, wl_args_t *args);
void wl_xdg_surface_request(wl_client_t *client, wl_object_t *object,
                            uint16_t opcode, wl_args_t *args);
void wl_xdg_toplevel_request(wl_client_t *client, wl_object_t *object,
                             uint16_t opcode, wl_args_t *args);
void wl_xdg_popup_request(wl_client_t *client, wl_object_t *object,
                          uint16_t opcode, wl_args_t *args);

/** 发一轮 configure（xdg_surface + xdg_toplevel），由 surface commit 触发。 */
void wl_surface_send_configure(wl_client_t *client,
                               wl_surface_state_t *surface);

#endif /* HBOS_WL_IFACE_H */
