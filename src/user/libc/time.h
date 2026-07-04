#ifndef HBOS_USER_LIBC_TIME_H
#define HBOS_USER_LIBC_TIME_H

typedef long time_t;

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
};

/* Treats *timer as if it were UTC (HBOS has no timezone concept) —
 * TinyCC's only use of this is cosmetic __DATE__/__TIME__ macro text. */
struct tm *localtime(const time_t *timer);

#endif
