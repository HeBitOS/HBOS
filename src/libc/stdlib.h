/**
 * @file    stdlib.h
 * @brief   HBOS 标准库 — 对标 ANSI C <stdlib.h>
 */

#ifndef HBOS_LIBC_STDLIB_H
#define HBOS_LIBC_STDLIB_H

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
long long atoll(const char *s);
double atof(const char *s);
long  strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);

void  abort(void);
void  exit(int status);
int   atexit(void (*func)(void));

int   system(const char *cmd);
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);

int   rand(void);
void  srand(unsigned int seed);

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *));
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

int   abs(int n);
long  labs(long n);

#endif