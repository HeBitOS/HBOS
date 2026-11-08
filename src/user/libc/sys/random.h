/**
 * @file    random.h
 * @brief   导出 3 个接口（0 类型 / 2 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    GRND_NONBLOCK  常量 / 特性宏
 *         GRND_RANDOM    常量 / 特性宏
 *   函数  getrandom   （待补）
 */
#ifndef HBOS_USER_LIBC_SYS_RANDOM_H
#define HBOS_USER_LIBC_SYS_RANDOM_H

#include <stddef.h>

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

long getrandom(void *buffer, size_t length, unsigned int flags);

#endif
