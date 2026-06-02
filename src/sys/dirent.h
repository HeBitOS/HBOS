/**
 * @file    sys/dirent.h
 * @brief   Linux 目录遍历 — HBOS 实现
 *
 * 对标 Linux dirent.h，定义 struct dirent 和 getdents 系统调用。
 */

#ifndef HBOS_SYS_DIRENT_H
#define HBOS_SYS_DIRENT_H

#include "types.h"

#define NAME_MAX 255

/** 目录项类型 */
enum {
    DT_UNKNOWN = 0,
    DT_REG     = 8,   /**< 普通文件 */
    DT_DIR     = 4,   /**< 目录 */
    DT_LNK     = 10,  /**< 符号链接 */
    DT_CHR     = 2,   /**< 字符设备 */
    DT_BLK     = 6,   /**< 块设备 */
};

/** Linux 标准目录项结构 */
struct dirent {
    uint64_t d_ino;             /**< inode 编号 */
    int64_t  d_off;             /**< 目录偏移（通常为目录项序号） */
    uint16_t d_reclen;          /**< 本条记录总长度 */
    uint8_t  d_type;            /**< 目录项类型 */
    char     d_name[NAME_MAX + 1]; /**< 文件名（以 '\0' 结尾） */
};

#ifdef __cplusplus
extern "C" {
#endif

int getdents(unsigned int fd, struct dirent *dirp, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif