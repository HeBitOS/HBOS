#include "socket.h"
#include "string.h"
#include "syscall.h"
#include "errno.h"

int socket(int domain, int type, int protocol) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_SOCKET, domain, type, protocol));
}

int socketpair(int domain, int type, int protocol, int pair[2]) {
    return (int)__syscall_errno(__syscall6(
        HBOS_SYS_SOCKETPAIR, domain, type, protocol, (long)pair, 0, 0));
}

int bind(int sockfd, const void *addr, socklen_t addrlen) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_BIND, sockfd, (long)addr, (long)addrlen));
}

int listen(int sockfd, int backlog) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_LISTEN, sockfd, backlog, 0));
}

int accept(int sockfd, void *addr, socklen_t *addrlen) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_ACCEPT, sockfd, (long)addr, (long)addrlen));
}

int accept4(int sockfd, void *addr, socklen_t *addrlen, int flags) {
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    int fd = accept(sockfd, addr, addrlen);
    if (fd < 0) return -1;
    if (flags & SOCK_NONBLOCK)
        (void)__syscall3(HBOS_SYS_FCNTL, fd, 2, SOCK_NONBLOCK);
    if (flags & SOCK_CLOEXEC)
        (void)__syscall3(HBOS_SYS_FCNTL, fd, 2, SOCK_CLOEXEC);
    return fd;
}

int connect(int sockfd, const void *addr, socklen_t addrlen) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_CONNECT, sockfd, (long)addr, (long)addrlen));
}

int getsockname(int sockfd, void *addr, socklen_t *addrlen) {
    return (int)__syscall_errno(__syscall3(
        HBOS_SYS_GETSOCKNAME, sockfd, (long)addr, (long)addrlen));
}

int getpeername(int sockfd, void *addr, socklen_t *addrlen) {
    return (int)__syscall_errno(__syscall3(
        HBOS_SYS_GETPEERNAME, sockfd, (long)addr, (long)addrlen));
}

long send(int sockfd, const void *buf, size_t len, int flags) {
    return __syscall_errno(__syscall6(
        HBOS_SYS_SEND, sockfd, (long)buf, (long)len, flags, 0, 0));
}

long recv(int sockfd, void *buf, size_t len, int flags) {
    return __syscall_errno(__syscall6(
        HBOS_SYS_RECV, sockfd, (long)buf, (long)len, flags, 0, 0));
}

long sendto(int sockfd, const void *buf, size_t len, int flags,
            const void *addr, socklen_t addrlen) {
    if (addr && connect(sockfd, addr, addrlen) < 0 && errno != EISCONN)
        return -1;
    return send(sockfd, buf, len, flags);
}

long recvfrom(int sockfd, void *buf, size_t len, int flags,
              void *addr, socklen_t *addrlen) {
    if (addrlen) *addrlen = 0;
    (void)addr;
    return recv(sockfd, buf, len, flags);
}

long sendmsg(int sockfd, const struct msghdr *message, int flags) {
    return __syscall_errno(__syscall3(
        HBOS_SYS_SENDMSG, sockfd, (long)message, flags));
}

long recvmsg(int sockfd, struct msghdr *message, int flags) {
    return __syscall_errno(__syscall3(
        HBOS_SYS_RECVMSG, sockfd, (long)message, flags));
}

int getsockopt(int sockfd, int level, int option,
               void *value, socklen_t *length) {
    return (int)__syscall_errno(__syscall6(
        HBOS_SYS_GETSOCKOPT, sockfd, level, option,
        (long)value, (long)length, 0));
}

int setsockopt(int sockfd, int level, int option,
               const void *value, socklen_t length) {
    return (int)__syscall_errno(__syscall6(
        HBOS_SYS_SETSOCKOPT, sockfd, level, option,
        (long)value, length, 0));
}

int shutdown(int sockfd, int how) {
    return (int)__syscall_errno(
        __syscall3(HBOS_SYS_SHUTDOWN, sockfd, how, 0));
}

int close(int fd) {
    return (int)__syscall_errno(__syscall1(HBOS_SYS_CLOSE, fd));
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
