/**
 * @file    devfs.c
 * @brief   /dev + /proc 伪文件系统实现
 *
 * 通过合成 VFS 节点和自定义操作接口，提供:
 *   /dev/null, /dev/zero, /dev/console
 *   /proc/uptime, /proc/meminfo, /proc/cpuinfo, /proc/<pid>/...
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "devfs.h"
#include "core/task.h"
#include "core/pmm.h"
#include "core/heap.h"
#include "string.h"
#include "shell/shell.h"
#include "crypto/chacha20_poly1305.h"

#define DEVFS_MAX_NODES 64

static vfs_node_t devfs_nodes[DEVFS_MAX_NODES];
static int devfs_node_count;

/** 用于 proc 动态内容的格式化缓冲区大小 */
#define PROC_BUF_SIZE 1024

/** 全局系统启动 TSC 计数器 */
static uint64_t boot_tsc;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void set_name(vfs_node_t *n, const char *s) {
    int i = 0;
    while (s[i] && i < VFS_MAX_NAME - 1) { n->name[i] = s[i]; i++; }
    n->name[i] = '\0';
}

/** 更新 proc 动态文件内容到私有缓冲区 */
static void proc_update(vfs_node_t *node) {
    char *buf = (char *)node->private_data;
    if (!buf) return;
    memset(buf, 0, PROC_BUF_SIZE);

    if (strcmp(node->name, "uptime") == 0) {
        uint64_t tsc = rdtsc();
        uint64_t sec = (tsc - boot_tsc) / 1000000000ULL;
        uint64_t usec = ((tsc - boot_tsc) % 1000000000ULL) / 1000;
        int pos = 0;
        uint64_t v = sec;
        char tmp[32]; int n = 0;
        do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
        while (n--) buf[pos++] = tmp[n];
        buf[pos++] = '.';
        v = usec; n = 0;
        do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
        while (n--) buf[pos++] = tmp[n];
        buf[pos++] = ' ';
        // 1 minute load average (dummy)
        buf[pos++] = '0'; buf[pos++] = '.'; buf[pos++] = '0'; buf[pos++] = '0';
        buf[pos++] = ' ';
        // 5 minute load (dummy)
        buf[pos++] = '0'; buf[pos++] = '.'; buf[pos++] = '0'; buf[pos++] = '0';
        buf[pos++] = ' ';
        // 15 minute load (dummy)
        buf[pos++] = '0'; buf[pos++] = '.'; buf[pos++] = '0'; buf[pos++] = '0';
        buf[pos++] = '\n';
        node->size = (uint32_t)pos;

    } else if (strcmp(node->name, "meminfo") == 0) {
        char tmp[32]; int n;
        int pos = 0;

        const char *h1 = "MemTotal:      ";
        while (*h1) buf[pos++] = *h1++;
        uint64_t total = pmm_get_total_mem() / 1024;
        n = 0; do { tmp[n++] = '0' + (total % 10); total /= 10; } while (total);
        while (n--) buf[pos++] = tmp[n];
        buf[pos++] = ' '; buf[pos++] = 'k'; buf[pos++] = 'B'; buf[pos++] = '\n';

        const char *h2 = "MemFree:       ";
        while (*h2) buf[pos++] = *h2++;
        uint64_t free_kb = pmm_get_free_mem() / 1024;
        n = 0; do { tmp[n++] = '0' + (free_kb % 10); free_kb /= 10; } while (free_kb);
        while (n--) buf[pos++] = tmp[n];
        buf[pos++] = ' '; buf[pos++] = 'k'; buf[pos++] = 'B'; buf[pos++] = '\n';

        node->size = (uint32_t)pos;

    } else if (strcmp(node->name, "cpuinfo") == 0) {
        int pos = 0;
        const char *h = "vendor_id   : HBOS CPU\n"
                        "model name  : HBOS x86_64 Virtual Processor\n"
                        "cpu MHz     : 1000.000\n"
                        "cache size  : 4096 KB\n"
                        "processor   : 0\n"
                        "bogomips    : 2000.00\n";
        while (*h) buf[pos++] = *h++;
        node->size = (uint32_t)pos;
    }
}

static int devfs_null_read(vfs_node_t *node, uint32_t offset, void *b, uint32_t count) {
    (void)node; (void)offset; (void)b; (void)count;
    return 0;
}

static int devfs_null_write(vfs_node_t *node, uint32_t offset, const void *b, uint32_t count) {
    (void)node; (void)offset; (void)b;
    return (int)count;
}

static int devfs_zero_read(vfs_node_t *node, uint32_t offset, void *b, uint32_t count) {
    (void)node; (void)offset;
    memset(b, 0, count);
    return (int)count;
}

static int devfs_console_read(vfs_node_t *node, uint32_t offset, void *b, uint32_t count) {
    (void)node; (void)offset;
    char *buf = (char *)b;
    int i = 0;
    while (i < (int)count) {
        int key = kb_get_key();
        if (key < 0 || key > 0xff) continue;
        buf[i++] = (char)key;
        if (key == '\n') break;
    }
    return i;
}

static int devfs_console_write(vfs_node_t *node, uint32_t offset, const void *b, uint32_t count) {
    (void)node; (void)offset;
    extern void console_write(const char *, int);
    console_write((const char *)b, (int)count);
    return (int)count;
}

static int devfs_proc_read(vfs_node_t *node, uint32_t offset, void *b, uint32_t count) {
    if (!node || !b) return -1;
    proc_update(node);
    char *buf = (char *)node->private_data;
    if (!buf) return -1;
    uint32_t sz = node->size;
    if (offset >= sz) return 0;
    uint32_t avail = sz - offset;
    if (count > avail) count = avail;
    memcpy(b, buf + offset, count);
    return (int)count;
}

static int devfs_proc_write(vfs_node_t *node, uint32_t offset, const void *b, uint32_t count) {
    (void)node; (void)offset; (void)b; (void)count;
    return -1;
}

static const vfs_ops_t devfs_null_ops = {
    .read = devfs_null_read,
    .write = devfs_null_write,
};

static const vfs_ops_t devfs_zero_ops = {
    .read = devfs_zero_read,
    .write = devfs_null_write,
};

static const vfs_ops_t devfs_console_ops = {
    .read = devfs_console_read,
    .write = devfs_console_write,
};

static const vfs_ops_t devfs_proc_ops = {
    .read = devfs_proc_read,
    .write = devfs_proc_write,
};

static int devfs_random_read(vfs_node_t *node, uint32_t offset, void *b, uint32_t count) {
    (void)node; (void)offset;
    static uint8_t seed[32];
    static int seeded;
    if (!seeded) {
        uint64_t t = rdtsc();
        memcpy(seed, &t, sizeof(t));
        memcpy(seed + 8, &t, sizeof(t));
        seeded = 1;
    }
    uint8_t key[32], nonce[12];
    memcpy(key, seed, 32);
    uint64_t c = rdtsc();
    memcpy(nonce, &c, sizeof(c));
    chacha20_xor(key, 0, nonce, (uint8_t *)b, (uint8_t *)b, count);
    uint64_t t2 = rdtsc();
    for (uint32_t i = 0; i < 16; i++)
        seed[i] ^= (uint8_t)(t2 >> (i * 4));
    return (int)count;
}

static const vfs_ops_t devfs_random_ops = {
    .read = devfs_random_read,
    .write = devfs_null_write,
};

static vfs_node_t *devfs_alloc_node(void) {
    if (devfs_node_count >= DEVFS_MAX_NODES) return NULL;
    vfs_node_t *n = &devfs_nodes[devfs_node_count++];
    memset(n, 0, sizeof(*n));
    n->capacity = PROC_BUF_SIZE;
    return n;
}

/** 注册静态节点 */
void devfs_init(void) {
    boot_tsc = rdtsc();
    devfs_node_count = 0;

    // /dev/null
    vfs_node_t *n = devfs_alloc_node();
    set_name(n, "/dev/null");
    n->type = VFS_NODE_CHARDEV;
    n->ops = &devfs_null_ops;

    // /dev/zero
    n = devfs_alloc_node();
    set_name(n, "/dev/zero");
    n->type = VFS_NODE_CHARDEV;
    n->ops = &devfs_zero_ops;

    // /dev/console
    n = devfs_alloc_node();
    set_name(n, "/dev/console");
    n->type = VFS_NODE_CHARDEV;
    n->ops = &devfs_console_ops;

    // /proc/uptime
    n = devfs_alloc_node();
    set_name(n, "/proc/uptime");
    n->type = VFS_NODE_FILE;
    n->ops = &devfs_proc_ops;
    n->private_data = kmalloc(PROC_BUF_SIZE);
    memset(n->private_data, 0, PROC_BUF_SIZE);

    // /proc/meminfo
    n = devfs_alloc_node();
    set_name(n, "/proc/meminfo");
    n->type = VFS_NODE_FILE;
    n->ops = &devfs_proc_ops;
    n->private_data = kmalloc(PROC_BUF_SIZE);
    memset(n->private_data, 0, PROC_BUF_SIZE);

    // /proc/cpuinfo
    n = devfs_alloc_node();
    set_name(n, "/proc/cpuinfo");
    n->type = VFS_NODE_FILE;
    n->ops = &devfs_proc_ops;
    n->private_data = kmalloc(PROC_BUF_SIZE);
    memset(n->private_data, 0, PROC_BUF_SIZE);

    // /dev/random
    n = devfs_alloc_node();
    set_name(n, "/dev/random");
    n->type = VFS_NODE_CHARDEV;
    n->ops = &devfs_random_ops;

    // /dev/urandom
    n = devfs_alloc_node();
    set_name(n, "/dev/urandom");
    n->type = VFS_NODE_CHARDEV;
    n->ops = &devfs_random_ops;
}

/** 按路径查找伪文件系统节点 */
vfs_node_t *devfs_lookup(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < devfs_node_count; i++) {
        if (strcmp(devfs_nodes[i].name, path) == 0)
            return &devfs_nodes[i];
    }

    // 动态 /proc/<pid>/* 节点
    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
        path[3] == 'o' && path[4] == 'c' && path[5] == '/') {
        const char *rest = path + 6;
        if (!rest[0]) return NULL;

        int pid = 0;
        while (*rest >= '0' && *rest <= '9') {
            pid = pid * 10 + (*rest - '0');
            rest++;
        }
        if (pid == 0) return NULL;

        const task_t *t = task_get_by_id((uint32_t)pid);
        if (!t || t->state == TASK_TERMINATED) return NULL;

        if (rest[0] == '/' && (strcmp(rest + 1, "cmdline") == 0 ||
                                strcmp(rest + 1, "status") == 0)) {
            vfs_node_t *n = devfs_alloc_node();
            if (!n) return NULL;
            set_name(n, path);
            n->type = VFS_NODE_FILE;
            n->ops = &devfs_proc_ops;
            n->private_data = kmalloc(PROC_BUF_SIZE);
            if (!n->private_data) { devfs_node_count--; return NULL; }
            memset(n->private_data, 0, PROC_BUF_SIZE);

            char *buf = (char *)n->private_data;
            int pos = 0;
            if (strcmp(rest + 1, "cmdline") == 0) {
                const char *nm = t->name;
                while (*nm) buf[pos++] = *nm++;
                buf[pos++] = '\0';
                n->size = (uint32_t)pos;
            } else {
                const char *h = "Name:\t";
                while (*h) buf[pos++] = *h++;
                const char *nm = t->name;
                while (*nm) buf[pos++] = *nm++;
                buf[pos++] = '\n';
                const char *h2 = "State:\t";
                while (*h2) buf[pos++] = *h2++;
                const char *state_str;
                switch (t->state) {
                    case TASK_READY:      state_str = "R (running)"; break;
                    case TASK_RUNNING:    state_str = "R (running)"; break;
                    case TASK_BLOCKED:    state_str = "S (sleeping)"; break;
                    case TASK_TERMINATED: state_str = "Z (zombie)"; break;
                    default:              state_str = "?"; break;
                }
                while (*state_str) buf[pos++] = *state_str++;
                buf[pos++] = '\n';
                n->size = (uint32_t)pos;
            }
            return n;
        }
    }

    return NULL;
}