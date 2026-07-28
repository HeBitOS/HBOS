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
/* 精确读取一次 TLS record 时允许的连续空轮询次数。网页主体使用上面的
 * 宽松默认值；图片/CSS 等可降级子资源用较小值，避免坏 CDN 卡住整页。 */
int tls_https_get_with_idle_limit(const char *host, uint32_t ip, uint16_t port,
                                  const char *path, char *out, uint32_t out_cap,
                                  uint32_t *out_len, uint32_t idle_limit);

#endif
