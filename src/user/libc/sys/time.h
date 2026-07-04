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
