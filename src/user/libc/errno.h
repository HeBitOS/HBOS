#ifndef HBOS_USER_LIBC_ERRNO_H
#define HBOS_USER_LIBC_ERRNO_H

/*
 * errno is thread-local at the API boundary.  HBOS does not yet load ELF
 * PT_TLS images, so __errno_location() uses a compact TID-keyed table until
 * the dynamic loader can provide compiler TLS.  The lookup only occurs on
 * an error path or when an application explicitly reads errno.
 */
int *__errno_location(void);
#define errno (*__errno_location())

/* Converts a syscall's raw return value (negative -errno on failure, per
 * src/syscall.c's finish_syscall() convention) into a POSIX-shaped result:
 * sets errno and returns -1 on failure, passes the value through on
 * success. Every libc wrapper around a raw __syscallN() call should route
 * its result through this instead of just `ret < 0 ? -1 : ret`, which
 * silently drops the error code. */
long __syscall_errno(long ret);

#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EPIPE           32
#define ERANGE          34
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP      95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ECONNRESET      104
#define EISCONN         106
#define ENOTCONN        107
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EALREADY        114
#define EINPROGRESS     115
#define EWOULDBLOCK     EAGAIN

#endif
