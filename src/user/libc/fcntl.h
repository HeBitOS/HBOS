/**
 * @file    fcntl.h
 * @brief   导出 4 个接口（0 类型 / 3 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    AT_FDCWD             以 cwd 为基准解析相对路径
 *         AT_SYMLINK_NOFOLLOW  不跟随末端符号链接
 *         AT_EMPTY_PATH        允许空路径操作 fd 自身
 *   函数  open        打开路径，返回 fd
 */
#ifndef HBOS_USER_LIBC_FCNTL_H
#define HBOS_USER_LIBC_FCNTL_H

/* O_* flag values already live in syscall.h (shared with the raw __syscallN
 * wrappers) — reuse them instead of redefining, so there's one source of
 * truth for what the kernel's open() syscall handler actually expects. */
#include "syscall.h"

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x0100
#define AT_EMPTY_PATH       0x1000

int open(const char *path, int flags, ...);

#endif
