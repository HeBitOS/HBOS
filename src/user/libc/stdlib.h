#ifndef HBOS_USER_LIBC_STDLIB_H
#define HBOS_USER_LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 32767

extern char **environ;

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);

void  abort(void);
void  exit(int status);

int   rand(void);
void  srand(unsigned int seed);

int   abs(int n);
long  labs(long n);

char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   putenv(char *string);
int   unsetenv(const char *name);

#endif