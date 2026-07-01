/**
 * @file    dirent.c
 * @brief   目录遍历用户态实现，基于 getdents / open / close 系统调用
 */
#include "dirent.h"
#include "syscall.h"
#include "stdlib.h"

/* 一次性目录读取缓冲区大小：内核 getdents 不维护游标，单次调用即返回全部
 * 目录项，故缓冲区需足够容纳整个目录。每条 sizeof(struct dirent)≈280 字节，
 * 16 KiB 约可容纳 58 个条目。 */
#define DIR_BUF_BYTES 16384

struct DIR {
    int  fd;                 /**< 底层目录文件描述符 */
    int  used;               /**< buf 中有效字节数 */
    int  pos;                /**< 当前读取偏移 */
    char buf[DIR_BUF_BYTES]; /**< getdents 一次性读入的目录项 */
};

int getdents(int fd, struct dirent *dirp, unsigned int count) {
    return (int)__syscall3(HBOS_SYS_GETDENTS, (long)fd, (long)dirp, (long)count);
}

DIR *opendir(const char *path) {
    if (!path) return 0;
    int fd = (int)__syscall3(HBOS_SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return 0;

    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) {
        __syscall1(HBOS_SYS_CLOSE, (long)fd);
        return 0;
    }
    d->fd  = fd;
    d->pos = 0;

    long n = __syscall3(HBOS_SYS_GETDENTS, (long)fd, (long)d->buf,
                        (long)sizeof(d->buf));
    d->used = (n < 0) ? 0 : (int)n;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) return 0;
    /* 内核按固定 sizeof(struct dirent) 步长打包记录，故此处同样固定步进 */
    if (d->pos + (int)sizeof(struct dirent) > d->used) return 0;
    struct dirent *e = (struct dirent *)(d->buf + d->pos);
    d->pos += (int)sizeof(struct dirent);
    return e;
}

int closedir(DIR *d) {
    if (!d) return -1;
    __syscall1(HBOS_SYS_CLOSE, (long)d->fd);
    free(d);
    return 0;
}
