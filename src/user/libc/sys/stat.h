/**
 * @file    stat.h
 * @brief   导出 28 个接口（0 类型 / 24 宏 / 4 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
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
 *   函数  stat        取路径元数据
 *         fstat       按 fd 取元数据
 *         mkdir       建目录
 *         lstat       取元数据（不跟随符号链接）
 */
#ifndef HBOS_USER_LIBC_SYS_STAT_H
#define HBOS_USER_LIBC_SYS_STAT_H

/* struct stat itself already lives in syscall.h (shared with the raw
 * __syscallN wrappers) — reuse it instead of redefining, so there's one
 * source of truth matching what the kernel's stat/fstat syscalls fill in. */
#include "../syscall.h"

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

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, mode_t mode);
/* lstat() does not follow the final ramfs/HBFS symbolic-link component. */
int lstat(const char *path, struct stat *buf);

#endif
