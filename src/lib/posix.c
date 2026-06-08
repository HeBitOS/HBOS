#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../errno.h"
#include "../fcntl.h"
#include "../fd.h"
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
static char g_posix_cwd[256] = "/";

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
        fds[fd].type = FD_FILE;
        fds[fd].pipe = NULL;
        return fd;
    }
    return set_errno(EMFILE);
}

static int resolve_user_path(const char *path, char out[256]) {
    if (!path) return set_errno(EFAULT);
    if (vfs_resolve_path(g_posix_cwd, path, out, 256) < 0) return set_errno(EINVAL);
    return 0;
}

static fd_entry_t *fd_get(int fd) {
    fd_entry_t *fds = fd_table();
    if (!fds || fd < 3 || fd >= POSIX_MAX_FDS || !fds[fd].used) return NULL;
    return &fds[fd];
}

static fd_entry_t *fd_get_redirectable(int fd) {
    fd_entry_t *fds = fd_table();
    if (!fds || fd < 0 || fd >= POSIX_MAX_FDS || !fds[fd].used) return NULL;
    return &fds[fd];
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (!buf && count) return set_errno(EFAULT);
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        fd_entry_t *redir = fd_get_redirectable(fd);
        if (redir) {
            if (redir->type == FD_PIPE && redir->pipe) {
                pipe_t *p = redir->pipe;
                uint32_t written = 0;
                while (written < count) {
                    if (p->count >= PIPE_BUF_SIZE) {
                        task_yield();
                        continue;
                    }
                    p->buf[p->write_pos] = ((const uint8_t *)buf)[written];
                    p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
                    p->count++;
                    written++;
                }
                return (ssize_t)written;
            }
            uint32_t offset = (redir->flags & O_APPEND) ? redir->node->size : redir->offset;
            int written = vfs_write(redir->node, offset, buf, (uint32_t)count);
            if (written < 0) return set_errno(ENOSPC);
            redir->offset = offset + (uint32_t)written;
            return (ssize_t)written;
        }
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        console_write((const char *)buf, count);
        return (ssize_t)count;
    }

    fd_entry_t *ent = fd_get(fd);
    if (!ent) return set_errno(EBADF);

    if (ent->type == FD_PIPE && ent->pipe) {
        pipe_t *p = ent->pipe;
        uint32_t written = 0;
        while (written < count) {
            if (p->count >= PIPE_BUF_SIZE) {
                task_yield();
                continue;
            }
            p->buf[p->write_pos] = ((const uint8_t *)buf)[written];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            p->count++;
            written++;
        }
        return (ssize_t)written;
    }

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
        fd_entry_t *redir = fd_get_redirectable(fd);
        if (redir) {
            if (redir->type == FD_PIPE && redir->pipe) {
                pipe_t *p = redir->pipe;
                uint32_t got = 0;
                while (got < count) {
                    if (p->count == 0) {
                        if (p->ref_count < 2) break;
                        task_yield();
                        continue;
                    }
                    ((uint8_t *)buf)[got] = p->buf[p->read_pos];
                    p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
                    p->count--;
                    got++;
                    if (p->count == 0) break;
                }
                return (ssize_t)got;
            }
            int got_i = vfs_read(redir->node, redir->offset, buf, (uint32_t)count);
            if (got_i < 0) return set_errno(EIO);
            uint32_t got = (uint32_t)got_i;
            redir->offset += got;
            return (ssize_t)got;
        }
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

    if (ent->type == FD_PIPE && ent->pipe) {
        pipe_t *p = ent->pipe;
        uint32_t got = 0;
        while (got < count) {
            if (p->count == 0) {
                if (p->ref_count < 2) break;
                task_yield();
                continue;
            }
            ((uint8_t *)buf)[got] = p->buf[p->read_pos];
            p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
            p->count--;
            got++;
            if (p->count == 0) break;
        }
        return (ssize_t)got;
    }

    if ((ent->flags & O_ACCMODE) == O_WRONLY) return set_errno(EBADF);

    int got_i = vfs_read(ent->node, ent->offset, buf, (uint32_t)count);
    if (got_i < 0) return set_errno(EIO);
    uint32_t got = (uint32_t)got_i;
    ent->offset += got;
    return (ssize_t)got;
}

int close(int fd) {
    if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) {
        fd_entry_t *redir = fd_get_redirectable(fd);
        if (redir) {
            if (redir->type == FD_PIPE && redir->pipe) {
                pipe_t *p = redir->pipe;
                p->ref_count--;
                if (p->ref_count <= 0) kfree(p);
            }
            redir->used = false;
            redir->node = NULL;
            redir->offset = 0;
            redir->flags = 0;
            redir->pipe = NULL;
        }
        return 0;
    }
    fd_entry_t *ent = fd_get(fd);
    if (ent) {
        if (ent->type == FD_PIPE && ent->pipe) {
            pipe_t *p = ent->pipe;
            p->ref_count--;
            if (p->ref_count <= 0) kfree(p);
        }
        ent->used = false;
        ent->node = NULL;
        ent->offset = 0;
        ent->flags = 0;
        ent->pipe = NULL;
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
    char full[256];
    if (resolve_user_path(path, full) < 0) return -1;
    if ((flags & O_ACCMODE) != O_RDONLY &&
        (flags & O_ACCMODE) != O_WRONLY &&
        (flags & O_ACCMODE) != O_RDWR) {
        return set_errno(EINVAL);
    }

    vfs_node_t *node = vfs_lookup(full);
    if (!node) {
        if (!(flags & O_CREAT)) return set_errno(ENOENT);
        node = vfs_create(full);
        if (!node) return set_errno(ENOSPC);
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return set_errno(EEXIST);
    }
    if ((flags & O_DIRECTORY) && node->type != VFS_NODE_DIR) return set_errno(ENOTDIR);
    if (node->type == VFS_NODE_DIR && (flags & O_ACCMODE) != O_RDONLY) return set_errno(EISDIR);

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
    st->st_mode = (ent->node->type == VFS_NODE_DIR ? S_IFDIR :
                   ent->node->type == VFS_NODE_CHARDEV ? S_IFCHR : S_IFREG) |
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
    st->st_nlink = 1;
    st->st_size = (off_t)ent->node->size;
    return 0;
}

int stat(const char *path, struct stat *st) {
    if (!st) return set_errno(EFAULT);
    char full[256];
    if (resolve_user_path(path, full) < 0) return -1;
    vfs_node_t *node = vfs_lookup(full);
    if (!node) return set_errno(ENOENT);

    memset(st, 0, sizeof(*st));
    st->st_mode = (node->type == VFS_NODE_DIR ? S_IFDIR : S_IFREG) | S_IRUSR | S_IRGRP | S_IROTH;
    st->st_nlink = 1;
    st->st_size = (off_t)node->size;
    return 0;
}

int unlink(const char *path) {
    char full[256];
    if (resolve_user_path(path, full) < 0) return -1;
    vfs_node_t *node = vfs_lookup(full);
    if (!node) return set_errno(ENOENT);
    if (node->type == VFS_NODE_DIR) return set_errno(EISDIR);

    fd_entry_t *fds = fd_table();
    if (!fds) return set_errno(EBADF);
    for (int fd = 3; fd < POSIX_MAX_FDS; fd++) {
        if (fds[fd].used && fds[fd].node == node)
            return set_errno(EBUSY);
    }

    if (vfs_unlink(full) < 0) return set_errno(ENOENT);
    return 0;
}

int access(const char *path, int mode) {
    if (mode & ~(F_OK | R_OK | W_OK | X_OK)) return set_errno(EINVAL);
    char full[256];
    if (resolve_user_path(path, full) < 0) return -1;
    vfs_node_t *node = vfs_lookup(full);
    if (!node) return set_errno(ENOENT);
    return 0;
}

int ftruncate(int fd, off_t length) {
    if (length != 0) return set_errno(EINVAL);
    fd_entry_t *ent = fd_get(fd);
    if (!ent) return set_errno(EBADF);
    if ((ent->flags & O_ACCMODE) == O_RDONLY) return set_errno(EBADF);
    if (vfs_truncate(ent->node) < 0) return set_errno(EIO);
    if (ent->offset > ent->node->size) ent->offset = ent->node->size;
    return 0;
}

char *getcwd(char *buf, size_t size) {
    size_t len;
    if (!buf || size == 0) {
        set_errno(EFAULT);
        return NULL;
    }
    len = strlen(g_posix_cwd);
    if (len + 1 > size) {
        set_errno(ERANGE);
        return NULL;
    }
    strcpy(buf, g_posix_cwd);
    return buf;
}

int chdir(const char *path) {
    char full[256];
    if (resolve_user_path(path, full) < 0) return -1;
    vfs_node_t *node = vfs_lookup(full);
    if (!node) return set_errno(ENOENT);
    if (node->type != VFS_NODE_DIR) return set_errno(ENOTDIR);
    strcpy(g_posix_cwd, full);
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    char full[256];
    if (resolve_user_path(path, full) < 0) return -1;
    if (vfs_lookup(full)) return set_errno(EEXIST);
    if (vfs_mkdir(full) < 0) return set_errno(ENOSPC);
    return 0;
}

int rmdir(const char *path) {
    char full[256];
    if (resolve_user_path(path, full) < 0) return -1;
    vfs_node_t *node = vfs_lookup(full);
    if (!node) return set_errno(ENOENT);
    if (node->type != VFS_NODE_DIR) return set_errno(ENOTDIR);
    if (vfs_rmdir(full) < 0) return set_errno(ENOTEMPTY);
    return 0;
}

int symlink(const char *target, const char *linkpath) {
    (void)target;
    (void)linkpath;
    return set_errno(ENOSYS);
}

int chmod(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    return set_errno(ENOSYS);
}

int chown(const char *path, uid_t uid, gid_t gid) {
    (void)path;
    (void)uid;
    (void)gid;
    return set_errno(ENOSYS);
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path;
    (void)buf;
    (void)bufsiz;
    return set_errno(ENOSYS);
}

pid_t getpid(void) {
    return (pid_t)task_get_id();
}

pid_t getppid(void) {
    return 0;
}

int pipe(int pipefd[2]) {
    if (!pipefd) return set_errno(EFAULT);
    fd_entry_t *fds = fd_table();
    if (!fds) return set_errno(EBADF);

    int r_fd = -1, w_fd = -1;
    for (int i = 3; i < POSIX_MAX_FDS; i++) {
        if (!fds[i].used) { r_fd = i; break; }
    }
    if (r_fd < 0) return set_errno(EMFILE);
    for (int i = r_fd + 1; i < POSIX_MAX_FDS; i++) {
        if (!fds[i].used) { w_fd = i; break; }
    }
    if (w_fd < 0) return set_errno(EMFILE);

    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return set_errno(ENOMEM);
    memset(p, 0, sizeof(pipe_t));
    p->ref_count = 2;

    fds[r_fd].used = true;
    fds[r_fd].type = FD_PIPE;
    fds[r_fd].node = NULL;
    fds[r_fd].offset = 0;
    fds[r_fd].flags = O_RDONLY;
    fds[r_fd].pipe = p;

    fds[w_fd].used = true;
    fds[w_fd].type = FD_PIPE;
    fds[w_fd].node = NULL;
    fds[w_fd].offset = 0;
    fds[w_fd].flags = O_WRONLY;
    fds[w_fd].pipe = p;

    pipefd[0] = r_fd;
    pipefd[1] = w_fd;
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
