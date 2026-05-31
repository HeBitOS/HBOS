#ifndef HBOS_NET_H
#define HBOS_NET_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NET_DRIVER_NONE = 0,
    NET_DRIVER_E1000,
    NET_DRIVER_RTL8139,
    NET_DRIVER_VIRTIO_NET,
    NET_DRIVER_UNKNOWN_ETHERNET
} net_driver_t;

typedef struct {
    bool present;
    bool mac_valid;
    net_driver_t driver;
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar0_raw;
    uint32_t bar0_base;
    bool bar0_io;
    uint8_t mac[6];
    bool link_ready;
    bool dhcp_ok;
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
} net_device_t;

void net_init(void);
const net_device_t *net_primary(void);
const char *net_driver_name(net_driver_t driver);
int net_dhcp(void);
int net_ping(uint32_t ip, uint32_t timeout_ms);
int net_dns_resolve(const char *name, uint32_t *out_ip);
int net_http_get(const char *host, uint32_t ip, uint16_t port, const char *path,
                 char *out, uint32_t out_cap, uint32_t *out_len);
uint32_t net_parse_ipv4(const char *s);
void net_ipv4_to_str(uint32_t ip, char out[16]);

#endif
