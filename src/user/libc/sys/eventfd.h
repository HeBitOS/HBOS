/**
 * @file    eventfd.h
 * @brief   导出 7 个接口（1 类型 / 3 宏 / 3 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  eventfd_t   结构体 / 类型别名
 *   宏    EFD_SEMAPHORE  常量 / 特性宏
 *         EFD_NONBLOCK   常量 / 特性宏
 *         EFD_CLOEXEC    常量 / 特性宏
 *   函数  eventfd        （待补）
 *         eventfd_read   （待补）
 *         eventfd_write  （待补）
 */
#ifndef HBOS_USER_LIBC_SYS_EVENTFD_H
#define HBOS_USER_LIBC_SYS_EVENTFD_H

#include <stdint.h>

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 1
#define EFD_NONBLOCK  04000
#define EFD_CLOEXEC   02000000

int eventfd(unsigned int initial_value, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#endif
