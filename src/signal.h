/**
 * @file    signal.h
 * @brief   POSIX 信号处理头文件 — HBOS 实现
 *
 * 定义标准信号编号、信号处理函数原型等。
 * 参考 Linux signal.h 实现，部分高级信号功能为占位。
 */
#ifndef HBOS_SIGNAL_H
#define HBOS_SIGNAL_H

#include "sys/types.h"

#define SIGHUP     1   /**< 终端挂起 */
#define SIGINT     2   /**< 中断 (Ctrl+C) */
#define SIGQUIT    3   /**< 退出 (Ctrl+\) */
#define SIGILL     4   /**< 非法指令 */
#define SIGTRAP    5   /**< 断点陷阱 */
#define SIGABRT    6   /**< 异常终止 */
#define SIGBUS     7   /**< 总线错误 */
#define SIGFPE     8   /**< 浮点异常 */
#define SIGKILL    9   /**< 强制终止（不可捕获） */
#define SIGUSR1   10   /**< 用户定义信号 1 */
#define SIGSEGV   11   /**< 段错误 */
#define SIGUSR2   12   /**< 用户定义信号 2 */
#define SIGPIPE   13   /**< 管道破裂 */
#define SIGALRM   14   /**< 定时器超时 */
#define SIGTERM   15   /**< 终止 */
#define SIGSTKFLT 16   /**< 协处理器栈错误 */
#define SIGCHLD   17   /**< 子进程状态改变 */
#define SIGCONT   18   /**< 继续执行（不可捕获） */
#define SIGSTOP   19   /**< 停止（不可捕获） */
#define SIGTSTP   20   /**< 终端停止 (Ctrl+Z) */
#define SIGTTIN   21   /**< 后台读终端 */
#define SIGTTOU   22   /**< 后台写终端 */
#define SIGURG    23   /**< 紧急套接字条件 */
#define SIGXCPU   24   /**< CPU 时间超限 */
#define SIGXFSZ   25   /**< 文件大小超限 */
#define SIGVTALRM 26   /**< 虚拟定时器超时 */
#define SIGPROF   27   /**< 性能分析定时器超时 */
#define SIGWINCH  28   /**< 窗口大小改变 */
#define SIGIO     29   /**< I/O 可操作 */
#define SIGPWR    30   /**< 电源故障 */
#define SIGSYS    31   /**< 错误系统调用 */

#define SIG_DFL ((void (*)(int))0)       /**< 默认信号处理 */
#define SIG_IGN ((void (*)(int))1)       /**< 忽略信号 */
#define SIG_ERR ((void (*)(int))-1)      /**< 信号处理错误返回 */

#define SIG_BLOCK   0   /**< 阻塞信号集 */
#define SIG_UNBLOCK 1   /**< 解除阻塞信号集 */
#define SIG_SETMASK 2   /**< 设置信号掩码 */

#define _NSIG       64   /**< 信号总数 */
#define _NSIG_BPW   64
#define _NSIG_WORDS (_NSIG / _NSIG_BPW)

typedef struct {
    uint64_t sig[_NSIG_WORDS];
} sigset_t;

void (*signal(int sig, void (*handler)(int)))(int);
int raise(int sig);
int kill(pid_t pid, int sig);

#endif
