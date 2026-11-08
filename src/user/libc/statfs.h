/**
 * @file    statfs.h
 * @brief   导出 4 个接口（1 类型 / 1 宏 / 2 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  statfs      （待补）
 *   宏    RAMFS_MAGIC  常量 / 特性宏
 *   函数  fstatfs     （待补）
 */
#ifndef HBOS_USER_LIBC_STATFS_H
#define HBOS_USER_LIBC_STATFS_H

#include <stdint.h>

/* Linux x86-64 struct statfs（glibc 布局，128 字节） */
struct statfs {
    long f_type;
    long f_bsize;
    long f_blocks;
    long f_bfree;
    long f_bavail;
    long f_files;
    long f_ffree;
    long f_fsid[2];
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

#define RAMFS_MAGIC 0x01021994L

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
