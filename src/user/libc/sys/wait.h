/**
 * @file    wait.h
 * @brief   等待队列：阻塞一个任务直到被唤醒
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    WNOHANG      常量 / 特性宏
 *         WIFEXITED    常量 / 特性宏
 *         WEXITSTATUS  常量 / 特性宏
 *         W_EXITCODE   常量 / 特性宏
 *   函数  waitpid     等待子进程状态变化
 */
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
