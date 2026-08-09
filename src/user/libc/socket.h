#ifndef HBOS_USER_LIBC_SOCKET_H
#define HBOS_USER_LIBC_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <sys/uio.h>

#define AF_UNIX       1
#define AF_LOCAL      AF_UNIX
#define AF_INET       2
#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000
#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000
#define MSG_CTRUNC     0x08

#define SOL_SOCKET    1
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_PASSCRED   16
#define SO_PEERCRED   17
#define SCM_RIGHTS    1

#define CMSG_ALIGN(length) \
    (((length) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(length) \
    (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(length))
#define CMSG_LEN(length) \
    (CMSG_ALIGN(sizeof(struct cmsghdr)) + (length))
#define CMSG_DATA(header) \
    ((unsigned char *)(header) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(message) \
    ((message)->msg_controllen >= sizeof(struct cmsghdr) \
        ? (struct cmsghdr *)(message)->msg_control : (struct cmsghdr *)0)

#define SHUT_RD       0
#define SHUT_WR       1
#define SHUT_RDWR     2

typedef unsigned int socklen_t;

typedef struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
} sockaddr;

typedef struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
} sockaddr_un;

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

struct ucred {
    int pid;
    unsigned int uid;
    unsigned int gid;
};

typedef struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} sockaddr_in;

int     socket(int domain, int type, int protocol);
int     socketpair(int domain, int type, int protocol, int pair[2]);
int     bind(int sockfd, const void *addr, socklen_t addrlen);
int     listen(int sockfd, int backlog);
int     accept(int sockfd, void *addr, socklen_t *addrlen);
int     accept4(int sockfd, void *addr, socklen_t *addrlen, int flags);
int     connect(int sockfd, const void *addr, socklen_t addrlen);
int     getsockname(int sockfd, void *addr, socklen_t *addrlen);
int     getpeername(int sockfd, void *addr, socklen_t *addrlen);
long    send(int sockfd, const void *buf, size_t len, int flags);
long    recv(int sockfd, void *buf, size_t len, int flags);
long    sendto(int sockfd, const void *buf, size_t len, int flags,
               const void *addr, socklen_t addrlen);
long    recvfrom(int sockfd, void *buf, size_t len, int flags,
                 void *addr, socklen_t *addrlen);
long    sendmsg(int sockfd, const struct msghdr *message, int flags);
long    recvmsg(int sockfd, struct msghdr *message, int flags);
int     getsockopt(int sockfd, int level, int option,
                   void *value, socklen_t *length);
int     setsockopt(int sockfd, int level, int option,
                   const void *value, socklen_t length);
int     shutdown(int sockfd, int how);
int     close(int fd);

uint32_t htonl(uint32_t hostlong);
uint16_t htons(uint16_t hostshort);
uint32_t ntohl(uint32_t netlong);
uint16_t ntohs(uint16_t netshort);

uint32_t inet_addr(const char *cp);

#endif
