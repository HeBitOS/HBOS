/**
 * @file    sys/select.h
 * @brief   Linux I/O 多路复用 select/poll — HBOS 实现
 *
 * 对标 Linux sys/select.h，定义 fd_set 类型及操作宏。
 * 当前为简化实现，仅支持最多 64 个 fd。
 */

#ifndef HBOS_SYS_SELECT_H
#define HBOS_SYS_SELECT_H

#include "types.h"

/** select 超时时间结构 (对标 Linux struct timeval) */
struct timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};

/** fd_set 位图长度（每个 fd 占 1 bit，64 位 = 64 个 fd） */
#define FD_SETSIZE 64

typedef struct {
    uint64_t fds_bits[1];   /**< 位图，每 bit 对应一个 fd */
} fd_set;

/** — fd_set 操作宏 (对标 Linux) — */

#define FD_ZERO(set)        ((set)->fds_bits[0] = 0)
#define FD_SET(fd, set)     ((set)->fds_bits[0] |= (1ULL << (fd)))
#define FD_CLR(fd, set)     ((set)->fds_bits[0] &= ~(1ULL << (fd)))
#define FD_ISSET(fd, set)   (((set)->fds_bits[0] >> (fd)) & 1)

/** structure for poll */
struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN  0x001   /**< 有数据可读 */
#define POLLOUT 0x004   /**< 可以写入 */

#ifdef __cplusplus
extern "C" {
#endif

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
int poll(struct pollfd *fds, unsigned int nfds, int timeout);

#ifdef __cplusplus
}
#endif

#endif