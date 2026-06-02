/**
 * @file vfs.h
 * @brief 虚拟文件系统（VFS）头文件，定义统一的文件操作接口与数据结构
 */
#ifndef HBOS_VFS_H
#define HBOS_VFS_H

#include "types.h"
#include "sys/types.h"

#define VFS_MAX_NAME 32         /**< 节点名称最大长度（含结尾 '\0'） */
#define VFS_MAX_NODES 64        /**< 最大 VFS 节点数 */

/** VFS 节点类型枚举 */
typedef enum {
    VFS_NODE_FILE = 0,          /**< 普通文件 */
    VFS_NODE_DIR,               /**< 目录 */
    VFS_NODE_CHARDEV            /**< 字符设备 */
} vfs_node_type_t;

typedef struct vfs_node vfs_node_t;

/** VFS 操作接口结构体，对标 Linux struct file_operations */
typedef struct {
    int (*read)(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
    int (*write)(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
    int (*truncate)(vfs_node_t *node);
    int (*unlink)(vfs_node_t *node);
    int (*ioctl)(vfs_node_t *node, uint32_t cmd, void *arg);
    int (*readdir)(vfs_node_t *node, uint32_t index, char *name, uint32_t *type);
} vfs_ops_t;

/** VFS 节点结构体，表示文件系统中的一个节点 */
struct vfs_node {
    char name[VFS_MAX_NAME];    /**< 节点名称 */
    vfs_node_type_t type;       /**< 节点类型 */
    uint32_t size;              /**< 当前数据大小（字节） */
    uint32_t capacity;          /**< 容量上限（字节） */
    void *private_data;         /**< 指向底层文件系统私有数据的指针 */
    const vfs_ops_t *ops;       /**< 操作接口函数指针表 */
};

int vfs_init(void);             /**< 初始化 VFS 层 */
vfs_node_t *vfs_lookup(const char *path); /**< 按路径查找 VFS 节点 */
vfs_node_t *vfs_create(const char *path); /**< 按路径创建 VFS 节点 */
int vfs_unlink(const char *path); /**< 按路径删除 VFS 节点 */
int vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count); /**< 从 VFS 节点读取数据 */
int vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count); /**< 向 VFS 节点写入数据 */
int vfs_truncate(vfs_node_t *node); /**< 截断 VFS 节点（清零大小） */
uint32_t vfs_count(void);      /**< 获取 VFS 节点数量 */
vfs_node_t *vfs_get(uint32_t index); /**< 按索引获取 VFS 节点 */

int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_opendir(const char *path);
int vfs_readdir(const char *path, char *name, uint32_t *type);
int vfs_closedir(const char *path);

#endif
