#include <stdarg.h>

#include "errno.h"
#include "fcntl.h"
#include "poll.h"
#include "sched.h"
#include "sys/epoll.h"
#include "sys/eventfd.h"
#include "sys/memfd.h"
#include "sys/mman.h"
#include "sys/random.h"
#include "sys/stat.h"
#include "sys/syscall.h"
#include "sys/uio.h"
#include "syscall.h"
#include "string.h"
#include "unistd.h"

#define LINUX_AT_FDCWD (-100)

/* Source-compat syscall() calls cannot inspect the kernel fd table directly.
 * Reuse the lightweight /proc/self/fd metadata already exposed by HBOS to
 * turn a directory fd into the same canonical absolute path. */
static int resolve_linux_at_path(long dirfd, const char *path,
                                 char output[256]) {
    if (!path) {
        errno = EFAULT;
        return -1;
    }
    size_t path_length = strlen(path);
    if (!path_length) {
        errno = ENOENT;
        return -1;
    }
    if (path[0] == '/' || dirfd == LINUX_AT_FDCWD) {
        if (path_length >= 256) {
            errno = EINVAL;
            return -1;
        }
        memcpy(output, path, path_length + 1);
        return 0;
    }

    struct stat status;
    long result = __syscall3(HBOS_SYS_FSTAT, dirfd, (long)&status, 0);
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    if (!S_ISDIR(status.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    char descriptor_path[32] = "/proc/self/fd/";
    char digits[12];
    unsigned int value = (unsigned int)dirfd;
    unsigned int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && count < sizeof(digits));
    size_t prefix = strlen(descriptor_path);
    for (unsigned int i = 0; i < count; i++)
        descriptor_path[prefix + i] = digits[count - i - 1];
    descriptor_path[prefix + count] = '\0';

    result = __syscall3(HBOS_SYS_READLINK, (long)descriptor_path,
                        (long)output, 255);
    if (result < 0) {
        errno = (int)-result;
        return -1;
    }
    size_t base_length = (size_t)result;
    if (base_length + 1 + path_length >= 256) {
        errno = EINVAL;
        return -1;
    }
    if (!base_length || output[base_length - 1] != '/')
        output[base_length++] = '/';
    memcpy(output + base_length, path, path_length + 1);
    return 0;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    long ret = __syscall3(HBOS_SYS_POLL, (long)fds, (long)nfds, timeout);
    return (int)__syscall_errno(ret);
}

int pipe2(int pipefd[2], int flags) {
    long ret = __syscall3(HBOS_SYS_PIPE2, (long)pipefd, flags, 0);
    return (int)__syscall_errno(ret);
}

int eventfd(unsigned int initial_value, int flags) {
    long ret = __syscall3(HBOS_SYS_EVENTFD2, initial_value, flags, 0);
    return (int)__syscall_errno(ret);
}

int eventfd_read(int fd, eventfd_t *value) {
    if (!value) {
        errno = EINVAL;
        return -1;
    }
    return read(fd, value, sizeof(*value)) == (long)sizeof(*value) ? 0 : -1;
}

int eventfd_write(int fd, eventfd_t value) {
    return write(fd, &value, sizeof(value)) == (long)sizeof(value) ? 0 : -1;
}

int epoll_create1(int flags) {
    long ret = __syscall1(HBOS_SYS_EPOLL_CREATE1, flags);
    return (int)__syscall_errno(ret);
}

int epoll_ctl(int epfd, int operation, int fd, struct epoll_event *event) {
    long ret = __syscall6(HBOS_SYS_EPOLL_CTL, epfd, operation, fd,
                          (long)event, 0, 0);
    return (int)__syscall_errno(ret);
}

int epoll_wait(int epfd, struct epoll_event *events,
               int maxevents, int timeout) {
    long ret = __syscall6(HBOS_SYS_EPOLL_WAIT, epfd, (long)events,
                          maxevents, timeout, 0, 0);
    return (int)__syscall_errno(ret);
}

int sched_yield(void) {
    long ret = __syscall1(HBOS_SYS_SCHED_YIELD, 0);
    return (int)__syscall_errno(ret);
}

long getrandom(void *buffer, size_t length, unsigned int flags) {
    long ret = __syscall3(HBOS_SYS_GETRANDOM, (long)buffer,
                          (long)length, flags);
    return __syscall_errno(ret);
}

int memfd_create(const char *name, unsigned int flags) {
    long ret = __syscall3(
        HBOS_SYS_MEMFD_CREATE, (long)name, (long)flags, 0);
    return (int)__syscall_errno(ret);
}

void *mmap(void *address, size_t length, int protection, int flags,
           int fd, long offset) {
    long ret = __syscall6(
        HBOS_SYS_MMAP, (long)address, (long)length, protection,
        flags, fd, offset);
    if (ret < 0) {
        (void)__syscall_errno(ret);
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *address, size_t length) {
    return (int)__syscall_errno(__syscall3(
        HBOS_SYS_MUNMAP, (long)address, (long)length, 0));
}

int mprotect(void *address, size_t length, int protection) {
    return (int)__syscall_errno(__syscall3(
        HBOS_SYS_MPROTECT, (long)address, (long)length, protection));
}

ssize_t readv(int fd, const struct iovec *vectors, int count) {
    if ((!vectors && count) || count < 0 || count > 1024) {
        errno = count > 1024 || count < 0 ? EINVAL : EFAULT;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        ssize_t result = read(fd, vectors[i].iov_base, vectors[i].iov_len);
        if (result < 0) return total ? total : -1;
        total += result;
        if ((size_t)result < vectors[i].iov_len) break;
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *vectors, int count) {
    if ((!vectors && count) || count < 0 || count > 1024) {
        errno = count > 1024 || count < 0 ? EINVAL : EFAULT;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        ssize_t result = write(fd, vectors[i].iov_base, vectors[i].iov_len);
        if (result < 0) return total ? total : -1;
        total += result;
        if ((size_t)result < vectors[i].iov_len) break;
    }
    return total;
}

typedef struct {
    uint32_t version;
    uint32_t size;
    uint64_t flags;
    uint64_t entry;
    uint64_t stack;
    uint64_t argument;
    uint64_t tls;
    uint64_t parent_tid;
    uint64_t child_tid;
    uint64_t clear_child_tid;
} hbos_clone_request_t;

typedef struct {
    int (*function)(void *);
    void *argument;
} hbos_clone_start_t;

static __attribute__((noreturn)) void hbos_clone_start(void *opaque) {
    hbos_clone_start_t *start = (hbos_clone_start_t *)opaque;
    int status = start->function(start->argument);
    (void)__syscall1(HBOS_SYS_EXIT, status);
    for (;;) { }
}

int clone(int (*function)(void *), void *child_stack, int flags,
          void *argument, ...) {
    const int required = CLONE_VM | CLONE_FILES |
                         CLONE_SIGHAND | CLONE_THREAD;
    const int supported = required | CLONE_FS | CLONE_SYSVSEM |
                          CLONE_SETTLS | CLONE_PARENT_SETTID |
                          CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;
    if (!function || !child_stack ||
        (flags & required) != required ||
        (flags & ~(supported | 0xff)) != 0) {
        errno = EINVAL;
        return -1;
    }

    void *parent_tid = NULL;
    void *tls = NULL;
    void *child_tid = NULL;
    va_list optional;
    va_start(optional, argument);
    if (flags & CLONE_PARENT_SETTID)
        parent_tid = va_arg(optional, void *);
    if (flags & CLONE_SETTLS)
        tls = va_arg(optional, void *);
    if (flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID))
        child_tid = va_arg(optional, void *);
    va_end(optional);

    /*
     * Linux clone() receives the top of a caller-owned stack.  Keep the
     * two-word start record above the initial RSP and preserve the SysV
     * x86-64 function-entry alignment (RSP % 16 == 8).
     */
    uintptr_t top = (uintptr_t)child_stack & ~(uintptr_t)15;
    if (top < sizeof(hbos_clone_start_t) + sizeof(uint64_t)) {
        errno = EINVAL;
        return -1;
    }
    hbos_clone_start_t *start =
        (hbos_clone_start_t *)(top - sizeof(hbos_clone_start_t));
    uintptr_t initial_stack = ((uintptr_t)start & ~(uintptr_t)15) - 8;
    *(uint64_t *)initial_stack = 0;
    start->function = function;
    start->argument = argument;

    hbos_clone_request_t request = {
        .version = 1,
        .size = sizeof(request),
        .flags = (uint64_t)(unsigned int)flags,
        .entry = (uint64_t)(uintptr_t)hbos_clone_start,
        .stack = (uint64_t)initial_stack,
        .argument = (uint64_t)(uintptr_t)start,
        .tls = (uint64_t)(uintptr_t)tls,
        .parent_tid = (uint64_t)(uintptr_t)parent_tid,
        .child_tid = (uint64_t)(uintptr_t)child_tid,
        .clear_child_tid = (uint64_t)(uintptr_t)child_tid
    };
    long ret = __syscall1(HBOS_SYS_CLONE_THREAD, (long)&request);
    return (int)__syscall_errno(ret);
}

typedef struct {
    long hbos_number;
    unsigned char arguments;
} syscall_translation_t;

static syscall_translation_t translate_linux_syscall(long number) {
    switch (number) {
        case SYS_read:            return (syscall_translation_t){HBOS_SYS_READ, 3};
        case SYS_write:           return (syscall_translation_t){HBOS_SYS_WRITE, 3};
        case SYS_open:            return (syscall_translation_t){HBOS_SYS_OPEN, 3};
        case SYS_close:           return (syscall_translation_t){HBOS_SYS_CLOSE, 1};
        case SYS_stat:            return (syscall_translation_t){HBOS_SYS_STAT, 2};
        case SYS_fstat:           return (syscall_translation_t){HBOS_SYS_FSTAT, 2};
        case SYS_poll:            return (syscall_translation_t){HBOS_SYS_POLL, 3};
        case SYS_lseek:           return (syscall_translation_t){HBOS_SYS_LSEEK, 3};
        case SYS_mmap:            return (syscall_translation_t){HBOS_SYS_MMAP, 6};
        case SYS_mprotect:        return (syscall_translation_t){HBOS_SYS_MPROTECT, 3};
        case SYS_munmap:          return (syscall_translation_t){HBOS_SYS_MUNMAP, 2};
        case SYS_brk:             return (syscall_translation_t){HBOS_SYS_BRK, 1};
        case SYS_rt_sigaction:    return (syscall_translation_t){HBOS_SYS_SIGACTION, 4};
        case SYS_rt_sigprocmask:  return (syscall_translation_t){HBOS_SYS_SIGPROCMASK, 4};
        case SYS_ioctl:           return (syscall_translation_t){HBOS_SYS_IOCTL, 3};
        case SYS_access:          return (syscall_translation_t){HBOS_SYS_ACCESS, 2};
        case SYS_pipe:            return (syscall_translation_t){HBOS_SYS_PIPE, 1};
        case SYS_sched_yield:     return (syscall_translation_t){HBOS_SYS_SCHED_YIELD, 0};
        case SYS_dup:             return (syscall_translation_t){HBOS_SYS_DUP, 1};
        case SYS_dup2:            return (syscall_translation_t){HBOS_SYS_DUP2, 2};
        case SYS_nanosleep:       return (syscall_translation_t){HBOS_SYS_NANOSLEEP, 2};
        case SYS_getpid:          return (syscall_translation_t){HBOS_SYS_GETPID, 0};
        case SYS_socket:          return (syscall_translation_t){HBOS_SYS_SOCKET, 3};
        case SYS_connect:         return (syscall_translation_t){HBOS_SYS_CONNECT, 3};
        case SYS_accept:          return (syscall_translation_t){HBOS_SYS_ACCEPT, 3};
        case SYS_sendto:          return (syscall_translation_t){HBOS_SYS_SEND, 4};
        case SYS_recvfrom:        return (syscall_translation_t){HBOS_SYS_RECV, 4};
        case SYS_bind:            return (syscall_translation_t){HBOS_SYS_BIND, 3};
        case SYS_listen:          return (syscall_translation_t){HBOS_SYS_LISTEN, 2};
        case SYS_getsockname:     return (syscall_translation_t){HBOS_SYS_GETSOCKNAME, 3};
        case SYS_getpeername:     return (syscall_translation_t){HBOS_SYS_GETPEERNAME, 3};
        case SYS_socketpair:      return (syscall_translation_t){HBOS_SYS_SOCKETPAIR, 4};
        case SYS_setsockopt:      return (syscall_translation_t){HBOS_SYS_SETSOCKOPT, 5};
        case SYS_getsockopt:      return (syscall_translation_t){HBOS_SYS_GETSOCKOPT, 5};
        case SYS_shutdown:        return (syscall_translation_t){HBOS_SYS_SHUTDOWN, 2};
        case SYS_fork:            return (syscall_translation_t){HBOS_SYS_FORK, 0};
        case SYS_execve:          return (syscall_translation_t){HBOS_SYS_EXECVE, 3};
        case SYS_exit:
        case SYS_exit_group:      return (syscall_translation_t){HBOS_SYS_EXIT, 1};
        case SYS_wait4:           return (syscall_translation_t){HBOS_SYS_WAITPID, 3};
        case SYS_kill:            return (syscall_translation_t){HBOS_SYS_KILL, 2};
        case SYS_uname:           return (syscall_translation_t){HBOS_SYS_UNAME, 1};
        case SYS_fcntl:           return (syscall_translation_t){HBOS_SYS_FCNTL, 3};
        case SYS_ftruncate:       return (syscall_translation_t){HBOS_SYS_FTRUNCATE, 2};
        case SYS_getcwd:          return (syscall_translation_t){HBOS_SYS_GETCWD, 2};
        case SYS_chdir:           return (syscall_translation_t){HBOS_SYS_CHDIR, 1};
        case SYS_rename:          return (syscall_translation_t){HBOS_SYS_RENAME, 2};
        case SYS_mkdir:           return (syscall_translation_t){HBOS_SYS_MKDIR, 2};
        case SYS_rmdir:           return (syscall_translation_t){HBOS_SYS_RMDIR, 1};
        case SYS_unlink:          return (syscall_translation_t){HBOS_SYS_UNLINK, 1};
        case SYS_symlink:         return (syscall_translation_t){HBOS_SYS_SYMLINK, 2};
        case SYS_readlink:        return (syscall_translation_t){HBOS_SYS_READLINK, 3};
        case SYS_chmod:           return (syscall_translation_t){HBOS_SYS_CHMOD, 2};
        case SYS_chown:           return (syscall_translation_t){HBOS_SYS_CHOWN, 3};
        case SYS_gettimeofday:    return (syscall_translation_t){HBOS_SYS_GETTOD, 2};
        case SYS_getuid:          return (syscall_translation_t){HBOS_SYS_GETUID, 0};
        case SYS_getgid:          return (syscall_translation_t){HBOS_SYS_GETGID, 0};
        case SYS_geteuid:         return (syscall_translation_t){HBOS_SYS_GETEUID, 0};
        case SYS_getegid:         return (syscall_translation_t){HBOS_SYS_GETEGID, 0};
        case SYS_getppid:         return (syscall_translation_t){HBOS_SYS_GETPPID, 0};
        case SYS_getgroups:       return (syscall_translation_t){HBOS_SYS_GETGROUPS, 2};
        case SYS_setgroups:       return (syscall_translation_t){HBOS_SYS_SETGROUPS, 2};
        case SYS_getpgid:         return (syscall_translation_t){HBOS_SYS_GETPGID, 1};
        case SYS_arch_prctl:      return (syscall_translation_t){HBOS_SYS_ARCH_PRCTL, 2};
        case SYS_gettid:          return (syscall_translation_t){HBOS_SYS_GETTID, 0};
        case SYS_futex:           return (syscall_translation_t){HBOS_SYS_FUTEX, 6};
        case SYS_set_tid_address: return (syscall_translation_t){HBOS_SYS_SET_TID_ADDRESS, 1};
        case SYS_clock_gettime:   return (syscall_translation_t){HBOS_SYS_CLOCK_GETTIME, 2};
        case SYS_epoll_wait:      return (syscall_translation_t){HBOS_SYS_EPOLL_WAIT, 4};
        case SYS_epoll_ctl:       return (syscall_translation_t){HBOS_SYS_EPOLL_CTL, 4};
        case SYS_epoll_pwait:     return (syscall_translation_t){HBOS_SYS_EPOLL_WAIT, 4};
        case SYS_eventfd2:        return (syscall_translation_t){HBOS_SYS_EVENTFD2, 2};
        case SYS_epoll_create1:   return (syscall_translation_t){HBOS_SYS_EPOLL_CREATE1, 1};
        case SYS_pipe2:           return (syscall_translation_t){HBOS_SYS_PIPE2, 2};
        case SYS_getrandom:       return (syscall_translation_t){HBOS_SYS_GETRANDOM, 3};
        case SYS_memfd_create:    return (syscall_translation_t){HBOS_SYS_MEMFD_CREATE, 2};
        case SYS_set_robust_list:  return (syscall_translation_t){HBOS_SYS_SET_ROBUST_LIST, 2};
        case SYS_get_robust_list:  return (syscall_translation_t){HBOS_SYS_GET_ROBUST_LIST, 3};
        default:                  return (syscall_translation_t){-1, 0};
    }
}

long syscall(long linux_number, ...) {
    /*
     * The *at and dup3 calls need argument adaptation rather than a simple
     * number translation.  HBOS currently has one process cwd and no dirfd
     * path adaptation.  AT_FDCWD remains the zero-copy fast path; other
     * directory fds resolve through HBOS's lightweight /proc/self/fd view.
     */
    if (linux_number == SYS_openat || linux_number == SYS_newfstatat ||
        linux_number == SYS_readlinkat || linux_number == SYS_dup3 ||
        linux_number == SYS_renameat || linux_number == SYS_renameat2 ||
        linux_number == SYS_symlinkat) {
        long arguments[5] = {0, 0, 0, 0, 0};
        unsigned int count = (linux_number == SYS_dup3 ||
                              linux_number == SYS_symlinkat) ? 3 :
            (linux_number == SYS_renameat2 ? 5 : 4);
        va_list special;
        va_start(special, linux_number);
        for (unsigned int i = 0; i < count; i++)
            arguments[i] = va_arg(special, long);
        va_end(special);

        if (linux_number == SYS_dup3) {
            if (arguments[0] == arguments[1] ||
                (arguments[2] & ~O_CLOEXEC)) {
                errno = EINVAL;
                return -1;
            }
            long ret = __syscall3(HBOS_SYS_DUP2, arguments[0],
                                  arguments[1], 0);
            if (ret >= 0 && (arguments[2] & O_CLOEXEC))
                (void)__syscall3(HBOS_SYS_FCNTL, ret, 2, O_CLOEXEC);
            return __syscall_errno(ret);
        }

        if (linux_number == SYS_renameat || linux_number == SYS_renameat2) {
            long flags = linux_number == SYS_renameat2 ? arguments[4] : 0;
            if (flags & ~1L) {
                errno = EOPNOTSUPP;
                return -1;
            }
            char old_path[256];
            char new_path[256];
            if (resolve_linux_at_path(arguments[0],
                                      (const char *)arguments[1],
                                      old_path) < 0 ||
                resolve_linux_at_path(arguments[2],
                                      (const char *)arguments[3],
                                      new_path) < 0)
                return -1;
            return __syscall_errno(__syscall3(
                HBOS_SYS_RENAME, (long)old_path, (long)new_path, flags));
        }

        if (linux_number == SYS_symlinkat) {
            char link_path[256];
            if (resolve_linux_at_path(arguments[1],
                                      (const char *)arguments[2],
                                      link_path) < 0)
                return -1;
            return __syscall_errno(__syscall3(
                HBOS_SYS_SYMLINK, arguments[0], (long)link_path, 0));
        }

        if (linux_number == SYS_newfstatat) {
            long flags = arguments[3];
            if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
                errno = EINVAL;
                return -1;
            }
            const char *input = (const char *)arguments[1];
            if (!input) {
                errno = EFAULT;
                return -1;
            }
            if (!input[0]) {
                if (!(flags & AT_EMPTY_PATH)) {
                    errno = ENOENT;
                    return -1;
                }
                if (arguments[0] != LINUX_AT_FDCWD)
                    return __syscall_errno(__syscall3(
                        HBOS_SYS_FSTAT, arguments[0], arguments[2], 0));
                char cwd[256];
                long got = __syscall3(HBOS_SYS_GETCWD, (long)cwd,
                                      sizeof(cwd), 0);
                if (got < 0) return __syscall_errno(got);
                return __syscall_errno(__syscall3(
                    HBOS_SYS_STAT, (long)cwd, arguments[2], 0));
            }
        }

        char path[256];
        if (resolve_linux_at_path(arguments[0],
                                  (const char *)arguments[1], path) < 0)
            return -1;
        if (linux_number == SYS_openat)
            return __syscall_errno(__syscall3(
                HBOS_SYS_OPEN, (long)path, arguments[2], arguments[3]));
        if (linux_number == SYS_newfstatat) {
            return __syscall_errno(__syscall3(
                HBOS_SYS_STAT, (long)path, arguments[2],
                arguments[3] & AT_SYMLINK_NOFOLLOW));
        }
        return __syscall_errno(__syscall3(
            HBOS_SYS_READLINK, (long)path, arguments[2], arguments[3]));
    }

    syscall_translation_t translation =
        translate_linux_syscall(linux_number);
    if (translation.hbos_number < 0) {
        errno = ENOSYS;
        return -1;
    }

    long arguments[6] = {0, 0, 0, 0, 0, 0};
    va_list list;
    va_start(list, linux_number);
    for (unsigned int i = 0; i < translation.arguments; i++)
        arguments[i] = va_arg(list, long);
    va_end(list);

    return __syscall_errno(__syscall6(
        translation.hbos_number, arguments[0], arguments[1], arguments[2],
        arguments[3], arguments[4], arguments[5]));
}
