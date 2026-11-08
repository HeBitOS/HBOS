/**
 * @file    uio.h
 * @brief   导出 3 个接口（1 类型 / 0 宏 / 2 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  iovec       scatter/gather 缓冲区
 *   函数  readv       （待补）
 *         writev      （待补）
 */
#ifndef HBOS_USER_LIBC_SYS_UIO_H
#define HBOS_USER_LIBC_SYS_UIO_H

#include <stddef.h>
#include <unistd.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *vectors, int count);
ssize_t writev(int fd, const struct iovec *vectors, int count);

#endif
