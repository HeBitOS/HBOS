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
