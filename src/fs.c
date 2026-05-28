#include "fs.h"
#include "string.h"

// 全局文件系统实例
static filesystem_t fs;
static uint8_t ramfs_storage[MAX_FILES][RAMFS_MAX_FILE_SIZE];

static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count);
static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count);
static int ramfs_vfs_truncate(vfs_node_t *node);
static int ramfs_vfs_unlink(vfs_node_t *node);

static const vfs_ops_t ramfs_ops = {
    .read = ramfs_vfs_read,
    .write = ramfs_vfs_write,
    .truncate = ramfs_vfs_truncate,
    .unlink = ramfs_vfs_unlink,
};

static const char *normalize_path(const char *path, char *out) {
    if (!path) return NULL;
    while (*path == '/') path++;
    if (*path == '\0') return NULL;

    uint32_t i = 0;
    while (path[i]) {
        if (path[i] == '/') return NULL;
        if (i >= MAX_FILENAME - 1) return NULL;
        out[i] = path[i];
        i++;
    }
    out[i] = '\0';
    return out;
}

// 文件系统初始化
int fs_init(void) {
    fs.file_count = 0;
    fs.total_sectors = 0;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        fs.files[i].name[0] = '\0';
        fs.files[i].size = 0;
        fs.files[i].capacity = RAMFS_MAX_FILE_SIZE;
        fs.files[i].data = ramfs_storage[i];
        fs.files[i].node.private_data = &fs.files[i];
        fs.files[i].used = 0;
        fs.files[i].type = 0;
    }
    return 1;
}

// 列出文件
void fs_list(void) {
    // Shell commands use fs_get_count/fs_get_file to render lists.
}

// 查找文件
file_t *fs_find_file(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return NULL;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (fs.files[i].used && strcmp(fs.files[i].name, norm) == 0)
            return &fs.files[i];
    }
    return NULL;
}

file_t *fs_create_file(const char *name) {
    char norm[MAX_FILENAME];
    if (!normalize_path(name, norm)) return NULL;
    file_t *existing = fs_find_file(norm);
    if (existing) return existing;

    for (uint32_t i = 0; i < MAX_FILES; i++) {
        file_t *f = &fs.files[i];
        if (f->used) continue;
        strcpy(f->name, norm);
        f->size = 0;
        f->capacity = RAMFS_MAX_FILE_SIZE;
        f->data = ramfs_storage[i];
        f->node.type = VFS_NODE_FILE;
        f->node.size = 0;
        f->node.capacity = RAMFS_MAX_FILE_SIZE;
        f->node.private_data = f;
        f->node.ops = &ramfs_ops;
        strcpy(f->node.name, norm);
        f->used = 1;
        f->type = 0;
        fs.file_count++;
        return f;
    }
    return NULL;
}

int fs_delete_file(const char *name) {
    file_t *f = fs_find_file(name);
    if (!f) return -1;
    f->used = 0;
    f->name[0] = '\0';
    f->size = 0;
    f->node.name[0] = '\0';
    f->node.size = 0;
    if (fs.file_count > 0) fs.file_count--;
    return 0;
}

int fs_truncate_file(file_t *f) {
    if (!f || !f->used) return -1;
    f->size = 0;
    f->node.size = 0;
    return 0;
}

uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count) {
    if (!f || !f->used || !buf) return 0;
    if (offset >= f->size) return 0;
    uint32_t available = f->size - offset;
    if (count > available) count = available;
    memcpy(buf, f->data + offset, count);
    return count;
}

int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count) {
    if (!f || !f->used || (!buf && count)) return -1;
    if (offset > f->capacity || count > f->capacity - offset) return -1;
    memcpy(f->data + offset, buf, count);
    uint32_t end = offset + count;
    if (end > f->size) {
        f->size = end;
        f->node.size = end;
    }
    return (int)count;
}

uint32_t fs_get_count(void) {
    return fs.file_count;
}

file_t *fs_get_file(uint32_t index) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < MAX_FILES; i++) {
        if (!fs.files[i].used) continue;
        if (seen == index) return &fs.files[i];
        seen++;
    }
    return NULL;
}

vfs_node_t *fs_get_node(uint32_t index) {
    file_t *f = fs_get_file(index);
    return f ? &f->node : NULL;
}

static int ramfs_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count) {
    if (!node) return -1;
    return (int)fs_read_file_data((file_t *)node->private_data, offset, buf, count);
}

static int ramfs_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count) {
    if (!node) return -1;
    int ret = fs_write_file_data((file_t *)node->private_data, offset, buf, count);
    if (ret >= 0) node->size = ((file_t *)node->private_data)->size;
    return ret;
}

static int ramfs_vfs_truncate(vfs_node_t *node) {
    if (!node) return -1;
    int ret = fs_truncate_file((file_t *)node->private_data);
    if (ret == 0) node->size = 0;
    return ret;
}

static int ramfs_vfs_unlink(vfs_node_t *node) {
    if (!node) return -1;
    return fs_delete_file(node->name);
}

// 读取文件
void fs_read_file(file_t *f) {
    (void)f;
}
