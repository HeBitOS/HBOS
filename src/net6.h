#ifndef HBOS_NET6_H
#define HBOS_NET6_H

#include <stddef.h>
#include <stdint.h>

#define NET6_RX_CAPACITY (32U * 1024U)

typedef struct {
    void *pcb;
    uint8_t rx[NET6_RX_CAPACITY];
    uint32_t rx_head;
    uint32_t rx_length;
    int connected;
    int closed;
    int error;
} net6_tcp_conn_t;

/** Initialize IPv6, link-local addressing, NDP and SLAAC on the primary NIC. */
int net6_init(void);

/** Parse and connect to an unbracketed IPv6 literal. */
int net6_tcp_connect(const char *address, uint16_t port,
                     net6_tcp_conn_t *connection, uint32_t timeout_ms);

/** Blocking-style transport operations used by the TLS BIO adapter. */
int net6_tcp_send(net6_tcp_conn_t *connection, const uint8_t *data,
                  uint32_t length, uint32_t timeout_ms);
int net6_tcp_recv(net6_tcp_conn_t *connection, uint8_t *data,
                  uint32_t capacity, uint32_t timeout_ms);
void net6_tcp_close(net6_tcp_conn_t *connection);

/** Render the first preferred IPv6 address, or return -1 if SLAAC is pending. */
int net6_primary_address(char *out, size_t capacity);

#endif
