#include "../graphics/graphics.h"
#include "../fcntl.h"
#include "../net.h"
#include "../string.h"
#include "../tls.h"
#include "../unistd.h"
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

static void print_uint64(uint64_t v) {
    char buf[21];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < (int)sizeof(buf));
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
    console_puts("  rx: packets=");
    print_uint64(dev->rx_packets);
    console_puts(" bytes=");
    print_uint64(dev->rx_bytes);
    console_puts(" dropped=");
    print_uint64(dev->rx_dropped);
    console_putchar('\n');
    console_puts("  tx: packets=");
    print_uint64(dev->tx_packets);
    console_puts(" bytes=");
    print_uint64(dev->tx_bytes);
    console_puts(" errors=");
    print_uint64(dev->tx_errors);
    console_putchar('\n');
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
        console_puts("dhcp: ");
        console_puts(net_last_error());
        console_putchar('\n');
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

static void print_config(const net_device_t *dev) {
    console_puts("ip=");
    print_ip(dev->ip);
    console_puts(" mask=");
    print_ip(dev->netmask);
    console_puts(" gw=");
    print_ip(dev->gateway);
    console_puts(" dns=");
    print_ip(dev->dns);
    console_putchar('\n');
}

static void cmd_ifconfig(int argc, char **argv) {
    const net_device_t *dev = net_primary();
    if (argc == 1) {
        if (!dev->present) {
            console_puts("ifconfig: no ethernet controller\n");
            return;
        }
        console_puts("eth0 ");
        console_puts(dev->link_ready ? "UP " : "DOWN ");
        if (dev->mac_valid) {
            console_puts("mac ");
            print_mac(dev->mac);
            console_putchar(' ');
        }
        if (dev->dhcp_ok) print_config(dev);
        else console_puts("not configured\n");
        console_puts("      RX packets ");
        print_uint64(dev->rx_packets);
        console_puts(" bytes ");
        print_uint64(dev->rx_bytes);
        console_puts("  TX packets ");
        print_uint64(dev->tx_packets);
        console_puts(" bytes ");
        print_uint64(dev->tx_bytes);
        console_putchar('\n');
        return;
    }

    if (argc >= 2 && strcmp(argv[1], "dhcp") == 0) {
        cmd_dhcp(argc, argv);
        return;
    }

    if (argc != 5) {
        console_puts("Usage: ifconfig | ifconfig dhcp | ifconfig <ip> <mask> <gw> <dns>\n");
        return;
    }

    uint32_t ip = net_parse_ipv4(argv[1]);
    uint32_t mask = net_parse_ipv4(argv[2]);
    uint32_t gw = net_parse_ipv4(argv[3]);
    uint32_t dns = net_parse_ipv4(argv[4]);
    if (!ip || !mask || net_configure(ip, mask, gw, dns) < 0) {
        console_puts("ifconfig: ");
        console_puts(net_last_error());
        console_putchar('\n');
        return;
    }
    console_puts("ifconfig: ok ");
    print_config(net_primary());
}

static void cmd_ping(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: ping <ip|host>\n");
        return;
    }
    uint32_t ip;
    if (net_dns_resolve(argv[1], &ip) < 0) {
        console_puts("ping: ");
        console_puts(net_last_error());
        console_putchar('\n');
        return;
    }
    console_puts("PING ");
    print_ip(ip);
    console_puts(": ");
    if (net_ping(ip, 1000) == 0) console_puts("reply\n");
    else {
        console_puts(net_last_error());
        console_putchar('\n');
    }
}

static void cmd_dns(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: dns <host>\n");
        return;
    }
    uint32_t ip;
    if (net_dns_resolve(argv[1], &ip) < 0) {
        console_puts("dns: ");
        console_puts(net_last_error());
        console_putchar('\n');
        return;
    }
    console_puts(argv[1]);
    console_puts(" -> ");
    print_ip(ip);
    console_putchar('\n');
}

static void cmd_nslookup(int argc, char **argv) {
    cmd_dns(argc, argv);
}

static void cmd_nettest(int argc, char **argv) {
    (void)argc;
    (void)argv;

    console_puts("[nettest] dhcp... ");
    if (net_dhcp() < 0) {
        console_puts("FAIL ");
        console_puts(net_last_error());
        console_putchar('\n');
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
        if (net_ping(dev->gateway, 1000) == 0) console_puts("OK\n");
        else {
            console_puts("FAIL ");
            console_puts(net_last_error());
            console_putchar('\n');
        }
    }

    {
        uint32_t ip;
        if (!dev->dns) console_puts("[nettest] (no DNS from DHCP, using 8.8.8.8 fallback)\n");
        console_puts("[nettest] dns example.com... ");
        if (net_dns_resolve("example.com", &ip) == 0) {
            console_puts("OK ");
            print_ip(ip);
            console_putchar('\n');
        } else {
            console_puts("FAIL ");
            console_puts(net_last_error());
            console_putchar('\n');
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
    else if (strncmp(p, "https://", 8) == 0) { p += 8; *port = 443; }
    else if (strstr(p, "://")) return "only http:// and https:// are supported";

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

static int http_status_code(const char *buf) {
    if (strncmp(buf, "HTTP/", 5) != 0) return 0;
    const char *p = strchr(buf, ' ');
    if (!p) return 0;
    p++;
    int code = 0;
    while (*p >= '0' && *p <= '9') code = code * 10 + (*p++ - '0');
    return code;
}

static const char *http_header_value(const char *buf, const char *name) {
    uint32_t name_len = (uint32_t)strlen(name);
    const char *p = strstr(buf, "\r\n");
    if (!p) return 0;
    p += 2;
    while (*p && !(p[0] == '\r' && p[1] == '\n')) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) return 0;
        if (strncmp(p, name, name_len) == 0 && p[name_len] == ':') {
            p += name_len + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        p = line_end + 2;
    }
    return 0;
}

static uint32_t copy_header_value(char *dst, uint32_t cap, const char *value) {
    if (!value || cap == 0) return 0;
    uint32_t n = 0;
    while (value[n] && !(value[n] == '\r' && value[n + 1] == '\n') && n + 1 < cap) {
        dst[n] = value[n];
        n++;
    }
    dst[n] = 0;
    return n;
}

static void copy_url_basename(char *dst, uint32_t cap, const char *url) {
    const char *end = url + strlen(url);
    while (end > url && (end[-1] == '/' || end[-1] == '?' || end[-1] == '#')) end--;
    const char *p = end;
    while (p > url && p[-1] != '/') p--;
    if (!*p || strstr(p, "://")) p = "index.html";

    uint32_t n = 0;
    while (p[n] && p + n < end && p[n] != '?' && p[n] != '#' && n + 1 < cap) {
        dst[n] = p[n];
        n++;
    }
    if (n == 0) {
        const char *fallback = "index.html";
        while (fallback[n] && n + 1 < cap) {
            dst[n] = fallback[n];
            n++;
        }
    }
    dst[n] = 0;
}

static int http_fetch(const char *method, const char *url, char *response,
                      uint32_t response_cap, uint32_t *len) {
    int https = strncmp(url, "https://", 8) == 0;
    char current[160];
    uint32_t n = 0;
    while (url[n] && n + 1 < sizeof(current)) {
        current[n] = url[n];
        n++;
    }
    current[n] = 0;

    for (int hop = 0; hop < 4; hop++) {
        char host_buf[96];
        const char *host;
        const char *path;
        uint16_t port;
        if (parse_url(current, &host, &port, &path, host_buf, sizeof(host_buf)))
            return -1;

        uint32_t ip;
        if (net_dns_resolve(host, &ip) < 0) return -1;
        if (https) {
            if (strcmp(method, "HEAD") == 0) method = "GET";
            if (tls_https_get(host, ip, port, path, response, response_cap, len) < 0)
                return -1;
        } else {
            if (net_http_request(method, host, ip, port, path, response, response_cap, len) < 0)
                return -1;
        }

        int code = http_status_code(response);
        if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308)
            return 0;

        const char *loc = http_header_value(response, "Location");
        char next[160];
        if (copy_header_value(next, sizeof(next), loc) == 0) return 0;

        if (strncmp(next, "http://", 7) == 0) {
            https = 0;
            strcpy(current, next);
        } else if (strncmp(next, "https://", 8) == 0) {
            if (!https) {
                return 0;
            }
            https = 1;
            strcpy(current, next);
        } else if (next[0] == '/') {
            uint32_t pos = 0;
            const char *prefix = https ? "https://" : "http://";
            for (const char *p = prefix; *p && pos + 1 < sizeof(current); p++) current[pos++] = *p;
            for (const char *p = host; *p && pos + 1 < sizeof(current); p++) current[pos++] = *p;
            if (port != 80 && pos + 8 < sizeof(current)) {
                current[pos++] = ':';
                char digits[6];
                uint16_t tmp = port;
                int d = 0;
                do { digits[d++] = (char)('0' + (tmp % 10)); tmp /= 10; } while (tmp);
                while (d-- && pos + 1 < sizeof(current)) current[pos++] = digits[d];
            }
            for (const char *p = next; *p && pos + 1 < sizeof(current); p++) current[pos++] = *p;
            current[pos] = 0;
        } else {
            return 0;
        }
        if (code == 303) method = "GET";
    }
    return 0;
}

static void print_http_headers(const char *buf) {
    const char *end = strstr(buf, "\r\n\r\n");
    if (!end) {
        console_puts(buf);
        return;
    }
    uint32_t len = (uint32_t)(end - buf);
    console_write(buf, len);
    console_putchar('\n');
}

static void print_curl_response(const char *buf) {
    const char *body = http_body(buf);
    if (body[0]) {
        print_http_body(buf);
        return;
    }
    int code = http_status_code(buf);
    if (code >= 300 && code < 400) {
        print_http_headers(buf);
        const char *loc = http_header_value(buf, "Location");
        if (loc) {
            char value[160];
            copy_header_value(value, sizeof(value), loc);
            console_puts("redirect: ");
            console_puts(value);
            console_putchar('\n');
            if (strncmp(value, "https://", 8) == 0)
                console_puts("https: tls handshake/decrypt is still being implemented\n");
        }
        return;
    }
    print_http_headers(buf);
}

static void cmd_curl(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: curl [-I] http[s]://host[:port]/path\n");
        return;
    }
    int headers_only = 0;
    int url_arg = 1;
    if (strcmp(argv[1], "-I") == 0) {
        headers_only = 1;
        url_arg = 2;
    }
    if (argc <= url_arg) {
        console_puts("Usage: curl [-I] http[s]://host[:port]/path\n");
        return;
    }
    static char response[8192];
    uint32_t len = 0;
    if (http_fetch(headers_only ? "HEAD" : "GET", argv[url_arg],
                   response, sizeof(response), &len) < 0) {
        console_puts("curl: ");
        console_puts(strncmp(argv[url_arg], "https://", 8) == 0 ? tls_last_error() : net_last_error());
        console_putchar('\n');
        return;
    }
    if (headers_only) print_http_headers(response);
    else print_curl_response(response);
    if (headers_only)
        console_puts("(curl -I shows headers only; use curl URL for body)\n");
}

static void cmd_wget(int argc, char **argv) {
    if (argc < 2) {
        console_puts("Usage: wget [-S] [-O file] http[s]://host[:port]/path [file]\n");
        return;
    }
    int show_headers = 0;
    const char *out_name = 0;
    const char *url = 0;
    int print_stdout = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) {
            show_headers = 1;
        } else if (strcmp(argv[i], "-O") == 0) {
            if (i + 1 >= argc) {
                console_puts("wget: missing -O file\n");
                return;
            }
            out_name = argv[++i];
            if (strcmp(out_name, "-") == 0) print_stdout = 1;
        } else if (!url) {
            url = argv[i];
        } else if (!out_name) {
            out_name = argv[i];
        } else {
            console_puts("Usage: wget [-S] [-O file] http[s]://host[:port]/path [file]\n");
            return;
        }
    }
    if (!url) {
        console_puts("Usage: wget [-S] [-O file] http[s]://host[:port]/path [file]\n");
        return;
    }
    static char response[8192];
    uint32_t len = 0;
    if (http_fetch("GET", url, response, sizeof(response), &len) < 0) {
        console_puts("wget: ");
        console_puts(strncmp(url, "https://", 8) == 0 ? tls_last_error() : net_last_error());
        console_putchar('\n');
        return;
    }
    const char *payload = show_headers ? response : http_body(response);
    uint32_t payload_len = (uint32_t)strlen(payload);
    char inferred[64];
    if (!out_name && !print_stdout) {
        copy_url_basename(inferred, sizeof(inferred), url);
        out_name = inferred;
    }
    if (out_name && !print_stdout) {
        const char *file = out_name;
        int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0 || write(fd, payload, payload_len) != (ssize_t)payload_len) {
            if (fd >= 0) close(fd);
            console_puts("wget: save failed\n");
            return;
        }
        close(fd);
        console_puts("saved ");
        console_puts(file);
        console_puts(" (HTTP ");
        print_uint((uint32_t)http_status_code(response));
        console_putchar(')');
        console_putchar('\n');
    } else {
        console_puts(payload);
        if (payload_len && payload[payload_len - 1] != '\n') console_putchar('\n');
    }
}

static void cmd_httpd(int argc, char **argv) {
    uint16_t port = 80;
    if (argc >= 2) {
        port = 0;
        const char *s = argv[1];
        while (*s >= '0' && *s <= '9') { port = port * 10 + (uint16_t)(*s - '0'); s++; }
    }
    if (port == 0) port = 80;
    console_puts("httpd: listening on port ");
    print_uint(port);
    console_puts("...\n");

    if (net_tcp_listen(port) < 0) {
        console_puts("httpd: listen failed: ");
        console_puts(net_last_error());
        console_putchar('\n');
        return;
    }

    while (1) {
        net_tcp_conn_t conn;
        if (net_tcp_accept(port, &conn, 5000) < 0) continue;

        /* Read request */
        uint8_t req[512];
        uint32_t rlen = 0;
        net_tcp_recv(&conn, req, sizeof(req) - 1, &rlen, 4);
        req[rlen] = '\0';

        /* Parse method and path */
        char method[8] = {0};
        char path[128] = {0};
        const char *p = (const char *)req;
        int mi = 0;
        while (*p && *p != ' ' && mi < 7) method[mi++] = *p++;
        while (*p == ' ') p++;
        int pi = 0;
        while (*p && *p != ' ' && *p != '\r' && pi < 127) path[pi++] = *p++;
        path[pi] = '\0';

        /* Strip leading / */
        const char *filepath = path;
        if (filepath[0] == '/') filepath++;

        console_puts("httpd: ");
        console_puts(method);
        console_putchar(' ');
        console_puts(filepath);
        console_putchar('\n');

        /* Build response */
        if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
            const char *resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n";
            net_tcp_send(&conn, (const uint8_t *)resp, (uint32_t)strlen(resp));
            net_tcp_close(&conn);
            continue;
        }

        /* Try to read file */
        vfs_node_t *node = vfs_lookup(filepath);
        if (!node) {
            const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            net_tcp_send(&conn, (const uint8_t *)resp, (uint32_t)strlen(resp));
            net_tcp_close(&conn);
            continue;
        }

        /* Determine content type */
        const char *ctype = "text/plain";
        int flen = (int)strlen(filepath);
        if (flen > 5 && strcmp(filepath + flen - 5, ".html") == 0) ctype = "text/html";
        else if (flen > 4 && strcmp(filepath + flen - 4, ".htm") == 0) ctype = "text/html";
        else if (flen > 3 && strcmp(filepath + flen - 3, ".js") == 0) ctype = "application/javascript";
        else if (flen > 4 && strcmp(filepath + flen - 4, ".css") == 0) ctype = "text/css";
        else if (flen > 4 && strcmp(filepath + flen - 4, ".png") == 0) ctype = "image/png";
        else if (flen > 4 && strcmp(filepath + flen - 4, ".jpg") == 0) ctype = "image/jpeg";

        uint32_t fsize = node->size;

        /* Build header */
        char hdr[256];
        uint32_t hp = 0;
        const char *prefix = "HTTP/1.1 200 OK\r\nContent-Type: ";
        while (*prefix && hp < 255) hdr[hp++] = *prefix++;
        while (*ctype && hp < 255) hdr[hp++] = *ctype++;
        const char *mid = "\r\nContent-Length: ";
        while (*mid && hp < 255) hdr[hp++] = *mid++;
        /* Append size as decimal */
        char siz[16]; int si = 0; uint32_t sv = fsize;
        do { siz[si++] = '0' + sv % 10; sv /= 10; } while (sv);
        while (si-- > 0 && hp < 255) hdr[hp++] = siz[si];
        const char *end = "\r\nConnection: close\r\n\r\n";
        while (*end && hp < 255) hdr[hp++] = *end++;
        hdr[hp] = '\0';

        net_tcp_send(&conn, (const uint8_t *)hdr, hp);

        /* Send file body */
        if (strcmp(method, "GET") == 0 && fsize > 0) {
            uint8_t buf[512];
            uint32_t off = 0;
            while (off < fsize) {
                uint32_t chunk = fsize - off;
                if (chunk > 512) chunk = 512;
                ssize_t n = vfs_read(node, off, buf, chunk);
                if (n <= 0) break;
                net_tcp_send(&conn, buf, (uint32_t)n);
                off += (uint32_t)n;
            }
        }

        net_tcp_close(&conn);
    }
}

void tool_net_init(void) {
    static const command_t cmds[] = {
        {"netinfo", CMD_GROUP_DEBUG, "Show network controller info", "netinfo", cmd_netinfo},
        {"dhcp", CMD_GROUP_SYSTEM, "Configure network with DHCP", "dhcp", cmd_dhcp},
        {"ifconfig", CMD_GROUP_SYSTEM, "Show or configure IPv4", "ifconfig | ifconfig dhcp | ifconfig <ip> <mask> <gw> <dns>", cmd_ifconfig},
        {"nettest", CMD_GROUP_SYSTEM, "Run DHCP/gateway/DNS checks", "nettest", cmd_nettest},
        {"ping", CMD_GROUP_SYSTEM, "Send one ICMP echo request", "ping <ip|host>", cmd_ping},
        {"dns", CMD_GROUP_SYSTEM, "Resolve a host name", "dns <host>", cmd_dns},
        {"nslookup", CMD_GROUP_SYSTEM, "Resolve a host name", "nslookup <host>", cmd_nslookup},
        {"curl", CMD_GROUP_SYSTEM, "HTTP(S) GET/HEAD", "curl [-I] http[s]://host[:port]/path", cmd_curl},
        {"wget", CMD_GROUP_SYSTEM, "HTTP(S) GET and print/save body", "wget [-S] [-O file] http[s]://host[:port]/path [file]", cmd_wget},
        {"httpd", CMD_GROUP_SYSTEM, "Simple HTTP file server", "httpd [port]", cmd_httpd},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
