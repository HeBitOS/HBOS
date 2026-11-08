/**
 * @file    time.h
 * @brief   导出 10 个接口（4 类型 / 2 宏 / 4 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  timespec    秒 + 纳秒
 *         tm          分解时间（年月日时分秒）
 *         time_t      秒级时间戳
 *         clockid_t   结构体 / 类型别名
 *   宏    CLOCK_REALTIME   时钟REALTIME
 *         CLOCK_MONOTONIC  时钟MONOTONIC
 *   函数  time           取秒级时间
 *         localtime      时间戳转本地分解时间
 *         localtime_r    （待补）
 *         clock_gettime  读某个时钟
 */
#ifndef HBOS_USER_LIBC_TIME_H
#define HBOS_USER_LIBC_TIME_H

typedef long time_t;
typedef int clockid_t;

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    long tv_sec;
    long tv_nsec;
};


/* Seconds since boot (approximate, RDTSC-based — see HBOS_SYS_GETTOD in
 * src/syscall.c). Not wall-clock time; good enough for callers that just
 * want a coarse timestamp (e.g. TinyCC's __TIMESTAMP__/__DATE__ support). */
time_t time(time_t *tloc);

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;   /* 0-11 */
    int tm_year;  /* years since 1900 */
    int tm_wday;  /* 0-6, Sunday = 0 */
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;   /* seconds east of UTC (HBOS: always 0) */
};

/* Treats *timer as if it were UTC (HBOS has no timezone concept) —
 * TinyCC's only use of this is cosmetic __DATE__/__TIME__ macro text. */
struct tm *localtime(const time_t *timer);
struct tm *localtime_r(const time_t *timer, struct tm *result);
int clock_gettime(clockid_t clockid, struct timespec *tp);

#endif
