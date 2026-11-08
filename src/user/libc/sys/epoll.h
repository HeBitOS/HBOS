/**
 * @file    epoll.h
 * @brief   导出 20 个接口（2 类型 / 15 宏 / 3 函数）
 *
 * @layer   user
 * @owner   src/user/libc/sys/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  epoll_data_t  结构体 / 类型别名
 *         epoll_event   epoll 事件
 *   宏    EPOLLIN         常量 / 特性宏
 *         EPOLLPRI        常量 / 特性宏
 *         EPOLLOUT        常量 / 特性宏
 *         EPOLLERR        常量 / 特性宏
 *         EPOLLHUP        常量 / 特性宏
 *         EPOLLRDNORM     常量 / 特性宏
 *         EPOLLRDHUP      常量 / 特性宏
 *         EPOLLEXCLUSIVE  常量 / 特性宏
 *         EPOLLWAKEUP     常量 / 特性宏
 *         EPOLLONESHOT    常量 / 特性宏
 *         EPOLLET         常量 / 特性宏
 *         EPOLL_CLOEXEC   常量 / 特性宏
 *         EPOLL_CTL_ADD   EPOLLCTL添加
 *         EPOLL_CTL_DEL   常量 / 特性宏
 *         EPOLL_CTL_MOD   常量 / 特性宏
 *   函数  epoll_create1  （待补）
 *         epoll_ctl      （待补）
 */
#ifndef HBOS_USER_LIBC_SYS_EPOLL_H
#define HBOS_USER_LIBC_SYS_EPOLL_H

#include <stdint.h>

#define EPOLLIN      0x001
#define EPOLLPRI     0x002
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDNORM  0x040
#define EPOLLRDHUP   0x2000
#define EPOLLEXCLUSIVE (1U << 28)
#define EPOLLWAKEUP    (1U << 29)
#define EPOLLONESHOT   (1U << 30)
#define EPOLLET        (1U << 31)
#define EPOLL_CLOEXEC  02000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t events;
    epoll_data_t data;
} __attribute__((packed));

int epoll_create1(int flags);
int epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events,
               int maxevents, int timeout);

#endif
