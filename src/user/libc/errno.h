#ifndef HBOS_USER_LIBC_ERRNO_H
#define HBOS_USER_LIBC_ERRNO_H

/* User-mode errno — HBOS's .hax apps are single-threaded processes, so a
 * plain global (no TLS / __errno_location() indirection like the kernel's
 * own src/errno.h) is enough. Numeric values mirror src/errno.h so they
 * agree with the negative -errno values HBOS's syscalls return. Not every
 * existing libc wrapper sets this yet (audited: none did, before this file
 * existed) — wire up specific call sites if something is found to actually
 * depend on an accurate value rather than just a compiling <errno.h>. */
extern int errno;

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
#define EWOULDBLOCK     EAGAIN

#endif
