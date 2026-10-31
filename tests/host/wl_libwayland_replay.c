/**
 * @file    wl_libwayland_replay.c
 * @brief   把 HBOS 服务端真实生成的 registry 事件流喂给真 libwayland 客户端
 *
 * 目的：判定我们发出的 252 字节能否被真 libwayland 解析出 7 个 global。
 *
 * 关键约束：被喂的字节**必须**由 src/gui/wayland/core/wl_wire.c 里真实的
 * wl_out_begin/wl_out_u32/wl_out_string/wl_out_end 生成 —— 我们怀疑的正是
 * 这份代码的行为，手抄常量验不出任何东西。
 *
 * 全局对象表按 wl_server.c:553-566 的注册顺序与 wl_protocol.h 的版本宏还原；
 * 版本号不写死，直接引用宏。
 *
 * 对象 id 说明：registry / callback 的 id 是客户端自己分配的（服务端只是
 * 回显 wl_display.get_registry 与 wl_display.sync 的 new_id），所以这里用
 * wl_proxy_get_id() 取 libwayland 真正分配的值，而不是猜。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>

#include <wayland-client.h>

/* 用相对路径而非 -Isrc：-Isrc 会让 <unistd.h>/<sys/types.h> 解析到 HBOS 的
 * 内核头，与 glibc 的 socket 声明冲突。wl_wire.c 自己按相对路径找 string.h。 */
#include "../../src/gui/wayland/wl_protocol.h"
#include "../../src/gui/wayland/core/wl_wire.h"

#define MAX_GLOBALS 7

/* 按 wl_server.c:553-566 的注册顺序：name 由 wl_register_global 递增分配。 */
static const struct {
    const char *interface;
    uint32_t version;
} g_globals[MAX_GLOBALS] = {
    { WL_IFACE_COMPOSITOR,          WL_COMPOSITOR_VERSION },
    { WL_IFACE_SHM,                 WL_SHM_VERSION },
    { WL_IFACE_SEAT,                WL_SEAT_VERSION },
    { WL_IFACE_XDG_WM_BASE,         WL_XDG_WM_BASE_VERSION },
    { WL_IFACE_OUTPUT,              WL_OUTPUT_VERSION },
    { WL_IFACE_DATA_DEVICE_MANAGER, WL_DATA_DEVICE_MANAGER_VERSION },
    { WL_IFACE_SUBCOMPOSITOR,       WL_SUBCOMPOSITOR_VERSION },
};

/* ── 客户端侧回调 ──────────────────────────────────────────── */

static int g_global_count;
static uint32_t g_cb_done_count;

static void on_global(void *data, struct wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version) {
    (void)data; (void)registry;
    printf("  [libwayland] global #%d: name=%u interface=%s version=%u\n",
           g_global_count + 1, name, interface ? interface : "(null)", version);
    if (g_global_count < MAX_GLOBALS) {
        /* 顺手核对：libwayland 报的名字/版本是否与我们声明的一致。 */
        int expect_ok = (strcmp(interface ? interface : "",
                                g_globals[g_global_count].interface) == 0) &&
                        version == g_globals[g_global_count].version &&
                        name == (uint32_t)(g_global_count + 1);
        if (!expect_ok)
            printf("    !! 与声明不符：期望 name=%d interface=%s version=%u\n",
                   g_global_count + 1, g_globals[g_global_count].interface,
                   g_globals[g_global_count].version);
    }
    g_global_count++;
}

static void on_global_remove(void *data, struct wl_registry *registry,
                             uint32_t name) {
    (void)data; (void)registry;
    printf("  [libwayland] global_remove: name=%u\n", name);
}

static const struct wl_registry_listener g_registry_listener = {
    on_global,
    on_global_remove,
};

static void on_cb_done(void *data, struct wl_callback *cb, uint32_t serial) {
    (void)data; (void)cb; (void)serial;
    g_cb_done_count++;
}

static const struct wl_callback_listener g_cb_listener = {
    on_cb_done,
};

/* ── 独立结构校验器 ────────────────────────────────────────────
 *
 * 不依赖 wl_wire，按 Wayland 线格式自己走一遍，逐字段核对。
 * 用来在 N != 7 时定位「第一个出问题的字节」：offset / 我们写的值 / 期望值。
 * 注意：这不是被喂的数据源，只是旁路核对。 */

static int g_bad_offset = -1;
static int g_bad_count;

static void report_bad(uint32_t off, const char *what, unsigned long got,
                       unsigned long want) {
    g_bad_count++;
    if (g_bad_offset >= 0) return;   /* 只记录第一个的位置 */
    g_bad_offset = (int)off;
    printf("  !! 第一个问题字节 @ offset %u (%s): 我们写的=%lu 期望=%lu\n",
           off, what, got, want);
}

static int verify_stream(const uint8_t *buf, uint32_t len, uint32_t registry_id,
                         uint32_t callback_id, uint32_t serial) {
    uint32_t off = 0;
    int msg = 0;

    g_bad_count = 0;
    g_bad_offset = -1;

    for (int i = 0; i < MAX_GLOBALS; i++) {
        if (off + 8 > len) {
            report_bad(off, "global 头越界", len, off + 8);
            break;
        }
        uint32_t obj, word;
        memcpy(&obj, buf + off, 4);
        memcpy(&word, buf + off + 4, 4);
        uint32_t opcode = word & 0xffffu;
        uint32_t size = word >> 16;

        const char *iface = g_globals[i].interface;
        uint32_t slen = (uint32_t)strlen(iface) + 1;      /* 含 NUL */
        uint32_t spadded = (slen + 3) & ~3u;
        uint32_t want_size = 8 + 4 + 4 + spadded + 4;

        if (obj != registry_id) report_bad(off, "object_id", obj, registry_id);
        if (opcode != WL_REGISTRY_E_GLOBAL)
            report_bad(off + 4, "opcode", opcode, WL_REGISTRY_E_GLOBAL);
        if (size != want_size) report_bad(off + 4, "size(高16位)", size, want_size);

        /* size 自身不可信时立刻停，否则会用坏 size 去步进。 */
        if (size < 8 || size > len - off) {
            report_bad(off + 4, "size 不可用于步进", size, want_size);
            break;
        }

        uint32_t name;
        memcpy(&name, buf + off + 8, 4);
        if (name != (uint32_t)(i + 1)) report_bad(off + 8, "global name", name, i + 1);

        uint32_t got_slen;
        memcpy(&got_slen, buf + off + 12, 4);
        if (got_slen != slen)
            report_bad(off + 12, "string 长度(应含 NUL)", got_slen, slen);

        if (off + 16 + slen <= len) {
            for (uint32_t k = 0; k < strlen(iface); k++) {
                if (buf[off + 16 + k] != (uint8_t)iface[k]) {
                    report_bad(off + 16 + k, "string 内容",
                               buf[off + 16 + k], (unsigned char)iface[k]);
                    break;
                }
            }
            if (buf[off + 16 + slen - 1] != 0)
                report_bad(off + 16 + slen - 1, "string 结尾 NUL",
                           buf[off + 16 + slen - 1], 0);
        }

        uint32_t ver_off = off + 8 + 4 + 4 + spadded;
        if (ver_off + 4 <= len) {
            uint32_t ver;
            memcpy(&ver, buf + ver_off, 4);
            if (ver != g_globals[i].version)
                report_bad(ver_off, "version", ver, g_globals[i].version);
        }

        off += size;
        msg++;
        if (g_bad_count) break;
    }

    /* 末尾的 wl_callback.done */
    if (!g_bad_count) {
        if (off + 12 > len) {
            report_bad(off, "callback.done 越界", len, off + 12);
        } else {
            uint32_t obj, word, s;
            memcpy(&obj, buf + off, 4);
            memcpy(&word, buf + off + 4, 4);
            memcpy(&s, buf + off + 8, 4);
            if (obj != callback_id) report_bad(off, "callback object_id", obj, callback_id);
            if ((word & 0xffffu) != WL_CALLBACK_E_DONE)
                report_bad(off + 4, "callback opcode", word & 0xffffu, WL_CALLBACK_E_DONE);
            if ((word >> 16) != 12) report_bad(off + 4, "callback size", word >> 16, 12);
            if (s != serial) report_bad(off + 8, "callback serial", s, serial);
            off += 12;
        }
    }

    if (!g_bad_count && off != len)
        report_bad(off, "尾部多余字节", len, off);

    printf("  独立结构校验：%d 条 global 消息 + callback.done，走到 offset %u/%u，%s\n",
           msg, off, len, g_bad_count ? "有问题" : "全部字段符合线格式");
    return g_bad_count;
}

/* ── 主流程 ────────────────────────────────────────────────── */

/* 生成一批字节。返回写入长度。 */
typedef uint32_t (*gen_fn)(uint32_t registry_id, uint32_t callback_id,
                           uint32_t serial, uint8_t *out, uint32_t cap);

/* 用服务端真实代码生成：7 个 global + 1 个 callback.done。 */
static uint32_t gen_real(uint32_t registry_id, uint32_t callback_id,
                         uint32_t serial, uint8_t *out, uint32_t cap) {
    wl_outbuf_t w;
    wl_outbuf_init(&w, out, cap);
    for (int i = 0; i < MAX_GLOBALS; i++) {
        wl_out_begin(&w, registry_id, WL_REGISTRY_E_GLOBAL);
        wl_out_u32(&w, (uint32_t)(i + 1));          /* name：注册顺序 1..7 */
        wl_out_string(&w, g_globals[i].interface);
        wl_out_u32(&w, g_globals[i].version);
        wl_out_end(&w);
    }
    wl_out_begin(&w, callback_id, WL_CALLBACK_E_DONE);
    wl_out_u32(&w, serial);
    wl_out_end(&w);
    return w.in_error ? 0 : w.length;
}

/* ── 负对照：对真实字节做定点破坏 ──
 *
 * 这些变异不是"服务端可能有 bug"的猜测，而是用来证明本复现台**有检出能力**：
 * 如果变异后 N 仍然等于 7，说明这个台子根本分辨不出对错，那 N=7 也就不可信。 */

/* 把某条消息头的 size/opcode 两半调换（wl_wire.h 注释里点名的高危错误）。 */
static void mutate_swap_halves(uint8_t *buf, uint32_t len, uint32_t off) {
    if (off + 8 > len) return;
    uint32_t word;
    memcpy(&word, buf + off + 4, 4);
    uint32_t opcode = word & 0xffffu, size = word >> 16;
    word = (opcode << 16) | (size & 0xffffu);
    memcpy(buf + off + 4, &word, 4);
}

/* 把第 idx 个 global 的 string 长度改成不含 NUL（少 1）。 */
static void mutate_string_len(uint8_t *buf, uint32_t len, uint32_t off) {
    if (off + 12 > len) return;
    uint32_t slen;
    memcpy(&slen, buf + off + 12, 4);
    slen -= 1;
    memcpy(buf + off + 12, &slen, 4);
}

/* 把第 idx 个 global 的 version 改成一个不存在的值。 */
static void mutate_version(uint8_t *buf, uint32_t len, uint32_t off,
                           uint32_t padded) {
    uint32_t ver_off = off + 8 + 4 + 4 + padded;
    if (ver_off + 4 > len) return;
    uint32_t bad = 0xdeadbeef;
    memcpy(buf + ver_off, &bad, 4);
}

/* 走一遍：新建 display → 生成字节 → 写入 → dispatch。
 * 返回 global 回调次数（-1 = 环境失败），*verr_out 收结构校验错误数。 */
static int replay_once(const char *label, gen_fn gen,
                       uint8_t *scratch, uint32_t cap,
                       int mutate, int *verr_out) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return -1;
    }
    struct wl_display *display = wl_display_connect_to_fd(sv[1]);
    if (!display) { fprintf(stderr, "connect_to_fd 失败\n"); return -1; }

    g_global_count = 0;
    g_cb_done_count = 0;
    g_bad_offset = -1;

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &g_registry_listener, NULL);
    uint32_t registry_id = wl_proxy_get_id((struct wl_proxy *)registry);

    struct wl_callback *cb = wl_display_sync(display);
    wl_callback_add_listener(cb, &g_cb_listener, NULL);
    uint32_t callback_id = wl_proxy_get_id((struct wl_proxy *)cb);

    uint32_t serial = 1;
    uint32_t len = gen(registry_id, callback_id, serial, scratch, cap);
    if (len == 0) { fprintf(stderr, "%s: 生成字节失败\n", label); return -1; }

    if (mutate) {
        uint32_t slen = (uint32_t)strlen(g_globals[0].interface) + 1;
        uint32_t padded = (slen + 3) & ~3u;
        switch (mutate) {
        case 1: mutate_swap_halves(scratch, len, 0); break;
        case 2: mutate_string_len(scratch, len, 0); break;
        case 3: mutate_version(scratch, len, 0, padded); break;
        default: break;
        }
    }

    printf("\n── %s ──\n", label);
    printf("  字节数 = %u\n", len);
    int verr = verify_stream(scratch, len, registry_id, callback_id, serial);
    if (verr_out) *verr_out = verr;

    if (write(sv[0], scratch, len) != (ssize_t)len) {
        fprintf(stderr, "写入失败\n");
        return -1;
    }

    int rounds = 0;
    while (g_cb_done_count == 0 && rounds < 64) {
        struct pollfd pfd = { .fd = sv[1], .events = POLLIN };
        int pr = poll(&pfd, 1, 2000);
        if (pr <= 0) { printf("  poll 超时/出错，停止\n"); break; }
        if (wl_display_dispatch(display) < 0) {
            printf("  wl_display_dispatch 失败: %s（libwayland 判定流非法）\n",
                   strerror(errno));
            break;
        }
        rounds++;
    }

    printf("  N = %d (global 回调), callback.done = %u, 结构校验错误 = %d\n",
           g_global_count, g_cb_done_count, verr);

    wl_display_disconnect(display);
    close(sv[0]);
    return g_global_count;
}

int main(void) {
    uint8_t out[4096];

    /* 正例：服务端真实代码生成的 252 字节。 */
    int n_real = replay_once("正例：wl_wire.c 真实生成", gen_real, out, sizeof out,
                             0, NULL);
    if (n_real < 0) return 1;

    printf("\n── 负对照（证明本复现台有检出能力）──\n");
    int v1 = 0, v2 = 0, v3 = 0;
    int m1 = replay_once("变异1：消息头 size/opcode 两半调换", gen_real,
                         out, sizeof out, 1, &v1);
    int m2 = replay_once("变异2：string 长度不含结尾 NUL", gen_real,
                         out, sizeof out, 2, &v2);
    int m3 = replay_once("变异3：version 改成 0xdeadbeef", gen_real,
                         out, sizeof out, 3, &v3);

    /* 检出 = libwayland 少收 global（N != 7）或我方结构校验报错，两者任一即可。
     * 变异3 说明 libwayland 不校验 version 取值，所以不能只看 N。 */
    int d1 = (m1 != MAX_GLOBALS) || v1 > 0;
    int d2 = (m2 != MAX_GLOBALS) || v2 > 0;
    int d3 = (m3 != MAX_GLOBALS) || v3 > 0;
    int detected = d1 + d2 + d3;

    printf("\n══ 结论 ══\n");
    printf("真 libwayland 对真实字节回调 wl_registry.global 次数 N = %d / %d\n",
           n_real, MAX_GLOBALS);
    for (int i = 0; i < MAX_GLOBALS && n_real == MAX_GLOBALS; i++)
        printf("  name=%d interface=%s version=%u\n", i + 1,
               g_globals[i].interface, g_globals[i].version);

    if (n_real == MAX_GLOBALS) {
        printf("字节序列对真 libwayland 是正确的，问题不在序列化层。\n");
        printf("据此排除：wl_wire 的消息头 size/opcode 位域、string 长度含 NUL、\n");
        printf("          string 4 字节补齐、消息 size 计算、global 的 name/version 字段。\n");
        printf("问题只可能在：服务端把字节写到 socket 的方式（分片/截断/时序）、\n");
        printf("          get_registry 之外的请求处理，或客户端 bind 之后的交互。\n");
    } else {
        printf("字节序列有问题：N = %d != %d\n", n_real, MAX_GLOBALS);
        printf("第一个出问题的字节见上方结构校验报告。\n");
    }

    printf("负对照检出 %d/3 个变异（应为 3/3）：m1=%d(m%d,v%d) m2=%d(m%d,v%d) m3=%d(m%d,v%d)\n",
           detected, d1, m1, v1, d2, m2, v2, d3, m3, v3);

    return (n_real == MAX_GLOBALS && detected == 3) ? 0 : 1;
}
