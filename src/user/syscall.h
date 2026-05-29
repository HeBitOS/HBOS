#ifndef HBOS_USER_SYSCALL_H
#define HBOS_USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>
#include "../sys/types.h"
#include "../sys/stat.h"

long hbos_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
long hbos_syscall3(long nr, long a0, long a1, long a2);

ssize_t hbos_read(int fd, void *buf, size_t count);
ssize_t hbos_write(int fd, const void *buf, size_t count);
int hbos_open(const char *path, int flags, int mode);
int hbos_close(int fd);
off_t hbos_lseek(int fd, off_t offset, int whence);
int hbos_fstat(int fd, struct stat *st);
int hbos_stat(const char *path, struct stat *st);
int hbos_unlink(const char *path);
int hbos_isatty(int fd);
pid_t hbos_getpid(void);
void *hbos_sbrk(intptr_t increment);
void hbos_puts(const char *s);

#endif
