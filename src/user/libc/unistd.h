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
int          unlink(const char *path);

ssize_t      read(int fd, void *buf, size_t count);
ssize_t      write(int fd, const void *buf, size_t count);
off_t        lseek(int fd, off_t offset, int whence);
int          close(int fd);
int          rmdir(const char *path);

/* Real exec — see unistd.c: on success this never returns (the calling
 * task is replaced/terminated by the kernel), -1 on failure. */
int          execve(const char *path, char *const argv[], char *const envp[]);
int          execvp(const char *file, char *const argv[]);

int          access(const char *path, int mode);
int          isatty(int fd);

#endif