/**
 * @file    fd.h
 * @brief   文件描述符表
 *
 * @layer   fs
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  pipe_t      结构体 / 类型别名
 *         fd_entry_t  结构体 / 类型别名
 *         fd_table_t  结构体 / 类型别名
 *         fd_entry_t.origin  fd 来源标签（open/compat/dup/scm/inherit/shm）
 *   宏    POSIX_MAX_FDS  常量 / 特性宏
 *         FD_ORIGIN_*    fd 来源标签取值（随条目复制，不靠全局日志）
 *         FD_FILE        常量 / 特性宏
 *         FD_SOCKET      FD套接字
 *         FD_PIPE        常量 / 特性宏
 *         FD_EVENT       FD事件
 *         FD_EPOLL       常量 / 特性宏
 *         FD_UNIX        常量 / 特性宏
 *         FD_MEMFD       常量 / 特性宏
 *         FD_INOTIFY     常量 / 特性宏
 *         FD_TIMER       常量 / 特性宏
 *         FD_SIGNALFD    常量 / 特性宏
 *         PIPE_BUF_SIZE  PIPE缓冲区
 *         FD_ACC_MASK          访问模式掩码（O_ACCMODE 的数值）
 *         FD_GETFL_MASK        F_GETFL 允许返回的状态位
 *         FD_SETFL_MASK        F_SETFL 允许修改的状态位
 *         FD_O_APPEND/FD_O_NONBLOCK/FD_O_CLOEXEC  Linux ABI 标志数值
 *   函数  fd_accmode_of         取条目 flags 的访问模式
 *         fd_getfl_flags        flags → F_GETFL 返回值
 *         fd_setfl_apply        F_SETFL 只改 APPEND/NONBLOCK，保留访问模式
 *         fd_getfd_flags        flags → F_GETFD 返回值（只有 FD_CLOEXEC 一位）
 *         fd_setfd_apply        F_SETFD 只改 FD_CLOEXEC，其它位保持
 *         fd_accmode_normalize  O_ACCMODE==3 兜底为 O_RDWR
 */
#ifndef HBOS_FD_H
#define HBOS_FD_H

#include <stdbool.h>
#include <stdint.h>

/* ═══ 访问模式记账规则（纯逻辑，宿主单测可直接包含） ═══════════════════
 *
 * Chromium 的 base::File 打开一个 fd 之后会
 *   fcntl(fd, F_GETFL) & O_ACCMODE  ==  它请求的访问模式
 * 不相等就判定"打开失败"并 IMMEDIATE_CRASH（int3;ud2）。
 * 所以条目里的 flags 必须**原样**记住 open 时的访问模式，F_GETFL 只吐
 * 状态位，F_SETFL 不许动访问模式 —— 这三条规则以前在 linux_fcntl 与
 * HBOS_SYS_FCNTL 两处各抄一份，抄错一次就崩一个进程。现在只在这里定义
 * 一次，宿主单测（tests/host/test_fd_accmode.c）用
 * -DHBOS_FD_FLAGS_ONLY 包含本头文件即可验证。
 *
 * 掩码取 Linux ABI 数值（与 src/fcntl.h 一致）：访问模式 0x3、
 * O_APPEND 0x400、O_NONBLOCK 0x800、O_CLOEXEC 0x80000。 */
#define FD_ACC_MASK      0x3
#define FD_O_APPEND      0x400
#define FD_O_NONBLOCK    0x800
#define FD_O_CLOEXEC     0x80000
#define FD_GETFL_MASK    (FD_ACC_MASK | FD_O_APPEND | FD_O_NONBLOCK)
#define FD_SETFL_MASK    (FD_O_APPEND | FD_O_NONBLOCK)

/** 取条目 flags 里的访问模式（0=只读 1=只写 2=读写）。 */
static inline int fd_accmode_of(int flags) {
    return flags & FD_ACC_MASK;
}

/** 把 open 期 flags 折算成 fcntl(F_GETFL) 该返回的状态位。
 *  O_CREAT / O_EXCL / O_TRUNC / O_CLOEXEC 属创建期标志，Linux 不在
 *  F_GETFL 里返回它们。 */
static inline int fd_getfl_flags(int flags) {
    return flags & FD_GETFL_MASK;
}

/** fcntl(F_GETFD)：Linux 只报 FD_CLOEXEC 这**一**位（数值 1）。
 *
 * 为什么单独一个函数：O_CLOEXEC 是"创建期标志"，它既不能出现在
 * F_GETFL 的返回值里（见 fd_getfl_flags），又必须能被 F_SETFD 单独
 * 清掉。HBOS 把两者折进同一个 flags 字段，所以这两个方向都要显式折算，
 * 不能让调用方直接把 flags 透出去。 */
static inline int fd_getfd_flags(int flags) {
    return (flags & FD_O_CLOEXEC) ? 1 : 0;
}

/** fcntl(F_SETFD, arg)：只改 FD_CLOEXEC，其它位一位都不动。
 *
 * 这条是 Chromium 子进程存活的硬依赖：共享内存 fd 用 O_CLOEXEC 创建，
 * 传子进程之前必须 `fcntl(fd, F_SETFD, 0)` 把它清掉；清不掉的话 exec
 * 就会把它们全关掉，而父进程**仍按号码**把这些 fd 写在命令行里
 * （`--metrics-shmem-handle=4,...`），子进程拿到的就是被复用的别的东西
 * ——实测 fd 4 变成了 icudtl.dat、fd 7 变成了 /dev/null。 */
static inline int fd_setfd_apply(int flags, int arg) {
    if (arg & 1) return flags | FD_O_CLOEXEC;
    return flags & ~FD_O_CLOEXEC;
}

/** fcntl(F_SETFL, arg)：只替换 O_APPEND / O_NONBLOCK，访问模式原样保留。 */
static inline int fd_setfl_apply(int flags, int arg) {
    return (flags & ~FD_SETFL_MASK) | (arg & FD_SETFL_MASK);
}

/** O_ACCMODE==3 是非法值，按 O_RDWR 兜底（与 overlay 记账规则一致）。 */
static inline int fd_accmode_normalize(int flags) {
    int mode = fd_accmode_of(flags);
    return mode == FD_ACC_MASK ? 2 : mode;
}

#ifndef HBOS_FD_FLAGS_ONLY
#include "vfs.h"

/* 每个进程的 fd 表容量。
 *
 * 保持 128：fd_entry_t 带 path[256]，单表约 37KB，×64 任务是 2.3MB 静态
 * BSS；提到 512 会变成 9.2MB，对内核不可接受。
 * Chromium 子进程会 F_DUPFD_CLOEXEC(fd, 512) 想要一个高位 fd —— 那个 512
 * 只是"希望不小于"的偏好，不是容量要求。解法是在 fcntl 里回退到任意可用
 * fd（见 src/syscall.c 的 case 1030），而不是把表撑大。 */
#define POSIX_MAX_FDS 128

#define FD_FILE    1
#define FD_SOCKET  2
#define FD_PIPE    3
#define FD_EVENT   4
#define FD_EPOLL   5
#define FD_UNIX    6
#define FD_MEMFD   7
#define FD_INOTIFY 8
#define FD_TIMER   9
#define FD_SIGNALFD 10

#define PIPE_BUF_SIZE 4096

typedef struct {
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int ref_count;
} pipe_t;

/* 这个 fd 条目"最后是怎么来的"。它随条目一起被 dup2/F_DUPFD/fork 复制，
 * 所以**只要 fd 存在就一定有出处** —— 不依赖任何全局环形缓冲（那玩意会被
 * 别的进程的 close 冲掉，实测 cf4/cf5 的链全是 no record）。 */
#define FD_ORIGIN_NONE    0
#define FD_ORIGIN_OPEN    1   /* open(2)/openat/creat（HBOS_SYS_OPEN 出口） */
#define FD_ORIGIN_COMPAT  2   /* eventfd/timerfd/epoll/unix/memfd/signalfd… */
#define FD_ORIGIN_DUP     3   /* dup/dup2/fcntl(F_DUPFD) */
#define FD_ORIGIN_SCM     4   /* SCM_RIGHTS 收到的 */
#define FD_ORIGIN_INHERIT 5   /* fork 之后从父进程继承 */
#define FD_ORIGIN_SHM     6   /* /dev/shm 或 /tmp 覆盖层 open */

typedef struct {
    bool used;
    vfs_node_t *node;
    uint32_t offset;
    uint32_t compat_id;
    int flags;
    int type;
    uint16_t local_port;
    uint8_t origin;      /* FD_ORIGIN_*：条目来源标签 */
    int32_t origin_pid;  /* FD_ORIGIN_INHERIT 时为父进程 pid */
    pipe_t *pipe;
    /* Resolved path this fd was opened with. vfs_node_t itself has no
     * parent/children links (see vfs.h) -- listing a directory's contents
     * only works via the path-string-based fs_readdir()/vfs_readdir_at(),
     * not a per-node op, so a directory fd needs this to answer getdents()
     * (see HBOS_SYS_GETDENTS in syscall.c). */
    char path[256];
} fd_entry_t;

typedef struct {
    fd_entry_t entries[POSIX_MAX_FDS];
    uint32_t refs;
} fd_table_t;

#endif /* HBOS_FD_FLAGS_ONLY */
#endif
