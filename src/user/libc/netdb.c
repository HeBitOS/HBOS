#include "netdb.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"

/* Linux 兼容层 DNS：getaddrinfo/gethostbyname → HBOS_SYS_DNS_RESOLVE
 * （内核 net_dns_resolve，DHCP/DNS 由内核栈完成）。v1：IPv4、数字端口
 * 或常见服务名；不支持 IPv6/多地址，但满足 Chromium 等程序的解析路径。 */

/* gethostbyname 错误码（libc 全局，静态即可） */
int h_errno;

static int dns_resolve(const char *name, uint32_t *ip) {
    long r = __syscall3(HBOS_SYS_DNS_RESOLVE, (long)name, (long)ip, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

struct hostent *gethostbyname(const char *name) {
    static char h_name_buf[256];
    static char *h_aliases[1];
    static char *h_addr_list[2];
    static char h_addr_buf[4];
    static struct hostent h;

    if (!name || !name[0]) {
        errno = EINVAL;
        return NULL;
    }
    uint32_t ip = 0;
    if (dns_resolve(name, &ip) < 0) {
        h_errno = 1;
        return NULL;
    }
    size_t n = strlen(name);
    if (n >= sizeof(h_name_buf)) n = sizeof(h_name_buf) - 1;
    memcpy(h_name_buf, name, n);
    h_name_buf[n] = 0;
    /* 网络字节序 IP（与 socket/connect 期望一致） */
    h_addr_buf[0] = (char)((ip >> 24) & 0xFF);
    h_addr_buf[1] = (char)((ip >> 16) & 0xFF);
    h_addr_buf[2] = (char)((ip >> 8) & 0xFF);
    h_addr_buf[3] = (char)(ip & 0xFF);
    h_aliases[0] = NULL;
    h_addr_list[0] = h_addr_buf;
    h_addr_list[1] = NULL;
    h.h_name = h_name_buf;
    h.h_aliases = h_aliases;
    h.h_addrtype = 2; /* AF_INET */
    h.h_length = 4;
    h.h_addr_list = h_addr_list;
    return &h;
}

static int port_from_service(const char *service, uint16_t *port) {
    if (!service || !service[0]) {
        *port = 0;
        return 0;
    }
    /* 数字端口 */
    int v = 0;
    const char *p = service;
    while (*p >= '0' && *p <= '9') { v = v * 10 + *p - '0'; p++; }
    if (!*p && v > 0 && v <= 65535) {
        *port = (uint16_t)v;
        return 0;
    }
    /* 常见服务名 */
    if (strcmp(service, "http") == 0) { *port = 80; return 0; }
    if (strcmp(service, "https") == 0) { *port = 443; return 0; }
    if (strcmp(service, "ssh") == 0) { *port = 22; return 0; }
    return EAI_SERVICE;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!res) return EAI_BADFLAGS;
    *res = NULL;
    if (!node || !node[0]) return EAI_NONAME;

    uint16_t port = 0;
    int pr = port_from_service(service, &port);
    if (pr != 0) return pr;

    int family = (hints && hints->ai_family) ? hints->ai_family : 2;
    if (family != 2 && family != 0) return EAI_FAMILY;

    uint32_t ip = 0;
    if (dns_resolve(node, &ip) < 0) return EAI_NONAME;

    struct addrinfo *ai = (struct addrinfo *)malloc(sizeof(*ai));
    if (!ai) return EAI_MEMORY;
    struct sockaddr_in *sa = (struct sockaddr_in *)malloc(sizeof(*sa));
    char *name_buf = (char *)malloc(strlen(node) + 1);
    if (!sa || !name_buf) {
        free(ai); free(sa); free(name_buf);
        return EAI_MEMORY;
    }
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = 2;
    sa->sin_port = htons(port);
    sa->sin_addr = ip;
    strcpy(name_buf, node);

    memset(ai, 0, sizeof(*ai));
    ai->ai_family = 2;
    ai->ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : 1;
    ai->ai_protocol = 0;
    ai->ai_addrlen = sizeof(*sa);
    ai->ai_addr = (struct sockaddr *)sa;
    ai->ai_canonname = (hints && (hints->ai_flags & AI_CANONNAME))
                           ? name_buf : NULL;
    ai->ai_next = NULL;
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}
