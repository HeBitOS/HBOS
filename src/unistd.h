#ifndef HBOS_UNISTD_H
#define HBOS_UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include "sys/types.h"

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int unlink(const char *path);
off_t lseek(int fd, off_t offset, int whence);
int isatty(int fd);
pid_t getpid(void);
pid_t getppid(void);
void *_sbrk(intptr_t increment);
void *sbrk(intptr_t increment);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);
void _exit(int status);

#endif
