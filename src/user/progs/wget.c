#include "../libc/stdio.h"
#include "../libc/string.h"
#include "../libc/stdlib.h"
#include "../libc/socket.h"

static int parse_url(const char *url, char *host, size_t host_sz,
                     uint16_t *port, char *path, size_t path_sz) {
    const char *p = url;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        printf("wget: https not supported\n");
        return -1;
    }

    const char *host_start = p;
    while (*p && *p != ':' && *p != '/') p++;

    size_t host_len = (size_t)(p - host_start);
    if (host_len >= host_sz) return -1;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    *port = 80;
    if (*p == ':') {
        p++;
        int pnum = 0;
        while (*p >= '0' && *p <= '9') {
            pnum = pnum * 10 + (*p - '0');
            p++;
        }
        if (pnum > 0 && pnum < 65536) *port = (uint16_t)pnum;
    }

    if (*p == '/') {
        const char *path_start = p;
        size_t path_len = strlen(path_start);
        if (path_len >= path_sz) return -1;
        memcpy(path, path_start, path_len);
        path[path_len] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: wget <url>\n");
        printf("  Example: wget http://example.com/\n");
        printf("  Hostnames are not resolved; use IP addresses directly.\n");
        printf("  Example: wget http://93.184.215.14/\n");
        return 1;
    }

    char host[256];
    char path[1024];
    uint16_t port;

    if (parse_url(argv[1], host, sizeof(host), &port, path, sizeof(path)) < 0) {
        printf("wget: invalid URL: %s\n", argv[1]);
        return 1;
    }

    uint32_t ip = inet_addr(host);
    if (ip == 0) {
        printf("wget: cannot resolve hostname '%s' (use IP)\n", host);
        return 1;
    }

    printf("Connecting to %s:%d ...\n", host, port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("wget: socket failed\n");
        return 1;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr   = ip;

    if (connect(sock, &addr, sizeof(addr)) < 0) {
        printf("wget: connect failed\n");
        close(sock);
        return 1;
    }

    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: HBOS-wget/0.1\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    printf("Sending request...\n");
    int sent = send(sock, request, (size_t)req_len, 0);
    if (sent < 0) {
        printf("wget: send failed\n");
        close(sock);
        return 1;
    }

    printf("--- Response ---\n");

    char buf[4096];
    while (1) {
        int n = recv(sock, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        printf("%s", buf);
    }

    printf("\n--- End ---\n");
    close(sock);
    return 0;
}