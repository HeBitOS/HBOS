/**
 * @file linux_compat.h
 * @brief Lightweight Linux/POSIX event compatibility for user programs.
 *
 * The implementation deliberately reuses the HBOS task fd table and
 * cooperative scheduler.  It does not add a second VFS or a resident
 * compatibility daemon.
 */

#ifndef HBOS_LINUX_COMPAT_H
#define HBOS_LINUX_COMPAT_H

#include <stddef.h>
#include <stdint.h>

struct task;

#define LINUX_POLLIN   0x001
#define LINUX_POLLOUT  0x004
#define LINUX_POLLERR  0x008
#define LINUX_POLLHUP  0x010
#define LINUX_POLLNVAL 0x020

#define LINUX_EPOLL_CTL_ADD 1
#define LINUX_EPOLL_CTL_DEL 2
#define LINUX_EPOLL_CTL_MOD 3

/* epoll event flags (mirror the user-space <sys/epoll.h> values). */
#define LINUX_EPOLLONESHOT (1U << 30)
#define LINUX_EPOLLET      (1U << 31)

#define LINUX_EFD_SEMAPHORE 1
#define LINUX_EFD_NONBLOCK  0x800
#define LINUX_EFD_CLOEXEC   0x80000

typedef struct {
    int fd;
    int16_t events;
    int16_t revents;
} linux_pollfd_t;

typedef union {
    void *ptr;
    int fd;
    uint32_t u32;
    uint64_t u64;
} linux_epoll_data_t;

typedef struct {
    uint32_t events;
    linux_epoll_data_t data;
} __attribute__((packed)) linux_epoll_event_t;

long linux_compat_read(int fd, void *buffer, size_t count);
long linux_compat_write(int fd, const void *buffer, size_t count);
int linux_compat_close(int fd);
void linux_compat_retain(int fd);
void linux_compat_retain_task(struct task *task);
void linux_compat_release_task(struct task *task);

int linux_compat_poll(linux_pollfd_t *fds, uint32_t count, int timeout_ms);
int linux_compat_pipe2(int pipefd[2], int flags);
int linux_compat_eventfd2(uint32_t initial_value, int flags);
int linux_compat_epoll_create1(int flags);
int linux_compat_epoll_ctl(int epfd, int operation, int fd,
                           const linux_epoll_event_t *event);
int linux_compat_epoll_wait(int epfd, linux_epoll_event_t *events,
                            int max_events, int timeout_ms);
long linux_compat_getrandom(void *buffer, size_t count, unsigned int flags);
int linux_compat_futex(uint32_t *address, int operation, uint32_t value,
                       const void *timeout);
int linux_compat_futex6(uint32_t *address, int operation, uint32_t value,
                        const void *timeout, uint32_t *address2,
                        uint32_t bitset);

int linux_compat_unix_socket(int type, int protocol);
int linux_compat_unix_socketpair(int type, int protocol, int pair[2]);
int linux_compat_unix_bind(int fd, const void *address, size_t length);
int linux_compat_unix_listen(int fd, int backlog);
int linux_compat_unix_accept(int fd, void *address, uint32_t *length);
int linux_compat_unix_connect(int fd, const void *address, size_t length);
long linux_compat_unix_send(int fd, const void *buffer, size_t count,
                            int flags);
long linux_compat_unix_recv(int fd, void *buffer, size_t count, int flags);
int linux_compat_unix_getsockopt(int fd, int level, int option,
                                 void *value, uint32_t *length);
int linux_compat_unix_setsockopt(int fd, int level, int option,
                                 const void *value, uint32_t length);
int linux_compat_unix_shutdown(int fd, int how);
int linux_compat_unix_send_rights(int fd, const int *fds, size_t count);
int linux_compat_unix_recv_rights(int fd, int *fds, size_t capacity,
                                  size_t *received, int *truncated);

int linux_compat_memfd_create(const char *name, unsigned int flags);
int linux_compat_memfd_truncate(int fd, uint64_t size);
int linux_compat_memfd_size(int fd, uint64_t *size);
int linux_compat_memfd_map(int fd, uint64_t address, size_t length,
                           uint64_t offset, int writable,
                           uint32_t *backing_id);
void linux_compat_memfd_unmap(uint32_t backing_id);

#endif /* HBOS_LINUX_COMPAT_H */
