#ifndef HBOS_USER_LIBC_SCHED_H
#define HBOS_USER_LIBC_SCHED_H

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

int sched_yield(void);
int clone(int (*function)(void *), void *child_stack, int flags,
          void *argument, ...);

#endif
