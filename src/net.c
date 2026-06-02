/**
 * @file net.c
 * @brief 网络子系统实现，基于 Intel E1000 网卡，支持 DHCP、ARP、ICMP ping、DNS、TCP、HTTP 等协议
 */

#include "net.h"
#include "pci.h"
#include "string.h"
#include "core/vmm.h"

/** @brief PCI 设备类型：网络控制器 */
#define PCI_CLASS_NETWORK      0x02
/** @brief PCI 子类型：以太网控制器 */
#define PCI_SUBCLASS_ETHERNET  0x00

/** @brief E1000 控制寄存器偏移 */
#define E1000_CTRL   0x0000
/** @brief E1000 状态寄存器偏移 */
#define E1000_STATUS 0x0008
/** @brief E1000 中断屏蔽清除寄存器偏移 */
#define E1000_IMC    0x00D8
/** @brief E1000 接收控制寄存器偏移 */
#define E1000_RCTL   0x0100
/** @brief E1000 发送控制寄存器偏移 */
#define E1000_TCTL   0x0400
/** @brief E1000 发送帧间间隔寄存器偏移 */
#define E1000_TIPG   0x0410
/** @brief E1000 接收描述符基地址低 32 位 */
#define E1000_RDBAL  0x2800
/** @brief E1000 接收描述符基地址高 32 位 */
#define E1000_RDBAH  0x2804
/** @brief E1000 接收描述符环长度 */
#define E1000_RDLEN  0x2808
/** @brief E1000 接收描述符头指针 */
#define E1000_RDH    0x2810
/** @brief E1000 接收描述符尾指针 */
#define E1000_RDT    0x2818
/** @brief E1000 发送描述符基地址低 32 位 */
#define E1000_TDBAL  0x3800
/** @brief E1000 发送描述符基地址高 32 位 */
#define E1000_TDBAH  0x3804
/** @brief E1000 发送描述符环长度 */
#define E1000_TDLEN  0x3808
/** @brief E1000 发送描述符头指针 */
#define E1000_TDH    0x3810
/** @brief E1000 发送描述符尾指针 */
#define E1000_TDT    0x3818
/** @brief E1000 接收地址低 32 位（MAC 地址寄存器 0） */
#define E1000_RAL0   0x5400
/** @brief E1000 接收地址高 32 位（MAC 地址寄存器 0） */
#define E1000_RAH0   0x5404

/** @brief 接收描述符环大小 */
#define RX_COUNT 32
/** @brief 发送描述符环大小 */
#define TX_COUNT 16
/** @brief 单个数据包缓冲区大小（字节） */
#define PKT_SIZE 2048

/** @brief 以太网帧类型：IPv4 */
#define ETH_TYPE_IP  0x0800
/** @brief 以太网帧类型：ARP */
#define ETH_TYPE_ARP 0x0806
/** @brief IP 协议号：ICMP */
#define IP_PROTO_ICMP 1
/** @brief IP 协议号：TCP */
#define IP_PROTO_TCP  6
/** @brief IP 协议号：UDP */
#define IP_PROTO_UDP  17

/** @brief DHCP 客户端端口号 */
#define DHCP_CLIENT_PORT 68
/** @brief DHCP 服务器端口号 */
#define DHCP_SERVER_PORT 67
/** @brief DNS 服务端口号 */
#define DNS_PORT 53
/** @brief DHCP 报文固定部分长度（含选项前缀） */
#define DHCP_FIXED_LEN 240
/** @brief 邻居缓存（ARP 表）条目数 */
#define NEIGH_CACHE_SIZE 16
/** @brief TCP 最大段大小（字节） */
#define TCP_MSS 1460

/**
 * @brief E1000 接收描述符结构体
 */
typedef struct {
    volatile uint64_t addr;        /**< 缓冲区物理地址 */
    volatile uint16_t length;      /**< 接收到的数据包长度 */
    volatile uint16_t checksum;    /**< 数据包校验和 */
    volatile uint8_t status;       /**< 描述符状态（bit0=DD 表示完成） */
    volatile uint8_t errors;       /**< 接收错误标志 */
    volatile uint16_t special;     /**< 特殊字段（VLAN 等） */
} __attribute__((packed)) e1000_rx_desc_t;

/**
 * @brief E1000 发送描述符结构体
 */
typedef struct {
    volatile uint64_t addr;        /**< 缓冲区物理地址 */
    volatile uint16_t length;      /**< 数据包长度 */
    volatile uint8_t cso;          /**< 校验和偏移 */
    volatile uint8_t cmd;          /**< 命令字段（bit0=EOP, bit1=IFCS, bit3=RS） */
    volatile uint8_t status;       /**< 描述符状态（bit0=DD 表示发送完成） */
    volatile uint8_t css;          /**< 校验和起始位置 */
    volatile uint16_t special;     /**< 特殊字段（VLAN 等） */
} __attribute__((packed)) e1000_tx_desc_t;

/**
 * @brief 以太网帧头结构体
 */
typedef struct {
    uint8_t dst[6];                /**< 目标 MAC 地址 */
    uint8_t src[6];                /**< 源 MAC 地址 */
    uint16_t type;                 /**< 以太网类型（如 IP、ARP） */
} __attribute__((packed)) eth_hdr_t;

/**
 * @brief IPv4 报头结构体
 */
typedef struct {
    uint8_t ver_ihl;               /**< 版本号（高 4 位）与首部长度（低 4 位，以 4 字节为单位） */
    uint8_t tos;                   /**< 服务类型 */
    uint16_t len;                  /**< 总长度（含首部与数据） */
    uint16_t id;                   /**< 标识符 */
    uint16_t frag;                 /**< 片偏移与标志 */
    uint8_t ttl;                   /**< 生存时间 */
    uint8_t proto;                 /**< 上层协议号 */
    uint16_t csum;                 /**< 首部校验和 */
    uint32_t src;                  /**< 源 IP 地址（网络字节序） */
    uint32_t dst;                  /**< 目的 IP 地址（网络字节序） */
} __attribute__((packed)) ipv4_hdr_t;

/**
 * @brief UDP 报头结构体
 */
typedef struct {
    uint16_t src;                  /**< 源端口号 */
    uint16_t dst;                  /**< 目标端口号 */
    uint16_t len;                  /**< UDP 报文总长度 */
    uint16_t csum;                 /**< 校验和 */
} __attribute__((packed)) udp_hdr_t;

/**
 * @brief TCP 报头结构体（不含选项）
 */
typedef struct {
    uint16_t src;                  /**< 源端口号 */
    uint16_t dst;                  /**< 目标端口号 */
    uint32_t seq;                  /**< 序列号 */
    uint32_t ack;                  /**< 确认号 */
    uint8_t off_flags_hi;          /**< 数据偏移（高 4 位，以 4 字节为单位）与保留位 */
    uint8_t flags;                 /**< TCP 标志位（SYN/ACK/FIN/RST 等） */
    uint16_t win;                  /**< 接收窗口大小 */
    uint16_t csum;                 /**< 校验和 */
    uint16_t urg;                  /**< 紧急指针 */
} __attribute__((packed)) tcp_hdr_t;

/**
 * @brief ARP 数据包结构体
 */
typedef struct {
    uint16_t htype;                /**< 硬件类型（1=以太网） */
    uint16_t ptype;                /**< 协议类型（0x0800=IPv4） */
    uint8_t hlen;                  /**< 硬件地址长度（6） */
    uint8_t plen;                  /**< 协议地址长度（4） */
    uint16_t op;                   /**< 操作码（1=请求, 2=应答） */
    uint8_t sha[6];                /**< 发送方硬件地址 */
    uint32_t spa;                  /**< 发送方协议地址（IP） */
    uint8_t tha[6];                /**< 目标硬件地址 */
    uint32_t tpa;                  /**< 目标协议地址（IP） */
} __attribute__((packed)) arp_pkt_t;

/**
 * @brief DHCP 数据包结构体
 */
typedef struct {
    uint8_t op, htype, hlen, hops; /**< 操作码(1=请求,2=应答)、硬件类型、硬件地址长度、跳数 */
    uint32_t xid;                  /**< 事务 ID */
    uint16_t secs, flags;          /**< 秒数、标志位 */
    uint32_t ciaddr, yiaddr, siaddr, giaddr; /**< 客户端/分配/服务器/中继 IP 地址 */
    uint8_t chaddr[16];            /**< 客户端硬件地址 */
    uint8_t legacy[192];           /**< 传统引导文件名与服务器名字段 */
    uint32_t magic;                /**< DHCP 魔数（0x63825363） */
    uint8_t opts[312];             /**< DHCP 选项区域 */
} __attribute__((packed)) dhcp_pkt_t;

/** @brief 主网卡设备信息 */
static net_device_t primary;
/** @brief 网络子系统是否已初始化标志 */
static int initialized;
/** @brief E1000 MMIO 寄存器基地址指针 */
static volatile uint8_t *mmio;
/** @brief E1000 接收描述符环 */
static e1000_rx_desc_t rx_desc[RX_COUNT] __attribute__((aligned(16)));
/** @brief E1000 发送描述符环 */
static e1000_tx_desc_t tx_desc[TX_COUNT] __attribute__((aligned(16)));
/** @brief 接收数据包缓冲区 */
static uint8_t rx_buf[RX_COUNT][PKT_SIZE] __attribute__((aligned(16)));
/** @brief 发送数据包缓冲区 */
static uint8_t tx_buf[TX_COUNT][PKT_SIZE] __attribute__((aligned(16)));
/** @brief 发送描述符尾指针索引 */
static uint16_t tx_tail;
/** @brief IP 报文标识符计数器 */
static uint16_t ip_id = 1;
/** @brief 下一个可用的临时源端口号 */
static uint16_t next_port = 49152;
/** @brief 最近一次错误描述字符串 */
static const char *last_error = "ok";

/**
 * @brief 邻居缓存条目（ARP 表项）
 */
typedef struct {
    int valid;                     /**< 条目是否有效 */
    uint32_t ip;                   /**< IP 地址（网络字节序） */
    uint8_t mac[6];                /**< MAC 地址 */
    uint32_t age;                  /**< 老化时间戳（用于 LRU 替换） */
} neigh_entry_t;

/** @brief 邻居缓存（ARP 表） */
static neigh_entry_t neigh_cache[NEIGH_CACHE_SIZE];
/** @brief 邻居缓存时钟，用于 LRU 老化 */
static uint32_t neigh_clock;

/** @brief 16 位字节序交换 */
static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
/** @brief 32 位字节序交换 */
static uint32_t bswap32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
/** @brief 主机序转网络序（16 位） */
#define htons(x) bswap16((uint16_t)(x))
/** @brief 网络序转主机序（16 位） */
#define ntohs(x) bswap16((uint16_t)(x))
/** @brief 主机序转网络序（32 位） */
#define htonl(x) bswap32((uint32_t)(x))
/** @brief 网络序转主机序（32 位） */
#define ntohl(x) bswap32((uint32_t)(x))

/**
 * @brief 从 E1000 MMIO 寄存器空间读取 32 位值
 * @param off 寄存器偏移量
 * @return 读取到的 32 位值
 */
static inline uint32_t reg_read(uint32_t off) {
    return *(volatile uint32_t *)(mmio + off);
}

/**
 * @brief 向 E1000 MMIO 寄存器空间写入 32 位值
 * @param off 寄存器偏移量
 * @param val 要写入的值
 */
static inline void reg_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(mmio + off) = val;
}

/**
 * @brief 根据 PCI 厂商 ID 和设备 ID 检测网卡驱动类型
 * @param vendor PCI 厂商 ID
 * @param device PCI 设备 ID
 * @return 对应的网卡驱动类型枚举值
 */
static net_driver_t detect_driver(uint16_t vendor, uint16_t device) {
    if (vendor == 0x8086) {
        switch (device) {
            case 0x1004: case 0x1008: case 0x1009: case 0x100C:
            case 0x100D: case 0x100E: case 0x100F: case 0x1015:
            case 0x1016: case 0x1017: case 0x10D3:
                return NET_DRIVER_E1000;
            default: break;
        }
    }
    if (vendor == 0x10EC && device == 0x8139) return NET_DRIVER_RTL8139;
    if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041)) return NET_DRIVER_VIRTIO_NET;
    return NET_DRIVER_UNKNOWN_ETHERNET;
}

/**
 * @brief 获取网卡驱动类型对应的名称字符串
 * @param driver 驱动类型枚举值
 * @return 驱动名称字符串
 */
const char *net_driver_name(net_driver_t driver) {
    switch (driver) {
        case NET_DRIVER_E1000: return "Intel E1000";
        case NET_DRIVER_RTL8139: return "Realtek RTL8139";
        case NET_DRIVER_VIRTIO_NET: return "VirtIO net";
        case NET_DRIVER_UNKNOWN_ETHERNET: return "unknown ethernet";
        default: return "none";
    }
}

/**
 * @brief 设置最近一次网络操作的错误描述
 * @param msg 错误描述字符串，若为 NULL 则使用默认 "error"
 */
static void set_error(const char *msg) {
    last_error = msg ? msg : "error";
}

/**
 * @brief 获取最近一次网络操作的错误描述字符串
 * @return 错误描述字符串
 */
const char *net_last_error(void) {
    return last_error;
}

/**
 * @brief 计算数据的标准互联网校验和（RFC 1071）
 * @param data 数据指针
 * @param len 数据长度
 * @return 16 位校验和（网络字节序）
 */
static uint16_t checksum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/**
 * @brief 计算 UDP 伪首部校验和
 * @param ip IPv4 首部指针
 * @param udp UDP 首部指针
 * @param payload 载荷数据指针
 * @param plen 载荷长度
 * @return 16 位校验和（网络字节序）
 */
static uint16_t udp_checksum(ipv4_hdr_t *ip, udp_hdr_t *udp, const uint8_t *payload, uint16_t plen) {
    uint32_t sum = 0;
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + plen);
    const uint8_t *src = (const uint8_t *)&ip->src;
    const uint8_t *dst = (const uint8_t *)&ip->dst;
    sum += ((uint16_t)src[0] << 8) | src[1]; sum += ((uint16_t)src[2] << 8) | src[3];
    sum += ((uint16_t)dst[0] << 8) | dst[1]; sum += ((uint16_t)dst[2] << 8) | dst[3];
    sum += IP_PROTO_UDP; sum += udp_len;
    const uint8_t *p = (const uint8_t *)udp;
    for (uint32_t i = 0; i < sizeof(udp_hdr_t); i += 2) sum += ((uint16_t)p[i] << 8) | p[i + 1];
    p = payload;
    for (uint16_t i = 0; i + 1 < plen; i += 2) sum += ((uint16_t)p[i] << 8) | p[i + 1];
    if (plen & 1) sum += (uint16_t)p[plen - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/**
 * @brief 计算 TCP 伪首部校验和
 * @param ip IPv4 首部指针
 * @param tcp TCP 首部指针
 * @param payload 载荷数据指针
 * @param plen 载荷长度
 * @return 16 位校验和（网络字节序）
 */
static uint16_t tcp_checksum(ipv4_hdr_t *ip, tcp_hdr_t *tcp, const uint8_t *payload, uint16_t plen) {
    uint32_t sum = 0;
    uint16_t tcp_len = (uint16_t)(20 + plen);
    const uint8_t *src = (const uint8_t *)&ip->src;
    const uint8_t *dst = (const uint8_t *)&ip->dst;
    sum += ((uint16_t)src[0] << 8) | src[1]; sum += ((uint16_t)src[2] << 8) | src[3];
    sum += ((uint16_t)dst[0] << 8) | dst[1]; sum += ((uint16_t)dst[2] << 8) | dst[3];
    sum += IP_PROTO_TCP; sum += tcp_len;
    const uint8_t *p = (const uint8_t *)tcp;
    for (uint32_t i = 0; i < 20; i += 2) sum += ((uint16_t)p[i] << 8) | p[i + 1];
    p = payload;
    for (uint16_t i = 0; i + 1 < plen; i += 2) sum += ((uint16_t)p[i] << 8) | p[i + 1];
    if (plen & 1) sum += (uint16_t)p[plen - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/**
 * @brief 通过 E1000 发送一个以太网帧
 * @param frame 帧数据指针
 * @param len 帧长度
 * @return 0 成功，-1 失败
 */
static int e1000_send(const void *frame, uint16_t len) {
    if (!primary.link_ready || !mmio || len > PKT_SIZE) {
        set_error("e1000 not ready");
        primary.tx_errors++;
        return -1;
    }
    uint16_t idx = tx_tail;
    uint32_t wait = 0;
    while (!(tx_desc[idx].status & 1) && wait++ < 1000000) {}
    if (!(tx_desc[idx].status & 1)) {
        set_error("tx ring full");
        primary.tx_errors++;
        return -1;
    }
    memcpy(tx_buf[idx], frame, len);
    tx_desc[idx].length = len;
    tx_desc[idx].cmd = 0x0B;
    tx_desc[idx].status = 0;
    tx_tail = (uint16_t)((idx + 1) % TX_COUNT);
    reg_write(E1000_TDT, tx_tail);
    primary.tx_packets++;
    primary.tx_bytes += len;
    return 0;
}

/** @brief 数据包回调函数类型，用于 net_poll 中处理接收到的数据包 */
typedef int (*packet_cb_t)(const uint8_t *pkt, uint16_t len, void *arg);

/**
 * @brief 轮询接收数据包，对每个收到的包调用回调函数
 * @param cb 数据包回调函数
 * @param arg 传递给回调的用户参数
 * @param spins 最大轮询次数
 * @return 0 正常，-1 网卡未就绪，回调返回非零值时透传
 */
static int net_poll(packet_cb_t cb, void *arg, uint32_t spins) {
    if (!primary.link_ready || !mmio) return -1;
    for (uint32_t s = 0; s < spins; s++) {
        uint32_t tail = reg_read(E1000_RDT);
        uint32_t idx = (tail + 1) % RX_COUNT;
        e1000_rx_desc_t *d = &rx_desc[idx];
        if (!(d->status & 1)) continue;
        uint16_t len = d->length;
        int ret = 0;
        if (len >= sizeof(eth_hdr_t) && len <= PKT_SIZE && cb) {
            primary.rx_packets++;
            primary.rx_bytes += len;
            ret = cb(rx_buf[idx], len, arg);
        } else {
            primary.rx_dropped++;
        }
        d->status = 0;
        reg_write(E1000_RDT, idx);
        if (ret) return ret;
    }
    return 0;
}

/**
 * @brief 构造以太网帧头
 * @param buf 输出缓冲区（至少 14 字节）
 * @param dst 目标 MAC 地址
 * @param type 以太网类型（如 ETH_TYPE_IP）
 */
static void make_eth(uint8_t *buf, const uint8_t dst[6], uint16_t type) {
    eth_hdr_t *eth = (eth_hdr_t *)buf;
    memcpy(eth->dst, dst, 6);
    memcpy(eth->src, primary.mac, 6);
    eth->type = htons(type);
}

/**
 * @brief 构造并发送一个 IPv4 数据包
 * @param dst_mac 下一跳 MAC 地址
 * @param dst_ip 目标 IP 地址
 * @param proto 上层协议号
 * @param payload 载荷数据
 * @param plen 载荷长度
 * @return 0 成功，-1 失败
 */
static int send_ip(const uint8_t dst_mac[6], uint32_t dst_ip, uint8_t proto,
                   const void *payload, uint16_t plen) {
    uint8_t frame[1514];
    make_eth(frame, dst_mac, ETH_TYPE_IP);
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->len = htons((uint16_t)(sizeof(ipv4_hdr_t) + plen));
    ip->id = htons(ip_id++);
    ip->frag = htons(0x4000);
    ip->ttl = 64;
    ip->proto = proto;
    ip->src = primary.ip;
    ip->dst = dst_ip;
    ip->csum = 0;
    ip->csum = checksum(ip, sizeof(ipv4_hdr_t));
    memcpy(frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t), payload, plen);
    return e1000_send(frame, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + plen));
}

/**
 * @brief 构造并发送一个 UDP 数据包（可自定义源 IP，用于 DHCP 阶段本机尚无 IP 的场景）
 * @param dst_mac 下一跳 MAC 地址
 * @param src_ip 源 IP 地址
 * @param dst_ip 目标 IP 地址
 * @param sport 源端口号
 * @param dport 目标端口号
 * @param payload 载荷数据
 * @param plen 载荷长度
 * @return 0 成功，-1 失败
 */
static int send_udp_raw(const uint8_t dst_mac[6], uint32_t src_ip, uint32_t dst_ip,
                        uint16_t sport, uint16_t dport, const void *payload, uint16_t plen) {
    uint8_t frame[1514];
    make_eth(frame, dst_mac, ETH_TYPE_IP);
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)(frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    uint8_t *data = frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t);
    memcpy(data, payload, plen);
    ip->ver_ihl = 0x45; ip->tos = 0;
    ip->len = htons((uint16_t)(sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + plen));
    ip->id = htons(ip_id++); ip->frag = htons(0x4000); ip->ttl = 64;
    ip->proto = IP_PROTO_UDP; ip->src = src_ip; ip->dst = dst_ip; ip->csum = 0;
    ip->csum = checksum(ip, sizeof(ipv4_hdr_t));
    udp->src = htons(sport); udp->dst = htons(dport);
    udp->len = htons((uint16_t)(sizeof(udp_hdr_t) + plen)); udp->csum = 0;
    udp->csum = udp_checksum(ip, udp, data, plen);
    return e1000_send(frame, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + plen));
}

/**
 * @brief 构造并发送一个 ARP 数据包
 * @param op ARP 操作码（1=请求, 2=应答）
 * @param target_ip 目标 IP 地址
 * @param target_mac 目标 MAC 地址（请求时可为 NULL）
 * @return 0 成功，-1 失败
 */
static int send_arp(uint16_t op, uint32_t target_ip, const uint8_t target_mac[6]) {
    uint8_t frame[sizeof(eth_hdr_t) + sizeof(arp_pkt_t)];
    uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    uint8_t zero[6] = {0,0,0,0,0,0};
    make_eth(frame, op == 1 ? bcast : target_mac, ETH_TYPE_ARP);
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    arp->htype = htons(1); arp->ptype = htons(ETH_TYPE_IP);
    arp->hlen = 6; arp->plen = 4; arp->op = htons(op);
    memcpy(arp->sha, primary.mac, 6); arp->spa = primary.ip;
    memcpy(arp->tha, op == 1 ? zero : target_mac, 6); arp->tpa = target_ip;
    return e1000_send(frame, sizeof(frame));
}

/**
 * @brief 根据目标 IP 计算下一跳地址（直连或网关）
 * @param dst_ip 目标 IP 地址
 * @param next_hop 输出下一跳 IP 地址
 * @return 0 成功，-1 失败（接口未配置或无网关）
 */
static int net_route_next_hop(uint32_t dst_ip, uint32_t *next_hop) {
    if (!next_hop || !primary.dhcp_ok) {
        set_error("interface not configured");
        return -1;
    }
    if (((dst_ip ^ primary.ip) & primary.netmask) == 0) {
        *next_hop = dst_ip;
        return 0;
    }
    if (!primary.gateway) {
        set_error("gateway missing");
        return -1;
    }
    *next_hop = primary.gateway;
    return 0;
}

/**
 * @brief 在邻居缓存中查找指定 IP 对应的 MAC 地址
 * @param ip 目标 IP 地址
 * @param mac 输出 MAC 地址
 * @return 0 找到，-1 未找到
 */
static int neigh_lookup(uint32_t ip, uint8_t mac[6]) {
    for (int i = 0; i < NEIGH_CACHE_SIZE; i++) {
        if (neigh_cache[i].valid && neigh_cache[i].ip == ip) {
            memcpy(mac, neigh_cache[i].mac, 6);
            neigh_cache[i].age = ++neigh_clock;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 更新邻居缓存条目（若已存在则更新，否则替换最旧条目）
 * @param ip IP 地址
 * @param mac MAC 地址
 */
static void neigh_update(uint32_t ip, const uint8_t mac[6]) {
    int slot = -1;
    uint32_t oldest = 0xffffffffU;
    for (int i = 0; i < NEIGH_CACHE_SIZE; i++) {
        if (neigh_cache[i].valid && neigh_cache[i].ip == ip) {
            slot = i;
            break;
        }
        if (!neigh_cache[i].valid) {
            slot = i;
            break;
        }
        if (neigh_cache[i].age < oldest) {
            oldest = neigh_cache[i].age;
            slot = i;
        }
    }
    if (slot < 0) return;
    neigh_cache[slot].valid = 1;
    neigh_cache[slot].ip = ip;
    memcpy(neigh_cache[slot].mac, mac, 6);
    neigh_cache[slot].age = ++neigh_clock;
}

/** @brief ARP 解析等待上下文 */
typedef struct { uint32_t ip; uint8_t mac[6]; int found; } arp_wait_t;

/**
 * @brief ARP 数据包接收回调，用于等待指定 IP 的 ARP 应答
 * @param pkt 接收到的原始数据包
 * @param len 数据包长度
 * @param arg 指向 arp_wait_t 的指针
 * @return 1 找到目标 ARP 应答，0 继续轮询
 */
static int arp_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    arp_wait_t *w = arg;
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_ARP) return 0;
    const arp_pkt_t *arp = (const arp_pkt_t *)(pkt + sizeof(eth_hdr_t));
    if (ntohs(arp->op) == 2) neigh_update(arp->spa, arp->sha);
    if (ntohs(arp->op) == 2 && arp->spa == w->ip) {
        memcpy(w->mac, arp->sha, 6);
        w->found = 1;
        return 1;
    }
    return 0;
}

/**
 * @brief 解析目标 IP 的 MAC 地址（先查缓存，未命中则发送 ARP 请求并等待应答）
 * @param ip 目标 IP 地址
 * @param mac 输出 MAC 地址
 * @return 0 成功，-1 超时
 */
static int arp_resolve(uint32_t ip, uint8_t mac[6]) {
    if (neigh_lookup(ip, mac) == 0) return 0;
    arp_wait_t w;
    w.ip = ip; w.found = 0;
    send_arp(1, ip, 0);
    for (int i = 0; i < 4 && !w.found; i++) net_poll(arp_cb, &w, 80000);
    if (!w.found) {
        set_error("arp timeout");
        return -1;
    }
    memcpy(mac, w.mac, 6);
    neigh_update(ip, mac);
    return 0;
}

/**
 * @brief 初始化 E1000 网卡硬件：映射 MMIO、读取 MAC 地址、配置收发描述符环和控制寄存器
 * @param pdev PCI 设备信息指针
 */
static void e1000_init_hw(const pci_device_t *pdev) {
    if (primary.bar0_io || primary.bar0_base == 0) return;
    uint64_t base = (uint64_t)(primary.bar0_base & ~0xFFFU);
    for (uint64_t off = 0; off < 0x20000; off += PAGE_SIZE)
        (void)vmm_map_page(base + off, base + off, VMM_W | VMM_CD);
    mmio = (volatile uint8_t *)(uintptr_t)primary.bar0_base;
    pci_enable_bus_master_mmio(pdev);
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | (1U << 6));

    uint32_t lo = reg_read(E1000_RAL0);
    uint32_t hi = reg_read(E1000_RAH0);
    primary.mac[0] = (uint8_t)(lo & 0xFF);
    primary.mac[1] = (uint8_t)((lo >> 8) & 0xFF);
    primary.mac[2] = (uint8_t)((lo >> 16) & 0xFF);
    primary.mac[3] = (uint8_t)((lo >> 24) & 0xFF);
    primary.mac[4] = (uint8_t)(hi & 0xFF);
    primary.mac[5] = (uint8_t)((hi >> 8) & 0xFF);
    primary.mac_valid = (primary.mac[0] | primary.mac[1] | primary.mac[2] |
                         primary.mac[3] | primary.mac[4] | primary.mac[5]) != 0;

    reg_write(E1000_IMC, 0xFFFFFFFF);
    for (int i = 0; i < RX_COUNT; i++) {
        rx_desc[i].addr = (uint64_t)(uintptr_t)rx_buf[i];
        rx_desc[i].status = 0;
    }
    for (int i = 0; i < TX_COUNT; i++) {
        tx_desc[i].addr = (uint64_t)(uintptr_t)tx_buf[i];
        tx_desc[i].status = 1;
    }
    reg_write(E1000_RDBAL, (uint32_t)(uintptr_t)rx_desc);
    reg_write(E1000_RDBAH, (uint32_t)((uint64_t)(uintptr_t)rx_desc >> 32));
    reg_write(E1000_RDLEN, sizeof(rx_desc));
    reg_write(E1000_RDH, 0);
    reg_write(E1000_RDT, RX_COUNT - 1);
    reg_write(E1000_RCTL, (1U << 1) | (1U << 2) | (1U << 15) | (1U << 26));
    reg_write(E1000_TDBAL, (uint32_t)(uintptr_t)tx_desc);
    reg_write(E1000_TDBAH, (uint32_t)((uint64_t)(uintptr_t)tx_desc >> 32));
    reg_write(E1000_TDLEN, sizeof(tx_desc));
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);
    tx_tail = 0;
    reg_write(E1000_TCTL, (1U << 1) | (1U << 3) | (0x10U << 4) | (0x40U << 12));
    reg_write(E1000_TIPG, 10 | (8 << 10) | (6 << 20));
    primary.link_ready = (reg_read(E1000_STATUS) & 2) != 0;
}

/**
 * @brief 初始化网络子系统：扫描 PCI 总线查找以太网卡，检测驱动类型并初始化硬件
 */
void net_init(void) {
    if (initialized) return;
    initialized = 1;
    pci_device_t dev;
    if (pci_find_class(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0xFF, &dev) < 0) {
        set_error("no ethernet controller");
        return;
    }
    primary.present = true;
    primary.driver = detect_driver(dev.vendor_id, dev.device_id);
    primary.bus = dev.bus; primary.slot = dev.slot; primary.func = dev.func;
    primary.vendor_id = dev.vendor_id; primary.device_id = dev.device_id;
    primary.bar0_raw = pci_bar(dev.bus, dev.slot, dev.func, 0);
    primary.bar0_io = (primary.bar0_raw & 1U) != 0;
    primary.bar0_base = primary.bar0_io ? (primary.bar0_raw & ~3U) : (primary.bar0_raw & ~0xFU);
    if (primary.driver == NET_DRIVER_E1000) e1000_init_hw(&dev);
}

/**
 * @brief 获取主网卡设备信息指针，若未初始化则自动调用 net_init
 * @return 主网卡设备信息的只读指针
 */
const net_device_t *net_primary(void) {
    net_init();
    return &primary;
}

/**
 * @brief 向 DHCP 选项区追加一个 TLV 选项
 * @param p 指向选项区写入位置的指针（写入后自动前移）
 * @param code 选项代码
 * @param len 选项值长度
 * @param data 选项值数据
 */
static void dhcp_opt(uint8_t **p, uint8_t code, uint8_t len, const void *data) {
    *(*p)++ = code; *(*p)++ = len; memcpy(*p, data, len); *p += len;
}

/**
 * @brief 构造并发送 DHCP 数据包（Discover/Request）
 * @param msg DHCP 消息类型（1=Discover, 3=Request）
 * @param xid 事务 ID
 * @param req_ip 请求的 IP 地址（用于 Request 阶段）
 * @param server 服务器标识 IP（用于 Request 阶段）
 * @return 0 成功，-1 失败
 */
static int send_dhcp(uint8_t msg, uint32_t xid, uint32_t req_ip, uint32_t server) {
    dhcp_pkt_t d;
    memset(&d, 0, sizeof(d));
    d.op = 1; d.htype = 1; d.hlen = 6; d.xid = htonl(xid); d.flags = htons(0x8000);
    memcpy(d.chaddr, primary.mac, 6); d.magic = htonl(0x63825363);
    uint8_t *o = d.opts;
    dhcp_opt(&o, 53, 1, &msg);
    if (req_ip) dhcp_opt(&o, 50, 4, &req_ip);
    if (server) dhcp_opt(&o, 54, 4, &server);
    uint8_t params[] = {1,3,6};
    dhcp_opt(&o, 55, sizeof(params), params);
    *o++ = 255;
    uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    return send_udp_raw(bcast, 0, 0xFFFFFFFFU, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &d, sizeof(d));
}

/** @brief DHCP 等待上下文，用于收集 DHCP 应答中的各项配置 */
typedef struct { uint32_t xid, yiaddr, server, mask, router, dns; uint8_t type; int found; } dhcp_wait_t;

/**
 * @brief DHCP 数据包接收回调，解析 DHCP Offer/ACK 并提取 IP、掩码、网关、DNS 等配置
 * @param pkt 接收到的原始数据包
 * @param len 数据包长度
 * @param arg 指向 dhcp_wait_t 的指针
 * @return 1 成功匹配到目标 DHCP 应答，0 继续轮询
 */
static int dhcp_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    dhcp_wait_t *w = arg;
    if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + DHCP_FIXED_LEN) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_IP) return 0;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    if (ip->proto != IP_PROTO_UDP) return 0;
    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    const udp_hdr_t *udp = (const udp_hdr_t *)((const uint8_t *)ip + ihl);
    if (ntohs(udp->dst) != DHCP_CLIENT_PORT) return 0;
    uint16_t udp_len = ntohs(udp->len);
    if (udp_len < sizeof(udp_hdr_t) + DHCP_FIXED_LEN) return 0;
    const dhcp_pkt_t *d = (const dhcp_pkt_t *)((const uint8_t *)udp + sizeof(udp_hdr_t));
    if (ntohl(d->xid) != w->xid) return 0;
    w->yiaddr = d->yiaddr;
    const uint8_t *o = d->opts;
    uint32_t opt_len = udp_len - sizeof(udp_hdr_t) - DHCP_FIXED_LEN;
    if (ntohl(d->magic) != 0x63825363U) return 0;
    for (uint32_t i = 0; i < opt_len && o[i] != 255;) {
        if (o[i] == 0) { i++; continue; }
        if (i + 1 >= opt_len) break;
        uint8_t code = o[i++], olen = o[i++];
        if (i + olen > opt_len) break;
        if (code == 53 && olen >= 1) w->type = o[i];
        if (code == 54 && olen >= 4) memcpy(&w->server, &o[i], 4);
        if (code == 1 && olen >= 4) memcpy(&w->mask, &o[i], 4);
        if (code == 3 && olen >= 4) memcpy(&w->router, &o[i], 4);
        if (code == 6 && olen >= 4) memcpy(&w->dns, &o[i], 4);
        i += olen;
    }
    w->found = 1;
    return 1;
}

/**
 * @brief 通过 DHCP 协议自动获取 IP 地址、子网掩码、网关和 DNS 配置
 * @return 0 成功，-1 失败（链路未就绪或超时）
 */
int net_dhcp(void) {
    net_init();
    if (!primary.link_ready || !primary.mac_valid) {
        set_error("link down");
        return -1;
    }
    uint32_t xid = 0x48424F53U;
    dhcp_wait_t w;
    memset(&w, 0, sizeof(w)); w.xid = xid;
    send_dhcp(1, xid, 0, 0);
    for (int i = 0; i < 6 && !w.found; i++) net_poll(dhcp_cb, &w, 80000);
    if (!w.found || w.type != 2) {
        set_error("dhcp discover timeout");
        return -1;
    }
    uint32_t offer = w.yiaddr, server = w.server;
    memset(&w, 0, sizeof(w)); w.xid = xid;
    send_dhcp(3, xid, offer, server);
    for (int i = 0; i < 6 && !w.found; i++) net_poll(dhcp_cb, &w, 80000);
    if (!w.found || w.type != 5) {
        set_error("dhcp request timeout");
        return -1;
    }
    primary.ip = w.yiaddr;
    primary.netmask = w.mask;
    primary.gateway = w.router;
    primary.dns = w.dns;
    primary.dhcp_ok = true;
    memset(neigh_cache, 0, sizeof(neigh_cache));
    if (primary.gateway) {
        uint8_t mac[6];
        (void)arp_resolve(primary.gateway, mac);
    }
    return 0;
}

/**
 * @brief 手动配置网络参数（IP、子网掩码、网关、DNS）
 * @param ip IPv4 地址（网络字节序）
 * @param netmask 子网掩码（网络字节序）
 * @param gateway 默认网关（网络字节序）
 * @param dns DNS 服务器地址（网络字节序）
 * @return 0 成功，-1 参数无效
 */
int net_configure(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns) {
    net_init();
    if (!primary.link_ready || !primary.mac_valid || ip == 0 || netmask == 0) {
        set_error("bad interface config");
        return -1;
    }
    primary.ip = ip;
    primary.netmask = netmask;
    primary.gateway = gateway;
    primary.dns = dns;
    primary.dhcp_ok = true;
    memset(neigh_cache, 0, sizeof(neigh_cache));
    return 0;
}

/** @brief ICMP ping 等待上下文 */
typedef struct { uint16_t id; int ok; } ping_wait_t;

/**
 * @brief ICMP 数据包接收回调，等待匹配的 Echo Reply
 * @param pkt 接收到的原始数据包
 * @param len 数据包长度
 * @param arg 指向 ping_wait_t 的指针
 * @return 1 收到匹配的 Echo Reply，0 继续轮询
 */
static int ping_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    ping_wait_t *w = arg;
    if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + 8) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_IP) return 0;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    if (ip->proto != IP_PROTO_ICMP || ip->dst != primary.ip) return 0;
    const uint8_t *icmp = (const uint8_t *)ip + ((ip->ver_ihl & 0x0F) * 4);
    if (icmp[0] == 0 && ntohs(*(const uint16_t *)(icmp + 4)) == w->id) {
        w->ok = 1; return 1;
    }
    return 0;
}

/**
 * @brief 向指定 IP 发送 ICMP Echo Request 并等待 Echo Reply
 * @param ip 目标 IP 地址（网络字节序）
 * @param timeout_ms 超时时间（毫秒，当前未使用）
 * @return 0 成功，-1 超时或网络未配置
 */
int net_ping(uint32_t ip, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    uint8_t mac[6];
    uint32_t next_hop;
    if (net_route_next_hop(ip, &next_hop) < 0) return -1;
    if (arp_resolve(next_hop, mac) < 0) return -1;
    uint8_t icmp[32];
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = 8;
    *(uint16_t *)(icmp + 4) = htons(0x4842);
    *(uint16_t *)(icmp + 6) = htons(1);
    for (int i = 8; i < 32; i++) icmp[i] = (uint8_t)i;
    *(uint16_t *)(icmp + 2) = checksum(icmp, sizeof(icmp));
    ping_wait_t w = {0x4842, 0};
    send_ip(mac, ip, IP_PROTO_ICMP, icmp, sizeof(icmp));
    for (int i = 0; i < 8 && !w.ok; i++) net_poll(ping_cb, &w, 80000);
    if (!w.ok) {
        set_error("icmp timeout");
        return -1;
    }
    return 0;
}

/**
 * @brief 将 DNS 域名编码为 DNS 报文中的标签格式（长度前缀 + 数据 + 终止零）
 * @param name 域名字符串（如 "example.com"）
 * @param out 输出缓冲区
 * @param cap 输出缓冲区容量
 * @return 编码后字节数，-1 表示缓冲区不足或域名过长
 */
static int dns_encode(const char *name, uint8_t *out, uint32_t cap) {
    uint32_t pos = 0, lab = 0, lab_pos = 0;
    while (name[pos]) {
        if (lab_pos >= cap) return -1;
        uint32_t len_pos = lab_pos++;
        uint8_t len = 0;
        while (name[pos] && name[pos] != '.') {
            if (lab_pos >= cap || len >= 63) return -1;
            out[lab_pos++] = (uint8_t)name[pos++];
            len++;
        }
        out[len_pos] = len;
        if (name[pos] == '.') pos++;
        lab++;
    }
    if (lab == 0 || lab_pos >= cap) return -1;
    out[lab_pos++] = 0;
    return (int)lab_pos;
}

/** @brief DNS 解析等待上下文 */
typedef struct { uint16_t id; uint32_t ip; int found; } dns_wait_t;

/**
 * @brief DNS 响应接收回调，解析 DNS 应答报文提取 A 记录对应的 IPv4 地址
 * @param pkt 接收到的原始数据包
 * @param len 数据包长度
 * @param arg 指向 dns_wait_t 的指针
 * @return 1 成功解析到目标 A 记录，0 继续轮询
 */
static int dns_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    dns_wait_t *w = arg;
    if (len < 60) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_IP) return 0;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    if (ip->proto != IP_PROTO_UDP || ip->dst != primary.ip) return 0;
    const udp_hdr_t *udp = (const udp_hdr_t *)((const uint8_t *)ip + ((ip->ver_ihl & 0x0F) * 4));
    const uint8_t *dns = (const uint8_t *)udp + sizeof(udp_hdr_t);
    uint16_t ulen = ntohs(udp->len);
    if (ulen < sizeof(udp_hdr_t) + 12) return 0;
    uint16_t dns_len = (uint16_t)(ulen - sizeof(udp_hdr_t));
    if (ntohs(*(const uint16_t *)dns) != w->id) return 0;
    uint16_t qd = ntohs(*(const uint16_t *)(dns + 4));
    uint16_t an = ntohs(*(const uint16_t *)(dns + 6));
    uint32_t pos = 12;
    for (uint16_t q = 0; q < qd; q++) {
        while (pos < dns_len && dns[pos]) pos += dns[pos] + 1;
        if (pos + 5 > dns_len) return 0;
        pos += 5;
    }
    for (uint16_t a = 0; a < an && pos + 12 <= dns_len; a++) {
        if ((dns[pos] & 0xC0) == 0xC0) pos += 2;
        else { while (pos < dns_len && dns[pos]) pos += dns[pos] + 1; pos++; }
        if (pos + 10 > dns_len) return 0;
        uint16_t type = ntohs(*(const uint16_t *)(dns + pos)); pos += 2;
        pos += 6;
        uint16_t rdlen = ntohs(*(const uint16_t *)(dns + pos)); pos += 2;
        if (type == 1 && rdlen == 4 && pos + 4 <= dns_len) {
            memcpy(&w->ip, dns + pos, 4);
            w->found = 1;
            return 1;
        }
        pos += rdlen;
    }
    return 0;
}

/**
 * @brief 通过 DNS 协议解析域名为 IPv4 地址
 * @param name 域名字符串
 * @param out_ip 输出解析到的 IPv4 地址（网络字节序）
 * @return 0 成功，-1 失败（参数无效或解析超时）
 */
int net_dns_resolve(const char *name, uint32_t *out_ip) {
    if (!name || !out_ip) {
        set_error("bad dns query");
        return -1;
    }
    uint32_t literal = net_parse_ipv4(name);
    if (literal) { *out_ip = literal; return 0; }
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    uint8_t mac[6];
    if (!primary.dns) {
        set_error("dns server missing");
        return -1;
    }
    uint32_t next_hop;
    if (net_route_next_hop(primary.dns, &next_hop) < 0) return -1;
    if (arp_resolve(next_hop, mac) < 0) return -1;
    uint8_t msg[300];
    memset(msg, 0, sizeof(msg));
    uint16_t id = 0x4248;
    *(uint16_t *)(msg + 0) = htons(id);
    *(uint16_t *)(msg + 2) = htons(0x0100);
    *(uint16_t *)(msg + 4) = htons(1);
    int qn = dns_encode(name, msg + 12, sizeof(msg) - 16);
    if (qn < 0) {
        set_error("bad dns name");
        return -1;
    }
    uint32_t len = 12 + (uint32_t)qn;
    *(uint16_t *)(msg + len) = htons(1); len += 2;
    *(uint16_t *)(msg + len) = htons(1); len += 2;
    dns_wait_t w = {id, 0, 0};
    send_udp_raw(mac, primary.ip, primary.dns, next_port++, DNS_PORT, msg, (uint16_t)len);
    for (int i = 0; i < 8 && !w.found; i++) net_poll(dns_cb, &w, 80000);
    if (!w.found) {
        set_error("dns timeout");
        return -1;
    }
    *out_ip = w.ip;
    return 0;
}

/**
 * @brief TCP 连接等待上下文，用于三次握手和数据收发过程中的状态跟踪
 */
typedef struct {
    uint32_t peer;                 /**< 对端 IP 地址 */
    uint32_t ack;                  /**< 期望接收的确认号 */
    uint32_t seq_seen;             /**< 最近一次看到的对端序列号 */
    uint32_t want_ack;             /**< 发送后期望收到的 ACK 序号 */
    uint16_t sport;                /**< 本端源端口 */
    int synack;                    /**< 是否收到 SYN+ACK */
    int done;                      /**< 对端是否发送 FIN（连接结束） */
    int rst;                       /**< 是否收到 RST */
    int need_ack;                  /**< 是否需要发送 ACK */
    int acked;                     /**< 发送的数据是否已被确认 */
    char *out;                     /**< 接收数据输出缓冲区 */
    uint32_t cap;                  /**< 输出缓冲区容量 */
    uint32_t len;                  /**< 已接收数据长度 */
} tcp_wait_t;

/**
 * @brief 构造并发送一个 TCP 数据包
 * @param mac 下一跳 MAC 地址
 * @param dst_ip 目标 IP 地址
 * @param sport 源端口号
 * @param dport 目标端口号
 * @param seq 序列号
 * @param ack 确认号
 * @param flags TCP 标志位（SYN=0x02, ACK=0x10, FIN=0x01, RST=0x04 等）
 * @param data 载荷数据（可为 NULL）
 * @param dlen 载荷长度
 * @return 0 成功，-1 失败
 */
static int send_tcp(const uint8_t mac[6], uint32_t dst_ip, uint16_t sport, uint16_t dport,
                    uint32_t seq, uint32_t ack, uint8_t flags, const void *data, uint16_t dlen) {
    uint8_t frame[1514];
    make_eth(frame, mac, ETH_TYPE_IP);
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    tcp_hdr_t *tcp = (tcp_hdr_t *)(frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    uint8_t *payload = (uint8_t *)tcp + 20;
    if (dlen) memcpy(payload, data, dlen);
    ip->ver_ihl = 0x45; ip->tos = 0; ip->len = htons((uint16_t)(20 + 20 + dlen));
    ip->id = htons(ip_id++); ip->frag = htons(0x4000); ip->ttl = 64; ip->proto = IP_PROTO_TCP;
    ip->src = primary.ip; ip->dst = dst_ip; ip->csum = 0; ip->csum = checksum(ip, 20);
    tcp->src = htons(sport); tcp->dst = htons(dport); tcp->seq = htonl(seq); tcp->ack = htonl(ack);
    tcp->off_flags_hi = 5 << 4; tcp->flags = flags; tcp->win = htons(4096);
    tcp->csum = 0; tcp->urg = 0; tcp->csum = tcp_checksum(ip, tcp, payload, dlen);
    return e1000_send(frame, (uint16_t)(sizeof(eth_hdr_t) + 20 + 20 + dlen));
}

/**
 * @brief TCP 数据包接收回调，处理 SYN+ACK、数据段、FIN、RST 等
 * @param pkt 接收到的原始数据包
 * @param len 数据包长度
 * @param arg 指向 tcp_wait_t 的指针
 * @return 1 有意义的包已处理，0 继续轮询
 */
static int tcp_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    tcp_wait_t *w = arg;
    if (len < sizeof(eth_hdr_t) + 40) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_IP) return 0;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    if (ip->proto != IP_PROTO_TCP || ip->src != w->peer || ip->dst != primary.ip) return 0;
    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)((const uint8_t *)ip + ihl);
    if (ntohs(tcp->dst) != w->sport) return 0;
    uint8_t flags = tcp->flags;
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ip_len = ntohs(ip->len);
    uint32_t thl = (tcp->off_flags_hi >> 4) * 4;
    const uint8_t *data = (const uint8_t *)tcp + thl;
    uint32_t dlen = ip_len > ihl + thl ? ip_len - ihl - thl : 0;
    if (flags & 0x04) { w->rst = 1; return 1; }
    if ((flags & 0x12) == 0x12 && !w->synack) {
        w->ack = seq + 1; w->synack = 1; return 1;
    }
    if ((flags & 0x10) && !dlen && (!w->want_ack || ntohl(tcp->ack) >= w->want_ack)) {
        w->acked = 1;
    }
    if (dlen && seq == w->ack) {
        w->seq_seen = seq;
        uint32_t copy = dlen;
        if (w->len + copy > w->cap) copy = w->cap - w->len;
        if (copy) memcpy(w->out + w->len, data, copy);
        w->len += copy;
        w->ack += dlen;
        w->need_ack = 1;
        if (flags & 0x01) {
            w->ack++;
            w->done = 1;
        }
        return 1;
    }
    if (dlen && seq < w->ack) {
        w->need_ack = 1;
        return 1;
    }
    if (flags & 0x01) { w->ack = seq + 1; w->done = 1; return 1; }
    return 0;
}

/**
 * @brief 发起 TCP 三次握手，建立到指定 IP:port 的连接
 * @param ip 目标 IP 地址（网络字节序）
 * @param port 目标端口号
 * @param conn 输出 TCP 连接结构体
 * @return 0 成功，-1 失败（超时或被 RST）
 */
int net_tcp_connect(uint32_t ip, uint16_t port, net_tcp_conn_t *conn) {
    if (!conn || port == 0) {
        set_error("bad tcp connect");
        return -1;
    }
    memset(conn, 0, sizeof(*conn));
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    uint32_t next_hop;
    if (net_route_next_hop(ip, &next_hop) < 0) return -1;
    if (arp_resolve(next_hop, conn->mac) < 0) return -1;
    conn->sport = next_port++;
    conn->dport = port;
    conn->peer = ip;
    conn->seq = 0x10000000U + conn->sport;

    tcp_wait_t w;
    memset(&w, 0, sizeof(w));
    w.peer = ip;
    w.sport = conn->sport;
    for (int attempt = 0; attempt < 3 && !w.synack && !w.rst; attempt++) {
        send_tcp(conn->mac, ip, conn->sport, port, conn->seq, 0, 0x02, 0, 0);
        for (int i = 0; i < 8 && !w.synack && !w.rst; i++) net_poll(tcp_cb, &w, 80000);
    }
    if (!w.synack || w.rst) {
        set_error(w.rst ? "tcp reset" : "tcp connect timeout");
        return -1;
    }
    conn->seq++;
    conn->ack = w.ack;
    if (send_tcp(conn->mac, ip, conn->sport, port, conn->seq, conn->ack, 0x10, 0, 0) < 0)
        return -1;
    conn->open = true;
    return 0;
}

/**
 * @brief 通过已建立的 TCP 连接发送数据
 * @param conn TCP 连接结构体
 * @param data 待发送数据
 * @param len 数据长度（不超过 TCP_MSS）
 * @return 0 成功，-1 失败
 */
int net_tcp_send(net_tcp_conn_t *conn, const uint8_t *data, uint32_t len) {
    if (!conn || !conn->open || !data || len == 0 || len > TCP_MSS) {
        set_error("bad tcp send");
        return -1;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        tcp_wait_t w;
        memset(&w, 0, sizeof(w));
        w.peer = conn->peer;
        w.sport = conn->sport;
        w.want_ack = conn->seq + len;
        if (conn->rx_len < NET_TCP_RXBUF_SIZE) {
            w.out = (char *)conn->rx_buf + conn->rx_len;
            w.cap = NET_TCP_RXBUF_SIZE - conn->rx_len;
        }
        if (send_tcp(conn->mac, conn->peer, conn->sport, conn->dport,
                     conn->seq, conn->ack, 0x18, data, (uint16_t)len) < 0)
            return -1;
        for (int i = 0; i < 8 && !w.rst; i++) {
            net_poll(tcp_cb, &w, 80000);
            if (w.acked) {
                conn->seq += len;
                return 0;
            }
            if (w.need_ack) {
                conn->ack = w.ack;
                send_tcp(conn->mac, conn->peer, conn->sport, conn->dport,
                         conn->seq + len, conn->ack, 0x10, 0, 0);
                w.need_ack = 0;
            }
        }
        if (w.len) conn->rx_len += w.len;
        if (w.rst) {
            set_error("tcp reset");
            conn->open = false;
            return -1;
        }
    }
    conn->seq += len;
    return 0;
}

/**
 * @brief 从已建立的 TCP 连接接收数据
 * @param conn TCP 连接结构体
 * @param buf 输出缓冲区
 * @param cap 缓冲区容量
 * @param len 输出实际接收到的字节数
 * @param poll_rounds 最大轮询次数
 * @return 0 成功，-1 连接被 RST
 */
int net_tcp_recv(net_tcp_conn_t *conn, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t poll_rounds) {
    if (!conn || !buf || !len || cap == 0) {
        set_error("bad tcp recv");
        return -1;
    }
    if (conn->rx_len) {
        uint32_t copy = conn->rx_len;
        if (copy > cap) copy = cap;
        memcpy(buf, conn->rx_buf, copy);
        if (copy < conn->rx_len) memmove(conn->rx_buf, conn->rx_buf + copy, conn->rx_len - copy);
        conn->rx_len -= copy;
        *len = copy;
        return 0;
    }
    if (!conn->open) {
        *len = 0;
        return 0;
    }
    tcp_wait_t w;
    uint8_t tmp[1536];
    memset(&w, 0, sizeof(w));
    w.peer = conn->peer;
    w.sport = conn->sport;
    w.ack = conn->ack;
    w.out = (char *)tmp;
    w.cap = sizeof(tmp);
    for (uint32_t i = 0; i < poll_rounds && !w.done && !w.rst; i++) {
        net_poll(tcp_cb, &w, 80000);
        if (w.need_ack) {
            conn->ack = w.ack;
            send_tcp(conn->mac, conn->peer, conn->sport, conn->dport,
                     conn->seq, conn->ack, 0x10, 0, 0);
            w.need_ack = 0;
            if (w.len) break;
        }
    }
    if (w.rst) {
        conn->open = false;
        set_error("tcp reset");
        return -1;
    }
    if (w.done) {
        conn->ack = w.ack;
        send_tcp(conn->mac, conn->peer, conn->sport, conn->dport,
                 conn->seq, conn->ack, 0x10, 0, 0);
        conn->open = false;
    }
    uint32_t copy = w.len;
    if (copy > cap) copy = cap;
    if (copy) memcpy(buf, tmp, copy);
    if (w.len > copy) {
        uint32_t rest = w.len - copy;
        if (rest > NET_TCP_RXBUF_SIZE) rest = NET_TCP_RXBUF_SIZE;
        memcpy(conn->rx_buf, tmp + copy, rest);
        conn->rx_len = rest;
    }
    *len = copy;
    return 0;
}

/**
 * @brief 发送 TCP FIN 包关闭连接
 * @param conn TCP 连接结构体
 */
void net_tcp_close(net_tcp_conn_t *conn) {
    if (!conn || !conn->open) return;
    send_tcp(conn->mac, conn->peer, conn->sport, conn->dport,
             conn->seq, conn->ack, 0x11, 0, 0);
    conn->seq++;
    conn->open = false;
}

/**
 * @brief 一次性完成 TCP 连接、发送请求、接收响应、关闭连接的完整交互
 * @param ip 目标 IP 地址（网络字节序）
 * @param port 目标端口号
 * @param request 请求数据
 * @param request_len 请求长度
 * @param response 响应输出缓冲区
 * @param response_cap 响应缓冲区容量
 * @param response_len 输出实际接收到的响应长度
 * @return 0 成功，-1 失败
 */
int net_tcp_exchange(uint32_t ip, uint16_t port, const uint8_t *request, uint32_t request_len,
                     uint8_t *response, uint32_t response_cap, uint32_t *response_len) {
    if (!request || !request_len || !response || !response_len || response_cap == 0 || port == 0 ||
        request_len > TCP_MSS) {
        set_error("bad tcp exchange");
        return -1;
    }
    net_tcp_conn_t conn;
    if (net_tcp_connect(ip, port, &conn) < 0) return -1;
    if (net_tcp_send(&conn, request, request_len) < 0) {
        net_tcp_close(&conn);
        return -1;
    }
    uint32_t total = 0;
    for (int i = 0; i < 80 && total + 1 < response_cap; i++) {
        uint32_t got = 0;
        if (net_tcp_recv(&conn, response + total, response_cap - total - 1, &got, 4) < 0) break;
        total += got;
        if (!conn.open) break;
    }
    net_tcp_close(&conn);
    response[total < response_cap ? total : response_cap - 1] = 0;
    *response_len = total;
    if (!total) {
        set_error("tcp response timeout");
        return -1;
    }
    return 0;
}

/**
 * @brief 发送 HTTP 请求，支持自定义方法（GET/POST 等）
 * @param method HTTP 方法字符串
 * @param host 主机名（用于 Host 头）
 * @param ip 目标 IP 地址（网络字节序）
 * @param port 目标端口号
 * @param path 请求路径
 * @param out 响应输出缓冲区
 * @param out_cap 缓冲区容量
 * @param out_len 输出实际响应长度
 * @return 0 成功，-1 失败
 */
int net_http_request(const char *method, const char *host, uint32_t ip, uint16_t port,
                     const char *path, char *out, uint32_t out_cap, uint32_t *out_len) {
    if (!method || !host || !path || !out || !out_len || out_cap == 0 || port == 0) {
        set_error("bad http request");
        return -1;
    }
    char req[512];
    uint32_t n = 0;
    const char *b = " HTTP/1.0\r\nHost: ";
    const char *c = "\r\nConnection: close\r\nUser-Agent: HBOS/0.1\r\n\r\n";
    for (const char *p = method; *p && n < sizeof(req); p++) req[n++] = *p;
    if (n < sizeof(req)) req[n++] = ' ';
    for (const char *p = path; *p && n < sizeof(req); p++) req[n++] = *p;
    for (const char *p = b; *p && n < sizeof(req); p++) req[n++] = *p;
    for (const char *p = host; *p && n < sizeof(req); p++) req[n++] = *p;
    for (const char *p = c; *p && n < sizeof(req); p++) req[n++] = *p;
    return net_tcp_exchange(ip, port, (const uint8_t *)req, n, (uint8_t *)out, out_cap, out_len);
}

/**
 * @brief 发送 HTTP GET 请求的便捷封装
 * @param host 主机名
 * @param ip 目标 IP 地址（网络字节序）
 * @param port 目标端口号
 * @param path 请求路径
 * @param out 响应输出缓冲区
 * @param out_cap 缓冲区容量
 * @param out_len 输出实际响应长度
 * @return 0 成功，-1 失败
 */
int net_http_get(const char *host, uint32_t ip, uint16_t port, const char *path,
                 char *out, uint32_t out_cap, uint32_t *out_len) {
    return net_http_request("GET", host, ip, port, path, out, out_cap, out_len);
}

/**
 * @brief 将点分十进制 IPv4 字符串解析为网络字节序的 32 位整数
 * @param s 点分十进制字符串（如 "192.168.1.1"）
 * @return 网络字节序的 IPv4 地址，格式无效时返回 0
 */
uint32_t net_parse_ipv4(const char *s) {
    if (!s) return 0;
    uint32_t parts[4] = {0,0,0,0};
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') {
            parts[i] = parts[i] * 10 + (uint32_t)(*s++ - '0');
            if (parts[i] > 255) return 0;
        }
        if (i < 3) { if (*s++ != '.') return 0; }
    }
    if (*s) return 0;
    return htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
}

/**
 * @brief 将网络字节序的 32 位 IPv4 地址转换为点分十进制字符串
 * @param ip IPv4 地址（网络字节序）
 * @param out 输出缓冲区（至少 16 字节）
 */
void net_ipv4_to_str(uint32_t ip, char out[16]) {
    uint32_t h = ntohl(ip);
    uint8_t p[4] = {(uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) {
        char tmp[3]; int t = 0;
        do { tmp[t++] = (char)('0' + (p[i] % 10)); p[i] /= 10; } while (p[i]);
        while (t--) out[n++] = tmp[t];
        if (i != 3) out[n++] = '.';
    }
    out[n] = 0;
}

static uint16_t listen_port;
static uint32_t listen_peer;
static uint8_t  listen_mac[6];
static uint16_t listen_sport;
static uint32_t listen_seq;
static int      listen_syn_acked;
static int      listen_established;

static int accept_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    (void)arg;
    if (len < sizeof(eth_hdr_t) + 40) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_IP) return 0;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    if (ip->proto != IP_PROTO_TCP || ip->dst != primary.ip) return 0;
    uint32_t ihl = (ip->ver_ihl & 0x0F) * 4;
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)((const uint8_t *)ip + ihl);
    if (ntohs(tcp->dst) != listen_port) return 0;
    uint8_t flags = tcp->flags;
    uint32_t seq = ntohl(tcp->seq);

    if ((flags & 0x02) && !(flags & 0x10) && !listen_syn_acked) {
        listen_peer  = ip->src;
        listen_sport = ntohs(tcp->src);
        listen_seq   = seq + 1;
        memcpy(listen_mac, eth->src, 6);
        listen_syn_acked = 1;
        return 1;
    }

    if ((flags & 0x10) && listen_syn_acked) {
        listen_established = 1;
        return 1;
    }

    if (flags & 0x04) {
        listen_syn_acked = 0;
        return 1;
    }

    return 0;
}

int net_tcp_listen(uint16_t port) {
    if (port == 0) return -1;
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    listen_port        = port;
    listen_syn_acked   = 0;
    listen_established = 0;
    return 0;
}

int net_tcp_accept(uint16_t port, net_tcp_conn_t *conn,
                   uint32_t timeout_ms) {
    if (!conn || port == 0) {
        set_error("bad tcp accept");
        return -1;
    }
    memset(conn, 0, sizeof(*conn));
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    if (listen_port != port || !listen_syn_acked) return -1;

    listen_established = 0;

    send_tcp(listen_mac, listen_peer, port, listen_sport,
             next_port, listen_seq, 0x12, 0, 0);

    uint32_t deadline_ms = 0;
    {
        uint32_t elapsed = 0;
        for (int i = 0; i < (int)(timeout_ms / 10) + 10 && !listen_established; i++) {
            net_poll(accept_cb, NULL, 10000);
            elapsed += 10;
            if (elapsed >= timeout_ms) break;
        }
    }
    (void)deadline_ms;

    if (!listen_established) {
        listen_syn_acked = 0;
        set_error("tcp accept timeout");
        return -1;
    }

    conn->peer  = listen_peer;
    conn->sport = next_port;
    conn->dport = listen_sport;
    conn->seq   = next_port + 1;
    conn->ack   = listen_seq;
    memcpy(conn->mac, listen_mac, 6);
    conn->open  = true;

    listen_syn_acked = 0;
    return 0;
}
