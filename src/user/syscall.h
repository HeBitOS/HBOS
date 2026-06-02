/**
 * @file    syscall.h
 * @brief   HBOS 用户态系统调用封装 — 应用程序接口
 *
 * 提供用户态应用程序调用的系统调用封装函数。
 * 所有系统调用通过 int 0x80 进入内核。
 *
 * 参考 CoolPotOS 和 Linux x86_64 系统调用表，当前实现了 62 个系统调用:
 *   - 文件 I/O: read, write, open, close, lseek, fstat, stat, unlink, isatty
 *   - 进程控制: getpid, getppid, exit, sleep, usleep, fork, execve, waitpid, kill
 *   - 系统信息: uname, gettimeofday
 *   - 文件系统扩展: access, ftruncate, mkdir, rmdir, getcwd, chdir, symlink, chmod, chown, readlink
 *   - 文件描述符: dup, dup2, pipe, fcntl, ioctl
 *   - 用户/组 ID: geteuid, getegid, getuid, getgid, setuid, setgid, getgroups, setgroups, getpgid
 *   - 信号处理: signal, sigaction, sigprocmask, pause
 *   - 内存管理: sbrk, mmap, munmap, mprotect, brk
 *   - 时间操作: nanosleep, clock_gettime, times
 *   - 网络套接字: socket, bind, listen, accept, connect, send, recv
 *   - 系统管理: reboot, sync, mount, umount
 */

#ifndef HBOS_USER_SYSCALL_H
#define HBOS_USER_SYSCALL_H

#include <stddef.h>
#include <stdint.h>
#include "../sys/types.h"
#include "../sys/stat.h"

// ============================================================
// 底层系统调用接口
// ============================================================

/**
 * 6 参数系统调用（int 0x80）
 * @param nr  系统调用号
 * @param a0-a5  参数
 * @return 系统调用返回值（负值表示 -errno）
 */
long hbos_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

/**
 * 3 参数系统调用（简化封装）
 */
long hbos_syscall3(long nr, long a0, long a1, long a2);

// ============================================================
// 文件 I/O
// ============================================================

/** 从文件描述符读取数据 */
ssize_t hbos_read(int fd, void *buf, size_t count);
/** 向文件描述符写入数据 */
ssize_t hbos_write(int fd, const void *buf, size_t count);
/** 打开文件 */
int hbos_open(const char *path, int flags, int mode);
/** 关闭文件描述符 */
int hbos_close(int fd);
/** 设置文件偏移量 */
off_t hbos_lseek(int fd, off_t offset, int whence);
/** 获取文件描述符的状态信息 */
int hbos_fstat(int fd, struct stat *st);
/** 获取指定路径文件的状态信息 */
int hbos_stat(const char *path, struct stat *st);
/** 删除文件 */
int hbos_unlink(const char *path);
/** 检查文件描述符是否为终端 */
int hbos_isatty(int fd);

// ============================================================
// 进程控制
// ============================================================

/** 获取当前进程 ID */
pid_t hbos_getpid(void);
/** 获取父进程 ID */
pid_t hbos_getppid(void);
/** 终止当前进程 */
void hbos_exit(int status);
/** 休眠指定秒数 */
unsigned int hbos_sleep(unsigned int seconds);
/** 休眠指定微秒数 */
int hbos_usleep(useconds_t usec);
/** 创建子进程（当前返回 -ENOSYS） */
pid_t hbos_fork(void);
/** 执行程序（当前返回 -ENOSYS） */
int hbos_execve(const char *path, char *const argv[], char *const envp[]);
/** 等待子进程 */
pid_t hbos_waitpid(pid_t pid, int *status, int options);
/** 向进程发送信号 */
int hbos_kill(pid_t pid, int sig);

// ============================================================
// 系统信息
// ============================================================

/** utsname 结构（与 POSIX 兼容） */
struct hbos_utsname {
    char sysname[65];       /**< 操作系统名称 */
    char nodename[65];      /**< 网络节点名称 */
    char release[65];       /**< 操作系统发行版本 */
    char version[65];       /**< 操作系统版本信息 */
    char machine[65];       /**< 硬件架构标识 */
};

/** 获取系统信息（uname） */
int hbos_uname(struct hbos_utsname *buf);

/** timeval 结构 */
struct hbos_timeval {
    uint64_t tv_sec;        /**< 秒 */
    uint64_t tv_usec;       /**< 微秒 */
};

/** 获取当前时间（gettimeofday） */
int hbos_gettimeofday(struct hbos_timeval *tv, void *tz);

/** timespec 结构 */
struct hbos_timespec {
    uint64_t tv_sec;        /**< 秒 */
    uint64_t tv_nsec;       /**< 纳秒 */
};

// ============================================================
// 文件系统扩展
// ============================================================

/** 检查文件访问权限 */
int hbos_access(const char *path, int mode);
/** 截断文件到指定长度 */
int hbos_ftruncate(int fd, off_t length);
/** 创建目录 */
int hbos_mkdir(const char *path, mode_t mode);
/** 删除目录 */
int hbos_rmdir(const char *path);
/** 获取当前工作目录 */
int hbos_getcwd(char *buf, size_t size);
/** 更改当前工作目录 */
int hbos_chdir(const char *path);
/** 创建符号链接（当前返回 -ENOSYS） */
int hbos_symlink(const char *target, const char *linkpath);
/** 修改文件权限 */
int hbos_chmod(const char *path, mode_t mode);
/** 修改文件所有者 */
int hbos_chown(const char *path, uid_t uid, gid_t gid);
/** 读取符号链接目标 */
ssize_t hbos_readlink(const char *path, char *buf, size_t bufsiz);

// ============================================================
// 文件描述符操作
// ============================================================

/** 复制文件描述符 */
int hbos_dup(int oldfd);
/** 复制文件描述符到指定编号 */
int hbos_dup2(int oldfd, int newfd);
/** 创建管道 */
int hbos_pipe(int pipefd[2]);
/** 文件描述符控制操作 */
int hbos_fcntl(int fd, int cmd, long arg);
/** 设备控制操作 */
int hbos_ioctl(int fd, unsigned long cmd, unsigned long arg);

// ============================================================
// 用户/组 ID
// ============================================================

/** 获取有效用户 ID */
uid_t hbos_geteuid(void);
/** 获取有效组 ID */
gid_t hbos_getegid(void);
/** 获取实际用户 ID */
uid_t hbos_getuid(void);
/** 获取实际组 ID */
gid_t hbos_getgid(void);
/** 设置用户 ID */
int hbos_setuid(uid_t uid);
/** 设置组 ID */
int hbos_setgid(gid_t gid);
/** 获取补充组 ID 列表 */
int hbos_getgroups(int size, gid_t list[]);
/** 设置补充组 ID 列表 */
int hbos_setgroups(int size, const gid_t list[]);
/** 获取进程组 ID */
pid_t hbos_getpgid(pid_t pid);

// ============================================================
// 信号处理
// ============================================================

/** 设置信号处理函数（当前返回 -ENOSYS） */
void (*hbos_signal(int sig, void (*handler)(int)))(int);
/** 检查/更改信号处理（当前返回 -ENOSYS） */
int hbos_sigaction(int sig, const void *act, void *oact);
/** 检查/更改信号掩码 */
int hbos_sigprocmask(int how, const void *set, void *oset);
/** 暂停等待信号 */
int hbos_pause(void);

// ============================================================
// 内存管理
// ============================================================

/** 调整进程数据段边界（brk） */
void *hbos_sbrk(intptr_t increment);
/** 设置程序断点 */
void *hbos_brk(void *addr);
/** 内存映射 */
void *hbos_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
/** 取消内存映射 */
int hbos_munmap(void *addr, size_t len);
/** 设置内存保护 */
int hbos_mprotect(void *addr, size_t len, int prot);

// ============================================================
// 时间操作
// ============================================================

/** 高精度休眠 */
int hbos_nanosleep(const struct hbos_timespec *req, struct hbos_timespec *rem);
/** 获取指定时钟时间 */
int hbos_clock_gettime(int clockid, struct hbos_timespec *tp);
/** 获取进程时间 */
clock_t hbos_times(void *buf);

// ============================================================
// 网络套接字
// ============================================================

/** 创建套接字 */
int hbos_socket(int domain, int type, int protocol);
/** 绑定套接字地址 */
int hbos_bind(int sockfd, const void *addr, size_t addrlen);
/** 监听套接字连接 */
int hbos_listen(int sockfd, int backlog);
/** 接受套接字连接 */
int hbos_accept(int sockfd, void *addr, size_t *addrlen);
/** 发起套接字连接 */
int hbos_connect(int sockfd, const void *addr, size_t addrlen);
/** 发送数据 */
ssize_t hbos_send(int sockfd, const void *buf, size_t len, int flags);
/** 接收数据 */
ssize_t hbos_recv(int sockfd, void *buf, size_t len, int flags);

// ============================================================
// 系统管理
// ============================================================

/** 重启/关机系统 */
int hbos_reboot(int cmd);
/** 同步文件系统缓存 */
void hbos_sync(void);
/** 挂载文件系统（当前返回 -ENOSYS） */
int hbos_mount(const char *src, const char *tgt, const char *fstype, unsigned long flags, const void *data);
/** 卸载文件系统（当前返回 -ENOSYS） */
int hbos_umount(const char *tgt, unsigned long flags);

// ============================================================
// 便捷函数
// ============================================================

/** 向标准输出写入字符串 */
void hbos_puts(const char *s);

#endif /* HBOS_USER_SYSCALL_H */
