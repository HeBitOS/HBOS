#ifndef HBOS_USER_LIBC_SOCKET_H
#define HBOS_USER_LIBC_SOCKET_H

#include <stdint.h>
#include <stddef.h>

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} sockaddr_in;

int     socket(int domain, int type, int protocol);
int     bind(int sockfd, const sockaddr_in *addr, size_t addrlen);
int     listen(int sockfd, int backlog);
int     accept(int sockfd);
int     connect(int sockfd, const sockaddr_in *addr, size_t addrlen);
int     send(int sockfd, const void *buf, size_t len, int flags);
int     recv(int sockfd, void *buf, size_t len);
int     close(int fd);

uint32_t htonl(uint32_t hostlong);
uint16_t htons(uint16_t hostshort);
uint32_t ntohl(uint32_t netlong);
uint16_t ntohs(uint16_t netshort);

uint32_t inet_addr(const char *cp);

#endif