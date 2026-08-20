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

uid_t getuid(void) {
    return (uid_t)__syscall1(HBOS_SYS_GETUID, 0);
}

uid_t geteuid(void) {
    return (uid_t)__syscall1(HBOS_SYS_GETEUID, 0);
}

gid_t getgid(void) {
    return (gid_t)__syscall1(HBOS_SYS_GETGID, 0);
}

gid_t getegid(void) {
    return (gid_t)__syscall1(HBOS_SYS_GETEGID, 0);
}

int setuid(uid_t uid) {
    return (int)__syscall_errno(__syscall1(HBOS_SYS_SETUID, (long)uid));
}

int setgid(gid_t gid) {
    return (int)__syscall_errno(__syscall1(HBOS_SYS_SETGID, (long)gid));
}

int getgroups(int size, gid_t list[]) {
    return (int)__syscall_errno(__syscall3(HBOS_SYS_GETGROUPS,
                                           size, (long)list, 0));
}

int setgroups(int size, const gid_t list[]) {
    return (int)__syscall_errno(__syscall3(HBOS_SYS_SETGROUPS,
                                           size, (long)list, 0));
}

pid_t getpgid(pid_t pid) {
    return (pid_t)__syscall_errno(__syscall1(HBOS_SYS_GETPGID, (long)pid));
}

char *getcwd(char *buf, size_t size) {
    long ret = __syscall3(HBOS_SYS_GETCWD, (long)buf, (long)size, 0);
    if (__syscall_errno(ret) < 0) return 0;
    return buf;
}

int chdir(const char *path) {
    long ret = __syscall1(HBOS_SYS_CHDIR, (long)path);
    return (int)__syscall_errno(ret);
}

int unlink(const char *path) {
    long ret = __syscall1(HBOS_SYS_UNLINK, (long)path);
    return (int)__syscall_errno(ret);
}

int symlink(const char *target, const char *linkpath) {
    long ret = __syscall3(HBOS_SYS_SYMLINK, (long)target,
                          (long)linkpath, 0);
    return (int)__syscall_errno(ret);
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    long ret = __syscall3(HBOS_SYS_READLINK, (long)path,
                          (long)buf, (long)bufsiz);
    return (ssize_t)__syscall_errno(ret);
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

int ftruncate(int fd, off_t length) {
    long ret = __syscall3(HBOS_SYS_FTRUNCATE, fd, length, 0);
    return (int)__syscall_errno(ret);
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
    long ret = __syscall3(HBOS_SYS_RENAME, (long)oldpath,
                          (long)newpath, 0);
    return (int)__syscall_errno(ret);
}

int hax_app_exists(const char *name) {
    long ret = __syscall1(HBOS_SYS_HAX_EXISTS, (long)name);
    return ret > 0 ? 1 : 0;
}
