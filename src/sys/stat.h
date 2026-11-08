/**
 * @file    stat.h
 * @brief   导出 28 个接口（1 类型 / 24 宏 / 3 函数）
 *
 * @layer   user
 * @owner   src/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  stat        取路径元数据
 *   宏    S_IFMT      常量 / 特性宏
 *         S_IFREG     常量 / 特性宏
 *         S_IFDIR     常量 / 特性宏
 *         S_IFCHR     常量 / 特性宏
 *         S_IFBLK     常量 / 特性宏
 *         S_IFIFO     常量 / 特性宏
 *         S_IFLNK     常量 / 特性宏
 *         S_IFSOCK    常量 / 特性宏
 *         S_IRUSR     常量 / 特性宏
 *         S_IWUSR     常量 / 特性宏
 *         S_IXUSR     常量 / 特性宏
 *         S_IRGRP     常量 / 特性宏
 *         S_IWGRP     常量 / 特性宏
 *         S_IXGRP     常量 / 特性宏
 *         S_IROTH     常量 / 特性宏
 *         S_IWOTH     常量 / 特性宏
 *         S_IXOTH     常量 / 特性宏
 *         S_ISREG     常量 / 特性宏
 *         S_ISDIR     常量 / 特性宏
 *         S_ISCHR     常量 / 特性宏
 *         S_ISBLK     常量 / 特性宏
 *         S_ISFIFO    常量 / 特性宏
 *         S_ISLNK     常量 / 特性宏
 *         S_ISSOCK    常量 / 特性宏
 *   函数  fstat       按 fd 取元数据
 *         lstat       取元数据（不跟随符号链接）
 */
#ifndef HBOS_SYS_STAT_H
#define HBOS_SYS_STAT_H

#include "types.h"

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRGRP  0040
#define S_IWGRP  0020
#define S_IXGRP  0010
#define S_IROTH  0004
#define S_IWOTH  0002
#define S_IXOTH  0001

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

int fstat(int fd, struct stat *st);
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);

#endif
