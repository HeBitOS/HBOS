/**
 * @file    wl_server.c
 * @brief   Wayland 服务端核心：监听、连接、对象表、分派
 *
 * 需要特别注意的两件事：
 *
 * 1) **字节流不是消息流。** 一次 read 可能只拿到半个 header，也可能一次
 *    拿到三条请求。必须有 input 缓冲 + 循环消费。
 *
 * 2) **监听 socket 必须在服务线程自己的 fd 表里创建。** HBOS 的 fd 表是
 *    每任务一份，在 shell 里 socket() 得到的 fd 在服务线程里查不到，
 *    accept 会一直 EBADF —— 表现是客户端 connect 成功但永远等不到事件。
 */

#include "wl_server.h"
#include "../iface/wl_iface.h"
#include "../backend/wl_backend.h"

#include "../../../core/heap.h"
#include "../../../core/task.h"
#include "../../../linux_compat.h"
#include "../../../string.h"

#define WL_SOCK_STREAM   1
#define WL_SOCK_CLOEXEC  0x80000
#define WL_SOCK_NONBLOCK 0x800

extern void console_puts(const char *text);
extern void console_putchar(char c);

/* 诊断用十进制输出。内核里没有统一的 print_uint 头，各文件自己带一份。 */
static void wl_put_dec(int value) {
    if (value < 0) {
        console_putchar('-');
        value = -value;
    }
    char buffer[12];
    int position = 0;
    if (!value) buffer[position++] = '0';
    while (value) {
        buffer[position++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (position) console_putchar(buffer[--position]);
}

/* 服务器 socket 的 sockaddr_un（必须和客户端构造的逐字节一致）。 */
typedef struct {
    uint16_t family;
    char path[108];
} wl_sockaddr_un_t;

static wl_client_t wl_clients[WL_MAX_CLIENTS];
/* 收数据日志的预算：只打前 8 条，正常运行时不会刷屏。 */
static int wl_rx_log_budget = 8;
static int wl_dbg_dispatch_budget = 8;
static int wl_dbg_flush_budget = 8;
static wl_global_t wl_globals[WL_MAX_GLOBALS];
static int wl_global_count;
static int wl_listen_fd = -1;
static int wl_task_started;
static volatile int wl_socket_ready;
static char wl_socket_path[108];
static wl_object_release_fn wl_release_hook;

/* ── 全局对象表 ─────────────────────────────────────────────── */

static void wl_register_global(const char *interface, uint32_t version) {
    if (wl_global_count >= WL_MAX_GLOBALS) return;
    wl_globals[wl_global_count].name = (uint32_t)(wl_global_count + 1);
    wl_globals[wl_global_count].interface = interface;
    wl_globals[wl_global_count].version = version;
    wl_global_count++;
}

const wl_global_t *wl_server_global_by_name(uint32_t name) {
    for (int i = 0; i < wl_global_count; i++)
        if (wl_globals[i].name == name) return &wl_globals[i];
    return NULL;
}

const wl_global_t *wl_server_global_by_interface(const char *interface) {
    for (int i = 0; i < wl_global_count; i++)
        if (strcmp(wl_globals[i].interface, interface) == 0)
            return &wl_globals[i];
    return NULL;
}

const wl_global_t *wl_server_global_list(int *count) {
    if (count) *count = wl_global_count;
    return wl_globals;
}

/* ── 对象表 ─────────────────────────────────────────────────── */

wl_object_t *wl_client_object(wl_client_t *client, uint32_t id) {
    if (!client || !id || id >= WL_MAX_OBJECTS) return NULL;
    if (!client->objects[id].id) return NULL;
    return &client->objects[id];
}

uint32_t wl_client_new_id(wl_client_t *client, wl_object_kind_t kind,
                          void *data) {
    if (!client) return 0;
    for (uint32_t id = 2; id < WL_MAX_OBJECTS; id++) {
        if (client->objects[id].id) continue;
        client->objects[id].id = id;
        client->objects[id].kind = kind;
        client->objects[id].version = 1;
        client->objects[id].global_name = 0;
        client->objects[id].data = data;
        return id;
    }
    return 0;
}

/**
 * 认领一个**客户端指定**的 new_id。
 *
 * 这是 Wayland 和"服务端分配句柄"式 IPC 最大的区别：new_id 是客户端在
 * 请求体里自己挑的号，服务端只是把号记下来。写成"要求该 id 已存在"会让
 * 每个带 new_id 的请求被静默丢弃 —— 表现是客户端 connect 成功、请求也
 * 发到了，但永远等不到事件。
 */
wl_object_t *wl_client_adopt_id(wl_client_t *client, uint32_t id,
                                wl_object_kind_t kind, void *data) {
    if (!client || id == 0 || id >= WL_MAX_OBJECTS) return NULL;
    if (client->objects[id].id) return NULL;
    client->objects[id].id = id;
    client->objects[id].kind = kind;
    client->objects[id].version = 1;
    client->objects[id].global_name = 0;
    client->objects[id].data = data;
    return &client->objects[id];
}

void wl_client_destroy(wl_client_t *client, uint32_t id) {
    wl_object_t *object = wl_client_object(client, id);
    if (!object) return;
    if (wl_release_hook) wl_release_hook(client, object);
    object->id = 0;
    object->kind = WL_OBJ_FREE;
    object->data = NULL;
}

uint32_t wl_client_find_id(wl_client_t *client, wl_object_kind_t kind,
                           const void *data) {
    if (!client) return 0;
    for (uint32_t id = 1; id < WL_MAX_OBJECTS; id++) {
        if (!client->objects[id].id) continue;
        if (client->objects[id].kind != kind) continue;
        if (data && client->objects[id].data != data) continue;
        return id;
    }
    return 0;
}

/* ── 事件写入包装 ───────────────────────────────────────────── */

void wl_event_begin(wl_client_t *client, uint32_t object_id, uint16_t opcode) {
    if (client) wl_out_begin(&client->out, object_id, opcode);
}

void wl_event_u32(wl_client_t *client, uint32_t value) {
    if (client) wl_out_u32(&client->out, value);
}

void wl_event_i32(wl_client_t *client, int32_t value) {
    if (client) wl_out_i32(&client->out, value);
}

void wl_event_string(wl_client_t *client, const char *text) {
    if (client) wl_out_string(&client->out, text);
}

void wl_event_end(wl_client_t *client) {
    if (client) wl_out_end(&client->out);
}

void wl_event_abort(wl_client_t *client) {
    if (client) wl_out_abort(&client->out);
}

int wl_client_flush(wl_client_t *client) {
    if (!client || client->fd < 0) return -1;
    uint32_t written = 0;
    uint32_t total = client->out.length;
    while (written < client->out.length) {
        long result = linux_compat_unix_send(
            client->fd, &client->out.buffer[written],
            client->out.length - written, 0x4000 /* MSG_NOSIGNAL */);
        if (result == -11 /* -EAGAIN */) {
            /* 对端缓冲满：保留没发出去的部分，下次再来。
             * 原来这里和真错误一起处理，既丢字节又把连接判死 —— accept
             * 出来的 fd 继承了监听 fd 的 SOCK_NONBLOCK，一次瞬时背压就
             * 会变成"截断 + 掉线"，客户端看到的是半条消息。 */
            if (written) {
                uint32_t remain = client->out.length - written;
                memmove(client->out.buffer, &client->out.buffer[written], remain);
                client->out.length = remain;
            }
            return 0;
        }
        if (result <= 0) {
            console_puts("[WL] flush failed at ");
            wl_put_dec((int)written);
            console_puts("/");
            wl_put_dec((int)total);
            console_puts(" ret=");
            wl_put_dec((int)result);
            console_puts("\n");
            client->out.length = 0;
            return -1;
        }
        written += (uint32_t)result;
    }
    if (total) {
        if (wl_dbg_flush_budget > 0) {
            wl_dbg_flush_budget--;
            console_puts("[WL] flush ");
            wl_put_dec((int)total);
            console_puts(" bytes\n");
        }
        /* 只有出过错才吱声；正常收发不刷屏（客户端每帧都在发请求）。 */
        if (client->out.in_error) console_puts("[WL] flush error\n");
    }
    client->out.length = 0;
    return client->out.in_error ? -1 : 0;
}

void wl_server_broadcast_global(const wl_global_t *global) {
    if (!global) return;
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        wl_client_t *client = &wl_clients[i];
        if (!client->used) continue;
        for (uint32_t id = 1; id < WL_MAX_OBJECTS; id++) {
            if (!client->objects[id].id) continue;
            if (client->objects[id].kind != WL_OBJ_REGISTRY) continue;
            wl_event_begin(client, id, WL_REGISTRY_E_GLOBAL);
            wl_event_u32(client, global->name);
            wl_event_string(client, global->interface);
            wl_event_u32(client, global->version);
            wl_event_end(client);
        }
        wl_client_flush(client);
    }
}

void wl_server_set_release_hook(wl_object_release_fn hook) {
    wl_release_hook = hook;
}

wl_client_t *wl_server_client_at(int index) {
    if (index < 0 || index >= WL_MAX_CLIENTS) return NULL;
    return wl_clients[index].used ? &wl_clients[index] : NULL;
}

int wl_server_client_count(void) {
    int count = 0;
    for (int i = 0; i < WL_MAX_CLIENTS; i++)
        if (wl_clients[i].used) count++;
    return count;
}

/* ── 请求分派 ───────────────────────────────────────────────── */

static void wl_dispatch_request(wl_client_t *client, uint32_t object_id,
                                uint16_t opcode, const uint8_t *body,
                                uint32_t body_length) {
    wl_object_t *object = wl_client_object(client, object_id);
    /* registry.bind 必须**无条件**打印：诊断"客户端收到 registry 后到底
     * 有没有继续 bind"时，之前用的 wl_dbg_dispatch_budget 是有限预算，
     * 前几条打完就静默了 —— 于是"日志里没有 bind"被误读成"客户端没发
     * bind"，两者根本不是一回事。这一行不受任何预算限制。 */
    if (object && object->kind == WL_OBJ_REGISTRY && opcode == WL_REGISTRY_BIND) {
        console_puts("[WL] BIND from client obj=");
        wl_put_dec((int)object_id);
        console_puts("\n");
    }
    if (wl_dbg_dispatch_budget > 0) {
        wl_dbg_dispatch_budget--;
        console_puts("[WL] req obj=");
        wl_put_dec((int)object_id);
        console_puts(" op=");
        wl_put_dec((int)opcode);
        console_puts(object ? " kind=" : " UNKNOWN\n");
        if (object) {
            wl_put_dec((int)object->kind);
            console_puts("\n");
        }
    }
    if (!object) return;

    wl_args_t args;
    args.data = body;
    args.length = body_length;
    args.offset = 0;
    args.error = 0;

    switch (object->kind) {
    case WL_OBJ_DISPLAY:
        wl_display_request(client, object, opcode, &args);
        break;
    case WL_OBJ_REGISTRY:
        wl_registry_request(client, object, opcode, &args);
        break;
    case WL_OBJ_CALLBACK:
        /* wl_callback 只有事件，没有请求。 */
        break;
    case WL_OBJ_COMPOSITOR:
        wl_compositor_request(client, object, opcode, &args);
        break;
    case WL_OBJ_SURFACE:
        wl_surface_request(client, object, opcode, &args);
        break;
    case WL_OBJ_REGION:
        wl_region_request(client, object, opcode, &args);
        break;
    case WL_OBJ_SHM:
        wl_shm_request(client, object, opcode, &args);
        break;
    case WL_OBJ_SHM_POOL:
        wl_shm_pool_request(client, object, opcode, &args);
        break;
    case WL_OBJ_BUFFER:
        wl_buffer_request(client, object, opcode, &args);
        break;
    case WL_OBJ_SEAT:
        wl_seat_request(client, object, opcode, &args);
        break;
    case WL_OBJ_POINTER:
    case WL_OBJ_KEYBOARD:
        wl_input_device_request(client, object, opcode, &args);
        break;
    case WL_OBJ_XDG_WM_BASE:
        wl_xdg_wm_base_request(client, object, opcode, &args);
        break;
    case WL_OBJ_XDG_SURFACE:
        wl_xdg_surface_request(client, object, opcode, &args);
        break;
    case WL_OBJ_XDG_TOPLEVEL:
        wl_xdg_toplevel_request(client, object, opcode, &args);
        break;
    case WL_OBJ_XDG_POPUP:
        wl_xdg_popup_request(client, object, opcode, &args);
        break;
    case WL_OBJ_OUTPUT:
        /* wl_output 只有一个请求 release。屏幕信息在 bind 时就发完了。 */
        if (opcode == 0) {
            object->kind = WL_OBJ_FREE;
            object->id = 0;
        }
        break;
    case WL_OBJ_DATA_DEVICE_MANAGER:
    case WL_OBJ_SUBCOMPOSITOR:
        /* 这两个接口的请求只创建子对象，没有实际副作用：
         *   data_device_manager: create_data_source / get_data_device
         *   subcompositor:       destroy / get_subsurface
         * 把 new_id 认领下来（客户端会持有它），不实现数据传输。 */
        if (object->kind == WL_OBJ_DATA_DEVICE_MANAGER &&
            (opcode == WL_DATA_DEVICE_MANAGER_CREATE_SOURCE ||
             opcode == WL_DATA_DEVICE_MANAGER_GET_DEVICE)) {
            uint32_t new_id = wl_arg_u32(&args);
            if (!wl_args_failed(&args) && new_id)
                wl_client_adopt_id(client, new_id, WL_OBJ_FREE, NULL);
        } else if (object->kind == WL_OBJ_SUBCOMPOSITOR && opcode == 1) {
            uint32_t new_id = wl_arg_u32(&args);
            if (!wl_args_failed(&args) && new_id)
                wl_client_adopt_id(client, new_id, WL_OBJ_FREE, NULL);
        }
        break;
    default:
        break;
    }
}

/* ── 连接读写 ───────────────────────────────────────────────── */

/**
 * 消费 input 缓冲里所有完整的请求。
 *
 * 必须能**独立于 recv** 反复调用：一次 read 可能只拿到半个请求，剩下半个
 * 要到下一次 read 才补齐；但如果客户端已经把请求全发完了，就再也不会有
 * "下一次 read" 来触发解析 —— 缓冲区里那半条请求会永远躺着。
 * 早先解析循环只写在 recv 后面，结果客户端连发 9 条请求，服务端只处理了
 * 前两条 bind 就停住，后面的 create_surface/create_pool 全丢。
 *
 * 返回这一轮消费掉的请求数。
 */
static int wl_client_consume(wl_client_t *client) {
    uint32_t consumed = 0;
    int requests = 0;
    while (client->input_length - consumed >= WL_MESSAGE_HEADER_SIZE) {
        wl_message_header_t header;
        if (!wl_message_parse(&client->input[consumed],
                              client->input_length - consumed, &header))
            break;
        wl_dispatch_request(client, header.object_id, header.opcode,
                            &client->input[consumed + WL_MESSAGE_HEADER_SIZE],
                            header.size - WL_MESSAGE_HEADER_SIZE);
        consumed += header.size;
        requests++;
    }
    if (consumed) {
        uint32_t remain = client->input_length - consumed;
        if (remain) memmove(client->input, &client->input[consumed], remain);
        client->input_length = remain;
    }
    return requests;
}

/**
 * 驱动一条连接一轮：收数据 → 消费缓冲 → 回写事件。
 *
 * 返回值语义很重要：
 *   1  = 这一轮真的处理了数据（有新连接或读到请求）
 *   0  = 没事可做（所有连接都没数据）
 *  -1  = 连接坏了，已经释放
 *
 * 事件循环靠它决定要不要 task_yield()。把"连接还在"也算成有活干会导致
 * 服务线程永远不让出 CPU，协作式调度下客户端直接被饿死。
 */
static int wl_client_read_and_dispatch(wl_client_t *client) {
    int progress = 0;
    if (client->input_length < WL_MAX_INPUT) {
        long got = linux_compat_unix_recv(
            client->fd, &client->input[client->input_length],
            WL_MAX_INPUT - client->input_length, 0x40 /* MSG_DONTWAIT */);
        if (got == -11 /* -EAGAIN */) {
            /* 没有新字节，但缓冲里可能还留着上次的半条请求。 */
            got = 0;
        } else if (got <= 0) {
            return -1;
        } else {
            client->input_length += (uint32_t)got;
            progress = 1;
            /* 只打前几条：能区分"连上但一个字节都没发"和"发了请求但
             * 服务端没回应"，这两种情况要查的地方完全不同。 */
            if (wl_rx_log_budget > 0) {
                wl_rx_log_budget--;
                console_puts("[WL] rx ");
                wl_put_dec((int)got);
                console_puts(" bytes\n");
            }
            /* 收本轮随消息过来的 ancillary fd（wl_shm.create_pool 传 memfd
             * 用）。必须在分派请求之前收：create_pool 处理时要从
             * pending_fds 里取。 */
            while (client->pending_fd_count < WL_MAX_PENDING_FDS) {
                size_t received = 0;
                int truncated = 0;
                if (linux_compat_unix_recv_rights(
                        client->fd,
                        &client->pending_fds[client->pending_fd_count],
                        (size_t)(WL_MAX_PENDING_FDS -
                                 client->pending_fd_count),
                        &received, &truncated) < 0)
                    break;
                if (!received) break;
                client->pending_fd_count += (int)received;
                if (truncated) break;
            }
        }
    }

    if (wl_client_consume(client)) progress = 1;
    if (wl_client_flush(client) < 0) return -1;
    return progress;
}

/* ── 连接生命周期 ───────────────────────────────────────────── */

static wl_client_t *wl_client_alloc(int fd) {
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        if (wl_clients[i].used) continue;
        wl_client_t *client = &wl_clients[i];
        memset(client, 0, sizeof(*client));
        client->used = 1;
        client->fd = fd;
        client->serial = 1;
        wl_outbuf_init(&client->out, client->output_buffer, WL_MAX_OUTPUT);
        /* id 1 永远是 wl_display，客户端不需要 bind 就能用。 */
        client->objects[1].id = 1;
        client->objects[1].kind = WL_OBJ_DISPLAY;
        client->objects[1].version = 1;
        return client;
    }
    return NULL;
}

static void wl_client_release(wl_client_t *client) {
    if (!client || !client->used) return;
    for (uint32_t id = 1; id < WL_MAX_OBJECTS; id++)
        if (client->objects[id].id) wl_client_destroy(client, id);
    for (int i = 0; i < client->pending_fd_count; i++)
        if (client->pending_fds[i] >= 0)
            (void)linux_compat_close(client->pending_fds[i]);
    if (client->fd >= 0) (void)linux_compat_close(client->fd);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

/* ── 事件循环 ───────────────────────────────────────────────── */

int wl_server_poll_once(void) {
    if (wl_listen_fd < 0) return 0;
    int handled = 0;
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        int fd = linux_compat_unix_accept(wl_listen_fd, NULL, NULL);
        if (fd < 0) break;
        /* 低频但关键：判断"客户端到底连没连上来"只能靠这行。
         * accept 成功却没有任何 dispatch，说明对端连上就卡住了。 */
        console_puts("[WL] client accepted\n");
        if (!wl_client_alloc(fd)) {
            (void)linux_compat_close(fd);
            break;
        }
        handled++;
    }
    for (int i = 0; i < WL_MAX_CLIENTS; i++) {
        wl_client_t *client = &wl_clients[i];
        if (!client->used) continue;
        int state = wl_client_read_and_dispatch(client);
        if (state < 0) wl_client_release(client);
        else if (state > 0) handled++;
    }
    if (handled) wl_backend_pump();
    return handled;
}

static void wl_server_thread(void *arg) {
    (void)arg;
    int fd = linux_compat_unix_socket(
        WL_SOCK_STREAM | WL_SOCK_NONBLOCK | WL_SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        wl_sockaddr_un_t address;
        memset(&address, 0, sizeof(address));
        address.family = 1;   /* AF_UNIX */
        size_t length = strlen(wl_socket_path);
        if (length < sizeof(address.path)) {
            memcpy(address.path, wl_socket_path, length);
            if (linux_compat_unix_bind(
                    fd, &address,
                    (uint32_t)(sizeof(uint16_t) + length + 1)) == 0 &&
                linux_compat_unix_listen(fd, 8) == 0) {
                wl_listen_fd = fd;
            }
        }
        if (wl_listen_fd < 0) (void)linux_compat_close(fd);
    }
    wl_socket_ready = 1;
    if (wl_listen_fd >= 0) console_puts("[WL] wayland server ready\n");

    for (;;) {
        if (wl_listen_fd < 0) {
            task_yield();
            continue;
        }
        if (wl_server_poll_once() == 0) task_yield();
    }
}

/* ── 启动 ───────────────────────────────────────────────────── */

int wl_server_running(void) {
    return wl_task_started && wl_listen_fd >= 0;
}

int wl_server_start(const char *path) {
    if (wl_server_running()) return 0;
    if (!path || !path[0]) return -22 /* -EINVAL */;
    if (strlen(path) >= sizeof(wl_socket_path)) return -36 /* -ENAMETOOLONG */;

    if (wl_global_count == 0) {
        wl_register_global(WL_IFACE_COMPOSITOR, WL_COMPOSITOR_VERSION);
        wl_register_global(WL_IFACE_SHM, WL_SHM_VERSION);
        wl_register_global(WL_IFACE_SEAT, WL_SEAT_VERSION);
        wl_register_global(WL_IFACE_XDG_WM_BASE, WL_XDG_WM_BASE_VERSION);
        /* wl_output 必须注册：客户端靠它的 mode 事件确定屏幕尺寸。
         * 缺了它 Chromium 会在拿到 registry 后停住（发完 get_registry +
         * sync、收到全部事件，然后一个字节都不再发）。 */
        wl_register_global(WL_IFACE_OUTPUT, WL_OUTPUT_VERSION);
        /* Chromium 的 ozone-wayland 初始化会 bind data_device_manager 做
         * 剪贴板。只注册不实现内容也够 —— 它需要的是"这个接口存在"，
         * 缺失时初始化会静默停住。 */
        wl_register_global(WL_IFACE_DATA_DEVICE_MANAGER,
                           WL_DATA_DEVICE_MANAGER_VERSION);
        wl_register_global(WL_IFACE_SUBCOMPOSITOR, WL_SUBCOMPOSITOR_VERSION);
    }

    strncpy(wl_socket_path, path, sizeof(wl_socket_path) - 1);
    wl_socket_path[sizeof(wl_socket_path) - 1] = '\0';
    wl_socket_ready = 0;
    wl_listen_fd = -1;
    wl_iface_init();
    wl_backend_init();

    if (task_create("wayland", wl_server_thread, NULL) < 0)
        return -11 /* -EAGAIN */;
    wl_task_started = 1;

    /* 等线程把 socket 建好：调用方打印"listening"之后立刻就可能跑客户端，
     * 慢一步就会 connect 失败。 */
    for (int spin = 0; spin < 2000 && !wl_socket_ready; spin++) task_yield();
    if (!wl_socket_ready) return -11;
    return wl_listen_fd >= 0 ? 0 : -98 /* -EADDRINUSE */;
}
