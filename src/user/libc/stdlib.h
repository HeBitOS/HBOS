#ifndef HBOS_USER_LIBC_STDLIB_H
#define HBOS_USER_LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);

#define PATH_MAX 256
char *realpath(const char *path, char *resolved_path);

void  abort(void);
void  exit(int status) __attribute__((noreturn));

int   rand(void);
void  srand(unsigned int seed);

int   abs(int n);
long  labs(long n);

void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

/* HBOS has no environment-variable subsystem; always returns NULL, meaning
 * "not set" to callers (e.g. TinyCC treats a NULL TCC_LIBRARY_PATH as
 * "use compiled-in default"). */
char *getenv(const char *name);

#endif
#define alloca(size) __builtin_alloca(size)
