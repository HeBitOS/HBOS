/**
 * @file    unistd.h
 * @brief   POSIX 标准头文件 — HBOS 实现
 *
 * 定义了 POSIX 标准 I/O、进程控制、文件系统操作等函数。
 * 参考 CoolPotOS 和 Linux POSIX 兼容层，新增:
 *   - 进程扩展: fork, execve, waitpid, kill
 *   - 用户/组 ID: getuid, getgid, setuid, setgid, getgroups, setgroups, getpgid
 *   - 文件描述符: dup2, pipe, fcntl, ioctl, readlink
 *   - 文件系统: mkdir, rmdir, symlink, chmod, chown
 *   - 内存管理: brk
 *   - 信号: signal, sigaction, sigprocmask, pause
 *   - 时间: nanosleep, clock_gettime, times
 *   - 网络: socket, bind, listen, accept, connect, send, recv
 *   - 系统: reboot, sync, mount, umount
 */

#ifndef HBOS_UNISTD_H
#define HBOS_UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include "sys/types.h"

// ============================================================
// 标准文件描述符
// ============================================================

#define STDIN_FILENO   0   /**< 标准输入 */
#define STDOUT_FILENO  1   /**< 标准输出 */
#define STDERR_FILENO  2   /**< 标准错误 */

// ============================================================
// lseek  whence 参数
// ============================================================

#define SEEK_SET 0   /**< 从文件开头 */
#define SEEK_CUR 1   /**< 从当前位置 */
#define SEEK_END 2   /**< 从文件末尾 */

// ============================================================
// access  mode 参数
// ============================================================

#define F_OK 0   /**< 测试文件是否存在 */
#define R_OK 4   /**< 测试读权限 */
#define W_OK 2   /**< 测试写权限 */
#define X_OK 1   /**< 测试执行权限 */

// ============================================================
// fcntl  cmd 参数
// ============================================================

#define F_DUPFD  0   /**< 复制文件描述符 */
#define F_GETFD  1   /**< 获取文件描述符标志 */
#define F_SETFD  2   /**< 设置文件描述符标志 */
#define F_GETFL  3   /**< 获取文件状态标志 */
#define F_SETFL  4   /**< 设置文件状态标志 */

// ============================================================
// reboot  cmd 参数 (Linux 兼容)
// ============================================================

#define LINUX_REBOOT_CMD_RESTART  0x01234567  /**< 重启 */
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDC /**< 关机 */

// ============================================================
// 函数声明
// ============================================================

// ---- 文件 I/O ----
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int unlink(const char *path);
off_t lseek(int fd, off_t offset, int whence);
int isatty(int fd);

// ---- 文件系统扩展 ----
int access(const char *path, int mode);
int ftruncate(int fd, off_t length);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int mkdir(const char *path, mode_t mode);
int rmdir(const char *path);
int symlink(const char *target, const char *linkpath);
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);

// ---- 文件描述符操作 ----
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);
int fcntl(int fd, int cmd, long arg);
int ioctl(int fd, unsigned long cmd, unsigned long arg);

// ---- 进程控制 ----
pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
int kill(pid_t pid, int sig);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);
void _exit(int status);

// ---- 内存管理 ----
void *_sbrk(intptr_t increment);
void *sbrk(intptr_t increment);
void *brk(void *addr);

// ---- 用户/组 ID ----
uid_t geteuid(void);
gid_t getegid(void);
uid_t getuid(void);
gid_t getgid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int getgroups(int size, gid_t list[]);
int setgroups(int size, const gid_t list[]);
pid_t getpgid(pid_t pid);

// ---- 信号 ----
void (*signal(int sig, void (*handler)(int)))(int);
int sigaction(int sig, const void *act, void *oact);
int sigprocmask(int how, const void *set, void *oset);
int pause(void);

// ---- 时间 ----
int nanosleep(const void *req, void *rem);
int clock_gettime(int clockid, void *tp);
clock_t times(void *buf);

// ---- 网络套接字 ----
int socket(int domain, int type, int protocol);
int bind(int sockfd, const void *addr, size_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, void *addr, size_t *addrlen);
int connect(int sockfd, const void *addr, size_t addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);

// ---- 系统管理 ----
int reboot(int cmd);
void sync(void);
int mount(const char *src, const char *tgt, const char *fstype, unsigned long flags, const void *data);
int umount(const char *tgt, unsigned long flags);

// ---- I/O 多路复用 & 目录遍历 ----
int select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout);
int poll(void *fds, unsigned int nfds, int timeout);

#endif
