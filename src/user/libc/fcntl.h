#ifndef HBOS_USER_LIBC_FCNTL_H
#define HBOS_USER_LIBC_FCNTL_H

/* O_* flag values already live in syscall.h (shared with the raw __syscallN
 * wrappers) — reuse them instead of redefining, so there's one source of
 * truth for what the kernel's open() syscall handler actually expects. */
#include "syscall.h"

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x0100
#define AT_EMPTY_PATH       0x1000

int open(const char *path, int flags, ...);

#endif
