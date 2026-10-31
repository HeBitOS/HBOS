/* 最小 Wayland 客户端：验证 HBOS 的 wayland-0 服务端能不能握手。
 *
 * 不用 libwayland（HBOS 上还没有这个 .so），直接按线协议手写：
 *   socket(AF_UNIX) → connect("/tmp/wayland-0")
 *   → wl_display.get_registry(new_id=2)
 *   → wl_display.sync(new_id=3)
 *   → 读事件，把 wl_registry.global 全打出来，直到 callback.done。
 *
 * 退出码：
 *   0 = 收到至少一个 global + callback.done（握手成功）
 *   1 = connect 失败
 *   2 = 一个 global 都没收到
 *   3 = 没等到 callback.done
 */

#define SYS_WRITE   1
#define SYS_EXIT    60
#define SYS_SOCKET  41
#define SYS_CONNECT 42
#define SYS_READ    0
#define SYS_CLOSE   3

#define AF_UNIX     1
#define SOCK_STREAM 1

static long raw_syscall3(long nr, long a0, long a1, long a2) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return result;
}

static void out(const char *text) {
    long length = 0;
    while (text[length]) length++;
    raw_syscall3(SYS_WRITE, 1, (long)text, length);
}

static void out_dec(unsigned long value) {
    char buffer[24];
    int position = 0;
    if (!value) buffer[position++] = '0';
    while (value) {
        buffer[position++] = (char)('0' + (value % 10));
        value /= 10;
    }
    char reversed[24];
    for (int i = 0; i < position; i++) reversed[i] = buffer[position - 1 - i];
    raw_syscall3(SYS_WRITE, 1, (long)reversed, position);
}

struct sockaddr_un_local {
    unsigned short family;
    char path[108];
};

static unsigned char g_buffer[8192];
static unsigned int g_length;

/* 从连接里读满 needed 字节。返回 0 成功。 */
static int read_exact(long fd, unsigned char *dst, unsigned int needed) {
    unsigned int got = 0;
    while (got < needed) {
        long result = raw_syscall3(SYS_READ, fd, (long)(dst + got),
                                   needed - got);
        if (result <= 0) return -1;
        got += (unsigned int)result;
    }
    return 0;
}

static unsigned int read_u32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

int main(void) {
    out("wayland-test: connecting to /tmp/wayland-0\n");

    long fd = raw_syscall3(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        out("wayland-test: socket() failed\n");
        return 1;
    }

    struct sockaddr_un_local address;
    for (unsigned long i = 0; i < sizeof(address); i++)
        ((unsigned char *)&address)[i] = 0;
    address.family = AF_UNIX;
    const char *path = "/tmp/wayland-0";
    unsigned int path_length = 0;
    while (path[path_length]) {
        address.path[path_length] = path[path_length];
        path_length++;
    }

    long connected = raw_syscall3(SYS_CONNECT, fd, (long)&address,
                                  sizeof(address.family) + path_length + 1);
    if (connected < 0) {
        out("wayland-test: connect() failed, errno=");
        out_dec((unsigned long)(-connected));
        out("\n");
        return 1;
    }
    out("wayland-test: connected\n");

    /* wl_display.get_registry(2)
     * header: object=1, (size=12)<<16 | opcode=1 */
    unsigned char request[12];
    unsigned int *words = (unsigned int *)request;
    words[0] = 1;            /* wl_display */
    words[1] = (12U << 16) | 1U;  /* get_registry */
    words[2] = 2;            /* new registry id */
    raw_syscall3(SYS_WRITE, fd, (long)request, 12);

    /* wl_display.sync(3) */
    words[0] = 1;
    words[1] = (12U << 16) | 0U;  /* sync */
    words[2] = 3;
    raw_syscall3(SYS_WRITE, fd, (long)request, 12);

    unsigned int globals = 0;
    int saw_done = 0;

    for (int guard = 0; guard < 64 && !saw_done; guard++) {
        unsigned char header[8];
        if (read_exact(fd, header, 8) < 0) {
            out("wayland-test: connection closed\n");
            break;
        }
        unsigned int object_id = read_u32(header);
        unsigned int word = read_u32(header + 4);
        unsigned int opcode = word & 0xffffU;
        unsigned int size = word >> 16;
        if (size < 8 || size > 4096) {
            out("wayland-test: bad event size\n");
            break;
        }
        unsigned int body = size - 8;
        if (body && read_exact(fd, g_buffer, body) < 0) break;
        g_length = body;

        if (object_id == 2 && opcode == 0) {   /* wl_registry.global */
            unsigned int name = read_u32(g_buffer);
            unsigned int string_length = read_u32(g_buffer + 4);
            char name_buffer[64];
            unsigned int copy = string_length;
            if (copy > 63) copy = 63;
            for (unsigned int i = 0; i < copy; i++) {
                char c = (char)g_buffer[8 + i];
                name_buffer[i] = (c >= 32 && c < 127) ? c : '?';
            }
            name_buffer[copy] = 0;
            unsigned int version = read_u32(g_buffer + 8 +
                                            ((string_length + 3) & ~3U));
            out("wayland-test: global ");
            out_dec(name);
            out(" ");
            out(name_buffer);
            out(" v");
            out_dec(version);
            out("\n");
            globals++;
        } else if (object_id == 3 && opcode == 0) {  /* wl_callback.done */
            out("wayland-test: sync done\n");
            saw_done = 1;
        } else {
            out("wayland-test: event obj=");
            out_dec(object_id);
            out(" op=");
            out_dec(opcode);
            out("\n");
        }
    }

    raw_syscall3(SYS_CLOSE, fd, 0, 0);

    if (!globals) {
        out("wayland-test: FAIL, no globals\n");
        return 2;
    }
    if (!saw_done) {
        out("wayland-test: FAIL, no sync done\n");
        return 3;
    }
    out("wayland-test: PASS\n");
    return 0;
}

/* 入口交给 glibc 的 crt —— HBOS 要求 ET_DYN（PIE），自己写 _start 就
 * 没法用 -pie 的默认启动代码。所有系统调用仍然是 raw syscall，
 * 这样测的是内核的协议处理，不是 libc 的封装。 */
