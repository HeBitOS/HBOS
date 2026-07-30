#include "net6.h"

#include "core/cpu.h"
#include "core/task.h"
#include "core/wait.h"
#include "net.h"
#include "string.h"

#include "lwip/init.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "lwip/ethip6.h"

#define ETH_TYPE_IPV6 0x86DDU

static struct netif ipv6_netif;
static int ipv6_initialized;
static uint32_t random_state = 0x48505436U;

uint32_t sys_now(void) {
    uint32_t frequency = pit_get_frequency_hz();
    uint64_t ticks = pit_get_ticks();
    if (!frequency) return (uint32_t)ticks;
    return (uint32_t)((ticks * 1000ULL) / frequency);
}

uint32_t hbos_lwip_rand(void) {
    uint64_t ticks = pit_get_ticks();
    random_state ^= (uint32_t)ticks ^ (uint32_t)(ticks >> 32);
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

void hbos_lwip_assert(const char *message) {
    (void)message;
}

static err_t net6_link_output(struct netif *interface, struct pbuf *packet) {
    (void)interface;
    const net_device_t *device = net_primary();
    if (!device || !device->send || packet->tot_len > 2048U)
        return ERR_IF;

    uint8_t frame[2048];
    uint16_t offset = 0;
    for (const struct pbuf *part = packet; part; part = part->next) {
        if ((uint32_t)offset + part->len > sizeof(frame)) return ERR_BUF;
        memcpy(frame + offset, part->payload, part->len);
        offset = (uint16_t)(offset + part->len);
    }
    return device->send(frame, offset) == 0 ? ERR_OK : ERR_IF;
}

static err_t net6_interface_init(struct netif *interface) {
    const net_device_t *device = net_primary();
    if (!device || !device->present || !device->mac_valid) return ERR_IF;

    interface->name[0] = 'h';
    interface->name[1] = 'b';
    interface->hwaddr_len = 6;
    memcpy(interface->hwaddr, device->mac, 6);
    interface->mtu = 1500;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                       NETIF_FLAG_ETHERNET | NETIF_FLAG_MLD6;
    interface->output_ip6 = ethip6_output;
    interface->linkoutput = net6_link_output;
    return ERR_OK;
}

static int net6_input_frame(const uint8_t *frame, uint16_t length,
                            void *context) {
    struct netif *interface = context;
    if (!frame || length < 14U ||
        (((uint16_t)frame[12] << 8) | frame[13]) != ETH_TYPE_IPV6)
        return 0;

    struct pbuf *packet = pbuf_alloc(PBUF_RAW, length, PBUF_POOL);
    if (!packet) return 0;
    if (pbuf_take(packet, frame, length) != ERR_OK ||
        interface->input(packet, interface) != ERR_OK)
        pbuf_free(packet);
    return 0;
}

static void net6_poll(void) {
    (void)net_poll_frames(net6_input_frame, &ipv6_netif, 256);
    sys_check_timeouts();
}

int net6_init(void) {
    if (ipv6_initialized) return 0;
    const net_device_t *device = net_primary();
    if (!device || !device->present || !device->link_ready) return -1;

    lwip_init();
    memset(&ipv6_netif, 0, sizeof(ipv6_netif));
    if (!netif_add_noaddr(&ipv6_netif, NULL, net6_interface_init,
                          ethernet_input))
        return -1;
    netif_set_default(&ipv6_netif);
    netif_set_up(&ipv6_netif);
    netif_set_link_up(&ipv6_netif);
    netif_create_ip6_linklocal_address(&ipv6_netif, 1);
    ipv6_netif.ip6_autoconfig_enabled = 1;
    ipv6_initialized = 1;
    return 0;
}

static err_t net6_connected(void *argument, struct tcp_pcb *pcb, err_t error) {
    (void)pcb;
    net6_tcp_conn_t *connection = argument;
    if (error == ERR_OK) connection->connected = 1;
    else connection->error = error;
    return ERR_OK;
}

static void net6_error(void *argument, err_t error) {
    net6_tcp_conn_t *connection = argument;
    connection->pcb = NULL;
    connection->error = error;
    connection->closed = 1;
}

static err_t net6_received(void *argument, struct tcp_pcb *pcb,
                           struct pbuf *packet, err_t error) {
    net6_tcp_conn_t *connection = argument;
    if (!packet) {
        connection->closed = 1;
        return ERR_OK;
    }
    if (error != ERR_OK || packet->tot_len >
        NET6_RX_CAPACITY - connection->rx_length) {
        pbuf_free(packet);
        connection->error = error == ERR_OK ? ERR_BUF : error;
        return ERR_OK;
    }

    uint32_t tail =
        (connection->rx_head + connection->rx_length) % NET6_RX_CAPACITY;
    uint16_t remaining = packet->tot_len;
    uint16_t source_offset = 0;
    while (remaining) {
        uint32_t chunk = NET6_RX_CAPACITY - tail;
        if (chunk > remaining) chunk = remaining;
        pbuf_copy_partial(packet, connection->rx + tail,
                          (u16_t)chunk, source_offset);
        tail = (tail + chunk) % NET6_RX_CAPACITY;
        source_offset = (uint16_t)(source_offset + chunk);
        remaining = (uint16_t)(remaining - chunk);
    }
    connection->rx_length += packet->tot_len;
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    return ERR_OK;
}

static int net6_source_ready(const ip6_addr_t *target) {
    int target_is_link_local = ip6_addr_islinklocal(target);
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        const ip6_addr_t *candidate = netif_ip6_addr(&ipv6_netif, i);
        if (!ip6_addr_ispreferred(netif_ip6_addr_state(&ipv6_netif, i)))
            continue;
        if ((ip6_addr_islinklocal(candidate) != 0) ==
            target_is_link_local)
            return 1;
    }
    return 0;
}

static int net6_wait_for_source(const ip6_addr_t *target,
                                uint32_t timeout_ms) {
    uint64_t deadline = pit_deadline_after_ms(timeout_ms);
    do {
        net6_poll();
        if (net6_source_ready(target)) return 0;
    } while (!pit_deadline_reached(deadline));
    return -1;
}

static int net6_tcp_connect_parsed(const ip6_addr_t *parsed, uint16_t port,
                                   net6_tcp_conn_t *connection,
                                   uint32_t timeout_ms) {
    if (!parsed || !connection || !port || net6_init() < 0) return -1;
    if (net6_wait_for_source(parsed, timeout_ms) < 0) return -1;
    ip_addr_t target;
    ip_addr_copy_from_ip6(target, *parsed);

    memset(connection, 0, sizeof(*connection));
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V6);
    if (!pcb) return -1;
    connection->pcb = pcb;
    tcp_arg(pcb, connection);
    tcp_recv(pcb, net6_received);
    tcp_err(pcb, net6_error);
    tcp_nagle_disable(pcb);
    if (tcp_connect(pcb, &target, port, net6_connected) != ERR_OK) {
        tcp_abort(pcb);
        connection->pcb = NULL;
        return -1;
    }

    uint64_t deadline = pit_deadline_after_ms(timeout_ms);
    while (!connection->connected && !connection->error &&
           !pit_deadline_reached(deadline))
        net6_poll();
    if (!connection->connected) {
        net6_tcp_close(connection);
        return -1;
    }
    return 0;
}

int net6_tcp_connect(const char *address, uint16_t port,
                     net6_tcp_conn_t *connection, uint32_t timeout_ms) {
    if (!address) return -1;
    ip6_addr_t parsed;
    if (!ip6addr_aton(address, &parsed)) return -1;
    return net6_tcp_connect_parsed(&parsed, port, connection, timeout_ms);
}

int net6_tcp_connect_address(const uint8_t address[16], uint16_t port,
                             net6_tcp_conn_t *connection,
                             uint32_t timeout_ms) {
    if (!address) return -1;
    ip6_addr_t parsed;
    memcpy(parsed.addr, address, 16);
    ip6_addr_clear_zone(&parsed);
    return net6_tcp_connect_parsed(&parsed, port, connection, timeout_ms);
}

int net6_tcp_send(net6_tcp_conn_t *connection, const uint8_t *data,
                  uint32_t length, uint32_t timeout_ms) {
    if (!connection || !connection->pcb || !data || !length) return -1;
    struct tcp_pcb *pcb = connection->pcb;
    uint32_t sent = 0;
    uint64_t deadline = pit_deadline_after_ms(timeout_ms);
    while (sent < length && !connection->error &&
           !pit_deadline_reached(deadline)) {
        uint32_t chunk = length - sent;
        uint16_t available = tcp_sndbuf(pcb);
        if (chunk > available) chunk = available;
        if (chunk > 65535U) chunk = 65535U;
        if (!chunk) {
            net6_poll();
            continue;
        }
        err_t write_status =
            tcp_write(pcb, data + sent, (u16_t)chunk,
                      TCP_WRITE_FLAG_COPY);
        if (write_status == ERR_MEM) {
            net6_poll();
            continue;
        }
        if (write_status != ERR_OK) return -1;
        if (tcp_output(pcb) != ERR_OK) return -1;
        sent += chunk;
        net6_poll();
    }
    return sent == length ? (int)sent : -1;
}

int net6_tcp_recv(net6_tcp_conn_t *connection, uint8_t *data,
                  uint32_t capacity, uint32_t timeout_ms) {
    if (!connection || !data || !capacity) return -1;
    uint64_t deadline = pit_deadline_after_ms(timeout_ms);
    while (!connection->rx_length && !connection->closed &&
           !connection->error && !pit_deadline_reached(deadline))
        net6_poll();
    if (!connection->rx_length)
        return connection->closed ? 0 : -1;

    uint32_t count = connection->rx_length;
    if (count > capacity) count = capacity;
    uint32_t first = NET6_RX_CAPACITY - connection->rx_head;
    if (first > count) first = count;
    memcpy(data, connection->rx + connection->rx_head, first);
    if (first < count) memcpy(data + first, connection->rx, count - first);
    connection->rx_head = (connection->rx_head + count) % NET6_RX_CAPACITY;
    connection->rx_length -= count;
    return (int)count;
}

void net6_tcp_close(net6_tcp_conn_t *connection) {
    if (!connection || !connection->pcb) return;
    struct tcp_pcb *pcb = connection->pcb;
    connection->pcb = NULL;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    connection->closed = 1;
}

int net6_primary_address(char *out, size_t capacity) {
    if (!out || capacity < IP6ADDR_STRLEN_MAX || net6_init() < 0) return -1;
    net6_poll();
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        if (ip6_addr_ispreferred(netif_ip6_addr_state(&ipv6_netif, i)) &&
            ip6addr_ntoa_r(netif_ip6_addr(&ipv6_netif, i), out,
                           (int)capacity))
            return 0;
    }
    return -1;
}
