/**
 * @file net.c
 * @brief 网络子系统实现，基于 Intel E1000 网卡，支持 DHCP、ARP、ICMP ping、DNS、TCP、HTTP 等协议
 */

#include "net.h"
#include "pci.h"
#include "string.h"
#include "core/cpu.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "core/task.h"
#include "rtc_tz.h"

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
/** @brief 默认公共 DNS 服务器（8.8.8.8），当链路未提供 DNS 时兜底使用。
 *  四个八位组都是 8，网络字节序与主机字节序相同，可直接用字面量。 */
#define DNS_FALLBACK_SERVER 0x08080808u
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
    if (vendor == 0x1022 && device == 0x2000) return NET_DRIVER_PCNET;
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
        case NET_DRIVER_PCNET: return "AMD PCnet";
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
/* E1000 poll helper — called via dispatch */
static int e1000_poll(packet_cb_t cb, void *arg, uint32_t spins) {
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

static int pcnet_poll(packet_cb_t cb, void *arg, uint32_t spins);
static int rtl8139_poll(packet_cb_t cb, void *arg, uint32_t spins);

static int net_poll(packet_cb_t cb, void *arg, uint32_t spins) {
    switch (primary.driver) {
    case NET_DRIVER_E1000:  return e1000_poll(cb, arg, spins);
    case NET_DRIVER_RTL8139: return rtl8139_poll(cb, arg, spins);
    case NET_DRIVER_PCNET:  return pcnet_poll(cb, arg, spins);
    default: return -1;
    }
}

int net_poll_frames(net_frame_callback_t callback, void *context,
                    uint32_t spins) {
    net_init();
    return net_poll(callback, context, spins);
}

/** PIT 固定以 100 Hz 初始化；将毫秒超时转换为不会提前结束的 tick 截止点。 */
static uint64_t net_deadline_after_ms(uint32_t timeout_ms) {
    uint64_t ticks = ((uint64_t)timeout_ms + 9U) / 10U;
    if (!ticks) ticks = 1;
    return pit_get_ticks() + ticks;
}

static int net_before_deadline(uint64_t deadline) {
    return (int64_t)(pit_get_ticks() - deadline) < 0;
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
    return primary.send(frame, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + plen));
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
    return primary.send(frame, (uint16_t)(sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + plen));
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
    return primary.send(frame, sizeof(frame));
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
    for (int attempt = 0; attempt < 3 && !w.found; attempt++) {
        if (send_arp(1, ip, 0) < 0) break;
        uint64_t deadline = net_deadline_after_ms(500);
        while (!w.found && net_before_deadline(deadline)) {
            net_poll(arp_cb, &w, 4096);
            task_yield();
        }
    }
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
    primary.link_ready = true; /* Force link ready (VirtualBox/QEMU LU status bit can be 0 initially during reset) */
}

/* ================================================================
 * Realtek RTL8139 driver — Linux 8139too-compatible polling path
 * ================================================================ */

#define RTL8139_RX_RING_SIZE 8192U
#define RTL8139_RX_ALLOC_SIZE (RTL8139_RX_RING_SIZE + 16U + PKT_SIZE)
#define RTL8139_TX_SLOTS 4U

#define RTL_MAC0       0x00
#define RTL_TX_STATUS0 0x10
#define RTL_TX_ADDR0   0x20
#define RTL_RX_BUF     0x30
#define RTL_CHIP_CMD   0x37
#define RTL_RX_BUF_PTR 0x38
#define RTL_INTR_MASK  0x3C
#define RTL_INTR_STAT  0x3E
#define RTL_TX_CONFIG  0x40
#define RTL_RX_CONFIG  0x44
#define RTL_CONFIG1    0x52
#define RTL_MEDIA_STAT 0x58

#define RTL_CMD_RESET  0x10
#define RTL_CMD_RX_EN  0x08
#define RTL_CMD_TX_EN  0x04
#define RTL_RX_EMPTY   0x01

#define RTL_RX_OK      0x0001
#define RTL_RX_BAD_ALIGN 0x0002
#define RTL_RX_CRC_ERR 0x0004
#define RTL_RX_TOO_LONG 0x0008
#define RTL_RX_RUNT    0x0010
#define RTL_RX_BAD_SYMBOL 0x0020
#define RTL_RX_ERROR_MASK (RTL_RX_BAD_ALIGN | RTL_RX_CRC_ERR | \
                           RTL_RX_TOO_LONG | RTL_RX_RUNT | RTL_RX_BAD_SYMBOL)

#define RTL_TX_HOST_OWNS 0x00002000U
#define RTL_TX_OK        0x00008000U
#define RTL_TX_OUT_OF_WINDOW 0x20000000U
#define RTL_TX_ABORTED   0x40000000U
#define RTL_TX_CARRIER_LOST 0x80000000U
#define RTL_TX_ERROR_MASK (RTL_TX_OUT_OF_WINDOW | RTL_TX_ABORTED | \
                           RTL_TX_CARRIER_LOST)

#define RTL_ISR_RX_OK       0x0001
#define RTL_ISR_RX_ERR      0x0002
#define RTL_ISR_TX_ERR      0x0008
#define RTL_ISR_RX_OVERFLOW 0x0010
#define RTL_ISR_RX_FIFO_OVER 0x0040

static uint16_t rtl8139_iobase;
static uint32_t rtl8139_rx_offset;
static uint8_t rtl8139_tx_slot;
static uint8_t rtl8139_rx_ring[RTL8139_RX_ALLOC_SIZE]
    __attribute__((aligned(16)));
static uint8_t rtl8139_tx_buf[RTL8139_TX_SLOTS][PKT_SIZE]
    __attribute__((aligned(16)));

static inline uint8_t rtl_in8(uint16_t reg) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value)
                     : "Nd"((uint16_t)(rtl8139_iobase + reg)));
    return value;
}

static inline uint16_t rtl_in16(uint16_t reg) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value)
                     : "Nd"((uint16_t)(rtl8139_iobase + reg)));
    return value;
}

static inline uint32_t rtl_in32(uint16_t reg) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value)
                     : "Nd"((uint16_t)(rtl8139_iobase + reg)));
    return value;
}

static inline void rtl_out8(uint16_t reg, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value),
                     "Nd"((uint16_t)(rtl8139_iobase + reg)));
}

static inline void rtl_out16(uint16_t reg, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value),
                     "Nd"((uint16_t)(rtl8139_iobase + reg)));
}

static inline void rtl_out32(uint16_t reg, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value),
                     "Nd"((uint16_t)(rtl8139_iobase + reg)));
}

/* 8139too uses a read-back after important writes to flush posted I/O. */
static inline void rtl_out8_flush(uint16_t reg, uint8_t value) {
    rtl_out8(reg, value);
    (void)rtl_in8(reg);
}

static int rtl8139_link_up(void) {
    return (rtl_in8(RTL_MEDIA_STAT) & 0x04U) == 0;
}

static int rtl8139_reset(void) {
    rtl_out8_flush(RTL_CHIP_CMD, RTL_CMD_RESET);
    for (uint32_t i = 0; i < 2000000U; i++) {
        if (!(rtl_in8(RTL_CHIP_CMD) & RTL_CMD_RESET)) break;
        if (i + 1U == 2000000U) {
            set_error("rtl8139 reset timeout");
            return -1;
        }
        __asm__ volatile("pause");
    }

    memset(rtl8139_rx_ring, 0, sizeof(rtl8139_rx_ring));
    memset(rtl8139_tx_buf, 0, sizeof(rtl8139_tx_buf));
    rtl8139_rx_offset = 0;
    rtl8139_tx_slot = 0;

    rtl_out32(RTL_RX_BUF, (uint32_t)(uintptr_t)rtl8139_rx_ring);
    for (uint32_t i = 0; i < RTL8139_TX_SLOTS; i++) {
        rtl_out32((uint16_t)(RTL_TX_ADDR0 + i * 4U),
                  (uint32_t)(uintptr_t)rtl8139_tx_buf[i]);
    }

    /* HBOS polls, so keep IRQs masked and acknowledge stale status. */
    rtl_out16(RTL_INTR_MASK, 0);
    rtl_out16(RTL_INTR_STAT, 0xFFFFU);
    rtl_out8_flush(RTL_CHIP_CMD, RTL_CMD_RX_EN | RTL_CMD_TX_EN);

    /*
     * 8 KiB ring, WRAP, 1024-byte DMA burst, no promiscuous mode:
     * accept own unicast, multicast and broadcast frames.
     */
    rtl_out32(RTL_RX_CONFIG, (7U << 13) | (6U << 8) |
                              (1U << 7) | 0x0EU);
    rtl_out32(RTL_TX_CONFIG, (3U << 24) | (6U << 8));
    __sync_synchronize();
    primary.link_ready = rtl8139_link_up();
    return 0;
}

static int rtl8139_init_hw(const pci_device_t *pdev) {
    if (!primary.bar0_io || !primary.bar0_base ||
        primary.bar0_base > 0xFFFFU) {
        set_error("rtl8139 requires I/O BAR0");
        return -1;
    }
    rtl8139_iobase = (uint16_t)primary.bar0_base;

    uint32_t cmd = pci_read32(pdev->bus, pdev->slot, pdev->func, 0x04);
    pci_write32(pdev->bus, pdev->slot, pdev->func, 0x04, cmd | 0x0005U);
    rtl_out8(RTL_CONFIG1, 0);

    if (rtl8139_reset() < 0) return -1;
    for (int i = 0; i < 6; i++) primary.mac[i] = rtl_in8(RTL_MAC0 + i);
    primary.mac_valid = (primary.mac[0] | primary.mac[1] | primary.mac[2] |
                         primary.mac[3] | primary.mac[4] | primary.mac[5]) != 0;
    if (!primary.mac_valid) {
        set_error("rtl8139 invalid MAC");
        primary.link_ready = false;
        return -1;
    }
    last_error = "ok";
    return 0;
}

static int rtl8139_recover(const char *reason) {
    primary.driver_resets++;
    if (rtl8139_reset() < 0) return -1;
    set_error(reason);
    return 0;
}

static int rtl8139_send(const void *frame, uint16_t len) {
    if (!frame || len > PKT_SIZE || len < sizeof(eth_hdr_t)) {
        primary.tx_errors++;
        set_error("rtl8139 invalid frame");
        return -1;
    }
    primary.link_ready = rtl8139_link_up();
    if (!primary.link_ready) {
        primary.tx_errors++;
        set_error("rtl8139 link down");
        return -1;
    }

    uint8_t slot = rtl8139_tx_slot;
    uint16_t status_reg = (uint16_t)(RTL_TX_STATUS0 + slot * 4U);
    uint32_t status = rtl_in32(status_reg);
    uint32_t wait;
    for (wait = 0; !(status & RTL_TX_HOST_OWNS) && wait < 2000000U; wait++) {
        status = rtl_in32(status_reg);
        __asm__ volatile("pause");
    }
    if (!(status & RTL_TX_HOST_OWNS)) {
        primary.tx_errors++;
        primary.tx_timeouts++;
        (void)rtl8139_recover("rtl8139 tx timeout");
        return -1;
    }
    if (status & RTL_TX_ERROR_MASK) {
        primary.tx_errors++;
        (void)rtl8139_recover("rtl8139 tx error");
        return -1;
    }

    uint16_t wire_len = len < 60U ? 60U : len;
    memcpy(rtl8139_tx_buf[slot], frame, len);
    if (wire_len > len) memset(rtl8139_tx_buf[slot] + len, 0, wire_len - len);
    __sync_synchronize();
    rtl_out32(status_reg, wire_len);
    rtl8139_tx_slot = (uint8_t)((slot + 1U) % RTL8139_TX_SLOTS);
    primary.tx_packets++;
    primary.tx_bytes += wire_len;
    last_error = "ok";
    return 0;
}

static int rtl8139_poll(packet_cb_t cb, void *arg, uint32_t spins) {
    primary.link_ready = rtl8139_link_up();
    if (!primary.link_ready) return -1;

    for (uint32_t s = 0; s < spins; s++) {
        uint16_t isr = rtl_in16(RTL_INTR_STAT);
        if (isr) rtl_out16(RTL_INTR_STAT, isr);
        if (isr & (RTL_ISR_RX_ERR | RTL_ISR_RX_OVERFLOW |
                   RTL_ISR_RX_FIFO_OVER)) {
            primary.rx_errors++;
            primary.rx_dropped++;
            (void)rtl8139_recover("rtl8139 rx overflow");
            return 0;
        }
        if (isr & RTL_ISR_TX_ERR) primary.tx_errors++;
        if (rtl_in8(RTL_CHIP_CMD) & RTL_RX_EMPTY) continue;

        uint8_t *packet = rtl8139_rx_ring + rtl8139_rx_offset;
        uint16_t status = *(volatile uint16_t *)packet;
        uint16_t rx_len = *(volatile uint16_t *)(packet + 2);
        if (!(status & RTL_RX_OK) || (status & RTL_RX_ERROR_MASK) ||
            rx_len < sizeof(eth_hdr_t) + 4U || rx_len > PKT_SIZE + 4U) {
            primary.rx_errors++;
            primary.rx_dropped++;
            (void)rtl8139_recover("rtl8139 bad rx frame");
            return 0;
        }

        uint16_t frame_len = (uint16_t)(rx_len - 4U); /* strip Ethernet CRC */
        primary.rx_packets++;
        primary.rx_bytes += frame_len;
        int ret = cb ? cb(packet + 4, frame_len, arg) : 0;

        rtl8139_rx_offset = (rtl8139_rx_offset + rx_len + 4U + 3U) & ~3U;
        rtl8139_rx_offset %= RTL8139_RX_RING_SIZE;
        rtl_out16(RTL_RX_BUF_PTR,
                  (uint16_t)(rtl8139_rx_offset - 16U));
        if (ret) return ret;
    }
    return 0;
}

/* ================================================================
 * AMD PCnet-PCI II (Am79C970A) driver — I/O port based
 * ================================================================ */

static uint32_t pcnet_iobase;

static inline uint16_t pcnet_inw(uint16_t port) {
    uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void pcnet_outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static uint16_t pcnet_read_csr(uint16_t reg) {
    pcnet_outw(pcnet_iobase + 0x12, reg);
    return pcnet_inw(pcnet_iobase + 0x10);
}
static void pcnet_write_csr(uint16_t reg, uint16_t val) {
    pcnet_outw(pcnet_iobase + 0x12, reg);
    pcnet_outw(pcnet_iobase + 0x10, val);
}
static uint16_t pcnet_read_bcr(uint16_t reg) {
    pcnet_outw(pcnet_iobase + 0x12, reg);
    return pcnet_inw(pcnet_iobase + 0x16);
}
static void pcnet_write_bcr(uint16_t reg, uint16_t val) {
    pcnet_outw(pcnet_iobase + 0x12, reg);
    pcnet_outw(pcnet_iobase + 0x16, val);
}

/* ── PCnet descriptor ring ───────────────────────────── */

#define PCNET_TX_COUNT 8
#define PCNET_RX_COUNT 16
#define PCNET_BUF_SIZE 2048

#pragma pack(push, 1)
typedef struct {
    volatile uint32_t base;      /* buffer physical address (16-byte aligned) */
    volatile int16_t  length;    /* negative=dev owns, positive=host owns */
    volatile uint16_t status;    /* TX: 0x0200=STP 0x0100=ENP 0x8000=OWN */
    volatile uint32_t msg_len;   /* reserved / message length */
    volatile uint32_t reserved;  /* Padding to 16 bytes */
} pcnet_desc_t;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    uint16_t mode;       /* 0x0000 */
    uint8_t  rlen;       /* log2(RX descriptors) in bits 7:4 */
    uint8_t  tlen;       /* log2(TX descriptors) in bits 7:4 */
    uint8_t  padr[6];    /* MAC address */
    uint16_t reserved;
    uint8_t  ladrf[8];   /* logical address filter */
    uint32_t rx_ring;    /* RX descriptor ring physical address */
    uint32_t tx_ring;    /* TX descriptor ring physical address */
} pcnet_init_block_t;
#pragma pack(pop)

static pcnet_desc_t *pcnet_tx_desc;
static pcnet_desc_t *pcnet_rx_desc;
static uint8_t *pcnet_tx_buf[PCNET_TX_COUNT];
static uint8_t *pcnet_rx_buf[PCNET_RX_COUNT];
static uint16_t pcnet_tx_tail;

static int pcnet_init_hw(pci_device_t *dev) {
    pcnet_iobase = primary.bar0_base; /* I/O bar, clear flags */

    /* Enable bus mastering (PCI command reg offset 4, bit 2) */
    {
        uint32_t v = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
        pci_write32(dev->bus, dev->slot, dev->func, 0x04, v | 0x0004);
    }

    /* Software Reset by reading RESET register (returns to 16-bit WIO mode) */
    (void)pcnet_inw(pcnet_iobase + 0x14);
    for (volatile int i = 0; i < 10000; i++) __asm__ volatile("" ::: "memory");

    /* Stop chip first to allow writing BCR registers */
    pcnet_write_csr(0, 0x0004);            /* STOP */
    for (volatile int i = 0; i < 10000; i++) __asm__ volatile("" ::: "memory");

    /* Set SWSTYLE to 2 (PCnet-PCI 32-bit style), which also sets SSIZE32 to 1 */
    pcnet_write_bcr(20, 0x0102);

    /* Match pcnet32 defaults: auto-select media, full duplex and auto padding. */
    pcnet_write_bcr(2, pcnet_read_bcr(2) | 0x0002);
    pcnet_write_bcr(9, pcnet_read_bcr(9) | 0x0003);
    pcnet_write_csr(4, 0x0915);

    /* Check if stop bit is set */
    uint16_t csr0 = pcnet_read_csr(0);
    if (!(csr0 & 0x0004)) {                /* STOP must be set */
        set_error("pcnet stop failed");
        return -1;
    }

    /* Read MAC from CSR12-14 */
    for (int i = 0; i < 3; i++) {
        uint16_t v = pcnet_read_csr(12 + (uint16_t)i);
        primary.mac[i * 2]     = (uint8_t)(v & 0xFF);
        primary.mac[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    primary.mac_valid = true;

    /* Allocate descriptor rings (16-byte aligned) */
    size_t tx_ring_sz = PCNET_TX_COUNT * sizeof(pcnet_desc_t);
    size_t rx_ring_sz = PCNET_RX_COUNT * sizeof(pcnet_desc_t);
    pcnet_tx_desc = (pcnet_desc_t *)kmalloc(tx_ring_sz + 16);
    pcnet_rx_desc = (pcnet_desc_t *)kmalloc(rx_ring_sz + 16);
    if (!pcnet_tx_desc || !pcnet_rx_desc) {
        set_error("pcnet ring alloc failed");
        return -1;
    }
    uint64_t tx_phys = ((uint64_t)(uintptr_t)pcnet_tx_desc + 15) & ~15ULL;
    uint64_t rx_phys = ((uint64_t)(uintptr_t)pcnet_rx_desc + 15) & ~15ULL;
    pcnet_tx_desc = (pcnet_desc_t *)(uintptr_t)tx_phys;
    pcnet_rx_desc = (pcnet_desc_t *)(uintptr_t)rx_phys;
    memset(pcnet_tx_desc, 0, tx_ring_sz);
    memset(pcnet_rx_desc, 0, rx_ring_sz);

    /* Allocate packet buffers */
    for (int i = 0; i < PCNET_TX_COUNT; i++) {
        pcnet_tx_buf[i] = (uint8_t *)kmalloc(PCNET_BUF_SIZE);
        if (!pcnet_tx_buf[i]) { set_error("tx buf fail"); return -1; }
    }
    for (int i = 0; i < PCNET_RX_COUNT; i++) {
        pcnet_rx_buf[i] = (uint8_t *)kmalloc(PCNET_BUF_SIZE);
        if (!pcnet_rx_buf[i]) { set_error("rx buf fail"); return -1; }
    }

    /* Set up RX descriptors (give them to the device) */
    for (int i = 0; i < PCNET_RX_COUNT; i++) {
        pcnet_rx_desc[i].base   = (uint32_t)(uintptr_t)pcnet_rx_buf[i];
        pcnet_rx_desc[i].length = (int16_t)(-(int32_t)PCNET_BUF_SIZE);
        pcnet_rx_desc[i].status = 0x8000;  /* OWN */
    }

    /* Build InitBlock (16-byte aligned) */
    pcnet_init_block_t *ib = (pcnet_init_block_t *)kmalloc(sizeof(*ib) + 16);
    if (!ib) { set_error("initblock fail"); return -1; }
    uint64_t ib_phys = ((uint64_t)(uintptr_t)ib + 15) & ~15ULL;
    ib = (pcnet_init_block_t *)(uintptr_t)ib_phys;
    memset(ib, 0, sizeof(*ib));
    ib->mode = 0x0000;
    ib->rlen = 4U << 4; /* 2^4 = 16 descriptors */
    ib->tlen = 3U << 4; /* 2^3 = 8 descriptors */
    memcpy(ib->padr, primary.mac, 6);
    ib->rx_ring = (uint32_t)rx_phys;
    ib->tx_ring = (uint32_t)tx_phys;

    /* Write InitBlock address and issue INIT */
    uint32_t ib_addr = (uint32_t)(uintptr_t)ib;
    pcnet_write_csr(1, (uint16_t)(ib_addr & 0xFFFF));
    pcnet_write_csr(2, (uint16_t)(ib_addr >> 16));
    pcnet_write_csr(0, 0x0001);            /* INIT */
    for (volatile int i = 0; i < 500000; i++) {
        if (pcnet_read_csr(0) & 0x0100) break;  /* IDON */
    }
    if (!(pcnet_read_csr(0) & 0x0100)) {
        set_error("pcnet init timeout");
        return -1;
    }

    /* Start chip (WITHOUT IENA) */
    pcnet_write_csr(0, 0x0002);            /* STRT */
    pcnet_tx_tail = 0;
    primary.link_ready = true;
    return 0;
}

static int pcnet_send(const void *frame, uint16_t len) {
    if (!primary.link_ready || len > PCNET_BUF_SIZE) {
        primary.tx_errors++;
        return -1;
    }
    if (!(pcnet_read_csr(0) & 0x0010)) {
        primary.tx_errors++;
        set_error("pcnet tx engine stopped");
        return -1;
    }
    uint16_t idx = pcnet_tx_tail;
    /* Wait for device to finish with this descriptor */
    if (pcnet_tx_desc[idx].status & 0x8000) {
        primary.tx_errors++;
        return -1;
    }
    memcpy(pcnet_tx_buf[idx], frame, len);
    pcnet_tx_desc[idx].length = (int16_t)(-(int32_t)len);
    pcnet_tx_desc[idx].msg_len = 0;
    pcnet_tx_desc[idx].reserved = 0;
    __sync_synchronize();
    pcnet_tx_desc[idx].status = 0x8300;    /* OWN | STP | ENP */
    __sync_synchronize();
    pcnet_tx_tail = (uint16_t)((idx + 1) % PCNET_TX_COUNT);
    /* Demand transmit (WITHOUT IENA) */
    pcnet_write_csr(0, 0x0008);
    for (uint32_t wait = 0; pcnet_tx_desc[idx].status & 0x8000; wait++) {
        if (wait >= 2000000U) {
            primary.tx_errors++;
            primary.tx_timeouts++;
            set_error("pcnet tx timeout");
            return -1;
        }
        __asm__ volatile("pause");
    }
    if (pcnet_tx_desc[idx].status & 0x4000) {
        primary.tx_errors++;
        set_error("pcnet tx error");
        return -1;
    }
    primary.tx_packets++;
    primary.tx_bytes += len;
    return 0;
}

static int pcnet_poll(packet_cb_t cb, void *arg, uint32_t spins) {
    if (!primary.link_ready) return -1;
    for (uint32_t s = 0; s < spins; s++) {
        for (int i = 0; i < PCNET_RX_COUNT; i++) {
            pcnet_desc_t *d = &pcnet_rx_desc[i];
            if (d->status & 0x8000) continue;  /* device still owns */
            uint16_t status = d->status;
            uint16_t rlen = (uint16_t)(d->msg_len & 0xFFF);
            int ret = 0;
            if (!(status & 0x4000) && (status & 0x0300) == 0x0300 &&
                rlen >= sizeof(eth_hdr_t) + 4U && rlen <= PCNET_BUF_SIZE) {
                rlen = (uint16_t)(rlen - 4U); /* strip Ethernet CRC */
                primary.rx_packets++;
                primary.rx_bytes += rlen;
                if (cb) ret = cb(pcnet_rx_buf[i], rlen, arg);
            } else {
                primary.rx_errors++;
                primary.rx_dropped++;
            }
            /* Return descriptor to device */
            d->length = (int16_t)(-(int32_t)PCNET_BUF_SIZE);
            d->status = 0x8000;
            if (ret) return ret;
        }
    }
    return 0;
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
    /* Dispatch to driver */
    if (primary.driver == NET_DRIVER_E1000) {
        primary.send = e1000_send;
        e1000_init_hw(&dev);
    } else if (primary.driver == NET_DRIVER_RTL8139) {
        primary.send = rtl8139_send;
        if (rtl8139_init_hw(&dev) < 0) primary.send = 0;
    } else if (primary.driver == NET_DRIVER_PCNET) {
        primary.send = pcnet_send;
        if (pcnet_init_hw(&dev) < 0) primary.send = 0;
    } else {
        set_error("no driver for this NIC");
        return;
    }
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
    for (int attempt = 0; attempt < 3 && !w.found; attempt++) {
        if (send_dhcp(1, xid, 0, 0) < 0) break;
        uint64_t deadline = net_deadline_after_ms(1000);
        while (!w.found && net_before_deadline(deadline)) {
            net_poll(dhcp_cb, &w, 4096);
            task_yield();
        }
    }
    if (!w.found || w.type != 2) {
        set_error("dhcp discover timeout");
        return -1;
    }
    uint32_t offer = w.yiaddr, server = w.server;
    memset(&w, 0, sizeof(w)); w.xid = xid;
    for (int attempt = 0; attempt < 3 && !w.found; attempt++) {
        if (send_dhcp(3, xid, offer, server) < 0) break;
        uint64_t deadline = net_deadline_after_ms(1000);
        while (!w.found && net_before_deadline(deadline)) {
            net_poll(dhcp_cb, &w, 4096);
            task_yield();
        }
    }
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
 * @param timeout_ms 超时时间（毫秒）
 * @return 0 成功，-1 超时或网络未配置
 */
int net_ping(uint32_t ip, uint32_t timeout_ms) {
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
    if (send_ip(mac, ip, IP_PROTO_ICMP, icmp, sizeof(icmp)) < 0) return -1;
    uint64_t deadline = net_deadline_after_ms(timeout_ms ? timeout_ms : 1000U);
    while (!w.ok && net_before_deadline(deadline)) {
        net_poll(ping_cb, &w, 4096);
        task_yield();
    }
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
    /* 链路未提供 DNS（DHCP 未下发或静态配置留空）时，兜底用公共 DNS 8.8.8.8，
     * 这样"网络未配置 DNS"也能解析域名，而不是直接报错。 */
    uint32_t dns_server = primary.dns ? primary.dns : DNS_FALLBACK_SERVER;
    uint32_t next_hop;
    if (net_route_next_hop(dns_server, &next_hop) < 0) return -1;
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

    for (int attempt = 0; attempt < 3; attempt++) {
        if (next_port < 49152) next_port = 49152;
        uint16_t sport = next_port++;
        dns_wait_t w = {id, 0, 0};
        if (send_udp_raw(mac, primary.ip, dns_server, sport, DNS_PORT,
                         msg, (uint16_t)len) < 0) break;
        uint64_t deadline = net_deadline_after_ms(1000);
        while (!w.found && net_before_deadline(deadline)) {
            net_poll(dns_cb, &w, 4096);
            task_yield();
        }
        if (w.found) {
            *out_ip = w.ip;
            return 0;
        }
    }
    set_error("dns timeout");
    return -1;
}

/** @brief NTP 服务端口号（RFC 5905） */
#define NTP_PORT 123
/** @brief NTP 时间戳纪元 (1900-01-01) 到 Unix 纪元 (1970-01-01) 的秒数差 */
#define NTP_UNIX_EPOCH_DELTA 2208988800ULL

/** @brief NTP 应答等待上下文 */
typedef struct { uint16_t sport; uint64_t unix_sec; int found; } ntp_wait_t;

/**
 * @brief NTP 应答接收回调，从 Transmit Timestamp 字段提取服务器时间
 * @return 1 收到匹配的应答，0 继续轮询
 */
static int ntp_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    ntp_wait_t *w = arg;
    if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + 48) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_IP) return 0;
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(pkt + sizeof(eth_hdr_t));
    if (ip->proto != IP_PROTO_UDP || ip->dst != primary.ip) return 0;
    const udp_hdr_t *udp = (const udp_hdr_t *)((const uint8_t *)ip + ((ip->ver_ihl & 0x0F) * 4));
    if (ntohs(udp->dst) != w->sport) return 0;
    uint16_t ulen = ntohs(udp->len);
    if (ulen < sizeof(udp_hdr_t) + 48) return 0;
    const uint8_t *ntp = (const uint8_t *)udp + sizeof(udp_hdr_t);
    int mode = ntp[0] & 0x07;
    if (mode != 4 && mode != 5) return 0;   /* 4=server, 5=broadcast（宽松接受） */
    uint32_t tx_sec = ntohl(*(const uint32_t *)(ntp + 40));
    if (tx_sec < (uint32_t)NTP_UNIX_EPOCH_DELTA) return 0;   /* 应答里没给出合理时间戳 */
    w->unix_sec = (uint64_t)tx_sec - NTP_UNIX_EPOCH_DELTA;
    w->found = 1;
    return 1;
}

/**
 * @brief 通过 NTP 协议向服务器同步时间
 * @param server 服务器域名或点分 IP（如 "pool.ntp.org" 或 "132.163.96.1"）
 * @return 0 成功（已更新 g_rtc_ntp_correction_sec），-1 失败
 */
int net_ntp_sync(const char *server) {
    if (!server || !*server) {
        set_error("bad ntp server");
        return -1;
    }
    uint32_t ntp_server;
    if (net_dns_resolve(server, &ntp_server) < 0) return -1;
    uint8_t mac[6];
    uint32_t next_hop;
    if (net_route_next_hop(ntp_server, &next_hop) < 0) return -1;
    if (arp_resolve(next_hop, mac) < 0) return -1;

    uint8_t req[48];
    memset(req, 0, sizeof(req));
    req[0] = (0 << 6) | (4 << 3) | 3;   /* LI=0, VN=4, Mode=3 (client) */

    for (int attempt = 0; attempt < 3; attempt++) {
        if (next_port < 49152) next_port = 49152;
        uint16_t sport = next_port++;
        ntp_wait_t w = {sport, 0, 0};
        if (send_udp_raw(mac, primary.ip, ntp_server, sport, NTP_PORT,
                         req, sizeof(req)) < 0) break;
        uint64_t deadline = net_deadline_after_ms(1000);
        while (!w.found && net_before_deadline(deadline)) {
            net_poll(ntp_cb, &w, 4096);
            task_yield();
        }
        if (w.found) {
            int64_t cmos_epoch = rtc_tz_cmos_epoch_now();
            g_rtc_ntp_correction_sec = (long long)w.unix_sec - cmos_epoch;
            return 0;
        }
    }
    set_error("ntp timeout");
    return -1;
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
    return primary.send(frame, (uint16_t)(sizeof(eth_hdr_t) + 20 + 20 + dlen));
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
    if (next_port < 49152) next_port = 49152;
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
        uint64_t deadline = net_deadline_after_ms(1000);
        while (!w.synack && !w.rst && net_before_deadline(deadline)) {
            net_poll(tcp_cb, &w, 4096);
            task_yield();
        }
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
        uint64_t deadline = net_deadline_after_ms(1000);
        while (!w.rst && net_before_deadline(deadline)) {
            net_poll(tcp_cb, &w, 4096);
            task_yield();
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
        task_yield();
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
    char req[1024];
    uint32_t n = 0;
    const char *b = " HTTP/1.0\r\nHost: ";
    const char *c = "\r\nConnection: close\r\nAccept-Encoding: identity\r\n"
                    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/131.0.0.0 Safari/537.36\r\n\r\n";
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

/* beta5 M5: 之前这里是一整套全局单变量（listen_port/listen_peer/...），
 * net_tcp_accept() 一进来就 `if (!listen_syn_acked) return -1;` ——但
 * listen_syn_acked 只有 accept_cb() 自己能置位，而 accept_cb() 只在这同
 * 一个"已经 syn_acked"的门槛后面才会被 net_poll() 调用。也就是说全新
 * listen() 之后第一次 accept() 永远在真正开始收包之前就直接失败退出，
 * httpd 的 `while(1){ accept(); continue; }` 循环会原地空转，从来碰不到
 * 网络——不是"同一时间只能一个连接"，是压根一个都建立不起来（QEMU
 * hostfwd + curl 实测复现：请求发出去后 httpd 侧从没打印过一行日志）。
 *
 * 现在改成一张小的"待处理连接"表：每收到一个 SYN 都在表里认领一个空
 * 位（不再要求"必须已经有一个在等"），accept() 对表里还没发过 SYN-ACK
 * 的项发 SYN-ACK，对已经完成三次握手的项直接摘下来交给调用方——这样
 * 一次 accept() 循环期间收到的第二个客户端 SYN 不会被无声丢弃，而是排
 * 在表里等下一次 accept() 调用来处理，不用等前一个连接完全处理完。 */
#define TCP_PENDING_MAX 4

/* 网卡驱动的 poll 循环（比如 e1000_poll）每处理完一个包，不管回调是不是
 * 真的"用上"了它，都会立刻把对应的 RX 描述符标记为空闲、可以被下一个
 * 包覆盖（"d->status = 0;" 无条件执行）——这个驱动层面没有另外留一份
 * 收包缓冲。所以三次握手最后一个 ACK 如果和客户端的请求数据挤在同一个
 * 包里（很常见：TCP 允许 ACK 捎带数据），或者数据包紧跟着单独的 ACK
 * 包到达、恰好也被这次 accept_cb 的同一轮 poll 看到，那这段数据在
 * accept_cb 手里就已经从网卡环形缓冲区里被取走了——如果 accept_cb 不
 * 顺手接住它，之后 net_tcp_recv() 用 tcp_cb 再去 poll 就永远看不到这个
 * 包了（已经被覆盖），实测复现：curl 发 GET 请求，httpd 侧 net_tcp_recv
 * 读到的永远是空的。rx_stage 就是用来在 accept_cb 里接住这份"卡在握手
 * 阶段"的数据，net_tcp_accept() 摘取连接时把它转交给 conn->rx_buf，
 * net_tcp_recv() 本来就会优先看 conn->rx_len 是否已经有数据。 */
#define TCP_PENDING_STAGE_CAP 2048

typedef struct {
    int      in_use;
    uint16_t port;         /* 监听端口，用于区分同一张表服务不同端口的场景 */
    uint32_t peer;
    uint16_t peer_port;
    uint8_t  peer_mac[6];
    uint32_t peer_seq;     /* 对端下一个期望序列号（= 收到的 SYN.seq + 1） */
    uint32_t isn;          /* 我们这边选的初始序列号，accept() 发 SYN-ACK 时才分配 */
    int      syn_acked;    /* 已经发过 SYN-ACK，等对端最后一个 ACK */
    int      established;  /* 三次握手完成，等 accept() 取走 */
    uint8_t  rx_stage[TCP_PENDING_STAGE_CAP];
    uint32_t rx_stage_len;
} tcp_pending_t;

static uint16_t listen_port;
static tcp_pending_t g_pending[TCP_PENDING_MAX];

static int tcp_pending_find(uint32_t peer, uint16_t peer_port) {
    for (int i = 0; i < TCP_PENDING_MAX; i++) {
        if (g_pending[i].in_use && g_pending[i].peer == peer && g_pending[i].peer_port == peer_port)
            return i;
    }
    return -1;
}

static int tcp_pending_alloc(void) {
    for (int i = 0; i < TCP_PENDING_MAX; i++) {
        if (!g_pending[i].in_use) return i;
    }
    return -1;
}

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
    uint32_t peer = ip->src;
    uint16_t peer_port = ntohs(tcp->src);

    if ((flags & 0x02) && !(flags & 0x10)) {
        /* 新的 SYN：同一个 peer/port 重传的 SYN 复用已有槽位（不重置状态，
         * 避免正在等最后 ACK 的连接被 SYN 重传打回原形），否则找个空槽位；
         * 表满了就丢——客户端会自己重传 SYN。 */
        int slot = tcp_pending_find(peer, peer_port);
        if (slot < 0) slot = tcp_pending_alloc();
        if (slot < 0) return 0;
        if (!g_pending[slot].in_use) {
            tcp_pending_t *p = &g_pending[slot];
            memset(p, 0, sizeof(*p));
            p->in_use = 1;
            p->port = listen_port;
            p->peer = peer;
            p->peer_port = peer_port;
            p->peer_seq = seq + 1;
            memcpy(p->peer_mac, eth->src, 6);
        }
        return 0; /* 不提前结束这次 poll：同一批还可能有别的客户端的 SYN */
    }

    if (flags & 0x10) {
        int slot = tcp_pending_find(peer, peer_port);
        if (slot < 0) return 0;
        tcp_pending_t *p = &g_pending[slot];
        if (p->syn_acked && !p->established) p->established = 1;

        /* 这个 ACK 可能捎带了数据（三次握手最后一个 ACK 和请求数据挤在
         * 一个包里，或者独立的数据包紧跟着到达、被这轮 poll 一起看到）
         * ——见 tcp_pending_t 定义处的注释，不接住的话这段数据会随着
         * RX 描述符被驱动回收而永久丢失。 */
        if (p->established) {
            uint32_t ip_len = ntohs(ip->len);
            uint32_t thl = (tcp->off_flags_hi >> 4) * 4;
            const uint8_t *data = (const uint8_t *)tcp + thl;
            uint32_t dlen = ip_len > ihl + thl ? ip_len - ihl - thl : 0;
            if (dlen && seq == p->peer_seq) {
                uint32_t copy = dlen;
                if (p->rx_stage_len + copy > TCP_PENDING_STAGE_CAP)
                    copy = TCP_PENDING_STAGE_CAP - p->rx_stage_len;
                if (copy) {
                    memcpy(p->rx_stage + p->rx_stage_len, data, copy);
                    p->rx_stage_len += copy;
                }
                p->peer_seq += dlen;
            }
        }
        return 0;
    }

    if (flags & 0x04) {
        int slot = tcp_pending_find(peer, peer_port);
        if (slot >= 0) g_pending[slot].in_use = 0;
        return 0;
    }

    return 0;
}

int net_tcp_listen(uint16_t port) {
    if (port == 0) return -1;
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    listen_port = port;
    memset(g_pending, 0, sizeof(g_pending));
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
    if (listen_port != port) return -1;

    uint32_t elapsed = 0;
    int slot = -1;
    while (elapsed <= timeout_ms) {
        /* 把还没发 SYN-ACK 的排队项都发出去（可能不止一个：上一轮 poll
         * 一次抓到了多个客户端的 SYN）。回包的源端口必须还是我们监听的
         * 那个固定端口 port（标准 TCP 服务端行为——客户端认的是"服务端
         * 在 port 上"，换成别的端口对客户端来说就是另一台服务器了）；
         * 区分并发连接靠对端 IP/端口这一侧变化，不是靠改自己的端口。
         * next_port 这里纯粹当一次性初始序列号使的，和它平时当"下一个
         * 要用的本地临时端口"是两回事，蹭个全局递增计数器省事而已。 */
        for (int i = 0; i < TCP_PENDING_MAX; i++) {
            tcp_pending_t *p = &g_pending[i];
            if (p->in_use && p->port == port && !p->syn_acked) {
                if (next_port < 49152) next_port = 49152;
                p->isn = next_port++;
                send_tcp(p->peer_mac, p->peer, port, p->peer_port,
                         p->isn, p->peer_seq, 0x12, 0, 0);
                p->syn_acked = 1;
            }
        }

        for (int i = 0; i < TCP_PENDING_MAX; i++) {
            if (g_pending[i].in_use && g_pending[i].port == port && g_pending[i].established) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) break;

        net_poll(accept_cb, NULL, 10000);
        task_yield();
        elapsed += 10;
    }

    if (slot < 0) {
        set_error("tcp accept timeout");
        return -1;
    }

    tcp_pending_t *p = &g_pending[slot];
    conn->peer  = p->peer;
    conn->sport = port;
    conn->dport = p->peer_port;
    conn->seq   = p->isn + 1;
    conn->ack   = p->peer_seq;
    memcpy(conn->mac, p->peer_mac, 6);
    conn->open  = true;

    /* 握手阶段就已经被 accept_cb 捎带收下的数据（见 tcp_pending_t 定义处
     * 的注释）搬进 conn->rx_buf——net_tcp_recv() 一进来就会先看这里有没
     * 有现成数据，不用重新等网络。 */
    if (p->rx_stage_len) {
        uint32_t copy = p->rx_stage_len;
        if (copy > NET_TCP_RXBUF_SIZE) copy = NET_TCP_RXBUF_SIZE;
        memcpy(conn->rx_buf, p->rx_stage, copy);
        conn->rx_len = copy;
    }

    p->in_use = 0;
    return 0;
}
