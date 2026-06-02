/**
 * @file fs.h
 * @brief HBFS 文件系统头文件，定义文件系统数据结构与接口
 */
#ifndef FS_H
#define FS_H

#include "types.h"
#include "vfs.h"

// 文件系统相关定义
#define SECTOR_SIZE 512          /**< 扇区大小（字节） */
#define MAX_FILES 64             /**< 最大文件数量 */
#define MAX_FILENAME 32          /**< 文件名最大长度（含结尾 '\0'） */
#define RAMFS_MAX_FILE_SIZE 8192 /**< 内存文件系统单文件最大容量（字节） */

// 文件结构
typedef struct file {
    char name[MAX_FILENAME];     /**< 文件名 */
    uint32_t size;               /**< 文件当前大小（字节） */
    uint32_t capacity;           /**< 文件容量上限（字节） */
    uint8_t *data;               /**< 文件数据指针（ramfs 模式下指向内存缓冲区，HBFS 模式下为 NULL） */
    vfs_node_t node;             /**< 对应的 VFS 节点 */
    uint32_t disk_slot;          /**< 磁盘上的槽位索引 */
    uint8_t used;                /**< 是否被占用（0=空闲, 1=已使用） */
    uint8_t type; // 0=文件, 1=目录
} file_t;

// 文件系统结构
typedef struct filesystem {
    file_t files[MAX_FILES];     /**< 文件表 */
    uint32_t file_count;         /**< 当前已使用的文件数 */
    uint32_t total_sectors;      /**< 磁盘总扇区数 */
} filesystem_t;

/** 分区信息结构，用于描述 MBR/GPT 分区表中的单个分区 */
typedef struct {
    uint8_t present;             /**< 分区是否存在 */
    uint8_t bootable;            /**< 是否可引导 */
    uint8_t type;                /**< 分区类型（MBR 类型码） */
    uint32_t start_lba;          /**< 分区起始 LBA 地址 */
    uint32_t sectors;            /**< 分区扇区数 */
} fs_partition_info_t;

/** 文件系统一致性检查结果 */
typedef struct {
    uint32_t files_seen;         /**< 实际遍历到的文件数 */
    uint32_t file_count;         /**< 记录的文件计数 */
    uint32_t used_bytes;         /**< 已使用字节数 */
    uint32_t capacity_bytes;     /**< 总容量字节数 */
    uint32_t errors;             /**< 检测到的错误数 */
    const char *first_error;     /**< 首个错误描述字符串 */
} fs_check_result_t;

// 函数声明
int fs_init(void);               /**< 初始化文件系统 */
int fs_check(fs_check_result_t *out); /**< 检查文件系统一致性 */
int fs_format_disk(void);        /**< 格式化磁盘（使用默认范围） */
int fs_install_disk(void);       /**< 在磁盘上安装 HBFS 文件系统 */
int fs_install_disk_at(uint32_t start_lba, uint32_t sectors); /**< 在指定 LBA 范围安装 HBFS */
const char *fs_last_error(void); /**< 获取最近一次错误描述 */
int fs_read_partitions(fs_partition_info_t out[4]); /**< 读取分区表信息（最多 4 个分区） */
int fs_mount_disk(void);         /**< 挂载磁盘上的 HBFS 文件系统 */
int fs_sync(void);               /**< 将文件表同步到磁盘 */
int fs_is_disk(void);            /**< 判断当前后端是否为磁盘文件系统 */
const char *fs_backend_name(void); /**< 获取当前后端名称字符串 */
uint32_t fs_disk_start_lba(void); /**< 获取 HBFS 分区起始 LBA */
uint32_t fs_disk_total_sectors(void); /**< 获取 HBFS 分区总扇区数 */
uint32_t fs_min_sectors(void);   /**< 获取 HBFS 所需最小扇区数 */
uint32_t fs_capacity_bytes(void); /**< 获取文件系统总容量（字节） */
uint32_t fs_used_bytes(void);    /**< 获取已使用字节数 */
void fs_list(void);              /**< 列出所有文件（预留接口） */
file_t *fs_find_file(const char *name); /**< 按文件名查找文件 */
file_t *fs_create_file(const char *name); /**< 创建文件（若已存在则返回已有文件） */
int fs_delete_file(const char *name); /**< 删除文件 */
int fs_truncate_file(file_t *f); /**< 截断文件（清零大小） */
uint32_t fs_read_file_data(file_t *f, uint32_t offset, void *buf, uint32_t count); /**< 读取文件数据 */
int fs_write_file_data(file_t *f, uint32_t offset, const void *buf, uint32_t count); /**< 写入文件数据 */
uint32_t fs_get_count(void);     /**< 获取已使用文件数 */
file_t *fs_get_file(uint32_t index); /**< 按索引获取文件 */
vfs_node_t *fs_get_node(uint32_t index); /**< 按索引获取 VFS 节点 */
void fs_read_file(file_t *f);    /**< 读取文件（预留接口） */

int fs_mkdir(const char *name);
int fs_rmdir(const char *name);
int fs_opendir(const char *name);
int fs_readdir(const char *name, char *out_name, uint32_t *out_type);
int fs_closedir(const char *name);

#endif
