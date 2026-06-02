#ifndef HBOS_TLS_H
#define HBOS_TLS_H

#include <stdint.h>

typedef enum {
    TLS_STATUS_UNSUPPORTED = -2,
    TLS_STATUS_ERROR = -1,
    TLS_STATUS_OK = 0
} tls_status_t;

const char *tls_last_error(void);
int tls_https_get(const char *host, uint32_t ip, uint16_t port, const char *path,
                  char *out, uint32_t out_cap, uint32_t *out_len);

#endif
