/**
 * @file linux_compat.h
 * @brief Lightweight Linux/POSIX event and file-watch compatibility.
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

/* Linux inotify event bits used by the VFS notification hook. */
#define LINUX_IN_ACCESS        0x00000001U
#define LINUX_IN_MODIFY        0x00000002U
#define LINUX_IN_ATTRIB        0x00000004U
#define LINUX_IN_CLOSE_WRITE   0x00000008U
#define LINUX_IN_CLOSE_NOWRITE 0x00000010U
#define LINUX_IN_OPEN          0x00000020U
#define LINUX_IN_MOVED_FROM    0x00000040U
#define LINUX_IN_MOVED_TO      0x00000080U
#define LINUX_IN_CREATE        0x00000100U
#define LINUX_IN_DELETE        0x00000200U
#define LINUX_IN_DELETE_SELF   0x00000400U
#define LINUX_IN_MOVE_SELF     0x00000800U
#define LINUX_IN_Q_OVERFLOW    0x00004000U
#define LINUX_IN_IGNORED       0x00008000U
#define LINUX_IN_ISDIR         0x40000000U

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
int linux_compat_inotify_init1(int flags);
int linux_compat_inotify_add_watch(int fd, const char *path, uint32_t mask);
int linux_compat_inotify_rm_watch(int fd, int watch_descriptor);
void linux_compat_inotify_notify(const char *path, uint32_t mask,
                                 int is_directory);
void linux_compat_inotify_move(const char *old_path, const char *new_path,
                               int is_directory);
void linux_compat_inotify_replace_target(const char *path,
                                         int is_directory);
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
int linux_compat_unix_getsockname(int fd, void *address, uint32_t *length);
int linux_compat_unix_getpeername(int fd, void *address, uint32_t *length);
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
int linux_compat_memfd_read_at(int fd, uint64_t offset,
                               void *buffer, size_t count);
int linux_compat_memfd_map(int fd, uint64_t address, size_t length,
                           uint64_t offset, int writable,
                           uint32_t *backing_id);
void linux_compat_memfd_unmap(uint32_t backing_id);
int linux_compat_memfd_retain_map(uint32_t backing_id);

#endif /* HBOS_LINUX_COMPAT_H */
