#ifndef HBOS_MBEDTLS_PORT_H
#define HBOS_MBEDTLS_PORT_H

#include <stddef.h>
#include <stdint.h>

void *hbos_mbedtls_calloc(size_t count, size_t size);
void hbos_mbedtls_free(void *pointer);
int64_t hbos_mbedtls_time(int64_t *result);
int hbos_mbedtls_snprintf(char *buffer, size_t size, const char *format, ...);

#endif
