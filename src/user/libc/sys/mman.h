/**
 * @file    mman.h
 * @brief   导出 17 个接口（0 类型 / 13 宏 / 4 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    PROT_NONE            常量 / 特性宏
 *         PROT_READ            PROT读
 *         PROT_WRITE           PROT写
 *         PROT_EXEC            常量 / 特性宏
 *         MAP_SHARED           映射SHARED
 *         MAP_PRIVATE          映射PRIVATE
 *         MAP_FIXED            映射FIXED
 *         MAP_ANONYMOUS        映射ANONYMOUS
 *         MAP_ANON             映射ANON
 *         MAP_FIXED_NOREPLACE  映射FIXEDNOREPLACE
 *         MAP_FAILED           映射FAILED
 *         MREMAP_MAYMOVE       常量 / 特性宏
 *         MREMAP_FIXED         常量 / 特性宏
 *   函数  munmap      解除映射
 *         mprotect    改映射权限
 */
#ifndef HBOS_USER_LIBC_SYS_MMAN_H
#define HBOS_USER_LIBC_SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED           0x01
#define MAP_PRIVATE          0x02
#define MAP_FIXED            0x10
#define MAP_ANONYMOUS        0x20
#define MAP_ANON             MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE  0x100000
#define MAP_FAILED ((void *)-1)

#define MREMAP_MAYMOVE 0x1
#define MREMAP_FIXED   0x2

void *mremap(void *old_address, size_t old_size, size_t new_size,
             int flags, ...);

void *mmap(void *address, size_t length, int protection, int flags,
           int fd, long offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);

#endif
