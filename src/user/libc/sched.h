/**
 * @file    sched.h
 * @brief   导出 13 个接口（0 类型 / 10 宏 / 3 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    CLONE_VM              常量 / 特性宏
 *         CLONE_FS              常量 / 特性宏
 *         CLONE_FILES           常量 / 特性宏
 *         CLONE_SIGHAND         常量 / 特性宏
 *         CLONE_THREAD          CLONE线程
 *         CLONE_SYSVSEM         常量 / 特性宏
 *         CLONE_SETTLS          常量 / 特性宏
 *         CLONE_PARENT_SETTID   常量 / 特性宏
 *         CLONE_CHILD_CLEARTID  常量 / 特性宏
 *         CLONE_CHILD_SETTID    常量 / 特性宏
 *   函数  sched_yield        （待补）
 *         sched_getaffinity  CPU 亲和位图：每个 bit 一个 CPU
 */
#ifndef HBOS_USER_LIBC_SCHED_H
#define HBOS_USER_LIBC_SCHED_H

#include <stddef.h>

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

int sched_yield(void);

/* CPU 亲和位图：每个 bit 一个 CPU */
int sched_getaffinity(int pid, size_t cpusetsize, unsigned long *mask);
int clone(int (*function)(void *), void *child_stack, int flags,
          void *argument, ...);

#endif
