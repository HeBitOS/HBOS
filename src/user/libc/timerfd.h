/**
 * @file    timerfd.h
 * @brief   导出 7 个接口（1 类型 / 3 宏 / 3 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  itimerspec  （待补）
 *   宏    TFD_TIMER_ABSTIME  常量 / 特性宏
 *         TFD_NONBLOCK       常量 / 特性宏
 *         TFD_CLOEXEC        常量 / 特性宏
 *   函数  timerfd_create   （待补）
 *         timerfd_gettime  （待补）
 */
#ifndef HBOS_USER_LIBC_TIMERFD_H
#define HBOS_USER_LIBC_TIMERFD_H

#include "time.h"

#define TFD_TIMER_ABSTIME 1
#define TFD_NONBLOCK     0x800
#define TFD_CLOEXEC      0x80000

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#endif
