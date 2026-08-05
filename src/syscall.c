/**
 * @file    syscall.c
 * @brief   HBOS 系统调用分发器
 *
 * 接收来自 int 0x80 汇编 stub 的系统调用帧，
 * 根据系统调用号分发到对应的 POSIX 实现函数。
 *
 * 返回值约定:
 *   - 成功: 返回非负值（具体含义因调用而异）
 *   - 失败: 返回负的 errno 值（如 -ENOENT）
 */

#include <stdint.h>

#include "errno.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "sys/wait.h"
#include "sys/dirent.h"
#include "syscall.h"
#include "unistd.h"
#include "core/task.h"
#include "core/vmm.h"
#include "fd.h"
#include "string.h"
#include "acpi.h"
#include "net.h"
#include "signal.h"
#include "fs.h"
#include "core/heap.h"
#include "elf.h"
#include "user/ldso.h"
#include "user/hax_app.h"
#include "vfs.h"
#include "version.h"
#include "api/gui_service.h"
#include "tls.h"
#include "https.h"
#include "linux_compat.h"

/* Interim buffered limit; the next loader step is fd-backed segment streaming. */
#define SYSCALL_EXEC_MAX_SIZE (512u * 1024u)
#define SYSCALL_HTTPS_MAX_SIZE (2u * 1024u * 1024u)

/**
 * 将 POSIX 函数返回值转换为系统调用返回值
 * POSIX 函数通过 errno 全局变量报告错误，
 * 系统调用通过负返回值报告错误。
 */
static uint64_t finish_syscall(long ret) {
    if (ret < 0 && errno > 0)
        return (uint64_t)(-(int64_t)errno);
    return (uint64_t)ret;
}

/*
 * Structures crossing the native Linux x86-64 syscall boundary must not be
 * passed straight to HBOS.  Keeping these small adapters here avoids a
 * second VFS/socket implementation while preserving the byte-for-byte ABI
 * expected by unmodified musl/glibc binaries.
 */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime;
    int64_t st_atime_nsec;
    int64_t st_mtime;
    int64_t st_mtime_nsec;
    int64_t st_ctime;
    int64_t st_ctime_nsec;
    int64_t reserved[3];
} linux_x86_stat_t;

typedef struct {
    void *base;
    uint64_t length;
} linux_x86_iovec_t;

typedef struct {
    void *name;
    uint32_t name_length;
    uint32_t name_padding;
    linux_x86_iovec_t *vectors;
    uint64_t vector_count;
    void *control;
    uint64_t control_length;
    int32_t flags;
    uint32_t flags_padding;
} linux_x86_msghdr_t;

typedef struct {
    uint64_t length;
    int32_t level;
    int32_t type;
} linux_x86_cmsghdr_t;

static void linux_copy_stat(linux_x86_stat_t *output,
                            const struct stat *input) {
    memset(output, 0, sizeof(*output));
    output->st_dev = input->st_dev;
    output->st_ino = input->st_ino;
    output->st_nlink = input->st_nlink;
    output->st_mode = input->st_mode;
    output->st_uid = input->st_uid;
    output->st_gid = input->st_gid;
    output->st_rdev = input->st_rdev;
    output->st_size = input->st_size;
    output->st_blksize = 4096;
    output->st_blocks =
        input->st_size > 0 ? (input->st_size + 511) / 512 : 0;
    output->st_atime = input->st_atime;
    output->st_mtime = input->st_mtime;
    output->st_ctime = input->st_ctime;
}

static uint64_t linux_stat_path(const char *path, linux_x86_stat_t *output) {
    if (!path || !output) return (uint64_t)(-EFAULT);
    struct stat native;
    if (stat(path, &native) < 0)
        return (uint64_t)(-(errno > 0 ? errno : EIO));
    linux_copy_stat(output, &native);
    return 0;
}

static uint64_t linux_fstat_fd(int fd, linux_x86_stat_t *output) {
    if (!output) return (uint64_t)(-EFAULT);
    struct stat native;
    task_t *current = task_current();
    if (current && current->fd_table && fd >= 0 && fd < POSIX_MAX_FDS &&
        current->fd_table->entries[fd].used &&
        current->fd_table->entries[fd].type == FD_MEMFD) {
        uint64_t size;
        if (linux_compat_memfd_size(fd, &size) < 0)
            return (uint64_t)(-(errno > 0 ? errno : EBADF));
        memset(&native, 0, sizeof(native));
        native.st_mode = S_IFREG | S_IRUSR | S_IWUSR;
        native.st_nlink = 1;
        native.st_size = (off_t)size;
        linux_copy_stat(output, &native);
        return 0;
    }
    if (fstat(fd, &native) < 0)
        return (uint64_t)(-(errno > 0 ? errno : EIO));
    linux_copy_stat(output, &native);
    return 0;
}

static int64_t linux_native_call(uint64_t number, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5) {
    hbos_syscall_frame_t native = {
        .nr = number, .a0 = a0, .a1 = a1, .a2 = a2,
        .a3 = a3, .a4 = a4, .a5 = a5
    };
    return (int64_t)syscall_dispatch_frame(&native);
}

static uint64_t linux_vector_io(int fd, const linux_x86_iovec_t *vectors,
                                uint64_t count, int write_operation) {
    if ((!vectors && count) || count > 1024)
        return (uint64_t)(count > 1024 ? -EINVAL : -EFAULT);
    int64_t total = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (!vectors[i].base && vectors[i].length)
            return total ? (uint64_t)total : (uint64_t)(-EFAULT);
        if (vectors[i].length > (uint64_t)(INT64_MAX - total))
            return total ? (uint64_t)total : (uint64_t)(-EINVAL);
        int64_t result = linux_native_call(
            write_operation ? HBOS_SYS_WRITE : HBOS_SYS_READ,
            (uint64_t)fd, (uint64_t)(uintptr_t)vectors[i].base,
            vectors[i].length, 0, 0, 0);
        if (result < 0) return total ? (uint64_t)total : (uint64_t)result;
        total += result;
        if ((uint64_t)result < vectors[i].length) break;
    }
    return (uint64_t)total;
}

static uint64_t linux_message_io(int fd, linux_x86_msghdr_t *message,
                                 int flags, int send_operation) {
    if (!message || (!message->vectors && message->vector_count))
        return (uint64_t)(-EFAULT);
    if (message->vector_count > 1024) return (uint64_t)(-EINVAL);
    int rights[4];
    size_t rights_count = 0;
    uint64_t control_capacity = message->control_length;
    task_t *current = task_current();
    int unix_fd = current && current->fd_table && fd >= 0 &&
        fd < POSIX_MAX_FDS && current->fd_table->entries[fd].used &&
        current->fd_table->entries[fd].type == FD_UNIX;
    if (send_operation && message->control && message->control_length) {
        if (message->control_length < sizeof(linux_x86_cmsghdr_t))
            return (uint64_t)(-EINVAL);
        linux_x86_cmsghdr_t *header =
            (linux_x86_cmsghdr_t *)message->control;
        if (header->length < sizeof(*header) ||
            header->length > message->control_length ||
            header->level != 1 || header->type != 1 ||
            (header->length - sizeof(*header)) % sizeof(int))
            return (uint64_t)(-EOPNOTSUPP);
        rights_count =
            (size_t)((header->length - sizeof(*header)) / sizeof(int));
        if (!rights_count || rights_count > 4)
            return (uint64_t)(-EINVAL);
        memcpy(rights, (uint8_t *)message->control + sizeof(*header),
               rights_count * sizeof(int));
        if (linux_compat_unix_send_rights(fd, rights, rights_count) < 0)
            return (uint64_t)(-(errno > 0 ? errno : EINVAL));
    } else if (send_operation && message->control_length) {
        return (uint64_t)(-EFAULT);
    }
    if (send_operation && message->name && message->name_length) {
        int64_t connected = linux_native_call(
            HBOS_SYS_CONNECT, (uint64_t)fd,
            (uint64_t)(uintptr_t)message->name, message->name_length,
            0, 0, 0);
        if (connected < 0 && connected != -EISCONN)
            return (uint64_t)connected;
    }
    if (!send_operation) {
        message->flags = 0;
        message->name_length = 0;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < message->vector_count; i++) {
        linux_x86_iovec_t *vector = &message->vectors[i];
        if (!vector->base && vector->length)
            return total ? (uint64_t)total : (uint64_t)(-EFAULT);
        int vector_flags = flags;
        if (!send_operation && total) vector_flags |= 0x40; /* MSG_DONTWAIT */
        int64_t result = linux_native_call(
            send_operation ? HBOS_SYS_SEND : HBOS_SYS_RECV,
            (uint64_t)fd, (uint64_t)(uintptr_t)vector->base,
            vector->length, (uint64_t)vector_flags, 0, 0);
        if (result < 0) {
            if (!send_operation && total && result == -EAGAIN) break;
            return total ? (uint64_t)total : (uint64_t)result;
        }
        total += result;
        if ((uint64_t)result < vector->length) break;
    }
    if (!send_operation && unix_fd) {
        size_t received = 0;
        int truncated = 0;
        int *output_fds = NULL;
        size_t fd_capacity = 0;
        if (message->control &&
            control_capacity >= sizeof(linux_x86_cmsghdr_t)) {
            output_fds = (int *)((uint8_t *)message->control +
                                 sizeof(linux_x86_cmsghdr_t));
            fd_capacity =
                (size_t)((control_capacity -
                          sizeof(linux_x86_cmsghdr_t)) / sizeof(int));
            if (fd_capacity > 4) fd_capacity = 4;
        }
        if (linux_compat_unix_recv_rights(
                fd, output_fds, fd_capacity, &received, &truncated) < 0)
            return total ? (uint64_t)total
                         : (uint64_t)(-(errno > 0 ? errno : EINVAL));
        if (received && message->control) {
            linux_x86_cmsghdr_t *header =
                (linux_x86_cmsghdr_t *)message->control;
            header->length = sizeof(*header) + received * sizeof(int);
            header->level = 1;
            header->type = 1;
            message->control_length =
                (header->length + 7) & ~(uint64_t)7;
            if (message->control_length > control_capacity)
                message->control_length = control_capacity;
        } else {
            message->control_length = 0;
        }
        if (truncated) message->flags |= 0x08; /* MSG_CTRUNC */
    } else if (!send_operation) {
        message->control_length = 0;
    }
    return (uint64_t)total;
}

static uint64_t linux_getdents64(int fd, void *buffer, uint64_t count) {
    if (!buffer) return (uint64_t)(-EFAULT);
    task_t *current = task_current();
    if (!current || !current->fd_table || fd < 0 || fd >= POSIX_MAX_FDS ||
        !current->fd_table->entries[fd].used)
        return (uint64_t)(-EBADF);
    fd_entry_t *entry = &current->fd_table->entries[fd];
    if (!entry->node || entry->node->type != VFS_NODE_DIR)
        return (uint64_t)(-ENOTDIR);
    if (!entry->path[0]) return 0;

    uint64_t written = 0;
    for (;;) {
        char name[VFS_MAX_NAME];
        uint32_t type;
        if (vfs_readdir_at(entry->path, entry->offset, name, &type) < 0)
            break;
        size_t name_length = strlen(name) + 1;
        uint64_t record_length = (19 + name_length + 7) & ~(uint64_t)7;
        if (written + record_length > count) break;
        uint8_t *record = (uint8_t *)buffer + written;
        memset(record, 0, (size_t)record_length);
        *(uint64_t *)(record + 0) = entry->offset + 1;
        *(int64_t *)(record + 8) = (int64_t)(entry->offset + 1);
        *(uint16_t *)(record + 16) = (uint16_t)record_length;
        record[18] = type == VFS_NODE_DIR ? 4 : 8;
        memcpy(record + 19, name, name_length);
        entry->offset++;
        written += record_length;
    }
    return written;
}

uint64_t linux_syscall_dispatch_frame(hbos_syscall_frame_t *linux_frame) {
    if (!linux_frame) return (uint64_t)(-EFAULT);
    hbos_syscall_frame_t native = *linux_frame;

    switch (linux_frame->nr) {
        case 0:   native.nr = HBOS_SYS_READ; break;
        case 1:   native.nr = HBOS_SYS_WRITE; break;
        case 2:   native.nr = HBOS_SYS_OPEN; break;
        case 3:   native.nr = HBOS_SYS_CLOSE; break;
        case 4:
            return linux_stat_path(
                (const char *)linux_frame->a0,
                (linux_x86_stat_t *)linux_frame->a1);
        case 5:
            return linux_fstat_fd(
                (int)linux_frame->a0,
                (linux_x86_stat_t *)linux_frame->a1);
        case 7:   native.nr = HBOS_SYS_POLL; break;
        case 8:   native.nr = HBOS_SYS_LSEEK; break;
        case 9:   native.nr = HBOS_SYS_MMAP; break;
        case 10:  native.nr = HBOS_SYS_MPROTECT; break;
        case 11:  native.nr = HBOS_SYS_MUNMAP; break;
        case 12:  native.nr = HBOS_SYS_BRK; break;
        case 13:  native.nr = HBOS_SYS_SIGACTION; break;
        case 14:  native.nr = HBOS_SYS_SIGPROCMASK; break;
        case 16:  native.nr = HBOS_SYS_IOCTL; break;
        case 19:
            return linux_vector_io(
                (int)linux_frame->a0,
                (const linux_x86_iovec_t *)linux_frame->a1,
                linux_frame->a2, 0);
        case 20:
            return linux_vector_io(
                (int)linux_frame->a0,
                (const linux_x86_iovec_t *)linux_frame->a1,
                linux_frame->a2, 1);
        case 21:  native.nr = HBOS_SYS_ACCESS; break;
        case 22:  native.nr = HBOS_SYS_PIPE; break;
        case 24:  native.nr = HBOS_SYS_SCHED_YIELD; break;
        case 32:  native.nr = HBOS_SYS_DUP; break;
        case 33:  native.nr = HBOS_SYS_DUP2; break;
        case 35:  native.nr = HBOS_SYS_NANOSLEEP; break;
        case 39:  native.nr = HBOS_SYS_GETPID; break;
        case 41:  native.nr = HBOS_SYS_SOCKET; break;
        case 42:  native.nr = HBOS_SYS_CONNECT; break;
        case 43:  native.nr = HBOS_SYS_ACCEPT; break;
        case 44:  native.nr = HBOS_SYS_SEND; break;
        case 45:  native.nr = HBOS_SYS_RECV; break;
        case 46:
            return linux_message_io(
                (int)linux_frame->a0,
                (linux_x86_msghdr_t *)linux_frame->a1,
                (int)linux_frame->a2, 1);
        case 47:
            return linux_message_io(
                (int)linux_frame->a0,
                (linux_x86_msghdr_t *)linux_frame->a1,
                (int)linux_frame->a2, 0);
        case 48:  native.nr = HBOS_SYS_SHUTDOWN; break;
        case 49:  native.nr = HBOS_SYS_BIND; break;
        case 50:  native.nr = HBOS_SYS_LISTEN; break;
        case 53:  native.nr = HBOS_SYS_SOCKETPAIR; break;
        case 54:  native.nr = HBOS_SYS_SETSOCKOPT; break;
        case 55:  native.nr = HBOS_SYS_GETSOCKOPT; break;
        case 57:  native.nr = HBOS_SYS_FORK; break;
        case 59:  native.nr = HBOS_SYS_EXECVE; break;
        case 60:
        case 231: native.nr = HBOS_SYS_EXIT; break;
        case 61:  native.nr = HBOS_SYS_WAITPID; break;
        case 62:  native.nr = HBOS_SYS_KILL; break;
        case 63:  native.nr = HBOS_SYS_UNAME; break;
        case 72:  native.nr = HBOS_SYS_FCNTL; break;
        case 77:  native.nr = HBOS_SYS_FTRUNCATE; break;
        case 79:  native.nr = HBOS_SYS_GETCWD; break;
        case 80:  native.nr = HBOS_SYS_CHDIR; break;
        case 83:  native.nr = HBOS_SYS_MKDIR; break;
        case 84:  native.nr = HBOS_SYS_RMDIR; break;
        case 87:  native.nr = HBOS_SYS_UNLINK; break;
        case 89:  native.nr = HBOS_SYS_READLINK; break;
        case 90:  native.nr = HBOS_SYS_CHMOD; break;
        case 92:  native.nr = HBOS_SYS_CHOWN; break;
        case 96:  native.nr = HBOS_SYS_GETTOD; break;
        case 102: native.nr = HBOS_SYS_GETUID; break;
        case 104: native.nr = HBOS_SYS_GETGID; break;
        case 107: native.nr = HBOS_SYS_GETEUID; break;
        case 108: native.nr = HBOS_SYS_GETEGID; break;
        case 110: native.nr = HBOS_SYS_GETPPID; break;
        case 115: native.nr = HBOS_SYS_GETGROUPS; break;
        case 116: native.nr = HBOS_SYS_SETGROUPS; break;
        case 121: native.nr = HBOS_SYS_GETPGID; break;
        case 158: native.nr = HBOS_SYS_ARCH_PRCTL; break;
        case 186: native.nr = HBOS_SYS_GETTID; break;
        case 202: native.nr = HBOS_SYS_FUTEX; break;
        case 217:
            return linux_getdents64(
                (int)linux_frame->a0, (void *)linux_frame->a1,
                linux_frame->a2);
        case 218: native.nr = HBOS_SYS_SET_TID_ADDRESS; break;
        case 228: native.nr = HBOS_SYS_CLOCK_GETTIME; break;
        case 232: native.nr = HBOS_SYS_EPOLL_WAIT; break;
        case 233: native.nr = HBOS_SYS_EPOLL_CTL; break;
        case 257:
            if ((int64_t)linux_frame->a0 != -100)
                return (uint64_t)(-ENOSYS);
            native.nr = HBOS_SYS_OPEN;
            native.a0 = linux_frame->a1;
            native.a1 = linux_frame->a2;
            native.a2 = linux_frame->a3;
            break;
        case 262:
            if ((int64_t)linux_frame->a0 != -100 || linux_frame->a3 != 0)
                return (uint64_t)(-ENOSYS);
            return linux_stat_path(
                (const char *)linux_frame->a1,
                (linux_x86_stat_t *)linux_frame->a2);
        case 267:
            if ((int64_t)linux_frame->a0 != -100)
                return (uint64_t)(-ENOSYS);
            native.nr = HBOS_SYS_READLINK;
            native.a0 = linux_frame->a1;
            native.a1 = linux_frame->a2;
            native.a2 = linux_frame->a3;
            break;
        case 273: native.nr = HBOS_SYS_SET_ROBUST_LIST; break;
        case 274: native.nr = HBOS_SYS_GET_ROBUST_LIST; break;
        case 281: native.nr = HBOS_SYS_EPOLL_WAIT; break;
        case 288: {
            int flags = (int)linux_frame->a3;
            if (flags & ~(0x800 | 0x80000))
                return (uint64_t)(-EINVAL);
            int64_t accepted = linux_native_call(
                HBOS_SYS_ACCEPT, linux_frame->a0, linux_frame->a1,
                linux_frame->a2, 0, 0, 0);
            if (accepted < 0) return (uint64_t)accepted;
            task_t *current = task_current();
            if (current && current->fd_table &&
                accepted < POSIX_MAX_FDS) {
                current->fd_table->entries[accepted].flags |= flags;
            }
            return (uint64_t)accepted;
        }
        case 290: native.nr = HBOS_SYS_EVENTFD2; break;
        case 291: native.nr = HBOS_SYS_EPOLL_CREATE1; break;
        case 292:
            if (linux_frame->a0 == linux_frame->a1 ||
                (linux_frame->a2 & ~O_CLOEXEC))
                return (uint64_t)(-EINVAL);
            native.nr = HBOS_SYS_DUP2;
            break;
        case 293: native.nr = HBOS_SYS_PIPE2; break;
        case 318: native.nr = HBOS_SYS_GETRANDOM; break;
        case 319:
            return (uint64_t)(int64_t)linux_compat_memfd_create(
                (const char *)linux_frame->a0,
                (unsigned int)linux_frame->a1);
        default: return (uint64_t)(-ENOSYS);
    }
    return syscall_dispatch_frame(&native);
}

static uint64_t align_page_up(uint64_t value) {
    return (value + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static int map_user_heap_growth(uint64_t old_brk, uint64_t new_brk) {
    if (new_brk <= old_brk) return 0;
    if (new_brk > UINT64_MAX - (PAGE_SIZE - 1)) return -1;

    uint64_t va_start = old_brk & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t va_end = align_page_up(new_brk);
    for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
        if (vmm_get_phys(va) != 0) continue;
        if (!vmm_alloc_page_at(va, VMM_P | VMM_W | VMM_U)) return -1;
    }
    return 0;
}

static uint64_t user_sbrk(intptr_t increment) {
    task_t *cur = task_current();
    task_mm_t *mm = cur ? cur->mm : NULL;
    if (!mm || !mm->user_heap_start ||
        mm->user_heap_limit <= mm->user_heap_start)
        return (uint64_t)(-ENOMEM);

    uint64_t old_brk = mm->user_brk ? mm->user_brk : mm->user_heap_start;
    uint64_t new_brk = old_brk;

    if (increment > 0) {
        uint64_t inc = (uint64_t)increment;
        if (inc > mm->user_heap_limit - old_brk) return (uint64_t)(-ENOMEM);
        new_brk = old_brk + inc;
    } else if (increment < 0) {
        uint64_t dec = (uint64_t)(-(increment + 1)) + 1;
        if (dec > old_brk - mm->user_heap_start) return (uint64_t)(-ENOMEM);
        new_brk = old_brk - dec;
    }

    if (map_user_heap_growth(old_brk, new_brk) != 0)
        return (uint64_t)(-ENOMEM);
    mm->user_brk = new_brk;
    return old_brk;
}

static uint64_t user_brk(uint64_t new_brk) {
    task_t *cur = task_current();
    task_mm_t *mm = cur ? cur->mm : NULL;
    if (!mm || !mm->user_heap_start ||
        mm->user_heap_limit <= mm->user_heap_start)
        return (uint64_t)(-ENOMEM);
    if (!new_brk) return mm->user_brk ? mm->user_brk : mm->user_heap_start;
    if (new_brk < mm->user_heap_start || new_brk > mm->user_heap_limit)
        return (uint64_t)(-ENOMEM);

    uint64_t old_brk = mm->user_brk ? mm->user_brk : mm->user_heap_start;
    if (map_user_heap_growth(old_brk, new_brk) != 0)
        return (uint64_t)(-ENOMEM);
    mm->user_brk = new_brk;
    return new_brk;
}

/**
 * 系统调用主分发函数
 * 由 interrupt_asm.asm 中的 syscall_int80_stub 调用
 *
 * @param f  系统调用帧（包含调用号和 6 个参数）
 * @return 系统调用返回值
 */
uint64_t syscall_dispatch_frame(hbos_syscall_frame_t *f) {
    if (!f) return (uint64_t)(-EFAULT);

    switch (f->nr) {
        // ============================================================
        // 文件 I/O (0-11)
        // ============================================================
        case HBOS_SYS_READ:
            if (task_current() && (int)f->a0 >= 0 &&
                (int)f->a0 < POSIX_MAX_FDS &&
                task_current()->fd_table->entries[(int)f->a0].used &&
                (task_current()->fd_table->entries[(int)f->a0].type == FD_EVENT ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_UNIX ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_MEMFD))
                return (uint64_t)linux_compat_read(
                    (int)f->a0, (void *)f->a1, (size_t)f->a2);
            return finish_syscall((long)read((int)f->a0, (void *)f->a1, (size_t)f->a2));

        case HBOS_SYS_WRITE:
            if (task_current() && (int)f->a0 >= 0 &&
                (int)f->a0 < POSIX_MAX_FDS &&
                task_current()->fd_table->entries[(int)f->a0].used &&
                (task_current()->fd_table->entries[(int)f->a0].type == FD_EVENT ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_UNIX ||
                 task_current()->fd_table->entries[(int)f->a0].type == FD_MEMFD))
                return (uint64_t)linux_compat_write(
                    (int)f->a0, (const void *)f->a1, (size_t)f->a2);
            return finish_syscall((long)write((int)f->a0, (const void *)f->a1, (size_t)f->a2));

        case HBOS_SYS_OPEN:
            return finish_syscall((long)open((const char *)f->a0, (int)f->a1, (int)f->a2));

        case HBOS_SYS_CLOSE:
            (void)linux_compat_close((int)f->a0);
            return finish_syscall((long)close((int)f->a0));

        case HBOS_SYS_LSEEK: {
            int fd = (int)f->a0;
            task_t *current = task_current();
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_MEMFD) {
                uint64_t size;
                if (linux_compat_memfd_size(fd, &size) < 0)
                    return (uint64_t)(-EBADF);
                fd_entry_t *entry = &current->fd_table->entries[fd];
                int64_t base = (int)f->a2 == SEEK_SET ? 0 :
                    ((int)f->a2 == SEEK_CUR ? entry->offset :
                     ((int)f->a2 == SEEK_END ? (int64_t)size : -1));
                if (base < 0) return (uint64_t)(-EINVAL);
                int64_t next = base + (int64_t)f->a1;
                if (next < 0 || (uint64_t)next > size)
                    return (uint64_t)(-EINVAL);
                entry->offset = (uint32_t)next;
                return (uint64_t)next;
            }
            return finish_syscall((long)lseek((int)f->a0, (off_t)f->a1, (int)f->a2));
        }

        case HBOS_SYS_FSTAT: {
            int fd = (int)f->a0;
            task_t *current = task_current();
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_MEMFD) {
                struct stat *output = (struct stat *)f->a1;
                uint64_t size;
                if (!output) return (uint64_t)(-EFAULT);
                if (linux_compat_memfd_size(fd, &size) < 0)
                    return (uint64_t)(-EBADF);
                memset(output, 0, sizeof(*output));
                output->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
                output->st_nlink = 1;
                output->st_size = (off_t)size;
                return 0;
            }
            return finish_syscall((long)fstat((int)f->a0, (struct stat *)f->a1));
        }

        case HBOS_SYS_STAT:
            return finish_syscall((long)stat((const char *)f->a0, (struct stat *)f->a1));

        case HBOS_SYS_UNLINK:
            return finish_syscall((long)unlink((const char *)f->a0));

        case HBOS_SYS_ISATTY:
            return finish_syscall((long)isatty((int)f->a0));

        case HBOS_SYS_GETPID:
            return (uint64_t)getpid();

        case HBOS_SYS_SBRK:
            return user_sbrk((intptr_t)f->a0);

        case HBOS_SYS_EXIT: {
            // 实际终止当前任务
            int status = (int)f->a0;
            task_set_exit_status(status);
            task_exit();
            return 0;  // 不会到达这里
        }

        // ============================================================
        // 进程控制 (12-14)
        // ============================================================
        case HBOS_SYS_GETPPID:
            return (uint64_t)getppid();

        case HBOS_SYS_SLEEP:
            return finish_syscall((long)sleep((unsigned int)f->a0));

        case HBOS_SYS_USLEEP:
            return finish_syscall((long)usleep((useconds_t)f->a0));

        // ============================================================
        // 系统信息 (15-16)
        // ============================================================
        case HBOS_SYS_UNAME: {
            // uname: 返回系统信息到 utsname 结构
            // 结构定义在 sys/utsname.h 中
            struct utsname {
                char sysname[65];
                char nodename[65];
                char release[65];
                char version[65];
                char machine[65];
            };
            struct utsname *buf = (struct utsname *)f->a0;
            if (!buf) return (uint64_t)(-EFAULT);
            memset(buf, 0, sizeof(struct utsname));
            memcpy(buf->sysname, "HBOS", 5);
            memcpy(buf->nodename, "hbos", 5);
            {
                const char *rel = HBOS_VERSION_REL;
                const char *ver = "HBOS " HBOS_VERSION_REL;
                memcpy(buf->release, rel, strlen(rel) + 1);
                memcpy(buf->version, ver, strlen(ver) + 1);
            }
            memcpy(buf->machine, "x86_64", 7);
            return 0;
        }

        case HBOS_SYS_GETTOD: {
            // gettimeofday: 返回当前时间（基于 RDTSC 的近似值）
            struct timeval {
                uint64_t tv_sec;
                uint64_t tv_usec;
            };
            struct timeval *tv = (struct timeval *)f->a0;
            if (!tv) return (uint64_t)(-EFAULT);
            // 使用 RDTSC 近似时间（从启动开始的秒数）
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = ((uint64_t)hi << 32) | lo;
            // 假设 ~1GHz TSC
            tv->tv_sec = tsc / 1000000000ULL;
            tv->tv_usec = (tsc % 1000000000ULL) / 1000;
            return 0;
        }

        // ============================================================
        // 文件系统扩展 (17-22)
        // ============================================================
        case HBOS_SYS_ACCESS: {
            return finish_syscall((long)access((const char *)f->a0, (int)f->a1));
        }

        case HBOS_SYS_FTRUNCATE: {
            int fd = (int)f->a0;
            task_t *current = task_current();
            if (current && current->fd_table && fd >= 0 &&
                fd < POSIX_MAX_FDS &&
                current->fd_table->entries[fd].used &&
                current->fd_table->entries[fd].type == FD_MEMFD) {
                int result = linux_compat_memfd_truncate(fd, f->a1);
                return result < 0
                    ? (uint64_t)(-(errno > 0 ? errno : EINVAL)) : 0;
            }
            return finish_syscall((long)ftruncate((int)f->a0, (off_t)f->a1));
        }

        case HBOS_SYS_MKDIR: {
            return finish_syscall((long)mkdir((const char *)f->a0, (mode_t)f->a1));
        }

        case HBOS_SYS_RMDIR: {
            return finish_syscall((long)rmdir((const char *)f->a0));
        }

        case HBOS_SYS_GETCWD: {
            char *buf = (char *)f->a0;
            size_t size = (size_t)f->a1;
            char *ret = getcwd(buf, size);
            if (!ret) return (uint64_t)(-(int64_t)errno);
            return (uint64_t)strlen(ret);
        }

        case HBOS_SYS_CHDIR: {
            return finish_syscall((long)chdir((const char *)f->a0));
        }

        // ============================================================
        // 文件描述符操作和时间 (23-26)
        // ============================================================
        case HBOS_SYS_DUP: {
            int oldfd = (int)f->a0;
            if (oldfd < 0 || oldfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[oldfd].used)
                return (uint64_t)(-EBADF);
            int newfd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fd_table->entries[i].used) { newfd = i; break; }
            }
            if (newfd < 0) return (uint64_t)(-EMFILE);
            cur->fd_table->entries[newfd] = cur->fd_table->entries[oldfd];
            linux_compat_retain(newfd);
            return (uint64_t)newfd;
        }

        case HBOS_SYS_GETEUID:
            // geteuid: 返回有效用户 ID（当前始终为 root）
            return 0;

        case HBOS_SYS_GETEGID:
            // getegid: 返回有效组 ID（当前始终为 root）
            return 0;

        case HBOS_SYS_GETTID:
            // gettid: 返回线程 ID（当前 = 任务 ID）
            return (uint64_t)task_get_id();

        // ============================================================
        // 文件描述符操作扩展 (27-31)
        // ============================================================
        case HBOS_SYS_DUP2: {
            int oldfd = (int)f->a0;
            int newfd = (int)f->a1;
            if (oldfd < 0 || newfd < 0 || oldfd >= POSIX_MAX_FDS || newfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[oldfd].used)
                return (uint64_t)(-EBADF);
            if (oldfd == newfd) return (uint64_t)newfd;
            if (cur->fd_table->entries[newfd].used) {
                (void)linux_compat_close(newfd);
                close(newfd);
            }
            cur->fd_table->entries[newfd] = cur->fd_table->entries[oldfd];
            linux_compat_retain(newfd);
            return (uint64_t)newfd;
        }

        case HBOS_SYS_PIPE: {
            int *pipefd = (int *)f->a0;
            if (!pipefd) return (uint64_t)(-EFAULT);
            extern int pipe(int pipefd[2]);
            int ret = pipe(pipefd);
            if (ret < 0) return (uint64_t)(-errno);
            return 0;
        }

        case HBOS_SYS_FCNTL: {
            int fd = (int)f->a0;
            int cmd = (int)f->a1;
            long arg = (long)f->a2;
            if (fd < 0 || fd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            switch (cmd) {
                case 0:   return cur->fd_table->entries[fd].flags;
                case 1:   cur->fd_table->entries[fd].flags = (int)arg; return 0;
                case 2:   cur->fd_table->entries[fd].flags |= (int)arg; return 0;
                case 3:   cur->fd_table->entries[fd].flags &= ~(int)arg; return 0;
                default:  return (uint64_t)(-EINVAL);
            }
        }

        case HBOS_SYS_IOCTL: {
            int fd = (int)f->a0;
            if (fd < 0 || fd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            return (uint64_t)(-ENOTTY);
        }

        case HBOS_SYS_READLINK: {
            const char *path = (const char *)f->a0;
            char *buf = (char *)f->a1;
            size_t bufsiz = (size_t)f->a2;
            if (!path) return (uint64_t)(-EFAULT);
            file_t *file = fs_find_file(path);
            if (!file) return (uint64_t)(-ENOENT);
            if (file->type != 2) return (uint64_t)(-EINVAL);
            uint32_t n = fs_read_file_data(file, 0, buf, (uint32_t)(bufsiz - 1));
            if (buf && bufsiz > 0) {
                if (n > bufsiz - 1) n = (uint32_t)(bufsiz - 1);
                ((uint8_t *)buf)[n] = '\0';
            }
            return (uint64_t)n;
        }

        // ============================================================
        // 进程管理扩展 (32-38)
        // ============================================================
        case HBOS_SYS_FORK: {
            int pid = task_fork();
            if (pid < 0) return (uint64_t)(-EAGAIN);
            return (uint64_t)pid;
        }

        case HBOS_SYS_EXECVE: {
            const char *path = (const char *)f->a0;
            char *const *argv = (char *const *)f->a1;
            char *const *envp = (char *const *)f->a2;
            if (!path) return (uint64_t)(-EFAULT);
            int fd = open(path, O_RDONLY);
            if (fd < 0) return (uint64_t)(-(int64_t)(errno ? errno : ENOENT));

            uint8_t *elf_buf = (uint8_t *)kmalloc(SYSCALL_EXEC_MAX_SIZE);
            if (!elf_buf) {
                close(fd);
                return (uint64_t)(-ENOMEM);
            }

            size_t size = 0;
            ssize_t n = 0;
            while ((n = read(fd, elf_buf + size, SYSCALL_EXEC_MAX_SIZE - size)) > 0) {
                size += (size_t)n;
                if (size >= SYSCALL_EXEC_MAX_SIZE) break;
            }
            int saved_errno = errno;
            close(fd);

            if (n < 0) {
                kfree(elf_buf);
                return (uint64_t)(-(int64_t)(saved_errno ? saved_errno : EIO));
            }
            if (size < sizeof(elf64_ehdr_t) || size >= SYSCALL_EXEC_MAX_SIZE) {
                kfree(elf_buf);
                return (uint64_t)(size >= SYSCALL_EXEC_MAX_SIZE ? -E2BIG : -ENOEXEC);
            }
            int ret = elf64_load_and_exec(elf_buf, size, argv, envp);
            kfree(elf_buf);
            if (ret < 0) return (uint64_t)(-ENOEXEC);
            return 0;
        }

        case HBOS_SYS_WAITPID: {
            pid_t pid = (pid_t)f->a0;
            int *status = (int *)f->a1;
            int options = (int)f->a2;
            if (pid <= 0) return (uint64_t)(-ECHILD);
            int st = 0;
            int ret = task_wait((uint32_t)pid, &st);
            if (ret < 0) return (uint64_t)(-ECHILD);
            if (options & WNOHANG) {
                const task_t *t = task_get_by_id((uint32_t)pid);
                if (t && t->state != TASK_TERMINATED) return 0;
            }
            if (status)
                *status = W_EXITCODE(st, 0);
            return (uint64_t)pid;
        }

        case HBOS_SYS_KILL: {
            pid_t pid = (pid_t)f->a0;
            int sig = (int)f->a1;
            if (pid <= 0) return (uint64_t)(-ESRCH);
            if (task_kill((uint32_t)pid, sig) < 0)
                return (uint64_t)(-ESRCH);
            return 0;
        }

        case HBOS_SYS_GETUID:
            return 0;

        case HBOS_SYS_GETGID:
            return 0;

        case HBOS_SYS_SETUID: {
            uid_t uid = (uid_t)f->a0;
            if (uid != 0) return (uint64_t)(-EPERM);
            return 0;
        }

        // ============================================================
        // 信号处理 (39-42)
        // ============================================================
        case HBOS_SYS_SIGNAL: {
            int sig = (int)f->a0;
            void (*handler)(int) = (void (*)(int))f->a1;
            if (sig <= 0 || sig >= _NSIG) {
                errno = EINVAL;
                return (uint64_t)(-EINVAL);
            }
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            void (*old)(int) = cur->sig_handler[sig];
            cur->sig_handler[sig] = handler;
            return (uint64_t)(uintptr_t)old;
        }

        case HBOS_SYS_SIGACTION: {
            int sig = (int)f->a0;
            void (*handler)(int) = (void (*)(int))f->a1;
            if (sig <= 0 || sig >= _NSIG) {
                errno = EINVAL;
                return (uint64_t)(-EINVAL);
            }
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            void (*old)(int) = cur->sig_handler[sig];
            cur->sig_handler[sig] = handler;
            void **oact = (void **)f->a2;
            if (oact) *oact = (void *)old;
            return 0;
        }

        case HBOS_SYS_SIGPROCMASK: {
            return 0;
        }

        case HBOS_SYS_PAUSE: {
            return (uint64_t)(-EINTR);
        }

        // ============================================================
        // 内存管理 (43-47)
        // ============================================================
        case HBOS_SYS_MMAP: {
            void *addr = (void *)f->a0;
            size_t len = (size_t)f->a1;
            int prot = (int)f->a2;
            int flags = (int)f->a3;
            int fd = (int)f->a4;
            off_t off = (off_t)f->a5;
            if (len == 0) return (uint64_t)(-EINVAL);
            if (off < 0 || ((uint64_t)off & (PAGE_SIZE - 1)))
                return (uint64_t)(-EINVAL);
            size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            void *p = addr;
            int fixed = flags & 0x10;
            int fixed_noreplace = flags & 0x100000;
            if ((fixed || fixed_noreplace) &&
                ((uint64_t)p & (PAGE_SIZE - 1)))
                return (uint64_t)(-EINVAL);
            if (p && !fixed && !fixed_noreplace)
                p = (void *)((uint64_t)p & ~(uint64_t)(PAGE_SIZE - 1));
            if (!p) {
                for (uint64_t va = 0x0000100000000000ULL;
                     va < 0x0000200000000000ULL; va += PAGE_SIZE) {
                    int free = 1;
                    for (uint64_t va2 = va;
                         va2 < va + pages * PAGE_SIZE; va2 += PAGE_SIZE) {
                        if (vmm_get_phys(va2) != 0) { free = 0; break; }
                    }
                    if (free) { p = (void *)va; break; }
                }
            }
            if (!p) return (uint64_t)(-ENOMEM);
            if (fixed_noreplace) {
                for (size_t i = 0; i < pages; i++) {
                    if (vmm_get_phys((uint64_t)p + i * PAGE_SIZE))
                        return (uint64_t)(-EEXIST);
                }
            }
            task_t *cur = task_current();
            if (!cur || !cur->mm) return (uint64_t)(-ESRCH);
            vm_area_t *vma = (vm_area_t *)kmalloc(sizeof(vm_area_t));
            if (!vma) return (uint64_t)(-ENOMEM);
            memset(vma, 0, sizeof(*vma));

            int file_backed = !(flags & 0x20) && fd >= 0;
            if (file_backed) {
                if (!(flags & 0x01)) {
                    kfree(vma);
                    return (uint64_t)(-EOPNOTSUPP);
                }
                uint32_t backing_id = 0;
                if (linux_compat_memfd_map(
                        fd, (uint64_t)p, len, (uint64_t)off,
                        (prot & 0x02) != 0, &backing_id) < 0) {
                    int saved = errno;
                    kfree(vma);
                    return (uint64_t)(-(saved > 0 ? saved : EINVAL));
                }
                vma->backing_type = 1;
                vma->backing_id = backing_id;
            } else {
                size_t mapped = 0;
                for (; mapped < pages; mapped++) {
                    uint64_t va = (uint64_t)p + mapped * PAGE_SIZE;
                    if (fixed && vmm_get_phys(va)) vmm_unmap_page(va);
                    if (!vmm_alloc_page_at(
                            va, VMM_P | VMM_U | VMM_W))
                        break;
                    memset((void *)va, 0, PAGE_SIZE);
                }
                if (mapped != pages) {
                    while (mapped)
                        vmm_unmap_page(
                            (uint64_t)p + --mapped * PAGE_SIZE);
                    kfree(vma);
                    return (uint64_t)(-ENOMEM);
                }
            }
            vma->start = (uint64_t)p;
            vma->end   = (uint64_t)p + pages * PAGE_SIZE;
            vma->next  = cur->mm->areas;
            cur->mm->areas = vma;
            return (uint64_t)p;
        }

        case HBOS_SYS_MUNMAP: {
            void *addr = (void *)f->a0;
            size_t len = (size_t)f->a1;
            if (!addr || len == 0) return (uint64_t)(-EINVAL);
            size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
            task_t *cur = task_current();
            vm_area_t **pp = (cur && cur->mm) ? &cur->mm->areas : NULL;
            while (pp && *pp) {
                if ((*pp)->start == (uint64_t)addr) {
                    for (size_t i = 0; i < pages; i++)
                        vmm_unmap_page((uint64_t)addr + i * PAGE_SIZE);
                    vm_area_t *vma = *pp;
                    *pp = vma->next;
                    if (vma->backing_type == 1)
                        linux_compat_memfd_unmap(vma->backing_id);
                    kfree(vma);
                    return 0;
                }
                pp = &(*pp)->next;
            }
            return (uint64_t)(-EINVAL);
        }

        case HBOS_SYS_MPROTECT: {
            return 0;
        }

        case HBOS_SYS_BRK: {
            return user_brk((uint64_t)f->a0);
        }

        case HBOS_SYS_SETGID: {
            gid_t gid = (gid_t)f->a0;
            if (gid != 0) return (uint64_t)(-EPERM);
            return 0;
        }

        // ============================================================
        // 文件系统扩展 II (48-49)
        // ============================================================
        case HBOS_SYS_SYMLINK: {
            const char *target = (const char *)f->a0;
            const char *linkpath = (const char *)f->a1;
            if (!target || !linkpath) return (uint64_t)(-EFAULT);
            if (fs_find_file(linkpath)) return (uint64_t)(-EEXIST);
            file_t *link = fs_create_file(linkpath);
            if (!link) return (uint64_t)(-ENOSPC);
            link->type = 2;
            size_t tlen = strlen(target);
            if (tlen > link->capacity) tlen = link->capacity;
            if (fs_write_file_data(link, 0, target, (uint32_t)tlen) < 0)
                return (uint64_t)(-EIO);
            link->size = (uint32_t)tlen;
            return 0;
        }

        case HBOS_SYS_CHMOD: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            (void)f->a1;
            struct stat st;
            if (stat(path, &st) < 0)
                return (uint64_t)(-errno);
            return 0;
        }

        // ============================================================
        // 用户/组 ID (50-53)
        // ============================================================
        case HBOS_SYS_CHOWN: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            struct stat st;
            if (stat(path, &st) < 0)
                return (uint64_t)(-errno);
            return 0;
        }

        case HBOS_SYS_GETGROUPS: {
            return 0;
        }

        case HBOS_SYS_SETGROUPS: {
            return (uint64_t)(-EPERM);
        }

        case HBOS_SYS_GETPGID: {
            pid_t pid = (pid_t)f->a0;
            if (pid == 0) pid = (pid_t)task_get_id();
            return (uint64_t)pid;
        }

        // ============================================================
        // 时间操作扩展 (54-56)
        // ============================================================
        case HBOS_SYS_NANOSLEEP: {
            const struct timespec_req {
                uint64_t tv_sec;
                uint64_t tv_nsec;
            } *req = (const struct timespec_req *)f->a0;
            if (!req) return (uint64_t)(-EFAULT);
            if (req->tv_sec > 0)
                sleep((unsigned int)req->tv_sec);
            else if (req->tv_nsec > 0)
                usleep((useconds_t)(req->tv_nsec / 1000));
            return 0;
        }

        case HBOS_SYS_CLOCK_GETTIME: {
            int clockid = (int)f->a0;
            struct timespec_out {
                uint64_t tv_sec;
                uint64_t tv_nsec;
            } *tp = (struct timespec_out *)f->a1;
            if (!tp) return (uint64_t)(-EFAULT);
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = ((uint64_t)hi << 32) | lo;
            tp->tv_sec = tsc / 1000000000ULL;
            tp->tv_nsec = tsc % 1000000000ULL;
            (void)clockid;
            return 0;
        }

        case HBOS_SYS_TIMES: {
            void *buf = (void *)f->a0;
            (void)buf;
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint64_t tsc = ((uint64_t)hi << 32) | lo;
            return (uint64_t)(tsc / 10000);
        }

        // ============================================================
        // 网络套接字 (57-63)
        // ============================================================
        case HBOS_SYS_SOCKET: {
            int domain = (int)f->a0;
            int type = (int)f->a1;
            int protocol = (int)f->a2;
            if (domain == 1)
                return finish_syscall(
                    linux_compat_unix_socket(type, protocol));
            if (domain != 2) return (uint64_t)(-EAFNOSUPPORT);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table) return (uint64_t)(-ESRCH);
            int fd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fd_table->entries[i].used) { fd = i; break; }
            }
            if (fd < 0) return (uint64_t)(-EMFILE);
            cur->fd_table->entries[fd].used = true;
            cur->fd_table->entries[fd].node = NULL;
            cur->fd_table->entries[fd].offset = 0;
            cur->fd_table->entries[fd].flags = O_RDWR;
            cur->fd_table->entries[fd].type = FD_SOCKET;
            cur->fd_table->entries[fd].local_port = 0;
            return (uint64_t)fd;
        }

        case HBOS_SYS_BIND: {
            int sockfd = (int)f->a0;
            const void *addr = (const void *)f->a1;
            size_t addrlen = (size_t)f->a2;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!addr) return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_bind(
                    sockfd, addr, addrlen));
            if (addrlen < 8) return (uint64_t)(-EINVAL);
            const uint8_t *addr_bytes = (const uint8_t *)addr;
            uint16_t port = ((uint16_t)addr_bytes[2] << 8) | addr_bytes[3];
            cur->fd_table->entries[sockfd].local_port = port;
            return 0;
        }

        case HBOS_SYS_LISTEN: {
            int sockfd = (int)f->a0;
            int backlog = (int)f->a1;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            (void)backlog;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(
                    linux_compat_unix_listen(sockfd, backlog));
            uint16_t port = cur->fd_table->entries[sockfd].local_port;
            net_tcp_listen(port);
            return 0;
        }

        case HBOS_SYS_ACCEPT: {
            int sockfd = (int)f->a0;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_accept(
                    sockfd, (void *)f->a1, (uint32_t *)f->a2));
            net_tcp_conn_t *conn = (net_tcp_conn_t *)kmalloc(sizeof(net_tcp_conn_t));
            if (!conn) return (uint64_t)(-ENOMEM);
            int ret = net_tcp_accept(cur->fd_table->entries[sockfd].local_port, conn, 3000);
            if (ret < 0) {
                kfree(conn);
                return (uint64_t)(-EAGAIN);
            }
            int newfd = -1;
            for (int i = 0; i < POSIX_MAX_FDS; i++) {
                if (!cur->fd_table->entries[i].used) { newfd = i; break; }
            }
            if (newfd < 0) { kfree(conn); return (uint64_t)(-EMFILE); }
            cur->fd_table->entries[newfd].used  = true;
            cur->fd_table->entries[newfd].type  = FD_SOCKET;
            cur->fd_table->entries[newfd].node  = (vfs_node_t *)conn;
            cur->fd_table->entries[newfd].flags = 0;
            return (uint64_t)newfd;
        }

        case HBOS_SYS_CONNECT: {
            int sockfd = (int)f->a0;
            const void *addr = (const void *)f->a1;
            size_t addrlen = (size_t)f->a2;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!addr) return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_connect(
                    sockfd, addr, addrlen));
            if (addrlen < 8) return (uint64_t)(-EINVAL);
            const uint8_t *addr_bytes = (const uint8_t *)addr;
            uint32_t ip = ((uint32_t)addr_bytes[4] << 24) |
                          ((uint32_t)addr_bytes[5] << 16) |
                          ((uint32_t)addr_bytes[6] << 8) |
                          (uint32_t)addr_bytes[7];
            uint16_t port = ((uint16_t)addr_bytes[2] << 8) | addr_bytes[3];
            net_tcp_conn_t *conn = (net_tcp_conn_t *)kmalloc(sizeof(net_tcp_conn_t));
            if (!conn) return (uint64_t)(-ENOMEM);
            int ret = net_tcp_connect(ip, port, conn);
            if (ret < 0) {
                kfree(conn);
                return (uint64_t)(-ECONNREFUSED);
            }
            cur->fd_table->entries[sockfd].node = (vfs_node_t *)conn;
            return 0;
        }

        case HBOS_SYS_SEND: {
            int sockfd = (int)f->a0;
            const void *buf = (const void *)f->a1;
            size_t len = (size_t)f->a2;
            int flags = (int)f->a3;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!buf) return (uint64_t)(-EFAULT);
            (void)flags;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return (uint64_t)linux_compat_unix_send(
                    sockfd, buf, len, flags);
            net_tcp_conn_t *conn = (net_tcp_conn_t *)cur->fd_table->entries[sockfd].node;
            if (!conn) return (uint64_t)(-ENOTCONN);
            /* net_tcp_send() returns a status code (0=success, <0=failure),
             * not a byte count -- POSIX send() must return the number of
             * bytes sent on success. It's all-or-nothing per call (any
             * len over TCP_MSS is rejected outright, no partial sends),
             * so on success the byte count is simply len. Returning the
             * raw status code here made every successful send() look like
             * "0 bytes sent" to callers, which breaks the common
             * `while (total<len) total += send(...)` retry pattern. */
            int ret = net_tcp_send(conn, (const uint8_t *)buf, (uint32_t)len);
            if (ret < 0) return (uint64_t)(-ECONNRESET);
            return (uint64_t)len;
        }

        case HBOS_SYS_RECV: {
            int sockfd = (int)f->a0;
            void *buf = (void *)f->a1;
            size_t len = (size_t)f->a2;
            int flags = (int)f->a3;
            if (sockfd < 0 || sockfd >= POSIX_MAX_FDS)
                return (uint64_t)(-EBADF);
            if (!buf) return (uint64_t)(-EFAULT);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table ||
                !cur->fd_table->entries[sockfd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[sockfd].type == FD_UNIX)
                return (uint64_t)linux_compat_unix_recv(
                    sockfd, buf, len, flags);
            net_tcp_conn_t *conn = (net_tcp_conn_t *)cur->fd_table->entries[sockfd].node;
            if (!conn) return (uint64_t)(-ENOTCONN);
            uint32_t recv_len = 0;
            int ret = net_tcp_recv(conn, (uint8_t *)buf, (uint32_t)len, &recv_len, 10);
            if (ret < 0) return (uint64_t)(-ECONNRESET);
            return (uint64_t)recv_len;
        }

        // ============================================================
        // 系统管理 (64-67)
        // ============================================================
        case HBOS_SYS_REBOOT: {
            int cmd = (int)f->a0;
            if (cmd != 0x1234567 && cmd != 0x01234567)
                return (uint64_t)(-EINVAL);
            acpi_poweroff();
            while (1) __asm__ volatile("hlt");
            return 0;
        }

        case HBOS_SYS_SYNC: {
            extern int fs_sync(void);
            fs_sync();
            return 0;
        }

        case HBOS_SYS_MOUNT: {
            const char *src = (const char *)f->a0;
            const char *tgt = (const char *)f->a1;
            if (!tgt) return (uint64_t)(-EFAULT);
            if (src && src[0]) {
                extern int fs_mount_disk(void);
                if (fs_mount_disk() < 0) return (uint64_t)(-ENODEV);
            }
            (void)f->a2; (void)f->a3; (void)f->a4;
            return 0;
        }

        case HBOS_SYS_UMOUNT: {
            const char *tgt = (const char *)f->a0;
            if (!tgt) return (uint64_t)(-EFAULT);
            struct stat st;
            if (stat(tgt, &st) < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        // ============================================================
        // I/O 多路复用 & 目录遍历 (68-69)
        // ============================================================
        case HBOS_SYS_SELECT: {
            int nfds = (int)f->a0;
            uint64_t *readfds = (uint64_t *)f->a1;
            uint64_t *writefds = (uint64_t *)f->a2;
            int count = 0;
            if (nfds > POSIX_MAX_FDS) nfds = POSIX_MAX_FDS;
            task_t *cur = task_current();
            if (!cur) return (uint64_t)(-ESRCH);
            uint64_t rfds = readfds ? *readfds : 0;
            uint64_t wfds = writefds ? *writefds : 0;
            for (int fd = 0; fd < nfds; fd++) {
                if ((rfds & (1ULL << fd)) && cur->fd_table->entries[fd].used) {
                    if (readfds) rfds |= (1ULL << fd);
                    else rfds &= ~(1ULL << fd);
                    count++;
                }
                if ((wfds & (1ULL << fd)) && cur->fd_table->entries[fd].used) {
                    if (writefds) wfds |= (1ULL << fd);
                    else wfds &= ~(1ULL << fd);
                    count++;
                }
            }
            if (readfds) *readfds = rfds;
            if (writefds) *writefds = wfds;
            return (uint64_t)count;
        }

        case HBOS_SYS_GETDENTS: {
            int fd = (int)f->a0;
            struct dirent *dirp = (struct dirent *)f->a1;
            unsigned int count = (unsigned int)f->a2;
            if (fd < 0 || fd >= POSIX_MAX_FDS || !dirp)
                return (uint64_t)(-EINVAL);
            task_t *cur = task_current();
            if (!cur || !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            vfs_node_t *dir = cur->fd_table->entries[fd].node;
            if (!dir || dir->type != VFS_NODE_DIR)
                return (uint64_t)(-ENOTDIR);
            /* 之前这里完全没用到上面解析出来的 dir 节点，而是走
             * fs_get_count()/fs_get_file() 遍历整个文件系统的扁平文件表——
             * 相当于不管打开的是哪个目录，getdents 永远把全盘所有文件都
             * 当作"这个目录的项"返回，d_name 里存的还是完整路径而不是
             * 单个文件名。vfs_node_t 本身没有父子链接（见 vfs.h），真正能
             * 列出"这一个目录下有什么"的只有 vfs.c 给 ls 用的那套按路径
             * 字符串 + 顺序游标的 vfs_opendir/vfs_readdir/vfs_closedir——
             * 所以这里改用同一套，配合新加的 fds[fd].path（fd_alloc 时
             * open() 存下来的解析后路径，见 src/fd.h/src/lib/posix.c）。 */
            const char *path = cur->fd_table->entries[fd].path;
            if (!path[0] || vfs_opendir(path) < 0)
                return 0;
            unsigned int bytes_written = 0;
            for (uint32_t i = 0; ; i++) {
                char name[VFS_MAX_NAME];
                uint32_t type;
                if (vfs_readdir(path, name, &type) < 0) break;
                struct dirent dent;
                dent.d_ino = (uint64_t)i;
                dent.d_off = (int64_t)i;
                dent.d_type = (type == VFS_NODE_DIR) ? DT_DIR : DT_REG;
                size_t name_len = strlen(name);
                if (name_len > NAME_MAX) name_len = NAME_MAX;
                memcpy(dent.d_name, name, name_len);
                dent.d_name[name_len] = '\0';
                dent.d_reclen = sizeof(struct dirent) - NAME_MAX - 1 + name_len + 1;
                unsigned int reclen = sizeof(dent);
                if (bytes_written + reclen > count) break;
                memcpy((uint8_t *)dirp + bytes_written, &dent, reclen);
                bytes_written += reclen;
            }
            vfs_closedir(path);
            return (uint64_t)bytes_written;
        }

        case HBOS_SYS_OPENDIR: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            int ret = vfs_opendir(path);
            if (ret < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        case HBOS_SYS_READDIR: {
            const char *path = (const char *)f->a0;
            char *out_name = (char *)f->a1;
            uint32_t *out_type = (uint32_t *)f->a2;
            if (!path || !out_name || !out_type) return (uint64_t)(-EFAULT);
            int ret = vfs_readdir(path, out_name, out_type);
            if (ret < 0) return (uint64_t)(-ENOENT);
            return 0;
        }

        case HBOS_SYS_CLOSEDIR: {
            const char *path = (const char *)f->a0;
            vfs_closedir(path);
            return 0;
        }

        case HBOS_SYS_SHMGET: {
            extern int shmget(int, size_t, int);
            int key = (int)f->a0;
            size_t size = (size_t)f->a1;
            int flags = (int)f->a2;
            int ret = shmget(key, size, flags);
            if (ret < 0) return (uint64_t)(-ENOSPC);
            return (uint64_t)ret;
        }

        case HBOS_SYS_SHMAT: {
            extern void *shmat(int, const void *, int);
            int shmid = (int)f->a0;
            const void *shmaddr = (const void *)f->a1;
            int flags = (int)f->a2;
            void *ret = shmat(shmid, shmaddr, flags);
            if (ret == (void *)-1) return (uint64_t)(-EINVAL);
            return (uint64_t)(uintptr_t)ret;
        }

        case HBOS_SYS_SHMDT: {
            extern int shmdt(const void *);
            const void *shmaddr = (const void *)f->a0;
            int ret = shmdt(shmaddr);
            if (ret < 0) return (uint64_t)(-EINVAL);
            return 0;
        }

        case HBOS_SYS_SHMCTL: {
            extern int shmctl(int, int, void *);
            int shmid = (int)f->a0;
            int cmd = (int)f->a1;
            void *buf = (void *)f->a2;
            int ret = shmctl(shmid, cmd, buf);
            if (ret < 0) return (uint64_t)(-EINVAL);
            return 0;
        }

        // ============================================================
        // GUI 窗体画布 (77-83)
        // ============================================================
        case HBOS_SYS_GUI_INFO:
            return (uint64_t)gui_service_canvas_info((int *)f->a0, (int *)f->a1);

        case HBOS_SYS_GUI_CLEAR:
            gui_service_canvas_clear((uint32_t)f->a0);
            return 0;

        case HBOS_SYS_GUI_RECT:
            gui_service_canvas_rect((int)f->a0, (int)f->a1, (int)f->a2,
                                    (int)f->a3, (uint32_t)f->a4);
            return 0;

        case HBOS_SYS_GUI_TEXT:
            gui_service_canvas_text((int)f->a0, (int)f->a1, (const char *)f->a2,
                                    (uint32_t)f->a3, (int)f->a4);
            return 0;

        case HBOS_SYS_GUI_PRESENT:
            gui_service_canvas_present();
            return 0;

        case HBOS_SYS_GUI_POLLKEY:
            return (uint64_t)(long)gui_service_canvas_pollkey();

        case HBOS_SYS_GUI_POLLMOUSE:
            return (uint64_t)(long)gui_service_canvas_pollmouse((int *)f->a0,
                                                               (int *)f->a1);

        // ============================================================
        // 并发窗口服务 (84-91)
        // ============================================================
        case HBOS_SYS_WIN_OPEN: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_open(
                tid, (const char *)f->a0, (int)f->a1, (int)f->a2);
        }
        case HBOS_SYS_WIN_INFO: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_info(
                tid, (int *)f->a0, (int *)f->a1);
        }
        case HBOS_SYS_WIN_CLEAR: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_clear(tid, (uint32_t)f->a0);
            return 0;
        }
        case HBOS_SYS_WIN_FILL: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_fill(tid, (int)f->a0, (int)f->a1,
                                    (int)f->a2, (int)f->a3, (uint32_t)f->a4);
            return 0;
        }
        case HBOS_SYS_WIN_TEXT: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_text(tid, (int)f->a0, (int)f->a1,
                                    (const char *)f->a2, (uint32_t)f->a3);
            return 0;
        }
        case HBOS_SYS_WIN_PRESENT: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_present(tid);
            return 0;
        }
        case HBOS_SYS_WIN_POLL: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_poll(tid, (int *)f->a0);
        }
        case HBOS_SYS_WIN_CLOSE: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_close(tid);
            return 0;
        }

        case HBOS_SYS_DLOPEN: {
            const char *path = (const char *)f->a0;
            if (!path) return (uint64_t)(-EFAULT);
            int fd = open(path, O_RDONLY);
            if (fd < 0) return (uint64_t)(-(int64_t)(errno ? errno : ENOENT));

            uint8_t *buf = (uint8_t *)kmalloc(SYSCALL_EXEC_MAX_SIZE);
            if (!buf) { close(fd); return (uint64_t)(-ENOMEM); }

            size_t size = 0;
            ssize_t n = 0;
            while ((n = read(fd, buf + size, SYSCALL_EXEC_MAX_SIZE - size)) > 0) {
                size += (size_t)n;
                if (size >= SYSCALL_EXEC_MAX_SIZE) break;
            }
            close(fd);

            void *handle = ldso_load(buf, size);
            kfree(buf);
            return (uint64_t)(uintptr_t)handle;
        }

        case HBOS_SYS_DLSYM: {
            void *handle = (void *)f->a0;
            const char *name = (const char *)f->a1;
            void *addr = ldso_dlsym(handle, name);
            return (uint64_t)(uintptr_t)addr;
        }

        case HBOS_SYS_DLCLOSE: {
            void *handle = (void *)f->a0;
            return (uint64_t)(long)ldso_close(handle);
        }

        case HBOS_SYS_HAX_EXISTS: {
            const char *name = (const char *)f->a0;
            if (!name) return 0;
            return hax_app_find(name) ? 1 : 0;
        }

        case HBOS_SYS_WIN2: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            return (uint64_t)(long)gui_service_window_v2(
                tid, (uint32_t)f->a0, (int)f->a1, (void *)f->a2);
        }

        case HBOS_SYS_WIN_BLIT: {
            uint32_t tid = task_current() ? task_current()->id : 0;
            gui_service_window_blit(tid, (int)f->a0, (int)f->a1,
                                    (int)f->a2, (int)f->a3,
                                    (const uint32_t *)f->a4, (int)f->a5);
            return 0;
        }

        case HBOS_SYS_HTTPS_GET: {
            const char *host = (const char *)f->a0;
            const char *path = (const char *)f->a3;
            char *out = (char *)f->a4;
            uint32_t out_cap = (uint32_t)f->a5;
            if (!host || !host[0] || !path || path[0] != '/' || !out ||
                out_cap < 2 || out_cap > SYSCALL_HTTPS_MAX_SIZE)
                return (uint64_t)(-EINVAL);
            uint32_t out_len = 0;
            int status = tls_https_get(
                host, (uint32_t)f->a1, (uint16_t)f->a2, path,
                out, out_cap, &out_len);
            return status < 0 ? (uint64_t)(-EIO) : (uint64_t)out_len;
        }

        case HBOS_SYS_HTTPS_GET_V2: {
            const hbos_https_request_v2_t *request =
                (const hbos_https_request_v2_t *)f->a0;
            if (!request ||
                request->version != HBOS_HTTPS_REQUEST_V2_VERSION ||
                request->ca_pem_length == 0 ||
                request->ca_pem_length > 128U * 1024U ||
                request->output_capacity < 2 ||
                request->output_capacity > SYSCALL_HTTPS_MAX_SIZE)
                return (uint64_t)(-EINVAL);
            uint32_t out_len = 0;
            int status = secure_https_get(request, &out_len);
            return status < 0 ? (uint64_t)(-EIO) : (uint64_t)out_len;
        }

        case HBOS_SYS_POLL: {
            int ret = linux_compat_poll((linux_pollfd_t *)f->a0,
                                        (uint32_t)f->a1, (int)f->a2);
            return finish_syscall(ret);
        }

        case HBOS_SYS_PIPE2: {
            int ret = linux_compat_pipe2((int *)f->a0, (int)f->a1);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EVENTFD2: {
            int ret = linux_compat_eventfd2((uint32_t)f->a0, (int)f->a1);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EPOLL_CREATE1: {
            int ret = linux_compat_epoll_create1((int)f->a0);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EPOLL_CTL: {
            int ret = linux_compat_epoll_ctl(
                (int)f->a0, (int)f->a1, (int)f->a2,
                (const linux_epoll_event_t *)f->a3);
            return finish_syscall(ret);
        }

        case HBOS_SYS_EPOLL_WAIT: {
            int ret = linux_compat_epoll_wait(
                (int)f->a0, (linux_epoll_event_t *)f->a1,
                (int)f->a2, (int)f->a3);
            return finish_syscall(ret);
        }

        case HBOS_SYS_SCHED_YIELD:
            task_yield();
            return 0;

        case HBOS_SYS_GETRANDOM:
            return (uint64_t)linux_compat_getrandom(
                (void *)f->a0, (size_t)f->a1, (unsigned int)f->a2);

        case HBOS_SYS_FUTEX: {
            int ret = linux_compat_futex6((uint32_t *)f->a0, (int)f->a1,
                                          (uint32_t)f->a2,
                                          (const void *)f->a3,
                                          (uint32_t *)f->a4,
                                          (uint32_t)f->a5);
            return finish_syscall(ret);
        }

        case HBOS_SYS_ARCH_PRCTL: {
            const int ARCH_SET_FS = 0x1002;
            const int ARCH_GET_FS = 0x1003;
            int operation = (int)f->a0;
            if (operation == ARCH_SET_FS)
                return task_set_fs_base(f->a1) == 0 ? 0 :
                       (uint64_t)(-EINVAL);
            if (operation == ARCH_GET_FS) {
                uint64_t *output = (uint64_t *)f->a1;
                if (!output) return (uint64_t)(-EFAULT);
                *output = task_get_fs_base();
                return 0;
            }
            return (uint64_t)(-EINVAL);
        }

        case HBOS_SYS_CLONE_THREAD: {
            int tid = task_clone_user_thread(
                (const hbos_clone_request_t *)f->a0);
            return tid < 0 ? (uint64_t)(-EINVAL) : (uint64_t)tid;
        }

        case HBOS_SYS_SET_TID_ADDRESS: {
            int tid = task_set_tid_address((uint32_t *)f->a0);
            return tid < 0 ? (uint64_t)(-EFAULT) : (uint64_t)tid;
        }

        case HBOS_SYS_SET_ROBUST_LIST:
            return task_set_robust_list((void *)f->a0, (size_t)f->a1) == 0 ?
                   0 : (uint64_t)(-EINVAL);

        case HBOS_SYS_GET_ROBUST_LIST:
            return task_get_robust_list((int)f->a0, (void **)f->a1,
                                        (size_t *)f->a2) == 0 ?
                   0 : (uint64_t)(-ESRCH);

        case HBOS_SYS_SOCKETPAIR: {
            int domain = (int)f->a0;
            if (domain != 1) return (uint64_t)(-EAFNOSUPPORT);
            int ret = linux_compat_unix_socketpair(
                (int)f->a1, (int)f->a2, (int *)f->a3);
            return finish_syscall(ret);
        }

        case HBOS_SYS_GETSOCKOPT: {
            int fd = (int)f->a0;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table || fd < 0 ||
                fd >= POSIX_MAX_FDS ||
                !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[fd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_getsockopt(
                    fd, (int)f->a1, (int)f->a2,
                    (void *)f->a3, (uint32_t *)f->a4));
            if ((int)f->a1 != 1 || !f->a3 || !f->a4)
                return (uint64_t)(-ENOPROTOOPT);
            uint32_t *length = (uint32_t *)f->a4;
            if (*length < sizeof(int)) return (uint64_t)(-EINVAL);
            if ((int)f->a2 != 3 && (int)f->a2 != 4)
                return (uint64_t)(-ENOPROTOOPT);
            *(int *)f->a3 = (int)f->a2 == 3 ? 1 : 0;
            *length = sizeof(int);
            return 0;
        }

        case HBOS_SYS_SETSOCKOPT: {
            int fd = (int)f->a0;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table || fd < 0 ||
                fd >= POSIX_MAX_FDS ||
                !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[fd].type == FD_UNIX)
                return finish_syscall(linux_compat_unix_setsockopt(
                    fd, (int)f->a1, (int)f->a2,
                    (const void *)f->a3, (uint32_t)f->a4));
            return 0;
        }

        case HBOS_SYS_SHUTDOWN: {
            int fd = (int)f->a0;
            task_t *cur = task_current();
            if (!cur || !cur->fd_table || fd < 0 ||
                fd >= POSIX_MAX_FDS ||
                !cur->fd_table->entries[fd].used)
                return (uint64_t)(-EBADF);
            if (cur->fd_table->entries[fd].type == FD_UNIX)
                return finish_syscall(
                    linux_compat_unix_shutdown(fd, (int)f->a1));
            return 0;
        }

        case HBOS_SYS_MEMFD_CREATE: {
            int fd = linux_compat_memfd_create(
                (const char *)f->a0, (unsigned int)f->a1);
            return fd < 0
                ? (uint64_t)(-(errno > 0 ? errno : EINVAL))
                : (uint64_t)fd;
        }

        case HBOS_SYS_SENDMSG:
            return linux_message_io(
                (int)f->a0, (linux_x86_msghdr_t *)f->a1,
                (int)f->a2, 1);

        case HBOS_SYS_RECVMSG:
            return linux_message_io(
                (int)f->a0, (linux_x86_msghdr_t *)f->a1,
                (int)f->a2, 0);

        default:
            return (uint64_t)(-ENOSYS);
    }
}
