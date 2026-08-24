#include "signalfd.h"
#include "syscall.h"
#include "errno.h"

int signalfd(int fd, const void *sigmask, size_t sizemask) {
    return (int)__syscall_errno(__syscall6(
        HBOS_SYS_SIGNALFD, fd, (long)sigmask, (long)sizemask,
        SFD_NONBLOCK | SFD_CLOEXEC, 0, 0));
}
