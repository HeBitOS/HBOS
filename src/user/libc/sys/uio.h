#ifndef HBOS_USER_LIBC_SYS_UIO_H
#define HBOS_USER_LIBC_SYS_UIO_H

#include <stddef.h>
#include <unistd.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *vectors, int count);
ssize_t writev(int fd, const struct iovec *vectors, int count);

#endif
