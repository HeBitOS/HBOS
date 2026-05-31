#ifndef FS_H
#define FS_H

#include "types.h"
#include "vfs.h"

// 文件系统相关定义
#define SECTOR_SIZE 512
#define MAX_FILES 64
#define MAX_FILENAME 32
#define RAMFS_MAX_FILE_SIZE 8192

// 文件结构
typedef struct file {
    char name[MAX_FILENAME];
    uint32_t size;
    uint32_t capacity;
    uint8_t *data;
    vfs_node_t node;
    uint32_t disk_slot;
    uint8_t used;
    uint8_t type; // 0=文件, 1=目录
} file_t;

// 文件系统结构
typedef struct filesystem {
    file_t files[MAX_FILES];
    uint32_t file_count;
    uint32_t total_sectors;
} filesystem_t;

typedef struct {
    uint8_t present;
    uint8_t bootable;
    uint8_t type;
    uint32_t start_lba;
    uint32_t sectors;
} fs_partition_info_t;

// 函数声明
int fs_init(void);
int fs_format_disk(void);
int fs_install_disk(void);
int fs_install_disk_at(uint32_t start_lba, uint32_t sectors);
const char *fs_last_error(void);
int fs_read_partitions(fs_partition_info_t out[4]);
int fs_mount_disk(void);
int fs_sync(void);
int fs_is_disk(void);
const char *fs_backend_name(void);
uint32_t fs_disk_start_lba(void);
uint32_t fs_disk_total_sectors(void);
uint32_t fs_min_sectors(void);
uint32_t fs_capacity_bytes(void);
uint32_t fs_used_bytes(void);
void fs_list(void);
file_t *fs_find_file(const char *name);
file_t *fs_create_file(const char *name);
int fs_delete_file(const char *name);
int fs_truncate_file(file_t *f);
uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count);
int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count);
uint32_t fs_get_count(void);
file_t *fs_get_file(uint32_t index);
vfs_node_t *fs_get_node(uint32_t index);
void fs_read_file(file_t *f);

#endif
