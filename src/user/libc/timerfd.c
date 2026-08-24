#include "timerfd.h"
#include "syscall.h"
#include "errno.h"

int timerfd_create(int clockid, int flags) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_TIMERFD_CREATE, clockid, flags, 0));
}

int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value) {
    return (int)__syscall_errno(__syscall6(
        HBOS_SYS_TIMERFD_SETTIME, fd, flags,
        (long)new_value, (long)old_value, 0, 0));
}

int timerfd_gettime(int fd, struct itimerspec *curr_value) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_TIMERFD_GETTIME, fd, (long)curr_value, 0));
}
