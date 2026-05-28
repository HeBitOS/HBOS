#ifndef HBOS_STDLIB_H
#define HBOS_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void exit(int status);
int atoi(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);

#endif
