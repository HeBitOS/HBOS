#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libc/stdlib.h"
#include "../libc/socket.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: ping <ip> [port]\n");
        printf("  TCP connectivity test (no raw ICMP in user mode)\n");
        printf("  Example: ping 93.184.215.14 80\n");
        return 1;
    }

    uint32_t ip = inet_addr(argv[1]);
    if (ip == 0) {
        printf("ping: invalid IP address: %s\n", argv[1]);
        return 1;
    }

    uint16_t port = 80;
    if (argc >= 3) {
        port = (uint16_t)atoi(argv[2]);
        if (port == 0) {
            printf("ping: invalid port: %s\n", argv[2]);
            return 1;
        }
    }

    printf("ping %s port %d ... ", argv[1], port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("FAIL (socket)\n");
        return 1;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr   = ip;

    int ret = connect(sock, &addr, sizeof(addr));
    if (ret < 0) {
        printf("FAIL (connect)\n");
        close(sock);
        return 1;
    }

    printf("OK\n");
    close(sock);
    return 0;
}