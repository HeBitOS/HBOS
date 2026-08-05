#ifndef HBOS_USER_LIBC_SYS_RANDOM_H
#define HBOS_USER_LIBC_SYS_RANDOM_H

#include <stddef.h>

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002

long getrandom(void *buffer, size_t length, unsigned int flags);

#endif
