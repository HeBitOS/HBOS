#include "https.h"

#include "core/heap.h"
#include "net.h"
#include "net6.h"
#include "net/mbedtls_port.h"
#include "string.h"

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

typedef enum {
    SECURE_TRANSPORT_IPV4,
    SECURE_TRANSPORT_IPV6
} secure_transport_kind_t;

typedef struct {
    secure_transport_kind_t kind;
    int active;
    union {
        net_tcp_conn_t ipv4;
        net6_tcp_conn_t ipv6;
    } connection;
} secure_transport_t;

static const char *last_error = "not initialized";
static int psa_initialized;

const char *secure_https_last_error(void) {
    return last_error;
}

static void set_error(const char *message) {
    last_error = message;
}

static int host_is_ipv6(const char *host) {
    return host && strchr(host, ':') != NULL;
}

static int transport_connect(secure_transport_t *transport,
                             const char *host, uint16_t port) {
    memset(transport, 0, sizeof(*transport));
    if (host_is_ipv6(host)) {
        transport->kind = SECURE_TRANSPORT_IPV6;
        int status = net6_tcp_connect(host, port,
                                      &transport->connection.ipv6, 10000);
        transport->active = status == 0;
        return status;
    }

    uint8_t address6[16];
    if (net_dns_resolve_ipv6(host, address6) == 0) {
        transport->kind = SECURE_TRANSPORT_IPV6;
        int status =
            net6_tcp_connect_address(address6, port,
                                     &transport->connection.ipv6, 10000);
        if (status == 0) {
            transport->active = 1;
            return 0;
        }
    }

    uint32_t address = net_parse_ipv4(host);
    if (!address && net_dns_resolve(host, &address) < 0) return -1;
    transport->kind = SECURE_TRANSPORT_IPV4;
    int status =
        net_tcp_connect(address, port, &transport->connection.ipv4);
    transport->active = status == 0;
    return status;
}

static int transport_send(void *context, const unsigned char *buffer,
                          size_t length) {
    secure_transport_t *transport = context;
    if (length > UINT32_MAX) return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    if (transport->kind == SECURE_TRANSPORT_IPV6)
        return net6_tcp_send(&transport->connection.ipv6, buffer,
                             (uint32_t)length, 10000);
    return net_tcp_send(&transport->connection.ipv4, buffer,
                        (uint32_t)length) == 0
         ? (int)length : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int transport_recv(void *context, unsigned char *buffer,
                          size_t length) {
    secure_transport_t *transport = context;
    if (length > UINT32_MAX) length = UINT32_MAX;
    if (transport->kind == SECURE_TRANSPORT_IPV6)
        return net6_tcp_recv(&transport->connection.ipv6, buffer,
                             (uint32_t)length, 10000);

    uint32_t received = 0;
    if (net_tcp_recv(&transport->connection.ipv4, buffer, (uint32_t)length,
                     &received, 80000) < 0)
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)received;
}

static void transport_close(secure_transport_t *transport) {
    if (transport->kind == SECURE_TRANSPORT_IPV6)
        net6_tcp_close(&transport->connection.ipv6);
    else
        net_tcp_close(&transport->connection.ipv4);
    transport->active = 0;
}

static int write_all(mbedtls_ssl_context *ssl, const uint8_t *data,
                     uint32_t length) {
    uint32_t offset = 0;
    while (offset < length) {
        int status = mbedtls_ssl_write(ssl, data + offset, length - offset);
        if (status == MBEDTLS_ERR_SSL_WANT_READ ||
            status == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (status <= 0) return -1;
        offset += (uint32_t)status;
    }
    return 0;
}

int secure_https_get(const hbos_https_request_v2_t *request,
                     uint32_t *output_length) {
    if (output_length) *output_length = 0;
    if (!request || request->version != HBOS_HTTPS_REQUEST_V2_VERSION ||
        !request->host || !request->host[0] || !request->path ||
        request->path[0] != '/' || !request->ca_pem ||
        !request->ca_pem_length || !request->output ||
        request->output_capacity < 2 || !request->port || !output_length) {
        set_error("invalid HTTPS v2 request");
        return -1;
    }

    if (!psa_initialized) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            set_error("PSA crypto initialization failed");
            return -1;
        }
        psa_initialized = 1;
    }

    char *ca_copy = kmalloc((size_t)request->ca_pem_length + 1U);
    if (!ca_copy) {
        set_error("CA allocation failed");
        return -1;
    }
    memcpy(ca_copy, request->ca_pem, request->ca_pem_length);
    ca_copy[request->ca_pem_length] = '\0';

    int result = -1;
    mbedtls_x509_crt roots;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    secure_transport_t transport;
    memset(&transport, 0, sizeof(transport));
    mbedtls_x509_crt_init(&roots);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);

    if (mbedtls_x509_crt_parse(&roots, (const unsigned char *)ca_copy,
                               request->ca_pem_length + 1U) < 0) {
        set_error("unable to parse trusted CA bundle");
        goto cleanup;
    }
    if (mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) < 0) {
        set_error("TLS client configuration failed");
        goto cleanup;
    }
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config, &roots, NULL);
    if (mbedtls_ssl_setup(&ssl, &config) < 0 ||
        mbedtls_ssl_set_hostname(&ssl, request->host) < 0) {
        set_error("TLS hostname configuration failed");
        goto cleanup;
    }
    if (transport_connect(&transport, request->host, request->port) < 0) {
        set_error("TCP connection failed");
        goto cleanup;
    }
    mbedtls_ssl_set_bio(&ssl, &transport, transport_send, transport_recv,
                        NULL);

    int status;
    do {
        status = mbedtls_ssl_handshake(&ssl);
    } while (status == MBEDTLS_ERR_SSL_WANT_READ ||
             status == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (status < 0 || mbedtls_ssl_get_verify_result(&ssl) != 0) {
        set_error("X.509 chain, validity, or hostname verification failed");
        goto cleanup;
    }

    char authority[288];
    if (host_is_ipv6(request->host))
        hbos_mbedtls_snprintf(authority, sizeof(authority),
                              "[%s]", request->host);
    else
        hbos_mbedtls_snprintf(authority, sizeof(authority),
                              "%s", request->host);
    char http_request[1024];
    int http_length = hbos_mbedtls_snprintf(
        http_request, sizeof(http_request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
        "User-Agent: HPT/0.2\r\nAccept: */*\r\n\r\n",
        request->path, authority);
    if (http_length <= 0 || (size_t)http_length >= sizeof(http_request) ||
        write_all(&ssl, (const uint8_t *)http_request,
                  (uint32_t)http_length) < 0) {
        set_error("HTTPS request write failed");
        goto cleanup;
    }

    uint32_t total = 0;
    while (total + 1U < request->output_capacity) {
        status = mbedtls_ssl_read(
            &ssl, (unsigned char *)request->output + total,
            request->output_capacity - total - 1U);
        if (status == MBEDTLS_ERR_SSL_WANT_READ ||
            status == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (status == 0 || status == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            break;
        if (status < 0) {
            set_error("HTTPS response read failed");
            goto cleanup;
        }
        total += (uint32_t)status;
    }
    request->output[total] = '\0';
    *output_length = total;
    if (!total) {
        set_error("empty HTTPS response");
        goto cleanup;
    }
    set_error("ok");
    result = 0;

cleanup:
    if (transport.active) {
        (void)mbedtls_ssl_close_notify(&ssl);
        transport_close(&transport);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_x509_crt_free(&roots);
    kfree(ca_copy);
    return result;
}
