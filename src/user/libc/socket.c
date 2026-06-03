#include "socket.h"
#include "string.h"
#include "syscall.h"

int socket(int domain, int type, int protocol) {
    return (int)__syscall3(HBOS_SYS_SOCKET, domain, type, protocol);
}

int bind(int sockfd, const sockaddr_in *addr, size_t addrlen) {
    return (int)__syscall3(HBOS_SYS_BIND, sockfd, (long)addr, (long)addrlen);
}

int listen(int sockfd, int backlog) {
    return (int)__syscall3(HBOS_SYS_LISTEN, sockfd, backlog, 0);
}

int accept(int sockfd) {
    return (int)__syscall3(HBOS_SYS_ACCEPT, sockfd, 0, 0);
}

int connect(int sockfd, const sockaddr_in *addr, size_t addrlen) {
    return (int)__syscall3(HBOS_SYS_CONNECT, sockfd, (long)addr, (long)addrlen);
}

int send(int sockfd, const void *buf, size_t len, int flags) {
    return (int)__syscall3(HBOS_SYS_SEND, sockfd, (long)buf, (long)len);
    (void)flags;
}

int recv(int sockfd, void *buf, size_t len) {
    return (int)__syscall3(HBOS_SYS_RECV, sockfd, (long)buf, (long)len);
}

int close(int fd) {
    return (int)__syscall1(HBOS_SYS_CLOSE, fd);
}

uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) |
           ((hostlong & 0xFF00) << 8) |
           ((hostlong & 0xFF0000) >> 8) |
           ((hostlong & 0xFF000000) >> 24);
}

uint16_t htons(uint16_t hostshort) {
    return (uint16_t)(((hostshort & 0xFF) << 8) |
                      ((hostshort & 0xFF00) >> 8));
}

uint32_t ntohl(uint32_t netlong) {
    return htonl(netlong);
}

uint16_t ntohs(uint16_t netshort) {
    return htons(netshort);
}

uint32_t inet_addr(const char *cp) {
    if (!cp) return 0;

    uint32_t ip = 0;
    int byte = 0;
    int num = 0;
    int dots = 0;

    for (const char *p = cp; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            num = num * 10 + (*p - '0');
            if (num > 255) return 0;
        } else if (*p == '.') {
            if (dots >= 3) return 0;
            if (byte >= 4) return 0;
            ip = (ip << 8) | (uint32_t)num;
            num = 0;
            dots++;
            byte++;
        } else {
            return 0;
        }
    }

    if (dots != 3 || num > 255) return 0;
    ip = (ip << 8) | (uint32_t)num;

    return ip;
}