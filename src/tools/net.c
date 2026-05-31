#include "../graphics/graphics.h"
#include "../net.h"
#include "../string.h"
#include "../vfs.h"
#include "tool.h"

static void print_hex_digit(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    console_putchar(hex[v & 0x0F]);
}

static void print_hex8(uint8_t v) {
    print_hex_digit((uint8_t)(v >> 4));
    print_hex_digit(v);
}

static void print_hex16(uint16_t v) {
    print_hex8((uint8_t)(v >> 8));
    print_hex8((uint8_t)v);
}

static void print_hex32(uint32_t v) {
    print_hex16((uint16_t)(v >> 16));
    print_hex16((uint16_t)v);
}

static void print_uint(uint32_t v) {
    char buf[11];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n--) console_putchar(buf[n]);
}

static void print_mac(const uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        if (i) console_putchar(':');
        print_hex8(mac[i]);
    }
}

static void print_ip(uint32_t ip) {
    char buf[16];
    net_ipv4_to_str(ip, buf);
    console_puts(buf);
}

static void cmd_netinfo(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const net_device_t *dev = net_primary();
    if (!dev->present) {
        console_puts("net: no ethernet controller found\n");
        return;
    }

    console_puts("Network device\n");
    console_puts("  driver: ");
    console_puts(net_driver_name(dev->driver));
    console_putchar('\n');

    console_puts("  pci: ");
    print_uint(dev->bus);
    console_putchar(':');
    print_uint(dev->slot);
    console_putchar('.');
    print_uint(dev->func);
    console_puts("  id: ");
    print_hex16(dev->vendor_id);
    console_putchar(':');
    print_hex16(dev->device_id);
    console_putchar('\n');

    console_puts("  bar0: ");
    console_puts(dev->bar0_io ? "io 0x" : "mmio 0x");
    print_hex32(dev->bar0_base);
    console_puts(" raw 0x");
    print_hex32(dev->bar0_raw);
    console_putchar('\n');

    console_puts("  mac: ");
    if (dev->mac_valid) print_mac(dev->mac);
    else console_puts("unknown");
    console_putchar('\n');

    console_puts("  link: ");
    console_puts(dev->link_ready ? "ready\n" : "down/unsupported\n");
    if (dev->dhcp_ok) {
        console_puts("  ip: "); print_ip(dev->ip);
        console_puts("  mask: "); print_ip(dev->netmask);
        console_puts("\n  gateway: "); print_ip(dev->gateway);
        console_puts("  dns: "); print_ip(dev->dns);
        console_putchar('\n');
    }
}

static void cmd_dhcp(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (net_dhcp() < 0) {
        console_puts("dhcp: failed\n");
        return;
    }
    const net_device_t *dev = net_primary();
    console_puts("dhcp: ok ip=");
    print_ip(dev->ip);
    console_puts(" gw=");
    print_ip(dev->gateway);
    console_puts(" dns=");
    print_ip(dev->dns);
    console_putchar('\n');
}

static void cmd_ping(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: ping <ip|host>\n");
        return;
    }
    uint32_t ip;
    if (net_dns_resolve(argv[1], &ip) < 0) {
        console_puts("ping: resolve failed\n");
        return;
    }
    console_puts("PING ");
    print_ip(ip);
    console_puts(": ");
    if (net_ping(ip, 1000) == 0) console_puts("reply\n");
    else console_puts("timeout\n");
}

static void cmd_dns(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: dns <host>\n");
        return;
    }
    uint32_t ip;
    if (net_dns_resolve(argv[1], &ip) < 0) {
        console_puts("dns: failed\n");
        return;
    }
    console_puts(argv[1]);
    console_puts(" -> ");
    print_ip(ip);
    console_putchar('\n');
}

static void cmd_nettest(int argc, char **argv) {
    (void)argc;
    (void)argv;

    console_puts("[nettest] dhcp... ");
    if (net_dhcp() < 0) {
        console_puts("FAIL\n");
        return;
    }
    console_puts("OK\n");

    const net_device_t *dev = net_primary();
    console_puts("[nettest] ip ");
    print_ip(dev->ip);
    console_puts(" gw ");
    print_ip(dev->gateway);
    console_puts(" dns ");
    print_ip(dev->dns);
    console_putchar('\n');

    if (dev->gateway) {
        console_puts("[nettest] ping gateway... ");
        console_puts(net_ping(dev->gateway, 1000) == 0 ? "OK\n" : "FAIL\n");
    }

    if (dev->dns) {
        uint32_t ip;
        console_puts("[nettest] dns example.com... ");
        if (net_dns_resolve("example.com", &ip) == 0) {
            console_puts("OK ");
            print_ip(ip);
            console_putchar('\n');
        } else {
            console_puts("FAIL\n");
        }
    }
}

static int parse_port(const char **p, uint16_t *port) {
    uint32_t v = 0;
    if (**p < '0' || **p > '9') return -1;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (uint32_t)(*(*p)++ - '0');
        if (v == 0 || v > 65535) return -1;
    }
    *port = (uint16_t)v;
    return 0;
}

static const char *parse_url(const char *url, const char **host, uint16_t *port,
                             const char **path, char *host_buf, uint32_t host_cap) {
    const char *p = url;
    *port = 80;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strstr(p, "://")) return "only http:// is supported";

    uint32_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 >= host_cap) return "host too long";
        if (*p == ':') {
            p++;
            if (parse_port(&p, port) < 0) return "bad port";
            break;
        }
        host_buf[n++] = *p++;
    }
    while (*p && *p != '/') return "bad url";
    host_buf[n] = 0;
    if (n == 0) return "missing host";
    *host = host_buf;
    *path = *p ? p : "/";
    return 0;
}

static void print_http_body(const char *buf) {
    const char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;
    else body = buf;
    console_puts(body);
    if (body[0] && body[strlen(body) - 1] != '\n') console_putchar('\n');
}

static const char *http_body(const char *buf) {
    const char *body = strstr(buf, "\r\n\r\n");
    return body ? body + 4 : buf;
}

static void cmd_curl(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: curl http://host/path\n");
        return;
    }
    char host_buf[96];
    const char *host;
    const char *path;
    uint16_t port;
    const char *err = parse_url(argv[1], &host, &port, &path, host_buf, sizeof(host_buf));
    if (err) {
        console_puts("curl: ");
        console_puts(err);
        console_putchar('\n');
        return;
    }
    uint32_t ip;
    if (net_dns_resolve(host, &ip) < 0) {
        console_puts("curl: resolve failed\n");
        return;
    }
    static char response[8192];
    uint32_t len = 0;
    if (net_http_get(host, ip, port, path, response, sizeof(response), &len) < 0) {
        console_puts("curl: request failed\n");
        return;
    }
    print_http_body(response);
}

static void cmd_wget(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: wget [-S] http://host[:port]/path [file]\n");
        return;
    }
    int show_headers = 0;
    int url_arg = 1;
    if (strcmp(argv[1], "-S") == 0) {
        show_headers = 1;
        url_arg = 2;
    }
    if (argc <= url_arg) {
        console_puts("Usage: wget [-S] http://host[:port]/path [file]\n");
        return;
    }
    char host_buf[96];
    const char *host;
    const char *path;
    uint16_t port;
    const char *err = parse_url(argv[url_arg], &host, &port, &path, host_buf, sizeof(host_buf));
    if (err) {
        console_puts("wget: ");
        console_puts(err);
        console_putchar('\n');
        return;
    }
    uint32_t ip;
    if (net_dns_resolve(host, &ip) < 0) {
        console_puts("wget: resolve failed\n");
        return;
    }
    static char response[8192];
    uint32_t len = 0;
    if (net_http_get(host, ip, port, path, response, sizeof(response), &len) < 0) {
        console_puts("wget: request failed\n");
        return;
    }
    const char *payload = show_headers ? response : http_body(response);
    uint32_t payload_len = (uint32_t)strlen(payload);
    if (argc > url_arg + 1) {
        const char *file = argv[url_arg + 1];
        vfs_node_t *node = vfs_lookup(file);
        if (!node) node = vfs_create(file);
        if (!node || vfs_truncate(node) < 0 || vfs_write(node, 0, payload, payload_len) < 0) {
            console_puts("wget: save failed\n");
            return;
        }
        console_puts("saved ");
        console_puts(file);
        console_putchar('\n');
    } else {
        console_puts(payload);
        if (payload_len && payload[payload_len - 1] != '\n') console_putchar('\n');
    }
}

void tool_net_init(void) {
    static const command_t cmds[] = {
        {"netinfo", CMD_GROUP_DEBUG, "Show network controller info", "netinfo", cmd_netinfo},
        {"dhcp", CMD_GROUP_SYSTEM, "Configure network with DHCP", "dhcp", cmd_dhcp},
        {"nettest", CMD_GROUP_SYSTEM, "Run DHCP/gateway/DNS checks", "nettest", cmd_nettest},
        {"ping", CMD_GROUP_SYSTEM, "Send one ICMP echo request", "ping <ip|host>", cmd_ping},
        {"dns", CMD_GROUP_SYSTEM, "Resolve a host name", "dns <host>", cmd_dns},
        {"curl", CMD_GROUP_SYSTEM, "HTTP GET and print body", "curl http://host[:port]/path", cmd_curl},
        {"wget", CMD_GROUP_SYSTEM, "HTTP GET and print/save body", "wget [-S] http://host[:port]/path [file]", cmd_wget},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
