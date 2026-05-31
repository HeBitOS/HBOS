#include "net.h"
#include "pci.h"
#include "string.h"
#include "core/vmm.h"

#define PCI_CLASS_NETWORK      0x02
#define PCI_SUBCLASS_ETHERNET  0x00

#define E1000_CTRL   0x0000
#define E1000_STATUS 0x0008
#define E1000_IMC    0x00D8
#define E1000_RCTL   0x0100
#define E1000_TCTL   0x0400
#define E1000_TIPG   0x0410
#define E1000_RDBAL  0x2800
#define E1000_RDBAH  0x2804
#define E1000_RDLEN  0x2808
#define E1000_RDH    0x2810
#define E1000_RDT    0x2818
#define E1000_TDBAL  0x3800
#define E1000_TDBAH  0x3804
#define E1000_TDLEN  0x3808
#define E1000_TDH    0x3810
#define E1000_TDT    0x3818
#define E1000_RAL0   0x5400
#define E1000_RAH0   0x5404

#define RX_COUNT 32
#define TX_COUNT 16
#define PKT_SIZE 2048

#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DNS_PORT 53
#define DHCP_FIXED_LEN 240

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    volatile uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint8_t cso;
    volatile uint8_t cmd;
    volatile uint8_t status;
    volatile uint8_t css;
    volatile uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed)) ipv4_hdr_t;

typedef struct {
    uint16_t src;
    uint16_t dst;
    uint16_t len;
    uint16_t csum;
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint16_t src;
    uint16_t dst;
    uint32_t seq;
    uint32_t ack;
    uint8_t off_flags_hi;
    uint8_t flags;
    uint16_t win;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed)) tcp_hdr_t;

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t legacy[192];
    uint32_t magic;
    uint8_t opts[312];
} __attribute__((packed)) dhcp_pkt_t;

static net_device_t primary;
static int initialized;
static volatile uint8_t *mmio;
static e1000_rx_desc_t rx_desc[RX_COUNT] __attribute__((aligned(16)));
static e1000_tx_desc_t tx_desc[TX_COUNT] __attribute__((aligned(16)));
static uint8_t rx_buf[RX_COUNT][PKT_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buf[TX_COUNT][PKT_SIZE] __attribute__((aligned(16)));
static uint16_t tx_tail;
static uint16_t ip_id = 1;
static uint16_t next_port = 49152;
static uint8_t gateway_mac[6];
static int gateway_mac_valid;

static uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t bswap32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}
#define htons(x) bswap16((uint16_t)(x))
#define ntohs(x) bswap16((uint16_t)(x))
#define htonl(x) bswap32((uint32_t)(x))
#define ntohl(x) bswap32((uint32_t)(x))

static inline uint32_t reg_read(uint32_t off) {
    return *(volatile uint32_t *)(mmio + off);
}

static inline void reg_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(mmio + off) = val;
}

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

const char *net_driver_name(net_driver_t driver) {
    switch (driver) {
        case NET_DRIVER_E1000: return "Intel E1000";
        case NET_DRIVER_RTL8139: return "Realtek RTL8139";
        case NET_DRIVER_VIRTIO_NET: return "VirtIO net";
        case NET_DRIVER_UNKNOWN_ETHERNET: return "unknown ethernet";
        default: return "none";
    }
}

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

static int e1000_send(const void *frame, uint16_t len) {
    if (!primary.link_ready || !mmio || len > PKT_SIZE) return -1;
    uint16_t idx = tx_tail;
    uint32_t wait = 0;
    while (!(tx_desc[idx].status & 1) && wait++ < 1000000) {}
    if (!(tx_desc[idx].status & 1)) return -1;
    memcpy(tx_buf[idx], frame, len);
    tx_desc[idx].length = len;
    tx_desc[idx].cmd = 0x0B;
    tx_desc[idx].status = 0;
    tx_tail = (uint16_t)((idx + 1) % TX_COUNT);
    reg_write(E1000_TDT, tx_tail);
    return 0;
}

typedef int (*packet_cb_t)(const uint8_t *pkt, uint16_t len, void *arg);

static int net_poll(packet_cb_t cb, void *arg, uint32_t spins) {
    if (!primary.link_ready || !mmio) return -1;
    for (uint32_t s = 0; s < spins; s++) {
        uint32_t tail = reg_read(E1000_RDT);
        uint32_t idx = (tail + 1) % RX_COUNT;
        e1000_rx_desc_t *d = &rx_desc[idx];
        if (!(d->status & 1)) continue;
        uint16_t len = d->length;
        int ret = 0;
        if (len >= sizeof(eth_hdr_t) && len <= PKT_SIZE && cb)
            ret = cb(rx_buf[idx], len, arg);
        d->status = 0;
        reg_write(E1000_RDT, idx);
        if (ret) return ret;
    }
    return 0;
}

static void make_eth(uint8_t *buf, const uint8_t dst[6], uint16_t type) {
    eth_hdr_t *eth = (eth_hdr_t *)buf;
    memcpy(eth->dst, dst, 6);
    memcpy(eth->src, primary.mac, 6);
    eth->type = htons(type);
}

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

typedef struct { uint32_t ip; uint8_t mac[6]; int found; } arp_wait_t;
static int arp_cb(const uint8_t *pkt, uint16_t len, void *arg) {
    arp_wait_t *w = arg;
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return 0;
    const eth_hdr_t *eth = (const eth_hdr_t *)pkt;
    if (ntohs(eth->type) != ETH_TYPE_ARP) return 0;
    const arp_pkt_t *arp = (const arp_pkt_t *)(pkt + sizeof(eth_hdr_t));
    if (ntohs(arp->op) == 2 && arp->spa == w->ip) {
        memcpy(w->mac, arp->sha, 6);
        w->found = 1;
        return 1;
    }
    return 0;
}

static int arp_resolve(uint32_t ip, uint8_t mac[6]) {
    if (gateway_mac_valid && ip == primary.gateway) {
        memcpy(mac, gateway_mac, 6);
        return 0;
    }
    arp_wait_t w;
    w.ip = ip; w.found = 0;
    send_arp(1, ip, 0);
    for (int i = 0; i < 8 && !w.found; i++) net_poll(arp_cb, &w, 250000);
    if (!w.found) return -1;
    memcpy(mac, w.mac, 6);
    if (ip == primary.gateway) {
        memcpy(gateway_mac, mac, 6);
        gateway_mac_valid = 1;
    }
    return 0;
}

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

void net_init(void) {
    if (initialized) return;
    initialized = 1;
    pci_device_t dev;
    if (pci_find_class(PCI_CLASS_NETWORK, PCI_SUBCLASS_ETHERNET, 0xFF, &dev) < 0) return;
    primary.present = true;
    primary.driver = detect_driver(dev.vendor_id, dev.device_id);
    primary.bus = dev.bus; primary.slot = dev.slot; primary.func = dev.func;
    primary.vendor_id = dev.vendor_id; primary.device_id = dev.device_id;
    primary.bar0_raw = pci_bar(dev.bus, dev.slot, dev.func, 0);
    primary.bar0_io = (primary.bar0_raw & 1U) != 0;
    primary.bar0_base = primary.bar0_io ? (primary.bar0_raw & ~3U) : (primary.bar0_raw & ~0xFU);
    if (primary.driver == NET_DRIVER_E1000) e1000_init_hw(&dev);
}

const net_device_t *net_primary(void) {
    net_init();
    return &primary;
}

static void dhcp_opt(uint8_t **p, uint8_t code, uint8_t len, const void *data) {
    *(*p)++ = code; *(*p)++ = len; memcpy(*p, data, len); *p += len;
}

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

typedef struct { uint32_t xid, yiaddr, server, mask, router, dns; uint8_t type; int found; } dhcp_wait_t;
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

int net_dhcp(void) {
    net_init();
    if (!primary.link_ready || !primary.mac_valid) return -1;
    uint32_t xid = 0x48424F53U;
    dhcp_wait_t w;
    memset(&w, 0, sizeof(w)); w.xid = xid;
    send_dhcp(1, xid, 0, 0);
    for (int i = 0; i < 12 && !w.found; i++) net_poll(dhcp_cb, &w, 250000);
    if (!w.found || w.type != 2) return -1;
    uint32_t offer = w.yiaddr, server = w.server;
    memset(&w, 0, sizeof(w)); w.xid = xid;
    send_dhcp(3, xid, offer, server);
    for (int i = 0; i < 12 && !w.found; i++) net_poll(dhcp_cb, &w, 250000);
    if (!w.found || w.type != 5) return -1;
    primary.ip = w.yiaddr;
    primary.netmask = w.mask;
    primary.gateway = w.router;
    primary.dns = w.dns;
    primary.dhcp_ok = true;
    gateway_mac_valid = 0;
    if (primary.gateway) (void)arp_resolve(primary.gateway, gateway_mac);
    return 0;
}

typedef struct { uint16_t id; int ok; } ping_wait_t;
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

int net_ping(uint32_t ip, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    uint8_t mac[6];
    uint32_t target = ((ip ^ primary.ip) & primary.netmask) ? primary.gateway : ip;
    if (arp_resolve(target, mac) < 0) return -1;
    uint8_t icmp[32];
    memset(icmp, 0, sizeof(icmp));
    icmp[0] = 8;
    *(uint16_t *)(icmp + 4) = htons(0x4842);
    *(uint16_t *)(icmp + 6) = htons(1);
    for (int i = 8; i < 32; i++) icmp[i] = (uint8_t)i;
    *(uint16_t *)(icmp + 2) = checksum(icmp, sizeof(icmp));
    ping_wait_t w = {0x4842, 0};
    send_ip(mac, ip, IP_PROTO_ICMP, icmp, sizeof(icmp));
    for (int i = 0; i < 12 && !w.ok; i++) net_poll(ping_cb, &w, 250000);
    return w.ok ? 0 : -1;
}

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

typedef struct { uint16_t id; uint32_t ip; int found; } dns_wait_t;
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

int net_dns_resolve(const char *name, uint32_t *out_ip) {
    if (!name || !out_ip) return -1;
    uint32_t literal = net_parse_ipv4(name);
    if (literal) { *out_ip = literal; return 0; }
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    uint8_t mac[6];
    if (arp_resolve(primary.gateway, mac) < 0) return -1;
    uint8_t msg[300];
    memset(msg, 0, sizeof(msg));
    uint16_t id = 0x4248;
    *(uint16_t *)(msg + 0) = htons(id);
    *(uint16_t *)(msg + 2) = htons(0x0100);
    *(uint16_t *)(msg + 4) = htons(1);
    int qn = dns_encode(name, msg + 12, sizeof(msg) - 16);
    if (qn < 0) return -1;
    uint32_t len = 12 + (uint32_t)qn;
    *(uint16_t *)(msg + len) = htons(1); len += 2;
    *(uint16_t *)(msg + len) = htons(1); len += 2;
    dns_wait_t w = {id, 0, 0};
    send_udp_raw(mac, primary.ip, primary.dns, next_port++, DNS_PORT, msg, (uint16_t)len);
    for (int i = 0; i < 12 && !w.found; i++) net_poll(dns_cb, &w, 250000);
    if (!w.found) return -1;
    *out_ip = w.ip;
    return 0;
}

typedef struct { uint32_t peer, seq, ack; uint16_t sport; int synack, done, rst; char *out; uint32_t cap, len; } tcp_wait_t;

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
    if (dlen && seq == w->ack) {
        uint32_t copy = dlen;
        if (w->len + copy > w->cap) copy = w->cap - w->len;
        if (copy) memcpy(w->out + w->len, data, copy);
        w->len += copy;
        w->ack += dlen;
        if (flags & 0x01) {
            w->ack++;
            w->done = 1;
        }
        return 1;
    }
    if (flags & 0x01) { w->ack = seq + 1; w->done = 1; return 1; }
    return 0;
}

int net_http_get(const char *host, uint32_t ip, uint16_t port, const char *path,
                 char *out, uint32_t out_cap, uint32_t *out_len) {
    if (!host || !path || !out || !out_len || out_cap == 0 || port == 0) return -1;
    if (!primary.dhcp_ok && net_dhcp() < 0) return -1;
    uint8_t mac[6];
    uint32_t target = ((ip ^ primary.ip) & primary.netmask) ? primary.gateway : ip;
    if (arp_resolve(target, mac) < 0) return -1;
    uint16_t sport = next_port++;
    uint32_t seq = 0x10000000U + sport;
    tcp_wait_t w;
    memset(&w, 0, sizeof(w));
    w.peer = ip; w.sport = sport; w.seq = seq; w.out = out; w.cap = out_cap - 1;
    send_tcp(mac, ip, sport, port, seq, 0, 0x02, 0, 0);
    for (int i = 0; i < 16 && !w.synack && !w.rst; i++) net_poll(tcp_cb, &w, 250000);
    if (!w.synack || w.rst) return -1;
    seq++;
    send_tcp(mac, ip, sport, port, seq, w.ack, 0x10, 0, 0);
    char req[512];
    uint32_t n = 0;
    const char *a = "GET "; const char *b = " HTTP/1.0\r\nHost: "; const char *c = "\r\nConnection: close\r\n\r\n";
    for (const char *p = a; *p && n < sizeof(req); p++) req[n++] = *p;
    for (const char *p = path; *p && n < sizeof(req); p++) req[n++] = *p;
    for (const char *p = b; *p && n < sizeof(req); p++) req[n++] = *p;
    for (const char *p = host; *p && n < sizeof(req); p++) req[n++] = *p;
    for (const char *p = c; *p && n < sizeof(req); p++) req[n++] = *p;
    send_tcp(mac, ip, sport, port, seq, w.ack, 0x18, req, (uint16_t)n);
    seq += n;
    for (int i = 0; i < 80 && !w.done && !w.rst; i++) {
        int before = (int)w.len;
        net_poll(tcp_cb, &w, 250000);
        if ((int)w.len != before)
            send_tcp(mac, ip, sport, port, seq, w.ack, 0x10, 0, 0);
    }
    if (w.done) send_tcp(mac, ip, sport, port, seq, w.ack, 0x10, 0, 0);
    out[w.len < out_cap ? w.len : out_cap - 1] = 0;
    *out_len = w.len;
    return w.len ? 0 : -1;
}

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
