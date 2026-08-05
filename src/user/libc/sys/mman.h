#ifndef HBOS_USER_LIBC_SYS_MMAN_H
#define HBOS_USER_LIBC_SYS_MMAN_H

#include <stddef.h>
#include <stdint.h>

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED           0x01
#define MAP_PRIVATE          0x02
#define MAP_FIXED            0x10
#define MAP_ANONYMOUS        0x20
#define MAP_ANON             MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE  0x100000
#define MAP_FAILED ((void *)-1)

void *mmap(void *address, size_t length, int protection, int flags,
           int fd, long offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);

#endif
