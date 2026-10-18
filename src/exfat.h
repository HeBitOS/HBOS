/**
 * @file    exfat.h
 * @brief   exFAT 只读驱动（挂载 / 列目录 / 读文件）
 *
 * 只读实现：exFAT 目前是 U 盘和移动硬盘的主流格式，HBOS 之前完全认不出来
 * （类型码 0x07 的盘会直接退回 ramfs）。这里先解决"能挂、能列、能读"，
 * 写入（分配位图、目录项校验和）留到后续。
 *
 * 与块设备解耦：通过读扇区回调访问，宿主机可以用文件镜像直接单测
 * （tests/host/test_exfat.c）。
 */

#ifndef HBOS_EXFAT_H
#define HBOS_EXFAT_H

#include <stdint.h>

#define EXFAT_NAME_MAX 256

typedef int (*exfat_read_fn)(void *ctx, uint64_t lba, void *buf);

typedef struct {
    exfat_read_fn rd;
    void *ctx;
    uint64_t volume_lba;        /* 分区起始 LBA */
    uint32_t bytes_per_sector;  /* 512 / 1024 / ... */
    uint32_t sectors_per_cluster;
    uint32_t fat_offset;        /* 相对分区起点的扇区号 */
    uint32_t fat_length;
    uint32_t heap_offset;
    uint32_t cluster_count;
    uint32_t root_cluster;
} exfat_fs_t;

typedef struct {
    char     name[EXFAT_NAME_MAX];   /* UTF-8 */
    uint32_t first_cluster;
    uint64_t size;
    uint8_t  attr;                   /* bit4=目录 */
} exfat_dirent_t;

/** 挂载：校验引导扇区并填 fs；成功返回 0。 */
int exfat_mount(exfat_fs_t *fs, exfat_read_fn rd, void *ctx, uint64_t volume_lba);

/** 列出根目录中的文件/目录，返回条目数（<0 失败）。 */
int exfat_list_root(exfat_fs_t *fs, exfat_dirent_t *out, int max);

/** 读取文件的一段；返回实际读取字节数（<0 失败）。 */
int exfat_read_file(exfat_fs_t *fs, uint32_t first_cluster, uint64_t size,
                    uint64_t offset, void *buf, uint32_t len);

/** 卷标（最多 11 个 UTF-16 字符），无卷标返回空串。 */
void exfat_volume_label(exfat_fs_t *fs, char *out, int cap);

#endif /* HBOS_EXFAT_H */
