/**
 * @file    errno.h
 * @brief   errno 编号与线程局部 errno
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    errno            常量 / 特性宏
 *         EPERM            操作不允许
 *         ENOENT           文件或目录不存在
 *         ESRCH            无此进程
 *         EINTR            调用被信号中断
 *         EIO              I/O 错误
 *         ENXIO            无此设备或地址
 *         E2BIG            参数列表过长
 *         ENOEXEC          格式错误，无法执行
 *         EBADF            文件描述符非法
 *         ECHILD           无子进程
 *         EAGAIN           资源暂时不可用
 *         ENOMEM           内存不足
 *         EACCES           权限不足
 *         EFAULT           地址错误
 *         EBUSY            设备或资源忙
 *         EEXIST           已存在
 *         EXDEV            跨设备链接
 *         ENODEV           无此设备
 *         ENOTDIR          不是目录
 *         EISDIR           是目录
 *         EINVAL           参数非法
 *         ENFILE           系统打开文件数超限
 *         EMFILE           进程打开文件数超限
 *         ENOTTY           不适当的 ioctl
 *         EFBIG            文件过大
 *         ENOSPC           设备空间不足
 *         ESPIPE           非法 seek
 *         EROFS            只读文件系统
 *         EPIPE            管道破裂
 *         ERANGE           结果超出范围
 *         ENOSYS           未实现的系统调用
 *         ENOTEMPTY        目录非空
 *         ELOOP            符号链接层级过多
 *         ENOTSOCK         不是套接字
 *         EDESTADDRREQ     需要目标地址
 *         EMSGSIZE         消息过长
 *         EPROTOTYPE       套接字协议类型错误
 *         ENOPROTOOPT      协议选项不可用
 *         EPROTONOSUPPORT  不支持的协议
 *         ESOCKTNOSUPPORT  不支持的套接字类型
 *         EOPNOTSUPP       操作不支持
 *         EAFNOSUPPORT     地址族不支持
 *         EADDRINUSE       地址已被占用
 *         EADDRNOTAVAIL    地址不可用
 *         ECONNRESET       连接被对端重置
 *         EISCONN          已连接
 *         ENOTCONN         未连接
 *         ETIMEDOUT        连接超时
 *         ECONNREFUSED     连接被拒绝
 *         EALREADY         操作正在进行
 *         EINPROGRESS      操作进行中
 *         EWOULDBLOCK      同 EAGAIN
 *   函数  __errno_location  errno is thread-local…
 *         __syscall_errno   Converts a syscall's…
 */
#ifndef HBOS_USER_LIBC_ERRNO_H
#define HBOS_USER_LIBC_ERRNO_H

/*
 * errno is thread-local at the API boundary.  HBOS does not yet load ELF
 * PT_TLS images, so __errno_location() uses a compact TID-keyed table until
 * the dynamic loader can provide compiler TLS.  The lookup only occurs on
 * an error path or when an application explicitly reads errno.
 */
int *__errno_location(void);
#define errno (*__errno_location())

/* Converts a syscall's raw return value (negative -errno on failure, per
 * src/syscall.c's finish_syscall() convention) into a POSIX-shaped result:
 * sets errno and returns -1 on failure, passes the value through on
 * success. Every libc wrapper around a raw __syscallN() call should route
 * its result through this instead of just `ret < 0 ? -1 : ret`, which
 * silently drops the error code. */
long __syscall_errno(long ret);

#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EPIPE           32
#define ERANGE          34
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP      95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ECONNRESET      104
#define EISCONN         106
#define ENOTCONN        107
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EALREADY        114
#define EINPROGRESS     115
#define EWOULDBLOCK     EAGAIN

#endif
