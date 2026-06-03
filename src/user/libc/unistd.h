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

unsigned int sleep(unsigned int seconds);
int          usleep(unsigned int useconds);
pid_t        getpid(void);
char        *getcwd(char *buf, size_t size);

#endif