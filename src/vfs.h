#ifndef HBOS_VFS_H
#define HBOS_VFS_H

#include "types.h"
#include "sys/types.h"

#define VFS_MAX_NAME 32
#define VFS_MAX_NODES 64

typedef enum {
    VFS_NODE_FILE = 0,
    VFS_NODE_DIR,
    VFS_NODE_CHARDEV
} vfs_node_type_t;

typedef struct vfs_node vfs_node_t;

typedef struct {
    int (*read)(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
    int (*write)(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
    int (*truncate)(vfs_node_t *node);
    int (*unlink)(vfs_node_t *node);
} vfs_ops_t;

struct vfs_node {
    char name[VFS_MAX_NAME];
    vfs_node_type_t type;
    uint32_t size;
    uint32_t capacity;
    void *private_data;
    const vfs_ops_t *ops;
};

int vfs_init(void);
vfs_node_t *vfs_lookup(const char *path);
vfs_node_t *vfs_create(const char *path);
int vfs_unlink(const char *path);
int vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
int vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
int vfs_truncate(vfs_node_t *node);
uint32_t vfs_count(void);
vfs_node_t *vfs_get(uint32_t index);

#endif
