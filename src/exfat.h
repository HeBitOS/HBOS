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
typedef int (*exfat_write_fn)(void *ctx, uint64_t lba, const void *buf);

typedef struct {
    exfat_read_fn rd;
    exfat_write_fn wr;          /* 写回调；为 NULL 时只读 */
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

/** 列出任意目录（以起始簇标识）中的条目；根目录传 fs->root_cluster。 */
int exfat_list_dir(exfat_fs_t *fs, uint32_t first_cluster,
                   exfat_dirent_t *out, int max);

/** 目录遍历回调：返回非 0 停止遍历。 */
typedef int (*exfat_dir_cb)(void *ctx, const exfat_dirent_t *e);

/** 逐条遍历目录，不缓存整目录（适合递归展开）。 */
int exfat_iter_dir(exfat_fs_t *fs, uint32_t first_cluster,
                   exfat_dir_cb cb, void *ctx);

/** 读取文件的一段；返回实际读取字节数（<0 失败）。 */
int exfat_read_file(exfat_fs_t *fs, uint32_t first_cluster, uint64_t size,
                    uint64_t offset, void *buf, uint32_t len);

/**
 * @brief 在根目录创建一个文件并写入数据（一次性，不支持追加）。
 *
 * 需要 wr 回调；目前只支持根目录、名字长度 <= 255、文件连续分配。
 * 返回 0 成功。
 */
int exfat_create_file(exfat_fs_t *fs, const char *name,
                      const void *data, uint32_t size);

/**
 * @brief 在已有文件上按偏移写入（可扩展；只支持根目录、ASCII 名字）。
 *
 * offset 必须 <= 当前文件大小（不支持挖洞）。返回 0 成功。
 */
int exfat_write_file(exfat_fs_t *fs, const char *name, uint64_t offset,
                     const void *data, uint32_t len);

/** 卷标（最多 11 个 UTF-16 字符），无卷标返回空串。 */
void exfat_volume_label(exfat_fs_t *fs, char *out, int cap);

#endif /* HBOS_EXFAT_H */
