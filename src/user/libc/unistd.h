#ifndef HBOS_USER_LIBC_UNISTD_H
#define HBOS_USER_LIBC_UNISTD_H

#include <stddef.h>

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef int pid_t;
typedef long ssize_t;
typedef long off_t;

unsigned int sleep(unsigned int seconds);
int          usleep(unsigned int useconds);
pid_t        getpid(void);
char        *getcwd(char *buf, size_t size);
int          chdir(const char *path);
int          unlink(const char *path);
int          symlink(const char *target, const char *linkpath);
ssize_t      readlink(const char *path, char *buf, size_t bufsiz);

ssize_t      read(int fd, void *buf, size_t count);
ssize_t      write(int fd, const void *buf, size_t count);
off_t        lseek(int fd, off_t offset, int whence);
int          ftruncate(int fd, off_t length);
int          close(int fd);
int          rmdir(const char *path);
int          pipe2(int pipefd[2], int flags);

/* Real exec — see unistd.c: on success this never returns (the calling
 * task is replaced/terminated by the kernel), -1 on failure. */
int          execve(const char *path, char *const argv[], char *const envp[]);
int          execvp(const char *file, char *const argv[]);

int          access(const char *path, int mode);
int          isatty(int fd);

/* Same-filesystem file rename.  ramfs/HBFS relink metadata in place;
 * ext2/FAT32 currently use the filesystem copy/delete fallback. */
int          rename(const char *oldpath, const char *newpath);

/* HBOS-specific: HBOS's "programs" are .hax apps looked up by name in an
 * embedded kernel registry (src/user/hax.c's hax_app_find()), not files in
 * the ramfs. There is no /bin directory listing them, so PATH-searching
 * tools (which, executable_exists()) must ask the kernel directly whether
 * a name is a known app rather than stat()'ing a path. Returns 1 if found,
 * 0 otherwise. */
int          hax_app_exists(const char *name);

#endif
