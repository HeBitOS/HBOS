/* Wayland 完整链路测试：shm 像素 → surface → xdg 窗口 → 上屏。
 *
 * 覆盖四件事，任何一步失败都会卡住或返回非 0：
 *   1. wl_shm.create_pool 的 fd 经 SCM_RIGHTS 送达服务端；
 *   2. 无 buffer 的首次 commit 能换来 xdg_toplevel.configure +
 *      xdg_surface.configure（拿到 serial 才能继续）；
 *   3. ack_configure + attach + commit 之后服务端能把像素读出来贴屏；
 *   4. buffer release 事件能回来（客户端据此复用内存）。
 *
 * 画的内容是一个可辨识的图案：左上角绿色方块 + 其余红色，方便截图核对。
 *
 * 退出码：0 全通；1 connect 失败；2 缺 global；3 没收到 configure；
 *         4 没收到 buffer release；5 中间某步协议错误。
 */

#define SYS_READ       0
#define SYS_WRITE      1
#define SYS_CLOSE      3
#define SYS_MMAP       9
#define SYS_SOCKET     41
#define SYS_CONNECT    42
#define SYS_FTRUNCATE  77
#define SYS_SENDMSG    46
#define SYS_EXIT       60
#define SYS_MEMFD      319

#define AF_UNIX        1
#define SOCK_STREAM    1
#define MAP_SHARED     0x01
#define MAP_ANONYMOUS  0x20

#define WIDTH   240
#define HEIGHT  160
#define STRIDE  (WIDTH * 4)
#define POOL_SZ (STRIDE * HEIGHT)

static long raw3(long nr, long a0, long a1, long a2) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return r;
}

static long raw6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2),
                       "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

static void out(const char *text) {
    long n = 0;
    while (text[n]) n++;
    raw3(SYS_WRITE, 1, (long)text, n);
}

static void out_dec(long value) {
    char buf[24];
    int pos = 0;
    if (value < 0) { out("-"); value = -value; }
    if (!value) buf[pos++] = '0';
    while (value) { buf[pos++] = (char)('0' + value % 10); value /= 10; }
    char rev[24];
    for (int i = 0; i < pos; i++) rev[i] = buf[pos - 1 - i];
    raw3(SYS_WRITE, 1, (long)rev, pos);
}

static unsigned int rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* 请求缓冲：一条请求一个 buf，写完直接发。 */
static unsigned char req[512];
static int req_len;

static void req_reset(void) { req_len = 0; }

/* 只写 object + opcode，size 留到发送前回填 —— 手写 size 太容易算错：
 * 字符串长度要含 NUL 再补齐到 4 字节，interface 名不同长度就不同，
 * 算错一位服务端就按错误的长度切分，后面所有请求跟着错位。 */
static void req_header(unsigned object, unsigned opcode) {
    unsigned *w = (unsigned *)(void *)req;
    w[0] = object;
    w[1] = opcode;
    req_len = 8;
}

static void req_finish(void) {
    unsigned *w = (unsigned *)(void *)req;
    w[1] = (w[1] & 0xffffU) | ((unsigned)req_len << 16);
}

static void req_u32(unsigned value) {
    *(unsigned *)(void *)&req[req_len] = value;
    req_len += 4;
}

static void req_string(const char *text) {
    unsigned n = 0;
    while (text[n]) n++;
    req_u32(n + 1);
    for (unsigned i = 0; i <= n; i++) req[req_len++] = (unsigned char)text[i];
    while (req_len & 3) req[req_len++] = 0;
}

static int send_req(long fd) {
    req_finish();
    long done = 0;
    while (done < req_len) {
        long r = raw3(SYS_WRITE, fd, (long)(req + done), req_len - done);
        if (r <= 0) return -1;
        done += r;
    }
    return 0;
}

/* 带 SCM_RIGHTS 的写：create_pool 需要把 memfd 传给服务端。 */
struct cmsghdr_local {
    unsigned long cmsg_len;
    int cmsg_level;
    int cmsg_type;
    int fds[1];
};

struct msghdr_local {
    void *msg_name;
    unsigned msg_namelen;
    void *msg_iov;
    unsigned long msg_iovlen;
    void *msg_control;
    unsigned long msg_controllen;
    unsigned msg_flags;
};

struct iovec_local { void *iov_base; unsigned long iov_len; };

/* cmsg_len 必须是 CMSG_LEN(sizeof(int)) = sizeof(cmsghdr) + 4 = 20，
 * 不是 sizeof(struct cmsghdr_local)。后者末尾按 8 字节对齐补到 24，
 * 内核据此算出「带了 2 个 fd」，把第二个槽里的垃圾当 fd 处理，整条
 * sendmsg 直接 EINVAL。 */
#define CMSG_LEN_ONE_FD 20
#define CMSG_SPACE_ONE_FD 24

static int send_req_with_fd(long fd, int passed_fd) {
    req_finish();
    struct iovec_local iov;
    iov.iov_base = req;
    iov.iov_len = (unsigned long)req_len;
    unsigned char control[CMSG_SPACE_ONE_FD];
    for (unsigned i = 0; i < sizeof(control); i++) control[i] = 0;
    struct cmsghdr_local *cmsg = (struct cmsghdr_local *)(void *)control;
    cmsg->cmsg_len = CMSG_LEN_ONE_FD;
    cmsg->cmsg_level = 1;   /* SOL_SOCKET */
    cmsg->cmsg_type = 1;    /* SCM_RIGHTS */
    cmsg->fds[0] = passed_fd;
    struct msghdr_local msg;
    for (unsigned i = 0; i < sizeof(msg); i++) ((unsigned char *)&msg)[i] = 0;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE_ONE_FD;
    long r = raw3(SYS_SENDMSG, fd, (long)&msg, 0);
    if (r < 0) {
        out("wl-surface: sendmsg ret=");
        out_dec(r);
        out("\n");
        return -1;
    }
    return 0;
}

static unsigned char event_body[4096];

/* 读一条事件；返回 0 成功，-1 连接断，-2 暂时没数据（非阻塞）。 */
static int read_event(long fd, unsigned *object, unsigned *opcode,
                      unsigned *body_len) {
    unsigned char header[8];
    unsigned got = 0;
    while (got < 8) {
        long r = raw3(SYS_READ, fd, (long)(header + got), 8 - got);
        if (r == -11 /* -EAGAIN */) return -2;
        if (r <= 0) {
            out("wl-surface: read ret=");
            out_dec(r);
            out("\n");
            return -1;
        }
        got += (unsigned)r;
    }
    *object = rd32(header);
    unsigned word = rd32(header + 4);
    *opcode = word & 0xffffU;
    unsigned size = word >> 16;
    if (size < 8 || size > 4096) {
        out("wl-surface: bad event size=");
        out_dec((long)size);
        out("\n");
        return -1;
    }
    *body_len = size - 8;
    got = 0;
    while (got < *body_len) {
        long r = raw3(SYS_READ, fd, (long)(event_body + got), *body_len - got);
        if (r == -11) continue;
        if (r <= 0) return -1;
        got += (unsigned)r;
    }
    return 0;
}

struct sockaddr_un_local {
    unsigned short family;
    char path[108];
};

int main(void) {
    out("wl-surface: start\n");
    /* 非阻塞：读事件时能立刻看到 EAGAIN，而不是卡死在 read 里 ——
     * 排查"服务端说发了、客户端没收到"这类问题时这是唯一的线索。 */
    long fd = raw3(SYS_SOCKET, AF_UNIX, SOCK_STREAM | 0x800, 0);
    if (fd < 0) { out("wl-surface: socket failed\n"); return 1; }
    struct sockaddr_un_local addr;
    for (unsigned i = 0; i < sizeof(addr); i++) ((unsigned char *)&addr)[i] = 0;
    addr.family = AF_UNIX;
    const char *path = "/tmp/wayland-0";
    unsigned plen = 0;
    while (path[plen]) { addr.path[plen] = path[plen]; plen++; }
    if (raw3(SYS_CONNECT, fd, (long)&addr, sizeof(addr.family) + plen + 1) < 0) {
        out("wl-surface: connect failed\n");
        return 1;
    }
    out("wl-surface: connected\n");

    /* 1) 取全局对象列表 */
    req_reset();
    req_header(1, 1);
    req_u32(2);             /* registry id */
    if (send_req(fd) < 0) return 5;

    /* 同时发一个 sync：没有它就没有任何"处理完了"的信号，
     * 循环会一直等下去（踩过这个坑，看起来像服务端没回）。 */
    req_reset();
    req_header(1, 0);
    req_u32(3);             /* callback id */
    if (send_req(fd) < 0) return 5;

    unsigned compositor_name = 0, shm_name = 0, xdg_name = 0, seat_name = 0;
    int saw_done = 0;
    int empty_rounds = 0;
    while (!saw_done) {
        unsigned object, opcode, len;
        int state = read_event(fd, &object, &opcode, &len);
        if (state == -2) {
            /* 非阻塞暂时没数据：让服务线程有机会跑一轮再试。 */
            if (++empty_rounds > 200000) {
                out("wl-surface: timed out waiting for globals\n");
                return 5;
            }
            continue;
        }
        if (state < 0) {
            out("wl-surface: read failed\n");
            return 5;
        }
        empty_rounds = 0;
        if (object == 3 && opcode == 0) {   /* wl_callback.done */
            saw_done = 1;
            continue;
        }
        if (object == 2 && opcode == 0) {
            unsigned name = rd32(event_body);
            unsigned slen = rd32(event_body + 4);
            char iface[64];
            unsigned copy = slen < 63 ? slen : 63;
            for (unsigned i = 0; i < copy; i++) iface[i] = (char)event_body[8 + i];
            iface[copy] = 0;
            if (iface[copy ? copy - 1 : 0] == 0) iface[copy ? copy - 1 : 0] = 0;
            out("wl-surface: global ");
            out_dec((long)name);
            out(" ");
            out(iface);
            out("\n");
            if (iface[0] == 'w' && iface[1] == 'l' && iface[2] == '_' &&
                iface[3] == 'c')
                compositor_name = name;
            else if (iface[0] == 'w' && iface[1] == 'l' && iface[2] == '_' &&
                     iface[3] == 's' && iface[4] == 'h')
                shm_name = name;
            else if (iface[0] == 'w' && iface[1] == 'l' && iface[2] == '_' &&
                     iface[3] == 's' && iface[4] == 'e')
                seat_name = name;
            else if (iface[0] == 'x')
                xdg_name = name;
        }
    }
    (void)plen;
    if (!compositor_name || !shm_name || !xdg_name) {
        out("wl-surface: missing globals\n");
        return 2;
    }
    /* 兜底：服务端的全局编号是固定顺序（compositor/shm/seat/xdg_wm_base），
     * 万一名字匹配没命中也不至于整段输入测试被跳过。 */
    if (!seat_name) seat_name = 3;
    out("wl-surface: globals ok\n");

    /* 2) bind 三个全局对象 */
    req_reset();
    req_header(2, 0);
    req_u32(compositor_name);
    req_string("wl_compositor");
    req_u32(4);
    req_u32(10);
    if (send_req(fd) < 0) return 5;

    req_reset();
    req_header(2, 0);
    req_u32(shm_name);
    req_string("wl_shm");
    req_u32(1);
    req_u32(11);
    if (send_req(fd) < 0) return 5;

    req_reset();
    req_header(2, 0);
    req_u32(xdg_name);
    req_string("xdg_wm_base");
    req_u32(1);
    req_u32(12);
    if (send_req(fd) < 0) return 5;

    /* 3) 建 surface / pool / buffer */
    req_reset();
    req_header(10, 0);
    req_u32(13);
    if (send_req(fd) < 0) return 5;

    /* 3b) 顺带建 pointer + keyboard：验证服务端能把输入事件推回来。
     * 服务端带 --probe-input 启动时，窗口首次上屏后会注入一次
     * enter/motion/button 和一次 key，这里负责把它们读出来。 */
    out("wl-surface: seat name=");
    out_dec((long)seat_name);
    out("\n");
    {
        req_reset();
        req_header(2, 0 /* bind */);
        req_u32(seat_name);
        req_string("wl_seat");
        req_u32(5);
        req_u32(18);
        if (send_req(fd) < 0) return 5;

        req_reset();
        req_header(18, 0 /* get_pointer */);
        req_u32(20);
        if (send_req(fd) < 0) return 5;

        req_reset();
        req_header(18, 1 /* get_keyboard */);
        req_u32(21);
        if (send_req(fd) < 0) return 5;
        out("wl-surface: seat/pointer/keyboard created\n");
    }

    long memfd = raw3(SYS_MEMFD, (long)"wl-test", 0, 0);
    if (memfd < 0) { out("wl-surface: memfd failed\n"); return 5; }
    if (raw3(SYS_FTRUNCATE, memfd, POOL_SZ, 0) < 0) {
        out("wl-surface: ftruncate failed\n");
        return 5;
    }
    unsigned char *pixels = (unsigned char *)raw6(
        SYS_MMAP, 0, POOL_SZ, 3 /* PROT_READ|WRITE */,
        MAP_SHARED, memfd, 0);
    if ((long)pixels < 0) {
        out("wl-surface: mmap failed\n");
        return 5;
    }

    req_reset();
    req_header(11, 0);
    req_u32(14);
    req_u32(0);            /* fd 索引（真 fd 走 SCM_RIGHTS） */
    req_u32(POOL_SZ);
    if (send_req_with_fd(fd, (int)memfd) < 0) {
        out("wl-surface: sendmsg pool failed\n");
        return 5;
    }

    req_reset();
    req_header(14, 0);
    req_u32(15);
    req_u32(0);            /* offset */
    req_u32(WIDTH);
    req_u32(HEIGHT);
    req_u32(STRIDE);
    req_u32(0);            /* ARGB8888 */
    if (send_req(fd) < 0) return 5;

    /* 4) xdg 窗口 */
    req_reset();
    req_header(12, 2);
    req_u32(16);
    req_u32(13);
    if (send_req(fd) < 0) return 5;

    req_reset();
    req_header(16, 1);
    req_u32(17);
    if (send_req(fd) < 0) return 5;

    req_reset();
    req_header(17, 2);
    req_string("wl-surface test");
    if (send_req(fd) < 0) return 5;

    /* 5) 首次 commit（无 buffer）→ 期待 configure */
    req_reset();
    req_header(13, 6);
    if (send_req(fd) < 0) return 5;

    unsigned surface_serial = 0;
    int got_configure = 0;
    int idle = 0;
    /* 上限要按"空转次数"给，不能按"收到的消息数"给：非阻塞读在服务端
     * 还没处理完时返回 EAGAIN，也会消耗一次循环，32 轮根本不够。 */
    while (!got_configure && idle < 300000) {
        unsigned object, opcode, len;
        int state = read_event(fd, &object, &opcode, &len);
        if (state == -2) { idle++; continue; }
        if (state < 0) break;
        idle = 0;
        if (object == 16 && opcode == 0) {          /* xdg_surface.configure */
            surface_serial = rd32(event_body);
            got_configure = 1;
        }
    }
    if (!got_configure) {
        out("wl-surface: no configure\n");
        return 3;
    }
    out("wl-surface: configure ok, serial=");
    out_dec((long)surface_serial);
    out("\n");

    /* 6) ack + 画像素 + attach + commit */
    req_reset();
    req_header(16, 4);
    req_u32(surface_serial);
    if (send_req(fd) < 0) return 5;

    /* 左上角绿色方块，其余红色；alpha 全 0xFF。 */
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            unsigned color = (x < 80 && y < 60) ? 0xFF00FF00u : 0xFFFF0000u;
            unsigned *p = (unsigned *)(void *)(pixels + y * STRIDE + x * 4);
            *p = color;
        }
    }

    req_reset();
    req_header(13, 1);
    req_u32(15);
    req_u32(0);
    req_u32(0);
    if (send_req(fd) < 0) return 5;

    req_reset();
    req_header(13, 2);
    req_u32(0);
    req_u32(0);
    req_u32(WIDTH);
    req_u32(HEIGHT);
    if (send_req(fd) < 0) return 5;

    req_reset();
    req_header(13, 6);
    if (send_req(fd) < 0) return 5;

    /* 7) 等 buffer release：服务端宣告"这块我用完了" */
    int released = 0;
    idle = 0;
    while (!released && idle < 300000) {
        unsigned object, opcode, len;
        int state = read_event(fd, &object, &opcode, &len);
        if (state == -2) { idle++; continue; }
        if (state < 0) break;
        idle = 0;
        if (object == 15 && opcode == 0) released = 1;
    }
    if (!released) {
        out("wl-surface: no buffer release (frame may still be drawn)\n");
        return 4;
    }
    out("wl-surface: buffer released\n");

    /* 8) 等输入事件。服务端带 --probe-input 启动时，窗口上屏后会注入
     * enter/motion/button + key；这里只关心"有没有收到"。 */
    if (seat_name) {
        int got_pointer = 0, got_keyboard = 0;
        idle = 0;
        while ((!got_pointer || !got_keyboard) && idle < 300000) {
            unsigned object, opcode, len;
            int state = read_event(fd, &object, &opcode, &len);
            if (state == -2) { idle++; continue; }
            if (state < 0) break;
            idle = 0;
            if (object == 20 && opcode == 2) got_pointer = 1;   /* motion */
            if (object == 21 && opcode == 3) got_keyboard = 1;  /* key */
        }
        if (got_pointer) out("wl-surface: pointer ok\n");
        if (got_keyboard) out("wl-surface: keyboard ok\n");
        if (!got_pointer && !got_keyboard) {
            out("wl-surface: no input events (probe off?)\n");
        }
    }

    out("wl-surface: PASS\n");
    raw3(SYS_CLOSE, fd, 0, 0);
    return 0;
}
