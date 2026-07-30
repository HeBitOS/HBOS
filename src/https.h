#ifndef HBOS_HTTPS_H
#define HBOS_HTTPS_H

#include <stdint.h>

#define HBOS_HTTPS_REQUEST_V2_VERSION 2U

typedef struct {
    uint32_t version;
    uint32_t flags;
    const char *host;
    const char *path;
    const char *ca_pem;
    char *output;
    uint32_t ca_pem_length;
    uint32_t output_capacity;
    uint16_t port;
    uint16_t reserved;
} hbos_https_request_v2_t;

/**
 * Perform an HTTPS GET with mandatory CA-chain, validity and hostname checks.
 * The host may be a DNS name, an IPv4 literal, or an unbracketed IPv6 literal.
 */
int secure_https_get(const hbos_https_request_v2_t *request,
                     uint32_t *output_length);
const char *secure_https_last_error(void);

#endif
