#ifndef HBOS_USER_LIBC_SYS_STAT_H
#define HBOS_USER_LIBC_SYS_STAT_H

/* struct stat itself already lives in syscall.h (shared with the raw
 * __syscallN wrappers) — reuse it instead of redefining, so there's one
 * source of truth matching what the kernel's stat/fstat syscalls fill in. */
#include "../syscall.h"

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_IRUSR  0400
#define S_IWUSR  0200
#define S_IXUSR  0100
#define S_IRGRP  0040
#define S_IWGRP  0020
#define S_IXGRP  0010
#define S_IROTH  0004
#define S_IWOTH  0002
#define S_IXOTH  0001

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, mode_t mode);
/* HBOS has no symlinks anywhere (ramfs/ext2/fat32 backends never set
 * S_IFLNK) -- lstat() is exactly stat() here, same as it would be on any
 * real system for a path that's never actually a symlink. */
int lstat(const char *path, struct stat *buf);

#endif
