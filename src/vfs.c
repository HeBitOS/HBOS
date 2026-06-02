/**
 * @file vfs.c
 * @brief 虚拟文件系统（VFS）实现，提供统一的文件操作接口
 */
#include "vfs.h"
#include "fs.h"
#include "devfs.h"

/** 初始化 VFS 层，委托给底层文件系统初始化 */
int vfs_init(void) {
    devfs_init();
    return fs_init();
}

/** 按路径查找 VFS 节点，找不到返回 NULL */
vfs_node_t *vfs_lookup(const char *path) {
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
    return fs_opendir(path);
}

int vfs_readdir(const char *path, char *name, uint32_t *type) {
    if (!path || !name || !type) return -1;
    return fs_readdir(path, name, type);
}

int vfs_closedir(const char *path) {
    return fs_closedir(path);
}
