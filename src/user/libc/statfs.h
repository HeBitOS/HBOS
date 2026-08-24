#ifndef HBOS_USER_LIBC_STATFS_H
#define HBOS_USER_LIBC_STATFS_H

#include <stdint.h>

/* Linux x86-64 struct statfs（glibc 布局，128 字节） */
struct statfs {
    long f_type;
    long f_bsize;
    long f_blocks;
    long f_bfree;
    long f_bavail;
    long f_files;
    long f_ffree;
    long f_fsid[2];
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

#define RAMFS_MAGIC 0x01021994L

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
