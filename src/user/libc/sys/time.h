/**
 * @file    time.h
 * @brief   导出 2 个接口（1 类型 / 0 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  timeval     秒 + 微秒
 *   函数  gettimeofday  取当前时间
 */
#ifndef HBOS_USER_LIBC_SYS_TIME_H
#define HBOS_USER_LIBC_SYS_TIME_H

struct timeval {
    long tv_sec;
    long tv_usec;
};

/* tcc.c's own getclock_ms() (for -bench) needs this; tz is always ignored
 * (HBOS has no timezone concept), matches struct timeval to HBOS_SYS_GETTOD
 * exactly (see src/user/libc/time.c). */
int gettimeofday(struct timeval *tv, void *tz);

#endif
