/**
 * @file    poll.h
 * @brief   导出 9 个接口（2 类型 / 6 宏 / 1 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  pollfd      poll 用单个描述符项
 *         nfds_t      结构体 / 类型别名
 *   宏    POLLIN      常量 / 特性宏
 *         POLLPRI     常量 / 特性宏
 *         POLLOUT     常量 / 特性宏
 *         POLLERR     常量 / 特性宏
 *         POLLHUP     常量 / 特性宏
 *         POLLNVAL    常量 / 特性宏
 *   函数  poll        等待一组 fd 就绪
 */
#ifndef HBOS_USER_LIBC_POLL_H
#define HBOS_USER_LIBC_POLL_H

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif
