#include "vfs.h"
#include "fs.h"

int vfs_init(void) {
    return fs_init();
}

vfs_node_t *vfs_lookup(const char *path) {
    file_t *file = fs_find_file(path);
    return file ? &file->node : NULL;
}

vfs_node_t *vfs_create(const char *path) {
    file_t *file = fs_create_file(path);
    return file ? &file->node : NULL;
}

int vfs_unlink(const char *path) {
    vfs_node_t *node = vfs_lookup(path);
    if (!node || !node->ops || !node->ops->unlink) return -1;
    return node->ops->unlink(node);
}

int vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, buf, count);
}

int vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, offset, buf, count);
}

int vfs_truncate(vfs_node_t *node) {
    if (!node || !node->ops || !node->ops->truncate) return -1;
    return node->ops->truncate(node);
}

uint32_t vfs_count(void) {
    return fs_get_count();
}

vfs_node_t *vfs_get(uint32_t index) {
    return fs_get_node(index);
}
