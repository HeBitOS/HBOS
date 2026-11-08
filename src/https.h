/**
 * @file    https.h
 * @brief   导出 4 个接口（1 类型 / 1 宏 / 2 函数）
 *
 * @layer   net
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  hbos_https_request_v2_t  结构体 / 类型别名
 *   宏    HBOS_HTTPS_REQUEST_V2_VERSION  HBOSHTTPSREQUESTV2版本
 *   函数  secure_https_last_error  （待补）
 */
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
