#include "sys/stat.h"
#include "syscall.h"
#include "errno.h"

int stat(const char *path, struct stat *buf) {
    long ret = __syscall3(HBOS_SYS_STAT, (long)path, (long)buf, 0);
    return (int)__syscall_errno(ret);
}

int fstat(int fd, struct stat *buf) {
    long ret = __syscall3(HBOS_SYS_FSTAT, fd, (long)buf, 0);
    return (int)__syscall_errno(ret);
}

int mkdir(const char *path, mode_t mode) {
    long ret = __syscall3(HBOS_SYS_MKDIR, (long)path, mode, 0);
    return (int)__syscall_errno(ret);
}

int lstat(const char *path, struct stat *buf) {
    long ret = __syscall3(HBOS_SYS_STAT, (long)path, (long)buf, 1);
    return (int)__syscall_errno(ret);
}
