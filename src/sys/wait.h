/**
 * @file    sys/wait.h
 * @brief   Linux/POSIX 进程等待宏 — HBOS 实现
 *
 * 定义 waitpid() 返回值的解析宏，对标 Linux sys/wait.h。
 * waitpid 的 status 编码格式（Linux 格式）：
 *   - 正常退出: status = (exit_code << 8)
 *   - 信号终止: status = signal_number
 *   - 信号停止: status = (signal_number << 8) | 0x7f
 */

#ifndef HBOS_SYS_WAIT_H
#define HBOS_SYS_WAIT_H

#include "types.h"

/** — waitpid options (对标 Linux) — */

#define WNOHANG     1     /**< 不阻塞，没有子进程时立即返回 0 */
#define WUNTRACED   2     /**< 也报告已停止的子进程 */
#define WCONTINUED  8     /**< 也报告已继续的子进程 */

/** — status 解析宏 (对标 Linux) — */

/** 是否正常退出 */
#define WIFEXITED(status)     (((status) & 0x7f) == 0)
/** 获取正常退出时的退出码 */
#define WEXITSTATUS(status)   (((status) >> 8) & 0xff)
/** 是否被信号终止 */
#define WIFSIGNALED(status)   (((signed char)((status) & 0x7f) > 0) && \
                               (((status) & 0x7f) < 0x7f))
/** 获取终止信号的编号 */
#define WTERMSIG(status)      ((status) & 0x7f)
/** 是否被信号暂停 */
#define WIFSTOPPED(status)    (((status) & 0xff) == 0x7f)
/** 获取暂停信号的编号 */
#define WSTOPSIG(status)      (((status) >> 8) & 0xff)
/** 是否因 SIGCONT 继续 */
#define WIFCONTINUED(status)  ((status) == 0xffff)

/** 构造 waitpid 的 status 值（内核内部使用） */
#define W_EXITCODE(ret, sig)  ((ret) << 8 | (sig))
#define W_STOPCODE(sig)       ((sig) << 8 | 0x7f)

#endif