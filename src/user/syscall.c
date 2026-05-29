#include "../errno.h"
#include "../string.h"
#include "../syscall.h"
#include "../unistd.h"
#include "syscall.h"

long hbos_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;

    __asm__ volatile(
        "int $0x80"
        : "+a"(nr)
        : "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");

    if (nr < 0) {
        errno = (int)-nr;
        return -1;
    }
    return nr;
}

long hbos_syscall3(long nr, long a0, long a1, long a2) {
    return hbos_syscall6(nr, a0, a1, a2, 0, 0, 0);
}

ssize_t hbos_read(int fd, void *buf, size_t count) {
    return (ssize_t)hbos_syscall3(HBOS_SYS_READ, fd, (long)buf, (long)count);
}

ssize_t hbos_write(int fd, const void *buf, size_t count) {
    return (ssize_t)hbos_syscall3(HBOS_SYS_WRITE, fd, (long)buf, (long)count);
}

int hbos_open(const char *path, int flags, int mode) {
    return (int)hbos_syscall3(HBOS_SYS_OPEN, (long)path, flags, mode);
}

int hbos_close(int fd) {
    return (int)hbos_syscall3(HBOS_SYS_CLOSE, fd, 0, 0);
}

off_t hbos_lseek(int fd, off_t offset, int whence) {
    return (off_t)hbos_syscall3(HBOS_SYS_LSEEK, fd, (long)offset, whence);
}

int hbos_fstat(int fd, struct stat *st) {
    return (int)hbos_syscall3(HBOS_SYS_FSTAT, fd, (long)st, 0);
}

int hbos_stat(const char *path, struct stat *st) {
    return (int)hbos_syscall3(HBOS_SYS_STAT, (long)path, (long)st, 0);
}

int hbos_unlink(const char *path) {
    return (int)hbos_syscall3(HBOS_SYS_UNLINK, (long)path, 0, 0);
}

int hbos_isatty(int fd) {
    return (int)hbos_syscall3(HBOS_SYS_ISATTY, fd, 0, 0);
}

pid_t hbos_getpid(void) {
    return (pid_t)hbos_syscall3(HBOS_SYS_GETPID, 0, 0, 0);
}

void *hbos_sbrk(intptr_t increment) {
    return (void *)hbos_syscall3(HBOS_SYS_SBRK, (long)increment, 0, 0);
}

void hbos_puts(const char *s) {
    if (!s) return;
    hbos_write(STDOUT_FILENO, s, strlen(s));
}
