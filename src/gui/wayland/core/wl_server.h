/**
 * @file    wl_server.h
 * @brief   Wayland 服务端核心：socket 监听、连接、对象表、请求分派
 *
 * 这一层不含任何协议语义 —— 它只知道"某个 object id 上来了一个 opcode
 * 和一段参数，交给 who 处理"。具体是谁，由 iface/ 里注册的对象类型决定。
 *
 * 分层的意义：换一套协议（比如以后接 X11 或自研 API）时，core/ 完全不动，
 * 只写新的 iface/ 和 backend/。
 *
 * @layer   gui
 * @owner   src/gui/wayland/core/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  wl_object_kind_t  结构体 / 类型别名
 *         wl_object_t       结构体 / 类型别名
 *         wl_client_t       结构体 / 类型别名
 *         wl_global_t       结构体 / 类型别名
 *   宏    WL_MAX_CLIENTS      常量 / 特性宏
 *         WL_MAX_OBJECTS      常量 / 特性宏
 *         WL_MAX_INPUT        常量 / 特性宏
 *         WL_MAX_OUTPUT       常量 / 特性宏
 *         WL_MAX_GLOBALS      常量 / 特性宏
 *         WL_MAX_PENDING_FDS  常量 / 特性宏
 *   函数  wl_server_start               启动服务端：监听 path 并创建一个内核…
 *         wl_server_running             （待补）
 *         wl_server_poll_once           事件循环推进一步（测试用）
 *         wl_client_object              （待补）
 *         wl_client_destroy             销毁客户端上的一个对象（接口层的 dest…
 *         wl_event_begin                （待补）
 *         wl_event_u32                  （待补）
 *         wl_event_i32                  （待补）
 *         wl_event_string               （待补）
 *         wl_event_end                  （待补）
 *         wl_event_abort                （待补）
 *         wl_client_flush               （待补）
 *         wl_server_broadcast_global    （待补）
 *         wl_server_set_release_hook    （待补）
 *         wl_dbg_note                   通用三值诊断行：`[WL] <label>…
 *         wl_server_global_by_interface  （待补）
 *         wl_server_global_by_name      （待补）
 *         wl_server_global_list         （待补）
 *         wl_server_client_at           （待补）
 *         wl_server_client_count        （待补）
 */
#ifndef HBOS_WL_SERVER_H
#define HBOS_WL_SERVER_H

#include <stdint.h>
#include "../wl_protocol.h"
#include "wl_wire.h"

#define WL_MAX_CLIENTS        8
/* 对象表按 id 下标寻址，而 id 由客户端单调递增分配：Chromium 一次导航就会
 * 建 surface/region/buffer/xdg_* 几十个对象，128 会在后续密集创建时悄悄
 * 用尽（adopt_id 失败是静默的，客户端只会"等不到事件"）。512 × 24B × 8
 * 客户端 ≈ 96KB 静态 BSS，换取不再有这种悬崖。 */
#define WL_MAX_OBJECTS        512
#define WL_MAX_INPUT          4096
#define WL_MAX_OUTPUT         8192
#define WL_MAX_GLOBALS        8
/* getaddrinfo 一次可以带多个 fd（dmabuf + 多个 shm），8 太窄。 */
#define WL_MAX_PENDING_FDS    16

/** 对象类型：决定该 id 上收到的请求由谁处理。 */
typedef enum {
    WL_OBJ_FREE = 0,
    WL_OBJ_DISPLAY,
    WL_OBJ_REGISTRY,
    WL_OBJ_CALLBACK,
    WL_OBJ_COMPOSITOR,
    WL_OBJ_SURFACE,
    WL_OBJ_REGION,
    WL_OBJ_SHM,
    WL_OBJ_SHM_POOL,
    WL_OBJ_BUFFER,
    WL_OBJ_SEAT,
    WL_OBJ_POINTER,
    WL_OBJ_KEYBOARD,
    WL_OBJ_POINTER_OR_KEYBOARD,
    WL_OBJ_XDG_WM_BASE,
    WL_OBJ_XDG_SURFACE,
    WL_OBJ_XDG_TOPLEVEL,
    WL_OBJ_XDG_POPUP,
    WL_OBJ_OUTPUT,
    WL_OBJ_DATA_DEVICE_MANAGER,
    WL_OBJ_SUBCOMPOSITOR
} wl_object_kind_t;

/**
 * 一个协议对象。
 *
 * `data` 指向实现侧数据（surface 状态、shm pool 描述等），由 kind 决定
 * 怎么解释。`version` 是客户端 bind 时声明的版本。
 */
typedef struct {
    uint32_t id;            /**< 客户端侧对象 id（0 = 空槽） */
    wl_object_kind_t kind;
    uint32_t version;
    uint32_t global_name;   /**< bind 时对应的全局编号 */
    void *data;
} wl_object_t;

typedef struct {
    int fd;                          /**< 服务端侧连接 fd（-1=空闲） */
    int used;
    uint32_t serial;                 /**< 事件序号（configure/ping 用） */
    wl_outbuf_t out;
    uint8_t output_buffer[WL_MAX_OUTPUT];
    wl_object_t objects[WL_MAX_OBJECTS];
    uint8_t input[WL_MAX_INPUT];
    uint32_t input_length;
    /** 本轮收到的 ancillary fd（wl_shm create_pool 用），按顺序消费。 */
    int pending_fds[WL_MAX_PENDING_FDS];
    int pending_fd_count;
} wl_client_t;

typedef struct {
    uint32_t name;          /**< 全局编号（1..N） */
    const char *interface;
    uint32_t version;
} wl_global_t;

/**
 * 启动服务端：监听 path 并创建一个内核线程跑事件循环。幂等。
 * 返回 0 成功；<0 失败（errno 取反）。
 */
int wl_server_start(const char *path);

int wl_server_running(void);

/** 事件循环推进一步（测试用）。返回处理的请求数。 */
int wl_server_poll_once(void);

/* ── 对象表 ─────────────────────────────────────────────────── */

wl_object_t *wl_client_object(wl_client_t *client, uint32_t id);

/** 服务端主动分配新 id（一般用不到，Wayland 的 id 由客户端给）。 */
uint32_t wl_client_new_id(wl_client_t *client, wl_object_kind_t kind,
                          void *data);

/** 认领客户端在 new_id 参数里指定的对象号。已占用返回 NULL。 */
wl_object_t *wl_client_adopt_id(wl_client_t *client, uint32_t id,
                                wl_object_kind_t kind, void *data);

/** 销毁客户端上的一个对象（接口层的 destroy 请求用）。 */
void wl_client_destroy(wl_client_t *client, uint32_t id);

/** 按 kind + data 找客户端里持有该数据的对象 id。 */
uint32_t wl_client_find_id(wl_client_t *client, wl_object_kind_t kind,
                           const void *data);

/* ── 事件写入（转发到 wl_outbuf_t，调用方不用自己管缓冲）──────── */

void wl_event_begin(wl_client_t *client, uint32_t object_id, uint16_t opcode);
void wl_event_u32(wl_client_t *client, uint32_t value);
void wl_event_i32(wl_client_t *client, int32_t value);
void wl_event_string(wl_client_t *client, const char *text);
void wl_event_end(wl_client_t *client);
void wl_event_abort(wl_client_t *client);

int wl_client_flush(wl_client_t *client);

void wl_server_broadcast_global(const wl_global_t *global);

typedef void (*wl_object_release_fn)(wl_client_t *client, wl_object_t *object);
void wl_server_set_release_hook(wl_object_release_fn hook);

/* registry.bind 的一行摘要：接口名、全局名、版本、新 id。
 * 只打一行，诊断"客户端到底绑了哪些全局、哪个没绑"时不必再猜。 */
void wl_dbg_bind(const char *interface, uint32_t name, uint32_t version,
                 uint32_t id);

/* 通用三值诊断行：`[WL] <label> a=.. b=.. c=..`。分派/接口层查"某个请求
 * 在哪一步被丢掉"时用，比给每处单独写一套打印省事。 */
void wl_dbg_note(const char *label, int a, int b, int c);

/* ── 全局对象查询 ───────────────────────────────────────────── */

const wl_global_t *wl_server_global_by_interface(const char *interface);
const wl_global_t *wl_server_global_by_name(uint32_t name);
const wl_global_t *wl_server_global_list(int *count);

/* ── 供 iface / backend 遍历连接 ────────────────────────────── */

wl_client_t *wl_server_client_at(int index);
int wl_server_client_count(void);

#endif /* HBOS_WL_SERVER_H */
