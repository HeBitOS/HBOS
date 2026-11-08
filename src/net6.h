/**
 * @file    net6.h
 * @brief   导出 9 个接口（1 类型 / 1 宏 / 7 函数）
 *
 * @layer   net
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  net6_tcp_conn_t  结构体 / 类型别名
 *   宏    NET6_RX_CAPACITY  常量 / 特性宏
 *   函数  net6_init             Initialize IPv6, link…
 *         net6_tcp_close        （待补）
 *         net6_primary_address  Render the first pref…
 */
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

/** Connect to a 16-byte IPv6 address returned by a DNS AAAA query. */
int net6_tcp_connect_address(const uint8_t address[16], uint16_t port,
                             net6_tcp_conn_t *connection,
                             uint32_t timeout_ms);

/** Blocking-style transport operations used by the TLS BIO adapter. */
int net6_tcp_send(net6_tcp_conn_t *connection, const uint8_t *data,
                  uint32_t length, uint32_t timeout_ms);
int net6_tcp_recv(net6_tcp_conn_t *connection, uint8_t *data,
                  uint32_t capacity, uint32_t timeout_ms);
void net6_tcp_close(net6_tcp_conn_t *connection);

/** Render the first preferred IPv6 address, or return -1 if SLAAC is pending. */
int net6_primary_address(char *out, size_t capacity);

#endif
