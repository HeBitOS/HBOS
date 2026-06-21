/**
 * @file vfs.c
 * @brief 虚拟文件系统（VFS）实现，提供统一的文件操作接口
 */
#include "vfs.h"
#include "fs.h"
#include "devfs.h"
#include "string.h"

static vfs_node_t vfs_root_node = {
    .name = "/",
    .type = VFS_NODE_DIR,
    .size = 0,
    .capacity = 0,
    .private_data = NULL,
    .ops = NULL,
};

static vfs_node_t vfs_virtual_dirs[] = {
    {.name = "/dev",  .type = VFS_NODE_DIR},
    {.name = "/proc", .type = VFS_NODE_DIR},
    {.name = "/home", .type = VFS_NODE_DIR},
    {.name = "/bin",  .type = VFS_NODE_DIR},
    {.name = "/tmp",  .type = VFS_NODE_DIR},
};
static uint32_t vfs_root_readdir_pos;

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

/** 初始化 VFS 层，委托给底层文件系统初始化 */
int vfs_init(void) {
    devfs_init();
    return fs_init();
}

/** 按路径查找 VFS 节点，找不到返回 NULL */
vfs_node_t *vfs_lookup(const char *path) {
    if (!path) return NULL;
    if (strcmp(path, "/") == 0) return &vfs_root_node;
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
            };
            return &proc_pid_node;
        }
    }
    vfs_node_t *df = devfs_lookup(path);
    if (df) return df;
    file_t *file = fs_find_file(path);
    return file ? &file->node : NULL;
}

/** 按路径创建 VFS 节点，若已存在则返回已有节点 */
vfs_node_t *vfs_create(const char *path) {
    file_t *file = fs_create_file(path);
    return file ? &file->node : NULL;
}

/** 按路径删除 VFS 节点，通过节点操作接口调用底层 unlink */
int vfs_unlink(const char *path) {
    vfs_node_t *node = vfs_lookup(path);
    if (!node || !node->ops || !node->ops->unlink) return -1;
    return node->ops->unlink(node);
}

/** 从 VFS 节点读取数据，通过节点操作接口调用底层 read */
int vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, buf, count);
}

/** 向 VFS 节点写入数据，通过节点操作接口调用底层 write */
int vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, offset, buf, count);
}

/** 截断 VFS 节点（清零大小），通过节点操作接口调用底层 truncate */
int vfs_truncate(vfs_node_t *node) {
    if (!node || !node->ops || !node->ops->truncate) return -1;
    return node->ops->truncate(node);
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
    return fs_mkdir(path);
}

int vfs_rmdir(const char *path) {
    if (!path) return -1;
    return fs_rmdir(path);
}

int vfs_opendir(const char *path) {
    if (!path) return -1;
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
    return fs_opendir(path);
}

int vfs_readdir(const char *path, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    if (strcmp(path, "/") == 0) {
        int ret = vfs_readdir_at("/", vfs_root_readdir_pos, name, type);
        if (ret == 0) vfs_root_readdir_pos++;
        return ret;
    }
    return fs_readdir(path, name, type);
}

int vfs_readdir_at(const char *path, uint32_t index, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    if (strcmp(path, "/") == 0) {
        static const char *roots[] = {"home", "bin", "tmp", "dev", "proc"};
        uint32_t root_count = sizeof(roots) / sizeof(roots[0]);
        if (index < root_count) {
            strncpy(name, roots[index], VFS_MAX_NAME);
            name[VFS_MAX_NAME - 1] = 0;
            *type = VFS_NODE_DIR;
            return 0;
        }
        fs_closedir("/");
        for (uint32_t i = root_count; i <= index; i++) {
            if (fs_readdir("/", name, type) < 0) {
                fs_closedir("/");
                return -1;
            }
        }
        fs_closedir("/");
        return 0;
    }
    if (strcmp(path, "/dev") == 0 ||
        strcmp(path, "/proc") == 0 ||
        (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
         path[3] == 'o' && path[4] == 'c' && path[5] == '/'))
        return devfs_readdir(path, index, name, type);
    if (vfs_opendir(path) < 0) return -1;
    for (uint32_t i = 0; i <= index; i++) {
        if (vfs_readdir(path, name, type) < 0) {
            vfs_closedir(path);
            return -1;
        }
    }
    vfs_closedir(path);
    return 0;
}

int vfs_closedir(const char *path) {
    if (path && strcmp(path, "/") == 0) vfs_root_readdir_pos = 0;
    return fs_closedir(path);
}
