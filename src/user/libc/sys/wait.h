#ifndef HBOS_USER_LIBC_SYS_WAIT_H
#define HBOS_USER_LIBC_SYS_WAIT_H

#include "../unistd.h"

/* Mirrors src/sys/wait.h (kernel side) — same encoding, since
 * HBOS_SYS_WAITPID (src/syscall.c) fills *status with W_EXITCODE(st, 0)
 * (no signal support, exit code only). */
#define WNOHANG 1

#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))

/* HBOS's HBOS_SYS_WAITPID (src/syscall.c) requires a specific positive pid
 * (returns -ECHILD for pid<=0) — there's no kernel-side "wait for any
 * child" support yet, so plain wait(status) isn't implementable correctly
 * and isn't declared here. Add it if/when the kernel gains that. */
pid_t waitpid(pid_t pid, int *status, int options);

#endif
