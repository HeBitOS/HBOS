#include "errno.h"

int errno;

long __syscall_errno(long ret) {
    if (ret < 0) {
        errno = (int)(-ret);
        return -1;
    }
    return ret;
}
