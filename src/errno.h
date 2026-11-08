/**
 * @file    errno.h
 * @brief   errno 编号与线程局部 errno
 *
 * @layer   user
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    EPERM            操作不允许
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
 *         EPFNOSUPPORT     不支持的协议族
 *         EAFNOSUPPORT     地址族不支持
 *         EADDRINUSE       地址已被占用
 *         EADDRNOTAVAIL    地址不可用
 *         ENETDOWN         网络已关闭
 *         ENETUNREACH      网络不可达
 *         ENETRESET        连接因重置而中断
 *         ECONNABORTED     连接被中止
 *         ECONNRESET       连接被对端重置
 *         ENOBUFS          缓冲区空间不足
 *         EISCONN          已连接
 *         ENOTCONN         未连接
 *         ESHUTDOWN        套接字已关闭
 *         ETOOMANYREFS     引用过多
 *         ETIMEDOUT        连接超时
 *         ECONNREFUSED     连接被拒绝
 *         EHOSTDOWN        主机已关闭
 *         EHOSTUNREACH     主机不可达
 *         EALREADY         操作正在进行
 *         EINPROGRESS      操作进行中
 *         EWOULDBLOCK      同 EAGAIN
 *         errno            常量 / 特性宏
 *   函数  __errno_location  （待补）
 */
#ifndef HBOS_ERRNO_H
#define HBOS_ERRNO_H

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
#define EPFNOSUPPORT    96
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ENETRESET       102
#define ECONNABORTED    103
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ESHUTDOWN       108
#define ETOOMANYREFS    109
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EHOSTDOWN       112
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115
#define EWOULDBLOCK     EAGAIN

int *__errno_location(void);
#define errno (*__errno_location())

#endif
