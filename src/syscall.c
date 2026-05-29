#include <stdint.h>

#include "errno.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "syscall.h"
#include "unistd.h"

static uint64_t finish_syscall(long ret) {
    if (ret < 0 && errno > 0)
        return (uint64_t)(-(int64_t)errno);
    return (uint64_t)ret;
}

uint64_t syscall_dispatch_frame(hbos_syscall_frame_t *f) {
    if (!f) return (uint64_t)(-EFAULT);

    switch (f->nr) {
        case HBOS_SYS_READ:
            return finish_syscall((long)read((int)f->a0, (void *)f->a1, (size_t)f->a2));
        case HBOS_SYS_WRITE:
            return finish_syscall((long)write((int)f->a0, (const void *)f->a1, (size_t)f->a2));
        case HBOS_SYS_OPEN:
            return finish_syscall((long)open((const char *)f->a0, (int)f->a1, (int)f->a2));
        case HBOS_SYS_CLOSE:
            return finish_syscall((long)close((int)f->a0));
        case HBOS_SYS_LSEEK:
            return finish_syscall((long)lseek((int)f->a0, (off_t)f->a1, (int)f->a2));
        case HBOS_SYS_FSTAT:
            return finish_syscall((long)fstat((int)f->a0, (struct stat *)f->a1));
        case HBOS_SYS_STAT:
            return finish_syscall((long)stat((const char *)f->a0, (struct stat *)f->a1));
        case HBOS_SYS_UNLINK:
            return finish_syscall((long)unlink((const char *)f->a0));
        case HBOS_SYS_ISATTY:
            return finish_syscall((long)isatty((int)f->a0));
        case HBOS_SYS_GETPID:
            return (uint64_t)getpid();
        case HBOS_SYS_SBRK:
            return finish_syscall((long)(intptr_t)sbrk((intptr_t)f->a0));
        case HBOS_SYS_EXIT:
            return f->a0;
        default:
            return (uint64_t)(-ENOSYS);
    }
}
