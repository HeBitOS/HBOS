#include "unistd.h"
#include "syscall.h"
#include "errno.h"

unsigned int sleep(unsigned int seconds) {
    long ret = __syscall1(HBOS_SYS_SLEEP, (long)seconds);
    return ret < 0 ? (unsigned int)seconds : 0;
}

int usleep(unsigned int useconds) {
    long ret = __syscall1(HBOS_SYS_USLEEP, (long)useconds);
    return (int)__syscall_errno(ret);
}

pid_t getpid(void) {
    return (pid_t)__syscall1(HBOS_SYS_GETPID, 0);
}

char *getcwd(char *buf, size_t size) {
    long ret = __syscall3(HBOS_SYS_GETCWD, (long)buf, (long)size, 0);
    if (__syscall_errno(ret) < 0) return 0;
    return buf;
}

int unlink(const char *path) {
    long ret = __syscall1(HBOS_SYS_UNLINK, (long)path);
    return (int)__syscall_errno(ret);
}

ssize_t read(int fd, void *buf, size_t count) {
    long ret = __syscall3(HBOS_SYS_READ, fd, (long)buf, (long)count);
    return (ssize_t)__syscall_errno(ret);
}

ssize_t write(int fd, const void *buf, size_t count) {
    long ret = __syscall3(HBOS_SYS_WRITE, fd, (long)buf, (long)count);
    return (ssize_t)__syscall_errno(ret);
}

off_t lseek(int fd, off_t offset, int whence) {
    long ret = __syscall3(HBOS_SYS_LSEEK, fd, offset, whence);
    return (off_t)__syscall_errno(ret);
}

int rmdir(const char *path) {
    long ret = __syscall1(HBOS_SYS_RMDIR, (long)path);
    return (int)__syscall_errno(ret);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    long ret = __syscall3(HBOS_SYS_EXECVE, (long)path, (long)argv, (long)envp);
    /* On success this never returns (src/syscall.c's HBOS_SYS_EXECVE calls
     * elf64_load_and_exec(), which terminates the calling task) — only
     * reachable here on failure. */
    return (int)__syscall_errno(ret);
}

/* HBOS has no environment variables (getenv() always returns NULL — see
 * stdlib.c), so there's no real $PATH to search. If `file` already has a
 * '/' it's used as-is; otherwise this tries /bin/<file> (HBOS's one
 * conventional program directory, see ramfs's default layout) before
 * falling back to a plain cwd-relative lookup. */
int execvp(const char *file, char *const argv[]) {
    if (!file) return -1;
    for (const char *p = file; *p; p++) {
        if (*p == '/') return execve(file, argv, 0);
    }
    char buf[256];
    int n = 0;
    const char *prefix = "/bin/";
    while (prefix[n] && n + 1 < (int)sizeof(buf)) { buf[n] = prefix[n]; n++; }
    int i = 0;
    while (file[i] && n + 1 < (int)sizeof(buf)) { buf[n++] = file[i++]; }
    buf[n] = 0;
    execve(buf, argv, 0);
    return execve(file, argv, 0);
}

int access(const char *path, int mode) {
    long ret = __syscall3(HBOS_SYS_ACCESS, (long)path, mode, 0);
    return (int)__syscall_errno(ret);
}

int isatty(int fd) {
    long ret = __syscall1(HBOS_SYS_ISATTY, fd);
    return ret > 0 ? 1 : 0;
}

int rename(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    errno = EXDEV;
    return -1;
}