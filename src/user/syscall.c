/**
 * @file    syscall.c
 * @brief   HBOS 用户态系统调用封装实现
 *
 * 提供所有系统调用的用户态封装函数。
 * 底层通过 int 0x80 进入内核。
 */

#include "../errno.h"
#include "../string.h"
#include "../syscall.h"
#include "../unistd.h"
#include "syscall.h"

// ============================================================
// 底层系统调用
// ============================================================

/**
 * 6 参数系统调用（int 0x80）
 * @param nr  系统调用号
 * @param a0-a5  参数
 * @return 系统调用返回值（负值表示 -errno）
 */
long hbos_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;

    __asm__ volatile(
        "int $0x80"
        : "+a"(nr)
        : "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");

    if (nr < 0) {
        errno = (int)-nr;
        return -1;
    }
    return nr;
}

/**
 * 3 参数系统调用（简化封装）
 * @param nr  系统调用号
 * @param a0  参数 0
 * @param a1  参数 1
 * @param a2  参数 2
 * @return 系统调用返回值
 */
long hbos_syscall3(long nr, long a0, long a1, long a2) {
    return hbos_syscall6(nr, a0, a1, a2, 0, 0, 0);
}

// ============================================================
// 文件 I/O
// ============================================================

/** 从文件描述符读取数据 */
ssize_t hbos_read(int fd, void *buf, size_t count) {
    return (ssize_t)hbos_syscall3(HBOS_SYS_READ, fd, (long)buf, (long)count);
}

/** 向文件描述符写入数据 */
ssize_t hbos_write(int fd, const void *buf, size_t count) {
    return (ssize_t)hbos_syscall3(HBOS_SYS_WRITE, fd, (long)buf, (long)count);
}

/** 打开文件 */
int hbos_open(const char *path, int flags, int mode) {
    return (int)hbos_syscall3(HBOS_SYS_OPEN, (long)path, flags, mode);
}

/** 关闭文件描述符 */
int hbos_close(int fd) {
    return (int)hbos_syscall3(HBOS_SYS_CLOSE, fd, 0, 0);
}

/** 设置文件偏移量 */
off_t hbos_lseek(int fd, off_t offset, int whence) {
    return (off_t)hbos_syscall3(HBOS_SYS_LSEEK, fd, (long)offset, whence);
}

/** 获取文件描述符的状态信息 */
int hbos_fstat(int fd, struct stat *st) {
    return (int)hbos_syscall3(HBOS_SYS_FSTAT, fd, (long)st, 0);
}

/** 获取指定路径文件的状态信息 */
int hbos_stat(const char *path, struct stat *st) {
    return (int)hbos_syscall3(HBOS_SYS_STAT, (long)path, (long)st, 0);
}

/** 删除文件 */
int hbos_unlink(const char *path) {
    return (int)hbos_syscall3(HBOS_SYS_UNLINK, (long)path, 0, 0);
}

/** 检查文件描述符是否为终端 */
int hbos_isatty(int fd) {
    return (int)hbos_syscall3(HBOS_SYS_ISATTY, fd, 0, 0);
}

// ============================================================
// 进程控制
// ============================================================

/** 获取当前进程 ID */
pid_t hbos_getpid(void) {
    return (pid_t)hbos_syscall3(HBOS_SYS_GETPID, 0, 0, 0);
}

/** 获取父进程 ID */
pid_t hbos_getppid(void) {
    return (pid_t)hbos_syscall3(HBOS_SYS_GETPPID, 0, 0, 0);
}

/** 终止当前进程 */
void hbos_exit(int status) {
    hbos_syscall3(HBOS_SYS_EXIT, status, 0, 0);
    while (1) __asm__ volatile("hlt");
}

/** 休眠指定秒数 */
unsigned int hbos_sleep(unsigned int seconds) {
    return (unsigned int)hbos_syscall3(HBOS_SYS_SLEEP, seconds, 0, 0);
}

/** 休眠指定微秒数 */
int hbos_usleep(useconds_t usec) {
    return (int)hbos_syscall3(HBOS_SYS_USLEEP, usec, 0, 0);
}

// ============================================================
// 系统信息
// ============================================================

/** 获取系统信息（uname） */
int hbos_uname(struct hbos_utsname *buf) {
    return (int)hbos_syscall3(HBOS_SYS_UNAME, (long)buf, 0, 0);
}

/** 获取当前时间（gettimeofday） */
int hbos_gettimeofday(struct hbos_timeval *tv, void *tz) {
    (void)tz;
    return (int)hbos_syscall3(HBOS_SYS_GETTOD, (long)tv, 0, 0);
}

// ============================================================
// 文件系统扩展
// ============================================================

/** 检查文件访问权限 */
int hbos_access(const char *path, int mode) {
    (void)mode;
    return (int)hbos_syscall3(HBOS_SYS_ACCESS, (long)path, 0, 0);
}

/** 截断文件到指定长度 */
int hbos_ftruncate(int fd, off_t length) {
    return (int)hbos_syscall3(HBOS_SYS_FTRUNCATE, fd, (long)length, 0);
}

/** 获取当前工作目录 */
int hbos_getcwd(char *buf, size_t size) {
    return (int)hbos_syscall3(HBOS_SYS_GETCWD, (long)buf, (long)size, 0);
}

/** 更改当前工作目录 */
int hbos_chdir(const char *path) {
    return (int)hbos_syscall3(HBOS_SYS_CHDIR, (long)path, 0, 0);
}

// ============================================================
// 文件描述符操作
// ============================================================

/** 复制文件描述符 */
int hbos_dup(int oldfd) {
    return (int)hbos_syscall3(HBOS_SYS_DUP, oldfd, 0, 0);
}

/** 获取有效用户 ID */
uid_t hbos_geteuid(void) {
    return (uid_t)hbos_syscall3(HBOS_SYS_GETEUID, 0, 0, 0);
}

/** 获取有效组 ID */
gid_t hbos_getegid(void) {
    return (gid_t)hbos_syscall3(HBOS_SYS_GETEGID, 0, 0, 0);
}

/** 获取线程 ID */
pid_t hbos_gettid(void) {
    return (pid_t)hbos_syscall3(HBOS_SYS_GETTID, 0, 0, 0);
}

// ============================================================
// 文件描述符操作扩展
// ============================================================

/** 复制文件描述符到指定编号 */
int hbos_dup2(int oldfd, int newfd) {
    return (int)hbos_syscall3(HBOS_SYS_DUP2, oldfd, newfd, 0);
}

/** 创建管道 */
int hbos_pipe(int pipefd[2]) {
    return (int)hbos_syscall3(HBOS_SYS_PIPE, (long)pipefd, 0, 0);
}

/** 文件描述符控制操作 */
int hbos_fcntl(int fd, int cmd, long arg) {
    return (int)hbos_syscall3(HBOS_SYS_FCNTL, fd, cmd, arg);
}

/** 设备控制操作 */
int hbos_ioctl(int fd, unsigned long cmd, unsigned long arg) {
    return (int)hbos_syscall6(HBOS_SYS_IOCTL, fd, (long)cmd, (long)arg, 0, 0, 0);
}

/** 读取符号链接目标 */
ssize_t hbos_readlink(const char *path, char *buf, size_t bufsiz) {
    return (ssize_t)hbos_syscall3(HBOS_SYS_READLINK, (long)path, (long)buf, (long)bufsiz);
}

// ============================================================
// 进程控制扩展
// ============================================================

/** 创建子进程（当前返回 -ENOSYS） */
pid_t hbos_fork(void) {
    return (pid_t)hbos_syscall3(HBOS_SYS_FORK, 0, 0, 0);
}

/** 执行程序（当前返回 -ENOSYS） */
int hbos_execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)hbos_syscall6(HBOS_SYS_EXECVE, (long)path, (long)argv, (long)envp, 0, 0, 0);
}

/** 等待子进程 */
pid_t hbos_waitpid(pid_t pid, int *status, int options) {
    return (pid_t)hbos_syscall3(HBOS_SYS_WAITPID, pid, (long)status, options);
}

/** 向进程发送信号 */
int hbos_kill(pid_t pid, int sig) {
    return (int)hbos_syscall3(HBOS_SYS_KILL, pid, sig, 0);
}

// ============================================================
// 用户/组 ID 扩展
// ============================================================

/** 获取实际用户 ID */
uid_t hbos_getuid(void) {
    return (uid_t)hbos_syscall3(HBOS_SYS_GETUID, 0, 0, 0);
}

/** 获取实际组 ID */
gid_t hbos_getgid(void) {
    return (gid_t)hbos_syscall3(HBOS_SYS_GETGID, 0, 0, 0);
}

/** 设置用户 ID */
int hbos_setuid(uid_t uid) {
    return (int)hbos_syscall3(HBOS_SYS_SETUID, uid, 0, 0);
}

/** 设置组 ID */
int hbos_setgid(gid_t gid) {
    return (int)hbos_syscall3(HBOS_SYS_SETGID, gid, 0, 0);
}

/** 获取补充组 ID 列表 */
int hbos_getgroups(int size, gid_t list[]) {
    return (int)hbos_syscall3(HBOS_SYS_GETGROUPS, size, (long)list, 0);
}

/** 设置补充组 ID 列表 */
int hbos_setgroups(int size, const gid_t list[]) {
    return (int)hbos_syscall3(HBOS_SYS_SETGROUPS, size, (long)list, 0);
}

/** 获取进程组 ID */
pid_t hbos_getpgid(pid_t pid) {
    return (pid_t)hbos_syscall3(HBOS_SYS_GETPGID, pid, 0, 0);
}

// ============================================================
// 信号处理
// ============================================================

/** 设置信号处理函数 */
void (*hbos_signal(int sig, void (*handler)(int)))(int) {
    long ret = hbos_syscall3(HBOS_SYS_SIGNAL, sig, (long)(uintptr_t)handler, 0);
    if (ret < 0) {
        errno = (int)(-ret);
        return (void (*)(int))-1;
    }
    return (void (*)(int))(uintptr_t)ret;
}

/** 检查/更改信号处理 */
int hbos_sigaction(int sig, const void *act, void *oact) {
    return (int)hbos_syscall3(HBOS_SYS_SIGACTION, sig, (long)act, (long)oact);
}

/** 检查/更改信号掩码 */
int hbos_sigprocmask(int how, const void *set, void *oset) {
    return (int)hbos_syscall3(HBOS_SYS_SIGPROCMASK, how, (long)set, (long)oset);
}

/** 暂停等待信号 */
int hbos_pause(void) {
    return (int)hbos_syscall3(HBOS_SYS_PAUSE, 0, 0, 0);
}

// ============================================================
// 内存管理扩展
// ============================================================

/** 设置程序断点 */
void *hbos_brk(void *addr) {
    return (void *)hbos_syscall3(HBOS_SYS_BRK, (long)addr, 0, 0);
}

/** 内存映射 */
void *hbos_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    return (void *)hbos_syscall6(HBOS_SYS_MMAP, (long)addr, (long)len, prot, flags, fd, (long)off);
}

/** 取消内存映射 */
int hbos_munmap(void *addr, size_t len) {
    return (int)hbos_syscall3(HBOS_SYS_MUNMAP, (long)addr, (long)len, 0);
}

/** 设置内存保护 */
int hbos_mprotect(void *addr, size_t len, int prot) {
    return (int)hbos_syscall3(HBOS_SYS_MPROTECT, (long)addr, (long)len, prot);
}

// ============================================================
// 文件系统扩展 II
// ============================================================

/** 创建目录 */
int hbos_mkdir(const char *path, mode_t mode) {
    return (int)hbos_syscall3(HBOS_SYS_MKDIR, (long)path, mode, 0);
}

/** 删除目录 */
int hbos_rmdir(const char *path) {
    return (int)hbos_syscall3(HBOS_SYS_RMDIR, (long)path, 0, 0);
}

/** 创建符号链接（当前返回 -ENOSYS） */
int hbos_symlink(const char *target, const char *linkpath) {
    return (int)hbos_syscall3(HBOS_SYS_SYMLINK, (long)target, (long)linkpath, 0);
}

/** 修改文件权限 */
int hbos_chmod(const char *path, mode_t mode) {
    return (int)hbos_syscall3(HBOS_SYS_CHMOD, (long)path, mode, 0);
}

/** 修改文件所有者 */
int hbos_chown(const char *path, uid_t uid, gid_t gid) {
    return (int)hbos_syscall3(HBOS_SYS_CHOWN, (long)path, uid, gid);
}

// ============================================================
// 时间操作
// ============================================================

/** 高精度休眠 */
int hbos_nanosleep(const struct hbos_timespec *req, struct hbos_timespec *rem) {
    return (int)hbos_syscall3(HBOS_SYS_NANOSLEEP, (long)req, (long)rem, 0);
}

/** 获取指定时钟时间 */
int hbos_clock_gettime(int clockid, struct hbos_timespec *tp) {
    return (int)hbos_syscall3(HBOS_SYS_CLOCK_GETTIME, clockid, (long)tp, 0);
}

/** 获取进程时间 */
clock_t hbos_times(void *buf) {
    return (clock_t)hbos_syscall3(HBOS_SYS_TIMES, (long)buf, 0, 0);
}

// ============================================================
// 网络套接字
// ============================================================

/** 创建套接字 */
int hbos_socket(int domain, int type, int protocol) {
    return (int)hbos_syscall3(HBOS_SYS_SOCKET, domain, type, protocol);
}

/** 绑定套接字地址 */
int hbos_bind(int sockfd, const void *addr, size_t addrlen) {
    return (int)hbos_syscall3(HBOS_SYS_BIND, sockfd, (long)addr, (long)addrlen);
}

/** 监听套接字连接 */
int hbos_listen(int sockfd, int backlog) {
    return (int)hbos_syscall3(HBOS_SYS_LISTEN, sockfd, backlog, 0);
}

/** 接受套接字连接 */
int hbos_accept(int sockfd, void *addr, size_t *addrlen) {
    return (int)hbos_syscall3(HBOS_SYS_ACCEPT, sockfd, (long)addr, (long)addrlen);
}

/** 发起套接字连接 */
int hbos_connect(int sockfd, const void *addr, size_t addrlen) {
    return (int)hbos_syscall3(HBOS_SYS_CONNECT, sockfd, (long)addr, (long)addrlen);
}

/** 发送数据 */
ssize_t hbos_send(int sockfd, const void *buf, size_t len, int flags) {
    return (ssize_t)hbos_syscall6(HBOS_SYS_SEND, sockfd, (long)buf, (long)len, flags, 0, 0);
}

/** 接收数据 */
ssize_t hbos_recv(int sockfd, void *buf, size_t len, int flags) {
    return (ssize_t)hbos_syscall6(HBOS_SYS_RECV, sockfd, (long)buf, (long)len, flags, 0, 0);
}

// ============================================================
// 系统管理
// ============================================================

/** 重启/关机系统 */
int hbos_reboot(int cmd) {
    return (int)hbos_syscall3(HBOS_SYS_REBOOT, cmd, 0, 0);
}

/** 同步文件系统缓存 */
void hbos_sync(void) {
    hbos_syscall3(HBOS_SYS_SYNC, 0, 0, 0);
}

/** 挂载文件系统（当前返回 -ENOSYS） */
int hbos_mount(const char *src, const char *tgt, const char *fstype, unsigned long flags, const void *data) {
    return (int)hbos_syscall6(HBOS_SYS_MOUNT, (long)src, (long)tgt, (long)fstype, (long)flags, (long)data, 0);
}

/** 卸载文件系统（当前返回 -ENOSYS） */
int hbos_umount(const char *tgt, unsigned long flags) {
    return (int)hbos_syscall3(HBOS_SYS_UMOUNT, (long)tgt, (long)flags, 0);
}

// ============================================================
// 内存管理
// ============================================================

/** 调整进程数据段边界（brk） */
void *hbos_sbrk(intptr_t increment) {
    return (void *)hbos_syscall3(HBOS_SYS_SBRK, (long)increment, 0, 0);
}

// ============================================================
// 便捷函数
// ============================================================

/** 向标准输出写入字符串 */
void hbos_puts(const char *s) {
    if (!s) return;
    hbos_write(STDOUT_FILENO, s, strlen(s));
}
