#ifndef HBOS_SYSCALL_H
#define HBOS_SYSCALL_H

#include <stdint.h>

#define HBOS_SYSCALL_VECTOR 0x80

typedef enum {
    HBOS_SYS_READ = 0,
    HBOS_SYS_WRITE,
    HBOS_SYS_OPEN,
    HBOS_SYS_CLOSE,
    HBOS_SYS_LSEEK,
    HBOS_SYS_FSTAT,
    HBOS_SYS_STAT,
    HBOS_SYS_UNLINK,
    HBOS_SYS_ISATTY,
    HBOS_SYS_GETPID,
    HBOS_SYS_SBRK,
    HBOS_SYS_EXIT,
    HBOS_SYS_MAX
} hbos_syscall_no_t;

typedef struct {
    uint64_t nr;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
} hbos_syscall_frame_t;

uint64_t syscall_dispatch_frame(hbos_syscall_frame_t *frame);

#endif
