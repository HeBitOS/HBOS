/**
 * @file    tmpfs.c
 * @brief   真 tmpfs：内存 inode 树 + 按页分配的动态容量
 *
 * 设计要点（为什么不是"再写一个 ramfs"）
 * ══════════════════════════════════
 * 1. 节点是一棵树，不是平铺的文件名表。目录/文件/符号链接都是节点，因此
 *    lookup/readdir/rename/unlink 只有一套语义；旧方案里"父目录在内存表、
 *    文件在 memfd 覆盖层"的割裂正是 Chromium 建完目录却 rename 不到的原因。
 * 2. 容量按 4096 字节页增长，`size` 与 `capacity` 分离：稀疏写（sqlite 写到
 *    4 KiB 边界、Chromium 把文件 preallocate 到几 MB）不会立刻吃满配额。
 * 3. 每个挂载点有 max_bytes 配额，超了返回 -ENOSPC —— 内存文件系统也必须
 *    有上限，否则一个 bug 就是 OOM panic。
 * 4. 分配器由外部注入：内核给 kmalloc/kfree，宿主单测给 malloc/free。
 *    这让整个模块可以在 tests/host 里被完整覆盖，不需要启动内核。
 *
 * 不变量：
 *  - 每个节点的 node.name 是**绝对路径**（与 VFS/stat/inotify 的口径一致），
 *    leaf 单独存一份用于兄弟匹配；rename 目录时必须递归刷新两者。
 *  - node.size ≤ node.capacity，且 data 覆盖 [0, capacity) 全部可读字节。
 *  - freed 掉的页立刻从 mount->used_bytes 扣除，配额不得漂移。
 */
#include "fs/tmpfs.h"

#include "errno.h"
#include "string.h"
#include "sys/stat.h"

typedef struct tmpfs_node {
    vfs_node_t node;              /**< 必须第一个成员：VFS 只认这个指针 */
    struct tmpfs_node *parent;
    struct tmpfs_node *children;  /**< 子节点单链表（无序，readdir 顺序稳定） */
    struct tmpfs_node *next;      /**< 兄弟链 */
    struct tmpfs_mount *mount;
    uint8_t *data;                /**< 数据页（容量按 TMPFS_PAGE_BYTES 对齐） */
    char leaf[VFS_MAX_NAME];      /**< 本节点的名字（不含父路径） */
} tmpfs_node_t;

typedef struct tmpfs_mount {
    char path[VFS_MAX_NAME];
    tmpfs_node_t *root;
    uint32_t max_bytes;
    uint32_t used_bytes;
    int used;
} tmpfs_mount_t;

static tmpfs_alloc_t g_alloc;
static tmpfs_mount_t g_mounts[TMPFS_MAX_MOUNTS];

static int tmpfs_op_read(vfs_node_t *node, uint32_t offset, void *buf,
                         uint32_t count);
static int tmpfs_op_write(vfs_node_t *node, uint32_t offset, const void *buf,
                          uint32_t count);
static int tmpfs_op_truncate(vfs_node_t *node, uint32_t size);
static int tmpfs_op_readdir(vfs_node_t *node, uint32_t index, char *name,
                            uint32_t *type);
static int tmpfs_op_unlink(vfs_node_t *node);

static const vfs_ops_t tmpfs_ops = {
    .read = tmpfs_op_read,
    .write = tmpfs_op_write,
    .truncate = tmpfs_op_truncate,
    .unlink = tmpfs_op_unlink,
    .ioctl = 0,
    .readdir = tmpfs_op_readdir,
};

static uint32_t tmpfs_round_up(uint32_t bytes) {
    return (bytes + TMPFS_PAGE_BYTES - 1u) & ~(TMPFS_PAGE_BYTES - 1u);
}

void tmpfs_set_allocator(const tmpfs_alloc_t *alloc) {
    if (alloc) g_alloc = *alloc;
}

static int tmpfs_ready(void) {
    return g_alloc.alloc != 0 && g_alloc.free != 0;
}

/**
 * @brief 找出 path 落在哪个挂载点里，返回相对挂载根的剩余路径。
 *
 * 取**最长**前缀匹配：将来若有 /tmp 与 /tmp/sub 两个挂载点，后者优先。
 * 前缀必须落在 '/' 边界上，否则 /tmpfoo 会被 /tmp 吞掉。
 */
static tmpfs_mount_t *tmpfs_mount_for(const char *path, const char **rel) {
    tmpfs_mount_t *best = 0;
    size_t best_len = 0;
    for (uint32_t i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        tmpfs_mount_t *m = &g_mounts[i];
        if (!m->used) continue;
        size_t len = strlen(m->path);
        if (strncmp(path, m->path, len) != 0) continue;
        if (path[len] != '\0' && path[len] != '/') continue;
        if (len >= best_len) { best = m; best_len = len; }
    }
    if (!best) return 0;
    const char *p = path + best_len;
    while (*p == '/') p++;
    if (rel) *rel = p;
    return best;
}

int tmpfs_is_mounted(const char *path) {
    if (!path || !path[0]) return 0;
    return tmpfs_mount_for(path, 0) != 0;
}

uint32_t tmpfs_mount_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < TMPFS_MAX_MOUNTS; i++)
        if (g_mounts[i].used) count++;
    return count;
}

const char *tmpfs_mount_path(uint32_t index) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].used) continue;
        if (seen++ == index) return g_mounts[i].path;
    }
    return 0;
}

uint32_t tmpfs_bytes_used(const char *path) {
    const char *rel = 0;
    tmpfs_mount_t *m = path ? tmpfs_mount_for(path, &rel) : 0;
    return m ? m->used_bytes : 0;
}

static tmpfs_node_t *tmpfs_node_alloc(tmpfs_mount_t *m, tmpfs_node_t *parent,
                                      const char *leaf, const char *abs_path,
                                      int type, uint32_t mode) {
    if (!tmpfs_ready()) return 0;
    tmpfs_node_t *n = (tmpfs_node_t *)g_alloc.alloc((uint32_t)sizeof(*n));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    strncpy(n->leaf, leaf, sizeof(n->leaf) - 1);
    strncpy(n->node.name, abs_path, sizeof(n->node.name) - 1);
    n->node.type = (vfs_node_type_t)type;
    /* mode 必须带 S_IFMT：stat/ls 靠它区分目录与普通文件，调用方通常只传
     * 权限位（0755/0644）。 */
    uint32_t ifmt = (type == VFS_NODE_DIR) ? S_IFDIR
                  : (type == VFS_NODE_SYMLINK) ? S_IFLNK : S_IFREG;
    n->node.mode = ifmt | (mode & 07777u);
    n->node.uid = 0;
    n->node.gid = 0;
    n->parent = parent;
    n->mount = m;
    n->node.private_data = n;
    n->node.ops = &tmpfs_ops;
    return n;
}

static int tmpfs_attach(tmpfs_node_t *parent, tmpfs_node_t *child) {
    if (!parent || parent->node.type != VFS_NODE_DIR) return -ENOTDIR;
    child->parent = parent;
    child->next = parent->children;
    parent->children = child;
    return 0;
}

static void tmpfs_detach(tmpfs_node_t *child) {
    tmpfs_node_t *parent = child ? child->parent : 0;
    if (!parent) return;
    tmpfs_node_t **link = &parent->children;
    while (*link) {
        if (*link == child) { *link = child->next; break; }
        link = &(*link)->next;
    }
    child->parent = 0;
    child->next = 0;
}

static int tmpfs_has_children(const tmpfs_node_t *dir) {
    return dir && dir->children != 0;
}

/** 释放数据页并同步配额。 */
static void tmpfs_release_data(tmpfs_node_t *n) {
    if (!n || !n->data) return;
    if (n->mount && n->mount->used_bytes >= n->node.capacity)
        n->mount->used_bytes -= n->node.capacity;
    g_alloc.free(n->data);
    n->data = 0;
    n->node.capacity = 0;
    n->node.size = 0;
}

/**
 * @brief 确保节点容量 ≥ need，按页增长；新增页清零。
 *
 * @return 0 成功；-ENOSPC 配额不足或分配失败（不改动节点）
 */
static int tmpfs_reserve(tmpfs_node_t *n, uint32_t need) {
    if (need <= n->node.capacity) return 0;
    tmpfs_mount_t *m = n->mount;
    uint32_t cap = tmpfs_round_up(need);
    if (m) {
        uint32_t accounted = m->used_bytes - n->node.capacity;
        if (accounted + cap > m->max_bytes) return -ENOSPC;
    }
    uint8_t *data = (uint8_t *)g_alloc.alloc(cap);
    if (!data) return -ENOSPC;
    memset(data, 0, cap);
    if (n->data && n->node.size)
        memcpy(data, n->data, n->node.size < n->node.capacity
                                  ? n->node.size : n->node.capacity);
    if (n->data) {
        if (m && m->used_bytes >= n->node.capacity)
            m->used_bytes -= n->node.capacity;
        g_alloc.free(n->data);
    }
    n->data = data;
    n->node.capacity = cap;
    if (m) m->used_bytes += cap;
    return 0;
}

/** 把容量收缩到 round_up(size)，释放尾部空闲页。 */
static void tmpfs_trim(tmpfs_node_t *n) {
    if (!n->data) return;
    uint32_t cap = tmpfs_round_up(n->node.size);
    if (cap >= n->node.capacity) return;
    if (cap == 0) { tmpfs_release_data(n); return; }
    /* 不需要搬数据：只是把尾部页还给分配器，但分配器没有 realloc，
     * 所以新开一块、拷贝、释放旧块。 */
    uint8_t *data = (uint8_t *)g_alloc.alloc(cap);
    if (!data) return;
    memset(data, 0, cap);
    memcpy(data, n->data, n->node.size);
    if (n->mount && n->mount->used_bytes >= n->node.capacity)
        n->mount->used_bytes -= n->node.capacity;
    g_alloc.free(n->data);
    n->data = data;
    n->node.capacity = cap;
    if (n->mount) n->mount->used_bytes += cap;
}

static void tmpfs_node_free(tmpfs_node_t *n) {
    if (!n) return;
    tmpfs_release_data(n);
    g_alloc.free(n);
}

/* ── vfs_ops ─────────────────────────────────────────────────── */

static int tmpfs_op_read(vfs_node_t *node, uint32_t offset, void *buf,
                         uint32_t count) {
    tmpfs_node_t *n = (tmpfs_node_t *)node;
    if (!n || !buf) return -EINVAL;
    if (node->type == VFS_NODE_DIR) return -EISDIR;
    if (offset >= node->size) return 0;
    if (count > node->size - offset) count = node->size - offset;
    if (count) {
        if (n->data) memcpy(buf, n->data + offset, count);
        else memset(buf, 0, count);
    }
    return (int)count;
}

static int tmpfs_op_write(vfs_node_t *node, uint32_t offset, const void *buf,
                          uint32_t count) {
    tmpfs_node_t *n = (tmpfs_node_t *)node;
    if (!n || !buf) return -EINVAL;
    if (node->type == VFS_NODE_DIR) return -EISDIR;
    if (node->type == VFS_NODE_SYMLINK) return -EINVAL;
    if (count > 0xFFFFFFFFu - offset) return -EFBIG;
    int r = tmpfs_reserve(n, offset + count);
    if (r < 0) return r;
    if (offset > node->size) memset(n->data + node->size, 0, offset - node->size);
    memcpy(n->data + offset, buf, count);
    if (offset + count > node->size) node->size = offset + count;
    return (int)count;
}

static int tmpfs_op_truncate(vfs_node_t *node, uint32_t size) {
    tmpfs_node_t *n = (tmpfs_node_t *)node;
    if (!n) return -EINVAL;
    if (node->type == VFS_NODE_DIR) return -EISDIR;
    if (size == node->size) return 0;
    if (size < node->size) {
        /* 缩小时把尾部清零：之后再扩回来必须读到 0，而不是旧数据 */
        if (n->data) memset(n->data + size, 0, node->size - size);
        node->size = size;
        tmpfs_trim(n);
        return 0;
    }
    int r = tmpfs_reserve(n, size);
    if (r < 0) return r;
    if (n->data) memset(n->data + node->size, 0, size - node->size);
    node->size = size;
    return 0;
}

static int tmpfs_op_readdir(vfs_node_t *node, uint32_t index, char *name,
                            uint32_t *type) {
    tmpfs_node_t *n = (tmpfs_node_t *)node;
    if (!n || !name || !type) return -EINVAL;
    if (node->type != VFS_NODE_DIR) return -ENOTDIR;
    tmpfs_node_t *child = n->children;
    uint32_t seen = 0;
    while (child) {
        if (seen == index) {
            strncpy(name, child->leaf, VFS_MAX_NAME);
            name[VFS_MAX_NAME - 1] = '\0';
            *type = (uint32_t)child->node.type;
            return 0;
        }
        seen++;
        child = child->next;
    }
    return -ENOENT;
}

static int tmpfs_op_unlink(vfs_node_t *node) {
    tmpfs_node_t *n = (tmpfs_node_t *)node;
    if (!n || !n->parent) return -EINVAL;
    if (node->type == VFS_NODE_DIR) {
        if (tmpfs_has_children(n)) return -ENOTEMPTY;
    }
    tmpfs_detach(n);
    tmpfs_node_free(n);
    return 0;
}

/* ── 路径解析 ────────────────────────────────────────────────── */

/** 取 path 的父路径 + 最后一级名字。 */
static int tmpfs_split_path(const char *path, char *parent, uint32_t parent_cap,
                            char *leaf, uint32_t leaf_cap) {
    if (!path || path[0] != '/') return -EINVAL;
    const char *slash = strrchr(path, '/');
    if (!slash) return -EINVAL;
    uint32_t plen = (uint32_t)(slash - path);
    if (plen == 0) plen = 1;                     /* "/x" → parent "/" */
    if (plen >= parent_cap) return -EINVAL;
    memcpy(parent, path, plen);
    parent[plen] = '\0';
    const char *name = slash + 1;
    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen == 0 || nlen >= leaf_cap) return -EINVAL;
    memcpy(leaf, name, nlen + 1);
    return 0;
}

/** 逐级查找（只走本挂载点的树）。 */
vfs_node_t *tmpfs_lookup(const char *path) {
    const char *rel = 0;
    tmpfs_mount_t *m = path ? tmpfs_mount_for(path, &rel) : 0;
    if (!m) return 0;
    tmpfs_node_t *cur = m->root;
    while (rel && *rel) {
        char name[VFS_MAX_NAME];
        uint32_t len = 0;
        while (rel[len] && rel[len] != '/' && len < sizeof(name) - 1) {
            name[len] = rel[len];
            len++;
        }
        name[len] = '\0';
        while (rel[len] == '/') len++;           /* 不移动 rel 指针，下面统一推进 */
        if (cur->node.type != VFS_NODE_DIR) return 0;
        tmpfs_node_t *child = cur->children;
        while (child && strcmp(child->leaf, name) != 0) child = child->next;
        if (!child) return 0;
        cur = child;
        rel += len;
    }
    return &cur->node;
}

/** 在 parent 下建节点；返回 0 或 -errno，成功时 out 回填。 */
static int tmpfs_add(const char *parent_path, const char *leaf, int type,
                     uint32_t mode, tmpfs_node_t **out) {
    tmpfs_mount_t *m = tmpfs_mount_for(parent_path, 0);
    if (!m) return -EINVAL;
    if (!tmpfs_ready()) return -ENOMEM;
    vfs_node_t *pv = tmpfs_lookup(parent_path);
    if (!pv) return -ENOENT;
    if (pv->type != VFS_NODE_DIR) return -ENOTDIR;
    tmpfs_node_t *parent = (tmpfs_node_t *)pv;

    tmpfs_node_t *probe = parent->children;
    while (probe && strcmp(probe->leaf, leaf) != 0) probe = probe->next;
    if (probe) return -EEXIST;

    char abs[VFS_MAX_NAME];
    size_t plen = strlen(parent_path);
    size_t llen = strlen(leaf);
    if (plen + llen + 2 > sizeof(abs)) return -EINVAL;
    strcpy(abs, parent_path);
    if (abs[plen - 1] != '/') strcat(abs, "/");
    strcat(abs, leaf);

    tmpfs_node_t *n = tmpfs_node_alloc(m, parent, leaf, abs, type, mode);
    if (!n) return -ENOMEM;
    tmpfs_attach(parent, n);
    if (out) *out = n;
    return 0;
}

vfs_node_t *tmpfs_create(const char *path, int type, uint32_t mode) {
    char parent[VFS_MAX_NAME], leaf[VFS_MAX_NAME];
    if (tmpfs_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0)
        return 0;
    tmpfs_node_t *n = 0;
    if (tmpfs_add(parent, leaf, type, mode, &n) < 0) return 0;
    return &n->node;
}

vfs_node_t *tmpfs_symlink(const char *path, const char *target) {
    if (!target || !target[0] || strlen(target) >= VFS_MAX_NAME) return 0;
    char parent[VFS_MAX_NAME], leaf[VFS_MAX_NAME];
    if (tmpfs_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0)
        return 0;
    tmpfs_node_t *n = 0;
    if (tmpfs_add(parent, leaf, VFS_NODE_SYMLINK, 0777, &n) < 0) return 0;
    uint32_t len = (uint32_t)strlen(target);
    if (tmpfs_reserve(n, len) < 0) {          /* 目标文本就是链接的数据 */
        tmpfs_detach(n);
        tmpfs_node_free(n);
        return 0;
    }
    memcpy(n->data, target, len);
    n->node.size = len;
    return &n->node;
}

int tmpfs_mkdir(const char *path) {
    if (!path) return -EINVAL;
    /* mkdir -p 语义：Chromium 会直接建多层 /tmp/.config/.../database */
    char partial[VFS_MAX_NAME];
    size_t len = strlen(path);
    if (len >= sizeof(partial) || path[0] != '/') return -EINVAL;
    memcpy(partial, path, len + 1);
    tmpfs_mount_t *m = tmpfs_mount_for(path, 0);
    if (!m) return -EINVAL;
    size_t start = strlen(m->path);
    for (size_t i = start + 1; i <= len; i++) {
        if (partial[i] != '/' && partial[i] != '\0') continue;
        char saved = partial[i];
        partial[i] = '\0';
        if (!tmpfs_lookup(partial)) {
            char parent[VFS_MAX_NAME], leaf[VFS_MAX_NAME];
            if (tmpfs_split_path(partial, parent, sizeof(parent),
                                 leaf, sizeof(leaf)) < 0) {
                partial[i] = saved;
                return -EINVAL;
            }
            vfs_node_t *pv = tmpfs_lookup(parent);
            if (!pv) { partial[i] = saved; return -ENOENT; }
            if (pv->type != VFS_NODE_DIR) { partial[i] = saved; return -ENOTDIR; }
            int r = tmpfs_add(parent, leaf, VFS_NODE_DIR, 0755, 0);
            if (r < 0 && r != -EEXIST) { partial[i] = saved; return r; }
        }
        partial[i] = saved;
    }
    return 0;
}

int tmpfs_rmdir(const char *path) {
    vfs_node_t *node = tmpfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type != VFS_NODE_DIR) return -ENOTDIR;
    tmpfs_node_t *n = (tmpfs_node_t *)node;
    if (!n->parent) return -EBUSY;              /* 挂载根不能删 */
    if (tmpfs_has_children(n)) return -ENOTEMPTY;
    tmpfs_detach(n);
    tmpfs_node_free(n);
    return 0;
}

int tmpfs_unlink(const char *path) {
    vfs_node_t *node = tmpfs_lookup(path);
    if (!node) return -ENOENT;
    if (node->type == VFS_NODE_DIR) return -EISDIR;
    tmpfs_node_t *n = (tmpfs_node_t *)node;
    tmpfs_detach(n);
    tmpfs_node_free(n);
    return 0;
}

int tmpfs_rename(const char *old_path, const char *new_path, int replace) {
    if (!old_path || !new_path) return -EINVAL;
    const char *rel = 0;
    tmpfs_mount_t *old_m = tmpfs_mount_for(old_path, &rel);
    tmpfs_mount_t *new_m = tmpfs_mount_for(new_path, 0);
    if (!old_m) return -ENOENT;
    if (old_m != new_m) return -EXDEV;

    vfs_node_t *src_v = tmpfs_lookup(old_path);
    if (!src_v) return -ENOENT;
    tmpfs_node_t *src = (tmpfs_node_t *)src_v;
    if (!src->parent) return -EBUSY;
    if (strcmp(old_path, new_path) == 0) return 0;

    char parent_path[VFS_MAX_NAME], leaf[VFS_MAX_NAME];
    if (tmpfs_split_path(new_path, parent_path, sizeof(parent_path),
                         leaf, sizeof(leaf)) < 0) return -EINVAL;
    vfs_node_t *pv = tmpfs_lookup(parent_path);
    if (!pv) return -ENOENT;
    if (pv->type != VFS_NODE_DIR) return -ENOTDIR;
    tmpfs_node_t *new_parent = (tmpfs_node_t *)pv;

    /* 目录不能搬进自己的子树 */
    if (src_v->type == VFS_NODE_DIR) {
        for (tmpfs_node_t *p = new_parent; p; p = p->parent)
            if (p == src) return -EINVAL;
    }

    vfs_node_t *tgt_v = tmpfs_lookup(new_path);
    if (tgt_v) {
        if (tgt_v == src_v) return 0;
        int src_is_dir = (src_v->type == VFS_NODE_DIR);
        int tgt_is_dir = (tgt_v->type == VFS_NODE_DIR);
        if (!src_is_dir && tgt_is_dir) return -EISDIR;
        if (src_is_dir && !tgt_is_dir) return -ENOTDIR;
        if (tgt_is_dir && tmpfs_has_children((tmpfs_node_t *)tgt_v))
            return -ENOTEMPTY;
        if (!replace) return -EEXIST;
        tmpfs_detach((tmpfs_node_t *)tgt_v);
        tmpfs_node_free((tmpfs_node_t *)tgt_v);
    }

    tmpfs_detach(src);
    tmpfs_attach(new_parent, src);

    /* 名字与绝对路径：目录必须递归刷新整棵子树 */
    size_t old_len = strlen(old_path);
    strncpy(src->leaf, leaf, sizeof(src->leaf) - 1);
    src->leaf[sizeof(src->leaf) - 1] = '\0';
    tmpfs_node_t *stack[64];
    uint32_t depth = 0;
    strncpy(src_v->name, new_path, sizeof(src_v->name) - 1);
    src_v->name[sizeof(src_v->name) - 1] = '\0';
    stack[depth++] = src;
    while (depth) {
        tmpfs_node_t *cur = stack[--depth];
        for (tmpfs_node_t *c = cur->children; c; c = c->next) {
            const char *suffix = c->node.name + old_len;
            char updated[VFS_MAX_NAME];
            strncpy(updated, new_path, sizeof(updated) - 1);
            updated[sizeof(updated) - 1] = '\0';
            if (strlen(updated) + strlen(suffix) < sizeof(updated))
                strcat(updated, suffix);
            strncpy(c->node.name, updated, sizeof(c->node.name) - 1);
            c->node.name[sizeof(c->node.name) - 1] = '\0';
            if (depth < sizeof(stack) / sizeof(stack[0])) stack[depth++] = c;
        }
    }
    return 0;
}

int tmpfs_readdir_at(const char *path, uint32_t index, char *name,
                     uint32_t *type) {
    vfs_node_t *node = tmpfs_lookup(path);
    if (!node) return -ENOENT;
    return tmpfs_op_readdir(node, index, name, type);
}

/* ── 挂载 ───────────────────────────────────────────────────── */

int tmpfs_mount(const char *path, uint32_t max_bytes) {
    if (!path || path[0] != '/' || !path[1]) return -EINVAL;
    if (strlen(path) >= VFS_MAX_NAME) return -EINVAL;
    if (!tmpfs_ready()) return -ENOMEM;
    if (tmpfs_mount_for(path, 0)) return -EEXIST;   /* 已挂载（或落在别的挂载点里） */
    for (uint32_t i = 0; i < TMPFS_MAX_MOUNTS; i++) {
        tmpfs_mount_t *m = &g_mounts[i];
        if (m->used) continue;
        memset(m, 0, sizeof(*m));
        strncpy(m->path, path, sizeof(m->path) - 1);
        m->max_bytes = max_bytes;
        /* 挂载根按 tmpfs 惯例给 1777（/tmp、/dev/shm 都是这个语义） */
        m->root = tmpfs_node_alloc(m, 0, "", path, VFS_NODE_DIR, 01777);
        if (!m->root) return -ENOMEM;
        m->used = 1;
        return 0;
    }
    return -ENOSPC;
}
