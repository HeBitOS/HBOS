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
#include "core/vmm.h"
#include "sys/stat.h"
#include "smp.h"
#include "net.h"
#include "core/heap.h"
#include "string.h"
#include "unistd.h"
#include "shell/shell.h"
#include "crypto/chacha20_poly1305.h"

#define DEVFS_MAX_NODES 192

static vfs_node_t devfs_nodes[DEVFS_MAX_NODES];
static int devfs_node_count;

/** 用于 proc 动态内容的格式化缓冲区大小 */
#define PROC_BUF_SIZE 4096

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

static void set_short_name(char *out, const char *s) {
    uint32_t i = 0;
    while (s[i] && i < VFS_MAX_NAME - 1) {
        out[i] = s[i];
        i++;
    }
    out[i] = 0;
}

static void u32_to_name(uint32_t value, char *out) {
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value && n < (int)sizeof(tmp));
    int pos = 0;
    while (n > 0 && pos < VFS_MAX_NAME - 1)
        out[pos++] = tmp[--n];
    out[pos] = 0;
}

static const char *basename_after(const char *path, const char *prefix) {
    uint32_t i = 0;
    while (prefix[i]) {
        if (path[i] != prefix[i]) return NULL;
        i++;
    }
    const char *name = path + i;
    if (!name[0] || strchr(name, '/')) return NULL;
    return name;
}

/** 更新 proc 动态文件内容到私有缓冲区 */
static void proc_append(char *buf, int *position, const char *text) {
    while (*text && *position < PROC_BUF_SIZE - 1)
        buf[(*position)++] = *text++;
}

static void proc_append_u64(char *buf, int *position, uint64_t value) {
    char temporary[32];
    int length = 0;
    do {
        temporary[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && length < (int)sizeof(temporary));
    while (length && *position < PROC_BUF_SIZE - 1)
        buf[(*position)++] = temporary[--length];
}

static void proc_append_hex(char *buf, int *position, uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0 && *position < PROC_BUF_SIZE - 1;
         shift -= 4)
        buf[(*position)++] = digits[(value >> shift) & 15U];
}

static const task_t *proc_task_from_path(const char *path,
                                         const char **leaf) {
    if (!path || strncmp(path, "/proc/", 6) != 0) return NULL;
    const char *cursor = path + 6;
    uint32_t pid = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        pid = pid * 10U + (uint32_t)(*cursor - '0');
        cursor++;
    }
    if (!pid || *cursor != '/') return NULL;
    if (leaf) *leaf = cursor + 1;
    const task_t *task = task_get_by_id(pid);
    return task && task->state != TASK_TERMINATED ? task : NULL;
}

static void proc_append_status(char *buf, int *position,
                               const task_t *task) {
    const char *state = task->state == TASK_BLOCKED ? "S (sleeping)" :
        (task->state == TASK_TERMINATED ? "Z (zombie)" : "R (running)");
    uint64_t virtual_kb = task->stack_size / 1024;
    if (task->mm) {
        for (const vm_area_t *area = task->mm->areas; area; area = area->next)
            virtual_kb += (area->end - area->start) / 1024;
        if (task->mm->user_brk > task->mm->user_heap_start)
            virtual_kb += (task->mm->user_brk - task->mm->user_heap_start) /
                          1024;
    }
    uint32_t threads = 0;
    for (uint32_t index = 0;; index++) {
        const task_t *candidate = task_get_active(index);
        if (!candidate) break;
        if (candidate->thread_group_id == task->thread_group_id) threads++;
    }
    proc_append(buf, position, "Name:\t");
    proc_append(buf, position, task->name);
    proc_append(buf, position, "\nState:\t");
    proc_append(buf, position, state);
    proc_append(buf, position, "\nTgid:\t");
    proc_append_u64(buf, position, task->thread_group_id);
    proc_append(buf, position, "\nPid:\t");
    proc_append_u64(buf, position, task->id);
    proc_append(buf, position, "\nPPid:\t");
    proc_append_u64(buf, position, task->parent_id);
    proc_append(buf, position, "\nUid:\t");
    proc_append_u64(buf, position, task->uid);
    proc_append(buf, position, "\t");
    proc_append_u64(buf, position, task->euid);
    proc_append(buf, position, "\t0\t0\nGid:\t");
    proc_append_u64(buf, position, task->gid);
    proc_append(buf, position, "\t");
    proc_append_u64(buf, position, task->egid);
    proc_append(buf, position, "\t0\t0\n");
    proc_append(buf, position, "FDSize:\t128\nThreads:\t");
    proc_append_u64(buf, position, threads);
    proc_append(buf, position, "\nVmSize:\t");
    proc_append_u64(buf, position, virtual_kb);
    proc_append(buf, position, " kB\nVmRSS:\t");
    proc_append_u64(buf, position, virtual_kb);
    proc_append(buf, position,
                " kB\nSigQ:\t0/64\nSigPnd:\t0000000000000000\n"
                "SigBlk:\t0000000000000000\nSeccomp:\t0\n");
}

static void proc_append_map_line(char *buf, int *position, uint64_t start,
                                 uint64_t end, uint64_t page_flags,
                                 uint64_t offset, const char *label) {
    proc_append_hex(buf, position, start);
    proc_append(buf, position, "-");
    proc_append_hex(buf, position, end);
    proc_append(buf, position, (page_flags & VMM_U) ? " r" : " -");
    proc_append(buf, position, (page_flags & VMM_W) ? "w" : "-");
    proc_append(buf, position, (page_flags & VMM_NX) ? "-" : "x");
    proc_append(buf, position, "p ");
    proc_append_hex(buf, position, offset);
    proc_append(buf, position, " 00:00 0");
    if (label && label[0]) {
        proc_append(buf, position, " ");
        proc_append(buf, position, label);
    }
    proc_append(buf, position, "\n");
}

static void proc_append_maps(char *buf, int *position, const task_t *task) {
    if (task->mm) {
        for (const vm_area_t *area = task->mm->areas; area; area = area->next) {
            const char *label = area->backing_type == VM_BACKING_MEMFD ?
                "/memfd:hbos" : (area->backing_type == VM_BACKING_ANONYMOUS ?
                                  "" : "[file]");
            proc_append_map_line(buf, position, area->start, area->end,
                                 vmm_get_page_flags(area->start),
                                 area->backing_offset, label);
        }
        uint64_t heap_end = task->mm->user_brk;
        if (heap_end > task->mm->user_heap_start)
            proc_append_map_line(buf, position, task->mm->user_heap_start,
                                 heap_end, VMM_U | VMM_W | VMM_NX, 0,
                                 "[heap]");
    }
}

static void proc_update(vfs_node_t *node) {
    char *buf = (char *)node->private_data;
    if (!buf) return;
    memset(buf, 0, PROC_BUF_SIZE);

    if (strcmp(node->name, "/proc/uptime") == 0) {
        uint64_t ticks = pit_get_ticks();
        uint32_t frequency = pit_get_frequency_hz();
        uint64_t sec = frequency ? ticks / frequency : 0;
        uint64_t centisec = frequency ?
            ((ticks % frequency) * 100ULL / frequency) : 0;
        int pos = 0;
        proc_append_u64(buf, &pos, sec);
        proc_append(buf, &pos, ".");
        if (centisec < 10) proc_append(buf, &pos, "0");
        proc_append_u64(buf, &pos, centisec);
        proc_append(buf, &pos, " 0.00\n");
        node->size = (uint32_t)pos;

    } else if (strcmp(node->name, "/proc/meminfo") == 0) {
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

        const char *extra = "MemAvailable:  ";
        while (*extra) buf[pos++] = *extra++;
        free_kb = pmm_get_free_mem() / 1024;
        n = 0; do { tmp[n++] = '0' + (free_kb % 10); free_kb /= 10; } while (free_kb);
        while (n--) buf[pos++] = tmp[n];
        proc_append(buf, &pos,
                    " kB\nBuffers:       0 kB\nCached:        0 kB\n"
                    "SwapTotal:      0 kB\nSwapFree:       0 kB\n");
        node->size = (uint32_t)pos;

    } else if (strcmp(node->name, "/proc/cpuinfo") == 0) {
        int pos = 0;
        const char *h = "vendor_id   : HBOS CPU\n"
                        "model name  : HBOS x86_64 Virtual Processor\n"
                        "cpu MHz     : 1000.000\n"
                        "cache size  : 4096 KB\n"
                        "processor   : 0\n"
                        "bogomips    : 2000.00\n";
        while (*h) buf[pos++] = *h++;
        node->size = (uint32_t)pos;
    } else {
        const char *leaf = NULL;
        const task_t *task = proc_task_from_path(node->name, &leaf);
        if (!task || !leaf) {
            node->size = 0;
            return;
        }
        int pos = 0;
        if (strcmp(leaf, "cmdline") == 0) {
            proc_append(buf, &pos, task->name);
            if (pos < PROC_BUF_SIZE) buf[pos++] = '\0';
        } else if (strcmp(leaf, "status") == 0) {
            proc_append_status(buf, &pos, task);
        } else if (strcmp(leaf, "maps") == 0) {
            proc_append_maps(buf, &pos, task);
        }
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

static int sysfs_format_cpu_list(char out[32]) {
    int count = smp_cpu_count();
    if (count < 1) count = 1;
    int position = 0;
    out[position++] = '0';
    if (count > 1) {
        out[position++] = '-';
        char digits[16];
        int length = 0;
        uint32_t last = (uint32_t)(count - 1);
        do {
            digits[length++] = (char)('0' + last % 10);
            last /= 10;
        } while (last);
        while (length) out[position++] = digits[--length];
    }
    out[position++] = '\n';
    out[position] = '\0';
    return position;
}

static int sysfs_format_mac(char out[32], const uint8_t mac[6]) {
    static const char hex[] = "0123456789abcdef";
    int position = 0;
    for (int i = 0; i < 6; i++) {
        if (i) out[position++] = ':';
        out[position++] = hex[mac[i] >> 4];
        out[position++] = hex[mac[i] & 15];
    }
    out[position++] = '\n';
    out[position] = '\0';
    return position;
}

static int devfs_sysfs_read(vfs_node_t *node, uint32_t offset,
                            void *buffer, uint32_t count) {
    if (!node || (!buffer && count)) return -1;
    char dynamic[64];
    const char *text = (const char *)node->private_data;
    uint32_t size = node->size;
    if (strcmp(node->name, "/sys/devices/system/cpu/online") == 0 ||
        strcmp(node->name, "/sys/devices/system/cpu/present") == 0 ||
        strcmp(node->name, "/sys/devices/system/cpu/possible") == 0) {
        size = (uint32_t)sysfs_format_cpu_list(dynamic);
        text = dynamic;
    } else if (strcmp(node->name, "/sys/class/net/eth0/address") == 0) {
        const net_device_t *device = net_primary();
        uint8_t zero[6] = {0};
        size = (uint32_t)sysfs_format_mac(
            dynamic, device && device->mac_valid ? device->mac : zero);
        text = dynamic;
    } else if (strcmp(node->name, "/sys/class/net/eth0/operstate") == 0 ||
               strcmp(node->name, "/sys/class/net/eth0/carrier") == 0) {
        const net_device_t *device = net_primary();
        int online = device && device->present && device->link_ready;
        text = strstr(node->name, "operstate") ?
            (online ? "up\n" : "down\n") : (online ? "1\n" : "0\n");
        size = (uint32_t)strlen(text);
    }
    if (!text || offset >= size) return 0;
    uint32_t available = size - offset;
    if (count > available) count = available;
    if (count) memcpy(buffer, text + offset, count);
    return (int)count;
}

static const vfs_ops_t devfs_sysfs_ops = {
    .read = devfs_sysfs_read,
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
    n->uid = 0;
    n->gid = 0;
    n->mode = S_IFCHR | 0666;
    return n;
}

static void devfs_register_sysfs_dir(const char *path) {
    vfs_node_t *node = devfs_alloc_node();
    if (!node) return;
    set_name(node, path);
    node->type = VFS_NODE_DIR;
    node->mode = S_IFDIR | 0555;
}

static void devfs_register_sysfs_file(const char *path, const char *content) {
    vfs_node_t *node = devfs_alloc_node();
    if (!node) return;
    set_name(node, path);
    node->type = VFS_NODE_FILE;
    node->mode = S_IFREG | 0444;
    node->ops = &devfs_sysfs_ops;
    node->private_data = (void *)content;
    node->size = content ? (uint32_t)strlen(content) : 0;
}

/** 注册静态节点 */
void devfs_init(void) {
    boot_tsc = rdtsc();
    devfs_node_count = 0;

    // /dev/null
    vfs_node_t *n = devfs_alloc_node();
    set_name(n, "/dev/null");
    n->type = VFS_NODE_CHARDEV;
    n->mode = S_IFCHR | 0666;
    n->ops = &devfs_null_ops;

    // /dev/zero
    n = devfs_alloc_node();
    set_name(n, "/dev/zero");
    n->type = VFS_NODE_CHARDEV;
    n->mode = S_IFCHR | 0666;
    n->ops = &devfs_zero_ops;

    // /dev/console
    n = devfs_alloc_node();
    set_name(n, "/dev/console");
    n->type = VFS_NODE_CHARDEV;
    n->mode = S_IFCHR | 0666;
    n->ops = &devfs_console_ops;

    // /proc/uptime
    n = devfs_alloc_node();
    set_name(n, "/proc/uptime");
    n->type = VFS_NODE_FILE;
    n->mode = S_IFREG | 0444;
    n->ops = &devfs_proc_ops;
    n->private_data = kmalloc(PROC_BUF_SIZE);
    memset(n->private_data, 0, PROC_BUF_SIZE);

    // /proc/meminfo
    n = devfs_alloc_node();
    set_name(n, "/proc/meminfo");
    n->type = VFS_NODE_FILE;
    n->mode = S_IFREG | 0444;
    n->ops = &devfs_proc_ops;
    n->private_data = kmalloc(PROC_BUF_SIZE);
    memset(n->private_data, 0, PROC_BUF_SIZE);

    // /proc/cpuinfo
    n = devfs_alloc_node();
    set_name(n, "/proc/cpuinfo");
    n->type = VFS_NODE_FILE;
    n->mode = S_IFREG | 0444;
    n->ops = &devfs_proc_ops;
    n->private_data = kmalloc(PROC_BUF_SIZE);
    memset(n->private_data, 0, PROC_BUF_SIZE);

    // /dev/random
    n = devfs_alloc_node();
    set_name(n, "/dev/random");
    n->type = VFS_NODE_CHARDEV;
    n->mode = S_IFCHR | 0666;
    n->ops = &devfs_random_ops;

    // /dev/urandom
    n = devfs_alloc_node();
    set_name(n, "/dev/urandom");
    n->type = VFS_NODE_CHARDEV;
    n->mode = S_IFCHR | 0666;
    n->ops = &devfs_random_ops;

    static const char *sysfs_dirs[] = {
        "/sys/devices", "/sys/devices/system",
        "/sys/devices/system/cpu", "/sys/devices/system/cpu/cpu0",
        "/sys/devices/system/cpu/cpu0/topology",
        "/sys/class", "/sys/class/net", "/sys/class/net/lo",
        "/sys/class/net/eth0"
    };
    for (uint32_t i = 0; i < sizeof(sysfs_dirs) / sizeof(sysfs_dirs[0]); i++)
        devfs_register_sysfs_dir(sysfs_dirs[i]);

    static const struct {
        const char *path;
        const char *content;
    } sysfs_files[] = {
        {"/sys/devices/system/cpu/online", NULL},
        {"/sys/devices/system/cpu/present", NULL},
        {"/sys/devices/system/cpu/possible", NULL},
        {"/sys/devices/system/cpu/kernel_max", "7\n"},
        {"/sys/devices/system/cpu/cpu0/online", "1\n"},
        {"/sys/devices/system/cpu/cpu0/topology/core_id", "0\n"},
        {"/sys/devices/system/cpu/cpu0/topology/physical_package_id", "0\n"},
        {"/sys/devices/system/cpu/cpu0/topology/core_siblings_list", "0\n"},
        {"/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "0\n"},
        {"/sys/class/net/lo/address", "00:00:00:00:00:00\n"},
        {"/sys/class/net/lo/operstate", "unknown\n"},
        {"/sys/class/net/lo/carrier", "1\n"},
        {"/sys/class/net/lo/mtu", "65536\n"},
        {"/sys/class/net/lo/ifindex", "1\n"},
        {"/sys/class/net/lo/type", "772\n"},
        {"/sys/class/net/eth0/address", NULL},
        {"/sys/class/net/eth0/operstate", NULL},
        {"/sys/class/net/eth0/carrier", NULL},
        {"/sys/class/net/eth0/mtu", "1500\n"},
        {"/sys/class/net/eth0/ifindex", "2\n"},
        {"/sys/class/net/eth0/type", "1\n"}
    };
    for (uint32_t i = 0; i < sizeof(sysfs_files) / sizeof(sysfs_files[0]); i++)
        devfs_register_sysfs_file(sysfs_files[i].path,
                                  sysfs_files[i].content);
}

/** 按路径查找伪文件系统节点 */
vfs_node_t *devfs_lookup(const char *path) {
    if (!path) return NULL;
    char canonical[64];
    if (strncmp(path, "/proc/self", 10) == 0 &&
        (path[10] == '\0' || path[10] == '/')) {
        char pid[16];
        u32_to_name(task_get_process_id(), pid);
        int position = 0;
        const char *prefix = "/proc/";
        while (*prefix && position < (int)sizeof(canonical) - 1)
            canonical[position++] = *prefix++;
        for (int i = 0; pid[i] && position < (int)sizeof(canonical) - 1; i++)
            canonical[position++] = pid[i];
        for (int i = 10; path[i] && position < (int)sizeof(canonical) - 1; i++)
            canonical[position++] = path[i];
        canonical[position] = '\0';
        return devfs_lookup(canonical);
    }
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

        if (*rest == '\0') {
            vfs_node_t *n = devfs_alloc_node();
            if (!n) return NULL;
            set_name(n, path);
            n->type = VFS_NODE_DIR;
            return n;
        }

        const char *leaf = rest[0] == '/' ? rest + 1 : NULL;
        if (leaf && strcmp(leaf, "fd") == 0) {
            vfs_node_t *n = devfs_alloc_node();
            if (!n) return NULL;
            set_name(n, path);
            n->type = VFS_NODE_DIR;
            return n;
        }
        if (leaf && (strcmp(leaf, "cmdline") == 0 ||
                     strcmp(leaf, "status") == 0 ||
                     strcmp(leaf, "maps") == 0)) {
            vfs_node_t *n = devfs_alloc_node();
            if (!n) return NULL;
            set_name(n, path);
            n->type = VFS_NODE_FILE;
            n->ops = &devfs_proc_ops;
            n->private_data = kmalloc(PROC_BUF_SIZE);
            if (!n->private_data) { devfs_node_count--; return NULL; }
            memset(n->private_data, 0, PROC_BUF_SIZE);

            proc_update(n);
            return n;
        }
    }

    return NULL;
}

static int devfs_emit_entry(uint32_t index, const char *const *entries,
                            uint32_t count, uint32_t directory_mask,
                            char *name, uint32_t *type) {
    if (index >= count) return -1;
    set_short_name(name, entries[index]);
    *type = (directory_mask & (1U << index)) ?
        VFS_NODE_DIR : VFS_NODE_FILE;
    return 0;
}

int devfs_readdir(const char *path, uint32_t index, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    if (strncmp(path, "/proc/self", 10) == 0 &&
        (path[10] == '\0' || path[10] == '/')) {
        char canonical[64];
        char pid[16];
        u32_to_name(task_get_process_id(), pid);
        int position = 0;
        const char *prefix = "/proc/";
        while (*prefix) canonical[position++] = *prefix++;
        for (int i = 0; pid[i]; i++) canonical[position++] = pid[i];
        for (int i = 10; path[i]; i++) canonical[position++] = path[i];
        canonical[position] = '\0';
        return devfs_readdir(canonical, index, name, type);
    }

    if (strcmp(path, "/dev") == 0) {
        uint32_t seen = 0;
        for (int i = 0; i < devfs_node_count; i++) {
            const char *short_name = basename_after(devfs_nodes[i].name, "/dev/");
            if (!short_name) continue;
            if (seen++ == index) {
                set_short_name(name, short_name);
                *type = devfs_nodes[i].type;
                return 0;
            }
        }
        return -1;
    }

    if (strcmp(path, "/proc") == 0) {
        static const char *static_proc[] = {
            "self", "uptime", "meminfo", "cpuinfo"
        };
        uint32_t static_count = sizeof(static_proc) / sizeof(static_proc[0]);
        if (index < static_count) {
            set_short_name(name, static_proc[index]);
            *type = index == 0 ? VFS_NODE_DIR : VFS_NODE_FILE;
            return 0;
        }

        uint32_t task_index = index - static_count;
        const task_t *task = task_get_active(task_index);
        if (!task) return -1;
        u32_to_name(task->id, name);
        *type = VFS_NODE_DIR;
        return 0;
    }

    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
        path[3] == 'o' && path[4] == 'c' && path[5] == '/') {
        const char *rest = path + 6;
        int pid = 0;
        while (*rest >= '0' && *rest <= '9') {
            pid = pid * 10 + (*rest - '0');
            rest++;
        }
        if (pid == 0) return -1;

        const task_t *task = task_get_by_id((uint32_t)pid);
        if (!task || task->state == TASK_TERMINATED) return -1;

        if (strcmp(rest, "/fd") == 0) {
            if (!task->fd_table) return -1;
            uint32_t seen = 0;
            for (int fd = 0; fd < POSIX_MAX_FDS; fd++) {
                if (fd > STDERR_FILENO &&
                    !task->fd_table->entries[fd].used) continue;
                if (seen++ == index) {
                    u32_to_name((uint32_t)fd, name);
                    *type = VFS_NODE_FILE;
                    return 0;
                }
            }
            return -1;
        }
        if (*rest != 0) return -1;

        static const char *entries[] = {
            "cmdline", "status", "maps", "exe", "fd"
        };
        if (index >= sizeof(entries) / sizeof(entries[0])) return -1;
        set_short_name(name, entries[index]);
        *type = index == 4 ? VFS_NODE_DIR : VFS_NODE_FILE;
        return 0;
    }

    if (strcmp(path, "/sys") == 0) {
        static const char *entries[] = {"devices", "class"};
        return devfs_emit_entry(index, entries, 2, 0x3, name, type);
    }
    if (strcmp(path, "/sys/devices") == 0) {
        static const char *entries[] = {"system"};
        return devfs_emit_entry(index, entries, 1, 0x1, name, type);
    }
    if (strcmp(path, "/sys/devices/system") == 0) {
        static const char *entries[] = {"cpu"};
        return devfs_emit_entry(index, entries, 1, 0x1, name, type);
    }
    if (strcmp(path, "/sys/devices/system/cpu") == 0) {
        static const char *entries[] = {
            "cpu0", "online", "present", "possible", "kernel_max"
        };
        return devfs_emit_entry(index, entries, 5, 0x1, name, type);
    }
    if (strcmp(path, "/sys/devices/system/cpu/cpu0") == 0) {
        static const char *entries[] = {"online", "topology"};
        return devfs_emit_entry(index, entries, 2, 0x2, name, type);
    }
    if (strcmp(path, "/sys/devices/system/cpu/cpu0/topology") == 0) {
        static const char *entries[] = {
            "core_id", "physical_package_id", "core_siblings_list",
            "thread_siblings_list"
        };
        return devfs_emit_entry(index, entries, 4, 0, name, type);
    }
    if (strcmp(path, "/sys/class") == 0) {
        static const char *entries[] = {"net"};
        return devfs_emit_entry(index, entries, 1, 0x1, name, type);
    }
    if (strcmp(path, "/sys/class/net") == 0) {
        static const char *entries[] = {"lo", "eth0"};
        return devfs_emit_entry(index, entries, 2, 0x3, name, type);
    }
    if (strcmp(path, "/sys/class/net/lo") == 0 ||
        strcmp(path, "/sys/class/net/eth0") == 0) {
        static const char *entries[] = {
            "address", "operstate", "carrier", "mtu", "ifindex", "type"
        };
        return devfs_emit_entry(index, entries, 6, 0, name, type);
    }

    return -1;
}
