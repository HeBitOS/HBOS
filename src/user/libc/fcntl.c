#include "fcntl.h"
#include "errno.h"
#include <stdarg.h>

int open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    long ret = __syscall3(HBOS_SYS_OPEN, (long)path, flags, mode);
    return (int)__syscall_errno(ret);
}
