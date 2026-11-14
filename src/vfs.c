/**
 * @file vfs.c
 * @brief 虚拟文件系统（VFS）实现，提供统一的文件操作接口
 */
#include "vfs.h"
#include "core/pmm.h"
#include "devfs.h"
#include "errno.h"
#include "fd.h"
#include "fs.h"
#include "fs/tmpfs.h"
#include "linux_compat.h"
#include "string.h"
#include "core/task.h"
#include "sys/stat.h"

static vfs_node_t vfs_root_node = {
    .name = "/",
    .type = VFS_NODE_DIR,
    .size = 0,
    .capacity = 0,
    .private_data = NULL,
    .ops = NULL,
    .uid = 0,
    .gid = 0,
    .mode = S_IFDIR | 0755,
};

static vfs_node_t vfs_virtual_dirs[] = {
    {.name = "/dev",  .type = VFS_NODE_DIR, .uid = 0, .gid = 0, .mode = S_IFDIR | 0755},
    {.name = "/proc", .type = VFS_NODE_DIR, .uid = 0, .gid = 0, .mode = S_IFDIR | 0555},
    {.name = "/sys",  .type = VFS_NODE_DIR, .uid = 0, .gid = 0, .mode = S_IFDIR | 0555},
    {.name = "/home", .type = VFS_NODE_DIR, .uid = 0, .gid = 0, .mode = S_IFDIR | 0755},
    {.name = "/bin",  .type = VFS_NODE_DIR, .uid = 0, .gid = 0, .mode = S_IFDIR | 0755},
    {.name = "/tmp",  .type = VFS_NODE_DIR, .uid = 0, .gid = 0, .mode = S_IFDIR | 0777},
};
/*
 * 全局文件系统临界区
 * ══════════════════
 * 调度是抢占式的：PIT 每个 tick 在 gdt_idt.c:1262 直接调 task_schedule()，
 * 而块 I/O 是**轮询**等待（ahci.c 的 ahci_cmd_once 用 cpu_relax 自旋，
 * 中断保持开启）。因此一个任务的 ext2 写操作完全可能在
 *   读位图 → 改位图 → 写位图
 * 的中间被切走；此时另一个任务进入 ext2 会：
 *   · 复用文件级静态缓冲（g_io_buf / g_map_buf / g_free_buf / g_ind_slot），
 *     把前一个操作的数据覆盖掉；
 *   · 对块位图/inode 位图/空闲计数做交错的 read-modify-write，直接丢分配；
 *   · tmpfs 的页表扩容、配额计数（used_bytes += …）同样会丢更新或 UAF。
 *
 * 结论：**不能**靠"单核 + 不睡眠"隐式成立 —— 抢占正是单核上的并发来源。
 * 正确性必须显式保证。这里用"整个文件系统一把锁"：
 *   · 同一 task 重入只加深度（fs.c ↔ vfs.c 互相调用不会自锁死）；
 *   · 持锁期间 task_preempt_disable()，同核定时器切不走临界区；
 *   · 跨核用 __sync_lock_test_and_set 原子位互斥，等待方 task_yield()
 *     让出 CPU（不烧核，也不会因为持有者在别的核上而饿死）。
 * 不变量：持锁区间内只做块 I/O 与内存拷贝，不做阻塞等待。
 */
#define VFS_FS_LOCK_CHUNK (16u * 1024u)

static volatile int vfs_fs_busy;
static task_t *vfs_fs_owner;   /* 只由持有者读写（持锁期间禁止本核抢占） */
static int vfs_fs_depth;

void vfs_fs_lock(void) {
    task_t *self = task_current();
    if (!self) return;                 /* 调度器起来之前的单线程初始化 */
    task_preempt_disable();
    if (vfs_fs_owner == self) {        /* 可重入 */
        vfs_fs_depth++;
        return;
    }
    while (__sync_lock_test_and_set(&vfs_fs_busy, 1)) {
        task_preempt_enable();         /* 等待期间必须可被调度，否则死等 */
        task_yield();
        task_preempt_disable();
    }
    vfs_fs_owner = self;
    vfs_fs_depth = 1;
}

void vfs_fs_unlock(void) {
    task_t *self = task_current();
    if (!self) return;
    if (vfs_fs_owner != self) {        /* 防御：未持锁的 unlock 不破坏计数 */
        task_preempt_enable();
        return;
    }
    if (--vfs_fs_depth > 0) {
        task_preempt_enable();
        return;
    }
    vfs_fs_owner = 0;
    __sync_lock_release(&vfs_fs_busy);
    task_preempt_enable();
}

static uint32_t vfs_root_readdir_pos;

/**
 * @brief 直接挂在 dir 下的一级 tmpfs 挂载点（/dev → shm，/ → run...）。
 *
 * 挂载点不是目录项，平铺表与 devfs 都不知道它；但 `ls` 必须看得见。
 * 这里从挂载表反推，而不是在 roots[]/devfs 里硬编码名字 —— 否则以后
 * 再挂 /mnt/x 或 /dev/mqueue 又要改两处。
 *
 * @return 命中的名字长度（>0）写入 name；没有则 -1
 */
static int vfs_mount_child_at(const char *dir, uint32_t index, char *name) {
    uint32_t seen = 0;
    size_t dlen = strlen(dir);
    for (uint32_t i = 0; i < tmpfs_mount_count(); i++) {
        const char *mp = tmpfs_mount_path(i);
        if (!mp) continue;
        const char *leaf = 0;
        if (dlen == 1 && dir[0] == '/') {
            if (mp[0] == '/' && mp[1] && !strchr(mp + 1, '/')) leaf = mp + 1;
        } else if (strncmp(mp, dir, dlen) == 0 && mp[dlen] == '/' &&
                   mp[dlen + 1] && !strchr(mp + dlen + 1, '/')) {
            leaf = mp + dlen + 1;
        }
        if (!leaf) continue;
        if (seen++ != index) continue;
        strncpy(name, leaf, VFS_MAX_NAME);
        name[VFS_MAX_NAME - 1] = '\0';
        return 0;
    }
    return -1;
}

static uint32_t vfs_mount_child_count(const char *dir) {
    uint32_t count = 0;
    char scratch[VFS_MAX_NAME];
    while (vfs_mount_child_at(dir, count, scratch) == 0) count++;
    return count;
}
/* tmpfs 的顺序 readdir 游标：vfs_readdir 的调用方（getdents）按"每轮取一项"
 * 推进，而 tmpfs 只有按索引取项的接口。每次 opendir/closedir 重置。 */
static uint32_t vfs_tmpfs_readdir_pos;
static uint32_t vfs_symlink_count;

static int vfs_path_in_tree(const char *path, const char *root) {
    size_t length = strlen(root);
    return strcmp(path, root) == 0 ||
           (strncmp(path, root, length) == 0 && path[length] == '/');
}

static void vfs_rewrite_open_paths(const char *old_path,
                                   const char *new_path,
                                   int include_descendants) {
    size_t old_length = strlen(old_path);
    task_preempt_disable();
    int active = task_get_count();
    for (int task_index = 0; task_index < active; task_index++) {
        const task_t *task = task_get_active((uint32_t)task_index);
        if (!task || !task->fd_table) continue;
        for (int fd = 0; fd < POSIX_MAX_FDS; fd++) {
            fd_entry_t *entry = &task->fd_table->entries[fd];
            if (!entry->used || !entry->path[0]) continue;
            int matches = include_descendants ?
                vfs_path_in_tree(entry->path, old_path) :
                strcmp(entry->path, old_path) == 0;
            if (!matches) continue;
            const char *suffix = entry->path + old_length;
            char rewritten[VFS_MAX_NAME];
            if (strlen(new_path) + strlen(suffix) >= sizeof(rewritten))
                continue;
            strcpy(rewritten, new_path);
            strcat(rewritten, suffix);
            strcpy(entry->path, rewritten);
        }
    }
    task_preempt_enable();
}

static void vfs_notify_node(vfs_node_t *node, uint32_t mask) {
    if (!node || !node->name[0]) return;
    if (node->name[0] == '/') {
        linux_compat_inotify_notify(node->name, mask,
                                    node->type == VFS_NODE_DIR);
        return;
    }
    char absolute[VFS_MAX_NAME];
    size_t length = strlen(node->name);
    if (length + 2 > sizeof(absolute)) return;
    absolute[0] = '/';
    memcpy(absolute + 1, node->name, length + 1);
    linux_compat_inotify_notify(absolute, mask,
                                node->type == VFS_NODE_DIR);
}

/* 内置 HAX 兼容文件 + TCC runtime/37 个头文件 + web vendor。
 * 这些资源直接引用内核 incbin 数据，避免启动时复制/写入 HBFS。 */
#define VFS_STATIC_FILES_MAX 128

typedef struct {
    vfs_node_t node;
    const uint8_t *data;
} vfs_static_file_t;

static vfs_static_file_t vfs_static_files[VFS_STATIC_FILES_MAX];
static uint32_t vfs_static_file_count;

static int vfs_static_read(vfs_node_t *node, uint32_t offset,
                           void *buffer, uint32_t count) {
    if (!node || (!buffer && count)) return -1;
    vfs_static_file_t *file = (vfs_static_file_t *)node->private_data;
    if (!file || offset >= node->size) return 0;
    uint32_t available = node->size - offset;
    if (count > available) count = available;
    if (count) memcpy(buffer, file->data + offset, count);
    return (int)count;
}

static int vfs_static_readonly(vfs_node_t *node, uint32_t offset,
                               const void *buffer, uint32_t count) {
    (void)node;
    (void)offset;
    (void)buffer;
    (void)count;
    return -1;
}

static int vfs_static_noop(vfs_node_t *node) {
    (void)node;
    return -1;
}

static int vfs_static_truncate_noop(vfs_node_t *node, uint32_t size) {
    (void)node;
    (void)size;
    return -1;
}

static const vfs_ops_t vfs_static_ops = {
    .read = vfs_static_read,
    .write = vfs_static_readonly,
    .truncate = vfs_static_truncate_noop,
    .unlink = vfs_static_noop,
};

int vfs_resolve_path(const char *cwd, const char *path, char *out, uint32_t cap) {
    char tmp[256];
    uint32_t pos = 0;
    uint32_t start = 0;
    if (!path || !out || cap == 0) return -1;

    if (path[0] == '/') {
        tmp[pos++] = '/';
    } else {
        const char *base = (cwd && cwd[0]) ? cwd : "/";
        while (base[start] && pos + 1 < sizeof(tmp)) tmp[pos++] = base[start++];
        if (pos == 0 || tmp[pos - 1] != '/') {
            if (pos + 1 >= sizeof(tmp)) return -1;
            tmp[pos++] = '/';
        }
    }

    for (uint32_t i = 0; path[i] && pos + 1 < sizeof(tmp); i++) {
        if (path[i] == '/' && pos > 0 && tmp[pos - 1] == '/') continue;
        tmp[pos++] = path[i];
    }
    tmp[pos] = 0;

    char norm[256];
    uint32_t npos = 0;
    norm[npos++] = '/';
    uint32_t i = 0;
    while (tmp[i]) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;

        char seg[64];
        uint32_t slen = 0;
        while (tmp[i] && tmp[i] != '/' && slen + 1 < sizeof(seg))
            seg[slen++] = tmp[i++];
        while (tmp[i] && tmp[i] != '/') i++;
        seg[slen] = 0;

        if (strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) {
            if (npos > 1) {
                if (norm[npos - 1] == '/') npos--;
                while (npos > 1 && norm[npos - 1] != '/') npos--;
                if (npos > 1) npos--;
                norm[npos] = 0;
            }
            continue;
        }

        if (npos > 1) {
            if (npos + 1 >= sizeof(norm)) return -1;
            norm[npos++] = '/';
        }
        if (npos + slen >= sizeof(norm)) return -1;
        for (uint32_t j = 0; j < slen; j++) norm[npos++] = seg[j];
        norm[npos] = 0;
    }

    if (npos == 0) norm[npos++] = '/';
    norm[npos] = 0;
    if (strlen(norm) + 1 > cap) return -1;
    strcpy(out, norm);
    return 0;
}

/*
 * tmpfs 的挂载点与配额
 * ═══════════════════
 * 配额是**上限**不是预留：超了返回 -ENOSPC（sqlite/Chromium 能正确回滚），
 * 而不是把内核 OOM 掉。数据页从 PMM 逐页申请（见 alloc_pages），因此这些
 * 数字不再受 2 MiB kmalloc 堆限制，只受物理内存与 PMM 剩余量约束：
 *   /dev/shm 512 MiB —— Chromium/Electron 的共享内存主战场
 *   /tmp     256 MiB —— profile、crashpad、缓存
 *   /run     256 MiB —— wayland socket 目录与运行时状态
 * 数值集中在这里，vfs_init 传参；要调只改这三个宏。
 */
#define VFS_TMPFS_TMP_BYTES     (256u * 1024u * 1024u)
#define VFS_TMPFS_RUN_BYTES     (256u * 1024u * 1024u)
#define VFS_TMPFS_SHM_BYTES     (512u * 1024u * 1024u)

/* 数据页直接向 PMM 要：pmm_alloc_blocks 返回**连续**物理页，内核在
 * PMM_LOW_LIMIT 以下恒等映射，所以物理地址可直接当指针用。 */
static void *vfs_tmpfs_alloc_pages(uint32_t count) {
    if (!count) return NULL;
    uint64_t phys = pmm_alloc_blocks(count);
    return (void *)(uintptr_t)phys;
}

static void vfs_tmpfs_free_pages(void *ptr, uint32_t count) {
    if (!ptr || !count) return;
    pmm_free_blocks((uint64_t)(uintptr_t)ptr, count);
}

/** 挂载 /tmp、/run、/dev/shm。失败不致命（退回只读/内存旁路路径）。 */
static void vfs_tmpfs_init(void) {
    tmpfs_alloc_t alloc = {vfs_tmpfs_alloc_pages, vfs_tmpfs_free_pages};
    tmpfs_set_allocator(&alloc);
    (void)tmpfs_mount("/tmp", VFS_TMPFS_TMP_BYTES);
    (void)tmpfs_mount("/run", VFS_TMPFS_RUN_BYTES);
    (void)tmpfs_mount("/dev/shm", VFS_TMPFS_SHM_BYTES);
}

/** 初始化 VFS 层，委托给底层文件系统初始化 */
int vfs_init(void) {
    vfs_static_file_count = 0;
    vfs_symlink_count = 0;
    memset(vfs_static_files, 0, sizeof(vfs_static_files));
    vfs_tmpfs_init();
    devfs_init();
    int result = fs_init();
    if (result < 0) return result;
    for (uint32_t i = 0; i < fs_get_count(); i++) {
        file_t *file = fs_get_file(i);
        if (file && file->used && file->type == 2) vfs_symlink_count++;
    }
    return 0;
}

vfs_node_t *vfs_register_static_file(const char *path, const void *data,
                                     uint32_t size) {
    if (!path || path[0] != '/' || !data || !size ||
        strlen(path) >= VFS_MAX_NAME)
        return NULL;
    for (uint32_t i = 0; i < vfs_static_file_count; i++) {
        if (strcmp(vfs_static_files[i].node.name, path) == 0) {
            vfs_static_files[i].data = (const uint8_t *)data;
            vfs_static_files[i].node.size = size;
            vfs_static_files[i].node.capacity = size;
            return &vfs_static_files[i].node;
        }
    }
    if (vfs_static_file_count >= VFS_STATIC_FILES_MAX) return NULL;
    vfs_static_file_t *file =
        &vfs_static_files[vfs_static_file_count++];
    memset(file, 0, sizeof(*file));
    strcpy(file->node.name, path);
    file->node.type = VFS_NODE_FILE;
    file->node.size = size;
    file->node.capacity = size;
    file->node.private_data = file;
    file->node.ops = &vfs_static_ops;
    file->node.uid = 0;
    file->node.gid = 0;
    file->node.mode = S_IFREG | 0444;
    file->data = (const uint8_t *)data;
    return &file->node;
}

/** 不做链接展开的单次节点查找。 */
static vfs_node_t *vfs_lookup_raw(const char *path) {
    if (!path) return NULL;
    if (strcmp(path, "/") == 0) return &vfs_root_node;
    for (uint32_t i = 0; i < vfs_static_file_count; i++) {
        if (strcmp(path, vfs_static_files[i].node.name) == 0)
            return &vfs_static_files[i].node;
    }
    /* tmpfs 先于虚拟目录：/tmp 在 vfs_virtual_dirs 里还有一个空的占位项，
     * 若让占位项胜出，readdir /tmp 会落到平铺文件表而不是 tmpfs 树。 */
    {
        /* 挂载点拥有自己的命名空间：在 tmpfs 下面查不到就是真不存在，
         * 不能再回落到 devfs/平铺文件表（否则磁盘根上残留的 "tmp/xxx"
         * 会在 /tmp 挂载后"幽灵可见"）。 */
        if (tmpfs_is_mounted(path)) return tmpfs_lookup(path);
    }
    for (uint32_t i = 0; i < sizeof(vfs_virtual_dirs) / sizeof(vfs_virtual_dirs[0]); i++) {
        if (strcmp(path, vfs_virtual_dirs[i].name) == 0)
            return &vfs_virtual_dirs[i];
    }
    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
        path[3] == 'o' && path[4] == 'c' && path[5] == '/') {
        const char *rest = path + 6;
        int pid = 0;
        while (*rest >= '0' && *rest <= '9') {
            pid = pid * 10 + (*rest - '0');
            rest++;
        }
        if (pid > 0 && *rest == 0) {
            char first_name[VFS_MAX_NAME];
            uint32_t first_type;
            if (devfs_readdir(path, 0, first_name, &first_type) < 0)
                return NULL;
            static vfs_node_t proc_pid_node = {
                .name = "/proc/pid",
                .type = VFS_NODE_DIR,
                .uid = 0,
                .gid = 0,
                .mode = S_IFDIR | 0555,
            };
            return &proc_pid_node;
        }
    }
    vfs_node_t *df = devfs_lookup(path);
    if (df) return df;
    file_t *file = fs_find_file(path);
    return file ? &file->node : NULL;
}

/* 用固定的栈缓冲和最多 8 层展开符号链接。HBOS 的 ramfs/HBFS 路径表很小，
 * 逐段查找比维护第二套 inode/dentry 缓存更轻；同时覆盖中间目录链接和相对
 * 目标。follow_final=0 对应 lstat/AT_SYMLINK_NOFOLLOW/readlink/unlink。 */
static int vfs_expand_symlinks(const char *path, int follow_final,
                               char output[VFS_MAX_NAME]) {
    char current[VFS_MAX_NAME];
    if (vfs_resolve_path("/", path, current, sizeof(current)) < 0)
        return -1;

    for (unsigned int depth = 0; depth <= 8; depth++) {
        int expanded = 0;
        size_t length = strlen(current);
        for (size_t end = 1; end <= length; end++) {
            if (end < length && current[end] != '/') continue;
            int final_component = end == length;
            if (final_component && !follow_final) break;

            char prefix[VFS_MAX_NAME];
            memcpy(prefix, current, end);
            prefix[end] = '\0';
            vfs_node_t *node = vfs_lookup_raw(prefix);
            if (!node || node->type != VFS_NODE_SYMLINK) continue;
            if (depth == 8) return -1;

            /* 用 ops->read 取链接目标：private_data 在 ramfs 上是 file_t、
             * 在 tmpfs 上是 tmpfs 节点，直接强转会读错结构。 */
            if (!node->ops || !node->ops->read || node->size >= VFS_MAX_NAME)
                return -1;
            char target[VFS_MAX_NAME];
            int got = node->ops->read(node, 0, target, node->size);
            if (got != (int)node->size) return -1;
            target[got] = '\0';

            char parent[VFS_MAX_NAME];
            const char *slash = strrchr(prefix, '/');
            if (!slash || slash == prefix) {
                strcpy(parent, "/");
            } else {
                size_t parent_length = (size_t)(slash - prefix);
                memcpy(parent, prefix, parent_length);
                parent[parent_length] = '\0';
            }
            char resolved_target[VFS_MAX_NAME];
            if (vfs_resolve_path(parent, target, resolved_target,
                                 sizeof(resolved_target)) < 0)
                return -1;
            const char *suffix = current + end;
            if (strlen(resolved_target) + strlen(suffix) >= VFS_MAX_NAME)
                return -1;
            char combined[VFS_MAX_NAME];
            strcpy(combined, resolved_target);
            strcat(combined, suffix);
            if (vfs_resolve_path("/", combined, current,
                                 sizeof(current)) < 0)
                return -1;
            expanded = 1;
            break;
        }
        if (!expanded) {
            strcpy(output, current);
            return 0;
        }
    }
    return -1;
}

/* 无锁实现：tmpfs 树遍历与平铺表查找本身不动磁盘，但别处可能正在
 * unlink 并释放节点，所以公开入口统一加锁。 */
static vfs_node_t *vfs_lookup_nolock(const char *path) {
    if (!path) return NULL;
    if (!vfs_symlink_count) return vfs_lookup_raw(path);
    char expanded[VFS_MAX_NAME];
    if (vfs_expand_symlinks(path, 1, expanded) < 0) return NULL;
    return vfs_lookup_raw(expanded);
}

static vfs_node_t *vfs_lookup_nofollow_nolock(const char *path) {
    if (!path) return NULL;
    if (!vfs_symlink_count) return vfs_lookup_raw(path);
    char expanded[VFS_MAX_NAME];
    if (vfs_expand_symlinks(path, 0, expanded) < 0) return NULL;
    return vfs_lookup_raw(expanded);
}

/** 按路径查找 VFS 节点，默认遵循 POSIX 语义展开最终符号链接。 */
vfs_node_t *vfs_lookup(const char *path) {
    vfs_fs_lock();
    vfs_node_t *node = vfs_lookup_nolock(path);
    vfs_fs_unlock();
    return node;
}

vfs_node_t *vfs_lookup_nofollow(const char *path) {
    vfs_fs_lock();
    vfs_node_t *node = vfs_lookup_nofollow_nolock(path);
    vfs_fs_unlock();
    return node;
}

/** 按路径创建 VFS 节点，若已存在则返回已有节点 */
vfs_node_t *vfs_create(const char *path) {
    int existed = vfs_lookup_nofollow(path) != NULL;
    vfs_fs_lock();
    if (tmpfs_is_mounted(path)) {
        vfs_node_t *node = tmpfs_create(path, VFS_NODE_FILE, S_IFREG | 0644);
        vfs_fs_unlock();
        if (node && !existed)
            linux_compat_inotify_notify(path, LINUX_IN_CREATE, 0);
        return node;
    }
    file_t *file = fs_create_file(path);
    vfs_fs_unlock();
    if (file && !existed)
        linux_compat_inotify_notify(path, LINUX_IN_CREATE, 0);
    return file ? &file->node : NULL;
}

vfs_node_t *vfs_symlink(const char *path, const char *target) {
    vfs_fs_lock();
    if (tmpfs_is_mounted(path)) {
        vfs_node_t *node = tmpfs_symlink(path, target);
        vfs_fs_unlock();
        if (node) {
            vfs_symlink_count++;
            linux_compat_inotify_notify(path, LINUX_IN_CREATE, 0);
        }
        return node;
    }
    file_t *link = fs_create_symlink(path, target);
    vfs_fs_unlock();
    if (link) {
        vfs_symlink_count++;
        linux_compat_inotify_notify(path, LINUX_IN_CREATE, 0);
    }
    return link ? &link->node : NULL;
}

/** 按路径删除 VFS 节点，通过节点操作接口调用底层 unlink */
int vfs_unlink(const char *path) {
    vfs_node_t *node = vfs_lookup_nofollow(path);
    if (!node || !node->ops || !node->ops->unlink) return -1;
    vfs_fs_lock();
    int result = node->ops->unlink(node);
    vfs_fs_unlock();
    if (result == 0) {
        if (node->type == VFS_NODE_SYMLINK && vfs_symlink_count)
            vfs_symlink_count--;
        linux_compat_inotify_notify(path, LINUX_IN_DELETE_SELF, 0);
    }
    return result;
}

static int vfs_rename_nolock(const char *old_path, const char *new_path,
                             int replace) {
    if (!old_path || !new_path) return -1;
    if (tmpfs_is_mounted(old_path) || tmpfs_is_mounted(new_path)) {
        vfs_node_t *src = vfs_lookup_nofollow(old_path);
        if (!src) return -ENOENT;
        if (!tmpfs_is_mounted(old_path) || !tmpfs_is_mounted(new_path))
            return -EXDEV;   /* 跨文件系统（tmpfs ↔ 磁盘）不支持 */
        int r = tmpfs_rename(old_path, new_path, replace);
        if (r < 0) return r;
        int is_dir = (src->type == VFS_NODE_DIR);
        vfs_rewrite_open_paths(old_path, new_path, is_dir);
        linux_compat_inotify_move(old_path, new_path, is_dir);
        return 0;
    }
    vfs_node_t *source = vfs_lookup_nofollow(old_path);
    if (!source || (source->type != VFS_NODE_FILE &&
                    source->type != VFS_NODE_DIR &&
                    source->type != VFS_NODE_SYMLINK))
        return -1;
    int is_directory = source->type == VFS_NODE_DIR;
    vfs_node_t *target = vfs_lookup_nofollow(new_path);
    if (target == source) return 0;
    if (target) {
        if (!replace ||
            ((target->type == VFS_NODE_DIR) != is_directory))
            return -1;
        if (is_directory) {
            if (fs_rmdir(new_path) < 0) return -1;
        } else {
            if (!target->ops || !target->ops->unlink ||
                target->ops->unlink(target) < 0)
                return -1;
            if (target->type == VFS_NODE_SYMLINK && vfs_symlink_count)
                vfs_symlink_count--;
        }
        linux_compat_inotify_replace_target(new_path, is_directory);
    }
    if (fs_rename_file(old_path, new_path) < 0) return -1;
    vfs_rewrite_open_paths(old_path, new_path, is_directory);
    linux_compat_inotify_move(old_path, new_path, is_directory);
    return 0;
}

int vfs_rename(const char *old_path, const char *new_path, int replace) {
    vfs_fs_lock();
    int r = vfs_rename_nolock(old_path, new_path, replace);
    vfs_fs_unlock();
    return r;
}

/**
 * @brief 从 VFS 节点读取数据（临界区保护，按 16 KiB 分块）。
 *
 * 分块的意义：块 I/O 同步轮询，一次持锁读 1 MiB 会让别的任务等几毫秒
 * 到几十毫秒。按 16 KiB 分段后，单次持锁最多约 16 个 1 KiB 块，其他任务
 * 可以在段与段之间排到锁。语义仍与 POSIX read 一致（返回已读字节数，
 * 中途出错返回已读部分；一个块都没读才返回负错误码）。
 */
int vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->read) return -1;
    if (!buf && count) return -EINVAL;
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > VFS_FS_LOCK_CHUNK) chunk = VFS_FS_LOCK_CHUNK;
        vfs_fs_lock();
        int r = node->ops->read(node, offset + done, out + done, chunk);
        vfs_fs_unlock();
        if (r < 0) return done ? (int)done : r;
        done += (uint32_t)r;
        if ((uint32_t)r < chunk) break;   /* 短读（EOF/后端上限） */
    }
    return (int)done;
}

/**
 * @brief 向 VFS 节点写入数据（临界区保护，按 16 KiB 分块）。
 *
 * 中途失败返回已写字节数（POSIX 语义）；一个字节都没写才返回负错误码
 * （-ENOSPC/-EROFS/… 现在能从 ext2/tmpfs 一路传到这里）。
 */
int vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->write) return -1;
    if (!buf && count) return -EINVAL;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > VFS_FS_LOCK_CHUNK) chunk = VFS_FS_LOCK_CHUNK;
        vfs_fs_lock();
        int r = node->ops->write(node, offset + done, in + done, chunk);
        vfs_fs_unlock();
        if (r < 0) {
            if (done) break;
            return r;
        }
        done += (uint32_t)r;
        if ((uint32_t)r < chunk) break;
    }
    if (done > 0) vfs_notify_node(node, LINUX_IN_MODIFY);
    return (int)done;
}

/** 把 VFS 节点截断/扩展到 size 字节，通过节点操作接口调用底层 truncate */
int vfs_truncate(vfs_node_t *node, uint32_t size) {
    if (!node || !node->ops || !node->ops->truncate) return -1;
    vfs_fs_lock();
    int result = node->ops->truncate(node, size);
    vfs_fs_unlock();
    if (result == 0) {
        if (size == 0) {
            node->size = 0;
            node->capacity = 0;
        } else if (size > node->size) {
            node->size = size;
            if (size > node->capacity) node->capacity = size;
        } else {
            node->size = size;   /* 缩小：底层可能暂不回收块，但可见长度必须变 */
        }
        vfs_notify_node(node, LINUX_IN_MODIFY);
    }
    return result;
}

/**
 * @brief 获取整个命名空间的节点数：平铺文件表 + 所有 tmpfs 挂载点。
 *
 * 为什么不能只返回 fs_get_count()：tmpfs 是独立内存树，不在平铺表里。
 * 以前 `fsinfo` 数文件、`find/du` 类工具遍历时都看不到 /tmp /run /dev/shm
 * 里的东西 —— "ls /tmp 有、全盘列举没有" 的割裂。这里做统一视图：
 * 前 fs_get_count() 项是平铺表（与旧行为完全一致），后面按挂载点顺序
 * 深度优先追加 tmpfs 节点。
 */
uint32_t vfs_count(void) {
    /* 与枚举本身保持一致：并发 unlink 可能正在释放 tmpfs 节点，
     * 遍历树必须和写路径互斥（否则会读到刚释放的节点）。 */
    vfs_fs_lock();
    uint32_t total = fs_get_count() + tmpfs_node_count();
    vfs_fs_unlock();
    return total;
}

/** 按索引获取 VFS 节点（索引语义与 vfs_count 一致） */
vfs_node_t *vfs_get(uint32_t index) {
    vfs_fs_lock();
    uint32_t flat = fs_get_count();
    vfs_node_t *node = index < flat ? fs_get_node(index)
                                    : tmpfs_node_at(index - flat);
    vfs_fs_unlock();
    return node;
}

int vfs_mkdir(const char *path) {
    if (!path) return -1;
    vfs_fs_lock();
    if (tmpfs_is_mounted(path)) {
        int result = tmpfs_mkdir(path);
        vfs_fs_unlock();
        if (result == 0)
            linux_compat_inotify_notify(path, LINUX_IN_CREATE, 1);
        return result;
    }
    int result = fs_mkdir(path);
    vfs_fs_unlock();
    if (result == 0)
        linux_compat_inotify_notify(path, LINUX_IN_CREATE, 1);
    return result;
}

int vfs_rmdir(const char *path) {
    if (!path) return -1;
    vfs_fs_lock();
    if (tmpfs_is_mounted(path)) {
        int result = tmpfs_rmdir(path);
        vfs_fs_unlock();
        if (result == 0)
            linux_compat_inotify_notify(path, LINUX_IN_DELETE_SELF, 1);
        return result;
    }
    int result = fs_rmdir(path);
    vfs_fs_unlock();
    if (result == 0)
        linux_compat_inotify_notify(path, LINUX_IN_DELETE_SELF, 1);
    return result;
}

static int vfs_opendir_nolock(const char *path) {
    if (!path) return -1;
    if (tmpfs_is_mounted(path)) {
        vfs_node_t *node = tmpfs_lookup(path);
        if (!node || node->type != VFS_NODE_DIR) return -1;
        vfs_tmpfs_readdir_pos = 0;
        return 0;
    }
    if (strcmp(path, "/") == 0) {
        vfs_root_readdir_pos = 0;
        fs_closedir("/");
        return 0;
    }
    for (uint32_t i = 0; i < sizeof(vfs_virtual_dirs) / sizeof(vfs_virtual_dirs[0]); i++) {
        if (strcmp(path, vfs_virtual_dirs[i].name) == 0) {
            if (fs_opendir(path) >= 0) {
                return 0;
            }
            fs_closedir(path);
            return 0;
        }
    }
    if (strncmp(path, "/proc/", 6) == 0 ||
        strncmp(path, "/sys/", 5) == 0) {
        vfs_node_t *node = vfs_lookup(path);
        return node && node->type == VFS_NODE_DIR ? 0 : -1;
    }
    return fs_opendir(path);
}

int vfs_opendir(const char *path) {
    vfs_fs_lock();
    int r = vfs_opendir_nolock(path);
    vfs_fs_unlock();
    return r;
}

int vfs_readdir(const char *path, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    vfs_fs_lock();
    if (strcmp(path, "/") == 0) {
        int ret = vfs_readdir_at("/", vfs_root_readdir_pos, name, type);
        if (ret == 0) vfs_root_readdir_pos++;
        vfs_fs_unlock();
        return ret;
    }
    if (tmpfs_is_mounted(path)) {
        int ret = tmpfs_readdir_at(path, vfs_tmpfs_readdir_pos, name, type);
        if (ret == 0) vfs_tmpfs_readdir_pos++;
        vfs_fs_unlock();
        return ret;
    }
    int ret = fs_readdir(path, name, type);
    vfs_fs_unlock();
    return ret;
}

static int vfs_readdir_at_nolock(const char *path, uint32_t index, char *name,
                                 uint32_t *type) {
    if (!path || !name || !type) return -1;
    /* 根目录：合成顶层目录 + 真实文件系统里的项。
     *
     * 早先只枚举硬编码的 roots[]（home/bin/tmp/dev/proc/sys）就返回，导致
     * 通过 vfs_register_static_file 建在根下的目录（如 /themes）永远不出现在
     * `ls /` 里——文件其实存在（cat /themes/ocean.theme 能读到），但列表看不见，
     * 属于"能读不能列"的割裂。现在先给合成的顶层目录，再继续走真实 fs 枚举，
     * 由调用方按 index 顺序取，两段都覆盖到。 */
    if (strcmp(path, "/") == 0) {
        static const char *roots[] = {"home", "bin", "tmp", "dev", "proc", "sys"};
        uint32_t root_count = sizeof(roots) / sizeof(roots[0]);
        if (index < root_count) {
            strncpy(name, roots[index], VFS_MAX_NAME);
            name[VFS_MAX_NAME - 1] = 0;
            *type = VFS_NODE_DIR;
            return 0;
        }
        /* 越过合成项后先列"直接挂在 / 下的挂载点"（/tmp /run …），
         * 再补真实 fs 的条目 —— 两段都不漏，顺序也稳定。 */
        uint32_t want = index - root_count;
        uint32_t mounts = vfs_mount_child_count("/");
        uint32_t seen_mounts = 0;
        for (uint32_t m = 0; m < mounts; m++) {
            char mount_name[VFS_MAX_NAME];
            if (vfs_mount_child_at("/", m, mount_name) < 0) continue;
            int duplicate = 0;
            for (uint32_t r = 0; r < root_count; r++)
                if (strcmp(mount_name, roots[r]) == 0) duplicate = 1;
            if (duplicate) continue;      /* /tmp 既是合成项又是挂载点：只列一次 */
            if (seen_mounts == want) {
                strncpy(name, mount_name, VFS_MAX_NAME);
                name[VFS_MAX_NAME - 1] = '\0';
                *type = VFS_NODE_DIR;
                return 0;
            }
            seen_mounts++;
        }
        want -= seen_mounts;
        fs_closedir("/");
        for (uint32_t i = 0; i <= want; i++) {
            if (fs_readdir("/", name, type) < 0) {
                fs_closedir("/");
                return -1;
            }
        }
        fs_closedir("/");
        return 0;
    }
    /* tmpfs 挂载点：目录内容全部来自内存树 */
    if (tmpfs_is_mounted(path))
        return tmpfs_readdir_at(path, index, name, type);
    /* devfs 目录（/dev、/proc/...）：先列直接挂载点（/dev → shm），再列 devfs */
    if (strcmp(path, "/dev") == 0 ||
        strcmp(path, "/proc") == 0 ||
        strcmp(path, "/sys") == 0 ||
        (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
         path[3] == 'o' && path[4] == 'c' && path[5] == '/') ||
        strncmp(path, "/sys/", 5) == 0) {
        uint32_t mounts = vfs_mount_child_count(path);
        if (index < mounts) {
            if (vfs_mount_child_at(path, index, name) < 0) return -1;
            *type = VFS_NODE_DIR;
            return 0;
        }
        return devfs_readdir(path, index - mounts, name, type);
    }
    /* 非根路径：先列出 vfs_register_static_file 注册的条目，再补真实 fs 的。
     *
     * 为什么必须处理静态表：静态文件存在**独立于 ramfs 的另一个表**里
     * （vfs_static_files[]），vfs_lookup 能找到它、cat 能读到它，但
     * fs_readdir 完全不知道它的存在——于是 /themes 下的主题包「能读、
     * 不能列」，进而"丢个文件进 /themes 就能换主题"这个功能实际不生效
     * （theme.c 靠 readdir 扫描目录）。这里把两张表拼起来，并做去重
     * （同路径以静态表为准，因为它优先被 lookup 命中）。 */
    {
        size_t plen = strlen(path);
        uint32_t listed = 0;
        for (uint32_t i = 0; i < vfs_static_file_count; i++) {
            const char *fn = vfs_static_files[i].node.name;
            if (strncmp(fn, path, plen) != 0) continue;
            if (fn[plen] != '/') continue;          /* 必须是该目录的直接子项 */
            const char *leaf = fn + plen + 1;
            if (!leaf[0] || strchr(leaf, '/')) continue;   /* 跳过子目录内部 */
            if (listed == index) {
                strncpy(name, leaf, VFS_MAX_NAME);
                name[VFS_MAX_NAME - 1] = 0;
                *type = vfs_static_files[i].node.type;
                return 0;
            }
            listed++;
        }

        /* 如果请求的 index 已落在静态条目范围内，就直接返回了（上面）；
         * 走到这里说明 index >= listed，真实 fs 的条目排在静态之后。
         *
         * 注意必须先判大小再相减：`index - listed` 在 index < listed 时会
         * 无符号下溢成约 42 亿，循环随即失控（实测导致进桌面时 page fault）。
         * 这里显式挡住，而不是依赖"上面一定 return"这个隐含前提。 */
        if (listed > index) return -1;

        uint32_t mounts = vfs_mount_child_count(path);
        uint32_t after_static = index - listed;
        if (after_static < mounts) {
            if (vfs_mount_child_at(path, after_static, name) < 0) return -1;
            *type = VFS_NODE_DIR;
            return 0;
        }

        if (vfs_opendir(path) < 0) return -1;
        uint32_t want = after_static - mounts;
        for (uint32_t i = 0; i <= want; i++) {
            if (vfs_readdir(path, name, type) < 0) {
                vfs_closedir(path);
                return -1;
            }
        }
        vfs_closedir(path);
        return 0;
    }
}

int vfs_readdir_at(const char *path, uint32_t index, char *name,
                   uint32_t *type) {
    vfs_fs_lock();
    int r = vfs_readdir_at_nolock(path, index, name, type);
    vfs_fs_unlock();
    return r;
}

int vfs_closedir(const char *path) {
    vfs_fs_lock();
    int r = 0;
    if (path && strcmp(path, "/") == 0) vfs_root_readdir_pos = 0;
    if (path && tmpfs_is_mounted(path)) {
        vfs_tmpfs_readdir_pos = 0;
    } else {
        r = fs_closedir(path);
    }
    vfs_fs_unlock();
    return r;
}
