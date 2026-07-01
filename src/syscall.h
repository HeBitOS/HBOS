/**
 * @file    syscall.h
 * @brief   HBOS 系统调用接口 — 内核端定义
 *
 * 系统调用使用 int 0x80 实现（与 Linux 旧式 ABI 兼容）。
 * 参数传递: nr(rax), a0(rdi), a1(rsi), a2(rdx), a3(r10), a4(r8), a5(r9)
 * 返回值: rax（负值表示 -errno）
 *
 * 参考 CoolPotOS 和 Linux x86_64 系统调用表，当前实现了 62 个系统调用，
 * 涵盖文件 I/O、进程控制、内存管理、信号处理、网络套接字、
 * 时间操作和系统信息查询。
 */

#ifndef HBOS_SYSCALL_H
#define HBOS_SYSCALL_H

#include <stdint.h>

/** int 0x80 中断向量号 */
#define HBOS_SYSCALL_VECTOR 0x80

/**
 * 系统调用号枚举
 * 按功能分组:
 *   0-11:   文件 I/O 和基本 POSIX
 *   12-14:  进程控制
 *   15-16:  系统信息
 *   17-22:  文件系统扩展
 *   23-26:  文件描述符操作和时间
 *   27-31:  文件描述符操作扩展
 *   32-38:  进程管理扩展
 *   39-42:  信号处理
 *   43-47:  内存管理
 *   48-49:  文件系统扩展 II
 *   50-53:  用户/组 ID
 *   54-56:  时间操作扩展
 *   57-63:  网络套接字
 *   64-67:  系统管理
 */
typedef enum {
    // ---- 文件 I/O (0-11) ----
    HBOS_SYS_READ = 0,       /**< read(fd, buf, count) */
    HBOS_SYS_WRITE,          /**< write(fd, buf, count) */
    HBOS_SYS_OPEN,           /**< open(path, flags, mode) */
    HBOS_SYS_CLOSE,          /**< close(fd) */
    HBOS_SYS_LSEEK,          /**< lseek(fd, offset, whence) */
    HBOS_SYS_FSTAT,          /**< fstat(fd, stat_buf) */
    HBOS_SYS_STAT,           /**< stat(path, stat_buf) */
    HBOS_SYS_UNLINK,         /**< unlink(path) */
    HBOS_SYS_ISATTY,         /**< isatty(fd) */
    HBOS_SYS_GETPID,         /**< getpid() */
    HBOS_SYS_SBRK,           /**< sbrk(increment) */
    HBOS_SYS_EXIT,           /**< _exit(status) */

    // ---- 进程控制 (12-14) ----
    HBOS_SYS_GETPPID,        /**< getppid() */
    HBOS_SYS_SLEEP,          /**< sleep(seconds) */
    HBOS_SYS_USLEEP,         /**< usleep(microseconds) */

    // ---- 系统信息 (15-16) ----
    HBOS_SYS_UNAME,          /**< uname(utsname_buf) */
    HBOS_SYS_GETTOD,         /**< gettimeofday(tv, tz) */

    // ---- 文件系统扩展 (17-22) ----
    HBOS_SYS_ACCESS,         /**< access(path, mode) */
    HBOS_SYS_FTRUNCATE,      /**< ftruncate(fd, length) */
    HBOS_SYS_MKDIR,          /**< mkdir(path, mode) */
    HBOS_SYS_RMDIR,          /**< rmdir(path) */
    HBOS_SYS_GETCWD,         /**< getcwd(buf, size) */
    HBOS_SYS_CHDIR,          /**< chdir(path) */

    // ---- 文件描述符操作 (23-26) ----
    HBOS_SYS_DUP,            /**< dup(oldfd) */
    HBOS_SYS_GETEUID,        /**< geteuid() */
    HBOS_SYS_GETEGID,        /**< getegid() */
    HBOS_SYS_GETTID,         /**< gettid() */

    // ---- 文件描述符操作扩展 (27-31) ----
    HBOS_SYS_DUP2,           /**< dup2(oldfd, newfd) */
    HBOS_SYS_PIPE,           /**< pipe(pipefd) */
    HBOS_SYS_FCNTL,          /**< fcntl(fd, cmd, arg) */
    HBOS_SYS_IOCTL,          /**< ioctl(fd, cmd, arg) */
    HBOS_SYS_READLINK,       /**< readlink(path, buf, bufsiz) */

    // ---- 进程管理扩展 (32-38) ----
    HBOS_SYS_FORK,           /**< fork() */
    HBOS_SYS_EXECVE,         /**< execve(path, argv, envp) */
    HBOS_SYS_WAITPID,        /**< waitpid(pid, status, options) */
    HBOS_SYS_KILL,           /**< kill(pid, sig) */
    HBOS_SYS_GETUID,         /**< getuid() */
    HBOS_SYS_GETGID,         /**< getgid() */
    HBOS_SYS_SETUID,         /**< setuid(uid) */

    // ---- 信号处理 (39-42) ----
    HBOS_SYS_SIGNAL,         /**< signal(sig, handler) */
    HBOS_SYS_SIGACTION,      /**< sigaction(sig, act, oact) */
    HBOS_SYS_SIGPROCMASK,    /**< sigprocmask(how, set, oset) */
    HBOS_SYS_PAUSE,          /**< pause() */

    // ---- 内存管理 (43-47) ----
    HBOS_SYS_MMAP,           /**< mmap(addr, len, prot, flags, fd, off) */
    HBOS_SYS_MUNMAP,         /**< munmap(addr, len) */
    HBOS_SYS_MPROTECT,       /**< mprotect(addr, len, prot) */
    HBOS_SYS_BRK,            /**< brk(addr) */
    HBOS_SYS_SETGID,         /**< setgid(gid) */

    // ---- 文件系统扩展 II (48-49) ----
    HBOS_SYS_SYMLINK,        /**< symlink(target, linkpath) */
    HBOS_SYS_CHMOD,          /**< chmod(path, mode) */

    // ---- 用户/组 ID (50-53) ----
    HBOS_SYS_CHOWN,          /**< chown(path, uid, gid) */
    HBOS_SYS_GETGROUPS,      /**< getgroups(size, list) */
    HBOS_SYS_SETGROUPS,      /**< setgroups(size, list) */
    HBOS_SYS_GETPGID,        /**< getpgid(pid) */

    // ---- 时间操作扩展 (54-56) ----
    HBOS_SYS_NANOSLEEP,      /**< nanosleep(req, rem) */
    HBOS_SYS_CLOCK_GETTIME,  /**< clock_gettime(clockid, tp) */
    HBOS_SYS_TIMES,          /**< times(buf) */

    // ---- 网络套接字 (57-63) ----
    HBOS_SYS_SOCKET,         /**< socket(domain, type, protocol) */
    HBOS_SYS_BIND,           /**< bind(sockfd, addr, addrlen) */
    HBOS_SYS_LISTEN,         /**< listen(sockfd, backlog) */
    HBOS_SYS_ACCEPT,         /**< accept(sockfd, addr, addrlen) */
    HBOS_SYS_CONNECT,        /**< connect(sockfd, addr, addrlen) */
    HBOS_SYS_SEND,           /**< send(sockfd, buf, len, flags) */
    HBOS_SYS_RECV,           /**< recv(sockfd, buf, len, flags) */

    // ---- 系统管理 (64-67) ----
    HBOS_SYS_REBOOT,         /**< reboot(cmd) */
    HBOS_SYS_SYNC,           /**< sync() */
    HBOS_SYS_MOUNT,          /**< mount(src, tgt, fstype, flags, data) */
    HBOS_SYS_UMOUNT,         /**< umount(tgt, flags) */

    // ---- I/O 多路复用 & 目录遍历 (68-69) ----
    HBOS_SYS_SELECT,         /**< select(nfds, readfds, writefds, exceptfds, timeout) */
    HBOS_SYS_GETDENTS,       /**< getdents(fd, dirp, count) */

    // ---- 目录操作 (70-72) ----
    HBOS_SYS_OPENDIR,        /**< opendir(name) */
    HBOS_SYS_READDIR,        /**< readdir(name, out_name, out_type) */
    HBOS_SYS_CLOSEDIR,       /**< closedir(name) */

    // ---- IPC (73-76) ----
    HBOS_SYS_SHMGET,         /**< shmget(key, size, flags) */
    HBOS_SYS_SHMAT,          /**< shmat(shmid, shmaddr, flags) */
    HBOS_SYS_SHMDT,          /**< shmdt(shmaddr) */
    HBOS_SYS_SHMCTL,         /**< shmctl(shmid, cmd, buf) */

    // ---- GUI 窗体绘制 (77-83) ----
    HBOS_SYS_GUI_INFO,       /**< gui_info(int* w, int* h) -> 1=可用 0=不可用 */
    HBOS_SYS_GUI_CLEAR,      /**< gui_clear(color) 整屏填充 */
    HBOS_SYS_GUI_RECT,       /**< gui_rect(x,y,w,h,color) 填充矩形 */
    HBOS_SYS_GUI_TEXT,       /**< gui_text(x,y,str,color,scale) 绘制文本 */
    HBOS_SYS_GUI_PRESENT,    /**< gui_present() 提交到屏幕 */
    HBOS_SYS_GUI_POLLKEY,    /**< gui_pollkey() -> 键值或 -1 */
    HBOS_SYS_GUI_POLLMOUSE,  /**< gui_pollmouse(int* x,int* y) -> 按键位 */

    // ---- 并发窗口服务 (84-91) ----
    HBOS_SYS_WIN_OPEN,       /**< win_open(title,w,h) -> id 或 -1 */
    HBOS_SYS_WIN_INFO,       /**< win_info(int*w,int*h) -> 1=活动 0=应关闭/无 */
    HBOS_SYS_WIN_CLEAR,      /**< win_clear(color) */
    HBOS_SYS_WIN_FILL,       /**< win_fill(x,y,w,h,color) */
    HBOS_SYS_WIN_TEXT,       /**< win_text(x,y,str,color) */
    HBOS_SYS_WIN_PRESENT,    /**< win_present() 提交并让出 */
    HBOS_SYS_WIN_POLL,       /**< win_poll(int*ev4) -> 事件类型或 0 */
    HBOS_SYS_WIN_CLOSE,      /**< win_close() 关闭窗口 */

    HBOS_SYS_MAX             /**< 系统调用总数 */
} hbos_syscall_no_t;

/**
 * 系统调用帧 — 由汇编 stub 在栈上构建
 * 字段顺序必须与 interrupt_asm.asm 中的 push 顺序一致
 */
typedef struct {
    uint64_t nr;             /**< 系统调用号 */
    uint64_t a0;             /**< 参数 0 (rdi) */
    uint64_t a1;             /**< 参数 1 (rsi) */
    uint64_t a2;             /**< 参数 2 (rdx) */
    uint64_t a3;             /**< 参数 3 (r10) */
    uint64_t a4;             /**< 参数 4 (r8) */
    uint64_t a5;             /**< 参数 5 (r9) */
} hbos_syscall_frame_t;

/**
 * 系统调用分发入口
 * @param frame  系统调用帧指针（由汇编 stub 传入）
 * @return 系统调用返回值（负值表示 -errno）
 */
uint64_t syscall_dispatch_frame(hbos_syscall_frame_t *frame);

#endif /* HBOS_SYSCALL_H */
