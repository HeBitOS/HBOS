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
static uint32_t vfs_root_readdir_pos;
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

/** 按路径查找 VFS 节点，默认遵循 POSIX 语义展开最终符号链接。 */
vfs_node_t *vfs_lookup(const char *path) {
    if (!path) return NULL;
    if (!vfs_symlink_count) return vfs_lookup_raw(path);
    char expanded[VFS_MAX_NAME];
    if (vfs_expand_symlinks(path, 1, expanded) < 0) return NULL;
    return vfs_lookup_raw(expanded);
}

vfs_node_t *vfs_lookup_nofollow(const char *path) {
    if (!path) return NULL;
    if (!vfs_symlink_count) return vfs_lookup_raw(path);
    char expanded[VFS_MAX_NAME];
    if (vfs_expand_symlinks(path, 0, expanded) < 0) return NULL;
    return vfs_lookup_raw(expanded);
}

/** 按路径创建 VFS 节点，若已存在则返回已有节点 */
vfs_node_t *vfs_create(const char *path) {
    int existed = vfs_lookup_nofollow(path) != NULL;
    if (tmpfs_is_mounted(path)) {
        vfs_node_t *node = tmpfs_create(path, VFS_NODE_FILE, S_IFREG | 0644);
        if (node && !existed)
            linux_compat_inotify_notify(path, LINUX_IN_CREATE, 0);
        return node;
    }
    file_t *file = fs_create_file(path);
    if (file && !existed)
        linux_compat_inotify_notify(path, LINUX_IN_CREATE, 0);
    return file ? &file->node : NULL;
}

vfs_node_t *vfs_symlink(const char *path, const char *target) {
    if (tmpfs_is_mounted(path)) {
        vfs_node_t *node = tmpfs_symlink(path, target);
        if (node) {
            vfs_symlink_count++;
            linux_compat_inotify_notify(path, LINUX_IN_CREATE, 0);
        }
        return node;
    }
    file_t *link = fs_create_symlink(path, target);
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
    int result = node->ops->unlink(node);
    if (result == 0) {
        if (node->type == VFS_NODE_SYMLINK && vfs_symlink_count)
            vfs_symlink_count--;
        linux_compat_inotify_notify(path, LINUX_IN_DELETE_SELF, 0);
    }
    return result;
}

int vfs_rename(const char *old_path, const char *new_path, int replace) {
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

/** 从 VFS 节点读取数据，通过节点操作接口调用底层 read */
int vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, buf, count);
}

/** 向 VFS 节点写入数据，通过节点操作接口调用底层 write */
int vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->write) return -1;
    int result = node->ops->write(node, offset, buf, count);
    if (result > 0) vfs_notify_node(node, LINUX_IN_MODIFY);
    return result;
}

/** 把 VFS 节点截断/扩展到 size 字节，通过节点操作接口调用底层 truncate */
int vfs_truncate(vfs_node_t *node, uint32_t size) {
    if (!node || !node->ops || !node->ops->truncate) return -1;
    int result = node->ops->truncate(node, size);
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

/** 获取 VFS 节点数量 */
uint32_t vfs_count(void) {
    return fs_get_count();
}

/** 按索引获取 VFS 节点 */
vfs_node_t *vfs_get(uint32_t index) {
    return fs_get_node(index);
}

int vfs_mkdir(const char *path) {
    if (!path) return -1;
    if (tmpfs_is_mounted(path)) {
        int result = tmpfs_mkdir(path);
        if (result == 0)
            linux_compat_inotify_notify(path, LINUX_IN_CREATE, 1);
        return result;
    }
    int result = fs_mkdir(path);
    if (result == 0)
        linux_compat_inotify_notify(path, LINUX_IN_CREATE, 1);
    return result;
}

int vfs_rmdir(const char *path) {
    if (!path) return -1;
    if (tmpfs_is_mounted(path)) {
        int result = tmpfs_rmdir(path);
        if (result == 0)
            linux_compat_inotify_notify(path, LINUX_IN_DELETE_SELF, 1);
        return result;
    }
    int result = fs_rmdir(path);
    if (result == 0)
        linux_compat_inotify_notify(path, LINUX_IN_DELETE_SELF, 1);
    return result;
}

int vfs_opendir(const char *path) {
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

int vfs_readdir(const char *path, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    if (strcmp(path, "/") == 0) {
        int ret = vfs_readdir_at("/", vfs_root_readdir_pos, name, type);
        if (ret == 0) vfs_root_readdir_pos++;
        return ret;
    }
    if (tmpfs_is_mounted(path)) {
        int ret = tmpfs_readdir_at(path, vfs_tmpfs_readdir_pos, name, type);
        if (ret == 0) vfs_tmpfs_readdir_pos++;
        return ret;
    }
    return fs_readdir(path, name, type);
}

int vfs_readdir_at(const char *path, uint32_t index, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    /* 根目录：合成顶层目录 + 真实文件系统里的项。
     *
     * 早先只枚举硬编码的 roots[]（home/bin/tmp/dev/proc/sys）就返回，导致
     * 通过 vfs_register_static_file 建在根下的目录（如 /themes）永远不出现在
     * `ls /` 里——文件其实存在（cat /themes/ocean.theme 能读到），但列表看不见，
     * 属于"能读不能列"的割裂。现在先给合成的顶层目录，再继续走真实 fs 枚举，
     * 由调用方按 index 顺序取，两段都覆盖到。 */
    if (strcmp(path, "/") == 0) {
        static const char *roots[] = {"home", "bin", "tmp", "dev", "proc",
                                      "sys", "run"};
        uint32_t root_count = sizeof(roots) / sizeof(roots[0]);
        if (index < root_count) {
            strncpy(name, roots[index], VFS_MAX_NAME);
            name[VFS_MAX_NAME - 1] = 0;
            *type = VFS_NODE_DIR;
            return 0;
        }
        /* 越过合成项后，从真实 fs 的第 0 项开始补齐（index - root_count） */
        uint32_t want = index - root_count;
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
    /* /dev 下挂载了 /dev/shm：devfs 不知道它，先给挂载点再列 devfs */
    if (strcmp(path, "/dev") == 0) {
        if (index == 0) {
            strncpy(name, "shm", VFS_MAX_NAME);
            name[VFS_MAX_NAME - 1] = '\0';
            *type = VFS_NODE_DIR;
            return 0;
        }
        return devfs_readdir(path, index - 1, name, type);
    }
    if (strcmp(path, "/dev") == 0 ||
        strcmp(path, "/proc") == 0 ||
        strcmp(path, "/sys") == 0 ||
        (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
         path[3] == 'o' && path[4] == 'c' && path[5] == '/') ||
        strncmp(path, "/sys/", 5) == 0)
        return devfs_readdir(path, index, name, type);
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

        if (vfs_opendir(path) < 0) return -1;
        uint32_t want = index - listed;
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

int vfs_closedir(const char *path) {
    if (path && strcmp(path, "/") == 0) vfs_root_readdir_pos = 0;
    if (path && tmpfs_is_mounted(path)) {
        vfs_tmpfs_readdir_pos = 0;
        return 0;
    }
    return fs_closedir(path);
}
