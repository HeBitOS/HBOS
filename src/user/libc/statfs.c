#include "statfs.h"
#include "syscall.h"
#include "errno.h"

int statfs(const char *path, struct statfs *buf) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_STATFS, (long)path, (long)buf, 0));
}

int fstatfs(int fd, struct statfs *buf) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_FSTATFS, fd, (long)buf, 0));
}
