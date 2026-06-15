/**
 * @file net.h
 * @brief 网络子系统头文件，基于 Intel E1000 网卡实现，支持 DHCP、ARP、ICMP、DNS、TCP、HTTP 等协议
 */

#ifndef HBOS_NET_H
#define HBOS_NET_H

#include <stdint.h>
#include <stdbool.h>

/** @brief TCP 连接接收缓冲区大小（字节） */
#define NET_TCP_RXBUF_SIZE 8192

/**
 * @brief 网卡驱动类型枚举
 */
typedef enum {
    NET_DRIVER_NONE = 0,           /**< 未检测到网卡驱动 */
    NET_DRIVER_E1000,              /**< Intel E1000 系列网卡 */
    NET_DRIVER_RTL8139,            /**< Realtek RTL8139 网卡 */
    NET_DRIVER_VIRTIO_NET,         /**< VirtIO 虚拟网卡 */
    NET_DRIVER_PCNET,              /**< AMD PCnet-PCI II (Am79C970A) */
    NET_DRIVER_UNKNOWN_ETHERNET    /**< 未知以太网卡 */
} net_driver_t;

/**
 * @brief 网络设备信息结构体，描述主网卡的状态与配置
 */
typedef struct {
    bool present;                  /**< 网卡是否存在 */
    bool mac_valid;                /**< MAC 地址是否有效 */
    net_driver_t driver;           /**< 网卡驱动类型 */
    uint8_t bus;                   /**< PCI 总线号 */
    uint8_t slot;                  /**< PCI 插槽号 */
    uint8_t func;                  /**< PCI 功能号 */
    uint16_t vendor_id;            /**< PCI 厂商 ID */
    uint16_t device_id;            /**< PCI 设备 ID */
    uint32_t bar0_raw;             /**< BAR0 原始值 */
    uint32_t bar0_base;            /**< BAR0 基地址（去除 I/O 或内存标志位） */
    bool bar0_io;                  /**< BAR0 是否为 I/O 端口映射（否则为内存映射） */
    uint8_t mac[6];                /**< 网卡 MAC 地址 */
    bool link_ready;               /**< 链路是否就绪 */
    bool dhcp_ok;                  /**< DHCP 是否已成功获取地址 */
    uint32_t ip;                   /**< 本机 IPv4 地址（网络字节序） */
    uint32_t netmask;              /**< 子网掩码（网络字节序） */
    uint32_t gateway;              /**< 默认网关地址（网络字节序） */
    uint32_t dns;                  /**< DNS 服务器地址（网络字节序） */
    uint64_t rx_packets;           /**< 已接收数据包计数 */
    uint64_t tx_packets;           /**< 已发送数据包计数 */
    uint64_t rx_bytes;             /**< 已接收字节数 */
    uint64_t tx_bytes;             /**< 已发送字节数 */
    uint64_t rx_dropped;           /**< 接收丢包计数 */
    uint64_t tx_errors;            /**< 发送错误计数 */

    /* ── Driver vtable ────────────────────────────────── */
    int (*send)(const void *frame, uint16_t len);   /**< 发送以太网帧 */
} net_device_t;

/**
 * @brief TCP 连接结构体，维护一条 TCP 连接的状态与缓冲区
 */
typedef struct {
    bool open;                     /**< 连接是否处于打开状态 */
    uint32_t peer;                 /**< 对端 IPv4 地址（网络字节序） */
    uint32_t seq;                  /**< 本端发送序列号 */
    uint32_t ack;                  /**< 本端期望接收的确认号 */
    uint16_t sport;                /**< 本端源端口 */
    uint16_t dport;                /**< 对端目标端口 */
    uint8_t mac[6];                /**< 对端 MAC 地址 */
    uint8_t rx_buf[NET_TCP_RXBUF_SIZE]; /**< 接收缓冲区 */
    uint32_t rx_len;               /**< 接收缓冲区中未读取的数据长度 */
} net_tcp_conn_t;

/** @brief 初始化网络子系统，探测并配置网卡硬件 */
void net_init(void);

/** @brief 获取主网卡设备信息指针，若未初始化则自动调用 net_init */
const net_device_t *net_primary(void);

/** @brief 获取网卡驱动类型对应的名称字符串 */
const char *net_driver_name(net_driver_t driver);

/** @brief 获取最近一次网络操作的错误描述字符串 */
const char *net_last_error(void);

/** @brief 通过 DHCP 协议自动获取 IP 地址、子网掩码、网关和 DNS 配置 */
int net_dhcp(void);

/** @brief 手动配置网络参数（IP、子网掩码、网关、DNS） */
int net_configure(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns);

/** @brief 向指定 IP 发送 ICMP Echo Request 并等待回复 */
int net_ping(uint32_t ip, uint32_t timeout_ms);

/** @brief 通过 DNS 协议解析域名为 IPv4 地址 */
int net_dns_resolve(const char *name, uint32_t *out_ip);

/** @brief 发起 TCP 三次握手，建立到指定 IP:port 的连接 */
int net_tcp_connect(uint32_t ip, uint16_t port, net_tcp_conn_t *conn);

/** @brief 通过已建立的 TCP 连接发送数据 */
int net_tcp_send(net_tcp_conn_t *conn, const uint8_t *data, uint32_t len);

/** @brief 从已建立的 TCP 连接接收数据 */
int net_tcp_recv(net_tcp_conn_t *conn, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t poll_rounds);

/** @brief 发送 TCP FIN 包关闭连接 */
void net_tcp_close(net_tcp_conn_t *conn);

/** @brief 在指定端口上开始 TCP 监听 */
int net_tcp_listen(uint16_t port);

/** @brief 接受一个 TCP 客户端连接 */
int net_tcp_accept(uint16_t port, net_tcp_conn_t *conn, uint32_t timeout_ms);

/** @brief 一次性完成 TCP 连接、发送请求、接收响应、关闭连接的完整交互 */
int net_tcp_exchange(uint32_t ip, uint16_t port, const uint8_t *request, uint32_t request_len,
                     uint8_t *response, uint32_t response_cap, uint32_t *response_len);

/** @brief 发送 HTTP 请求，支持自定义方法（GET/POST 等） */
int net_http_request(const char *method, const char *host, uint32_t ip, uint16_t port,
                     const char *path, char *out, uint32_t out_cap, uint32_t *out_len);

/** @brief 发送 HTTP GET 请求的便捷封装 */
int net_http_get(const char *host, uint32_t ip, uint16_t port, const char *path,
                 char *out, uint32_t out_cap, uint32_t *out_len);

/** @brief 将点分十进制 IPv4 字符串解析为网络字节序的 32 位整数 */
uint32_t net_parse_ipv4(const char *s);

/** @brief 将网络字节序的 32 位 IPv4 地址转换为点分十进制字符串 */
void net_ipv4_to_str(uint32_t ip, char out[16]);

#endif
