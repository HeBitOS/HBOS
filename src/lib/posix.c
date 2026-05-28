#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../errno.h"
#include "../fcntl.h"
#include "../stdlib.h"
#include "../string.h"
#include "../sys/stat.h"
#include "../unistd.h"
#include "../core/heap.h"
#include "../core/task.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../vfs.h"

static int g_errno;

int *__errno_location(void) {
    return &g_errno;
}

static int set_errno(int e) {
    errno = e;
    return -1;
}

static fd_entry_t *fd_table(void) {
    task_t *task = task_current();
    return task ? task->fds : NULL;
}

static int fd_alloc(vfs_node_t *node, int flags) {
    fd_entry_t *fds = fd_table();
    if (!fds) return set_errno(EBADF);
    for (int fd = 3; fd < POSIX_MAX_FDS; fd++) {
        if (fds[fd].used) continue;
        fds[fd].used = true;
        fds[fd].node = node;
        fds[fd].flags = flags;
        fds[fd].offset = (flags & O_APPEND) ? node->size : 0;
        return fd;
    }
    return set_errno(EMFILE);
}

static fd_entry_t *fd_get(int fd) {
    fd_entry_t *fds = fd_table();
    if (!fds || fd < 3 || fd >= POSIX_MAX_FDS || !fds[fd].used) return NULL;
    return &fds[fd];
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!buf && count) return set_errno(EFAULT);
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        console_write((const char *)buf, count);
        return (ssize_t)count;
    }

    fd_entry_t *ent = fd_get(fd);
    if (!ent) return set_errno(EBADF);
    if ((ent->flags & O_ACCMODE) == O_RDONLY) return set_errno(EBADF);

    uint32_t offset = (ent->flags & O_APPEND) ? ent->node->size : ent->offset;
    int written = vfs_write(ent->node, offset, buf, (uint32_t)count);
    if (written < 0) return set_errno(ENOSPC);
    ent->offset = offset + (uint32_t)written;
    return (ssize_t)count;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!buf && count) return set_errno(EFAULT);
    if (fd == STDIN_FILENO) {
        char *out = buf;
        size_t i = 0;
        while (i < count) {
            int key = kb_get_key();
            if (key < 0 || key > 0xff) continue;
            out[i++] = (char)key;
            if (key == '\n') break;
        }
        return (ssize_t)i;
    }

    fd_entry_t *ent = fd_get(fd);
    if (!ent) return set_errno(EBADF);
    if ((ent->flags & O_ACCMODE) == O_WRONLY) return set_errno(EBADF);

    int got_i = vfs_read(ent->node, ent->offset, buf, (uint32_t)count);
    if (got_i < 0) return set_errno(EIO);
    uint32_t got = (uint32_t)got_i;
    ent->offset += got;
    return (ssize_t)got;
}

int close(int fd) {
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) return 0;
    fd_entry_t *ent = fd_get(fd);
    if (ent) {
        ent->used = false;
        ent->node = NULL;
        ent->offset = 0;
        ent->flags = 0;
        return 0;
    }
    return set_errno(EBADF);
}

int isatty(int fd) {
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) return 1;
    return set_errno(EBADF);
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) return set_errno(ESPIPE);
    fd_entry_t *ent = fd_get(fd);
    if (!ent) return set_errno(EBADF);

    int64_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = ent->offset;
    else if (whence == SEEK_END) base = ent->node->size;
    else return set_errno(EINVAL);

    int64_t next = base + offset;
    if (next < 0 || next > (int64_t)ent->node->capacity) return set_errno(EINVAL);
    ent->offset = (uint32_t)next;
    return (off_t)ent->offset;
}

int open(const char *path, int flags, ...) {
    if (!path) return set_errno(EFAULT);
    if ((flags & O_ACCMODE) != O_RDONLY &&
        (flags & O_ACCMODE) != O_WRONLY &&
        (flags & O_ACCMODE) != O_RDWR) {
        return set_errno(EINVAL);
    }

    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        if (!(flags & O_CREAT)) return set_errno(ENOENT);
        node = vfs_create(path);
        if (!node) return set_errno(ENOSPC);
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return set_errno(EEXIST);
    }

    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY) {
        if (vfs_truncate(node) < 0) return set_errno(EIO);
    }

    return fd_alloc(node, flags);
}

int fstat(int fd, struct stat *st) {
    if (!st) return set_errno(EFAULT);
    memset(st, 0, sizeof(*st));

    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        st->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        st->st_nlink = 1;
        return 0;
    }

    fd_entry_t *ent = fd_get(fd);
    if (!ent) return set_errno(EBADF);
    st->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    st->st_nlink = 1;
    st->st_size = (off_t)ent->node->size;
    return 0;
}

int stat(const char *path, struct stat *st) {
    if (!path || !st) return set_errno(EFAULT);
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return set_errno(ENOENT);

    memset(st, 0, sizeof(*st));
    st->st_mode = (node->type == VFS_NODE_DIR ? S_IFDIR : S_IFREG) | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_nlink = 1;
    st->st_size = (off_t)node->size;
    return 0;
}

int unlink(const char *path) {
    if (!path) return set_errno(EFAULT);
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return set_errno(ENOENT);

    fd_entry_t *fds = fd_table();
    if (!fds) return set_errno(EBADF);
    for (int fd = 3; fd < POSIX_MAX_FDS; fd++) {
        if (fds[fd].used && fds[fd].node == node)
            return set_errno(EBUSY);
    }

    if (vfs_unlink(path) < 0) return set_errno(ENOENT);
    return 0;
}

pid_t getpid(void) {
    return (pid_t)task_get_id();
}

pid_t getppid(void) {
    return 0;
}

#define POSIX_BRK_SIZE (64 * 1024)
static uint8_t brk_pool[POSIX_BRK_SIZE] __attribute__((aligned(16)));
static uintptr_t brk_cur;

void *sbrk(intptr_t increment) {
    if (brk_cur == 0) brk_cur = (uintptr_t)brk_pool;
    uintptr_t base = (uintptr_t)brk_pool;
    uintptr_t end = base + POSIX_BRK_SIZE;
    uintptr_t old = brk_cur;

    if (increment < 0 && (uintptr_t)(-increment) > old - base) {
        errno = ENOMEM;
        return (void *)-1;
    }
    if (increment > 0 && (uintptr_t)increment > end - old) {
        errno = ENOMEM;
        return (void *)-1;
    }

    brk_cur = old + increment;
    return (void *)old;
}

void *_sbrk(intptr_t increment) {
    return sbrk(increment);
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int usleep(useconds_t usec) {
    const uint64_t tsc_per_second = 1000000000ULL;
    uint64_t ticks = (tsc_per_second / 1000000ULL) * usec;
    uint64_t start = rdtsc();
    while (rdtsc() - start < ticks) {
        task_yield();
    }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    for (unsigned int i = 0; i < seconds; i++) usleep(1000000);
    return 0;
}

void _exit(int status) {
    (void)status;
    task_exit();
    while (1) __asm__ volatile("hlt");
}

void exit(int status) {
    _exit(status);
}

void *malloc(size_t size) {
    return kmalloc(size);
}

void *calloc(size_t nmemb, size_t size) {
    return kcalloc(nmemb, size);
}

void *realloc(void *ptr, size_t size) {
    return krealloc(ptr, size);
}

void free(void *ptr) {
    kfree(ptr);
}

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long sign = 1;
    long value = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
        s++;
    } else if (base == 0) {
        base = 10;
    }

    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        value = value * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return value * sign;
}
