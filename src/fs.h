#ifndef FS_H
#define FS_H

#include "types.h"

// 文件系统相关定义
#define SECTOR_SIZE 512
#define MAX_FILES 128
#define MAX_FILENAME 32

// 文件结构
typedef struct file {
    char name[MAX_FILENAME];
    uint32_t size;
    uint32_t start_sector;
    uint8_t type; // 0=文件, 1=目录
} file_t;

// 文件系统结构
typedef struct filesystem {
    file_t files[MAX_FILES];
    uint32_t file_count;
    uint32_t total_sectors;
} filesystem_t;

// 函数声明
int fs_init(void);
void fs_list(void);
file_t *fs_find_file(const char *name);
void fs_read_file(file_t *f);

#endif