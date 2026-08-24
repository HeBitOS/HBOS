#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

/* G1 非阻塞 connect 验证：SOCK_NONBLOCK 的 AF_INET socket 上 connect
 * 应立即返回 EINPROGRESS，poll(POLLOUT) 在握手完成后就绪，
 * getsockopt(SO_ERROR)==0，随后 send/recv 正常。
 * 用法：run linux_nbconnect <ip> <port> */

static void say(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    while (n) {
        long w = write(1, s, n);
        if (w <= 0) break;
        s += w; n -= (size_t)w;
    }
}

static int parse_port(const char *s) {
    int v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; s++; }
    return v;
}

#define REQ "GET /a.txt HTTP/1.0\r\nHost: hbos\r\n\r\n"
#define NEEDLE "hello single-conn"

int main(int argc, char **argv) {
    const char *ip = (argc > 1) ? argv[1] : "10.0.2.15";
    uint16_t port = (argc > 2) ? (uint16_t)parse_port(argv[2]) : 18080;

    int fd = socket(2, 1 | 0x800, 0); /* AF_INET, SOCK_STREAM|SOCK_NONBLOCK */
    if (fd < 0) { say("LINUX_NBCONNECT: FAIL (socket)\n"); return 1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = 2;
    addr.sin_port = htons(port);
    addr.sin_addr = inet_addr(ip);
    int rc = connect(fd, (void *)&addr, sizeof(addr));
    if (rc != -1 || errno != EINPROGRESS) {
        say("LINUX_NBCONNECT: FAIL (not EINPROGRESS)\n");
        return 1;
    }

    /* poll POLLOUT：握手完成时（内核 httpd 已回 SYN-ACK）就绪 */
    struct pollfd pw = { fd, POLLOUT, 0 };
    int rw = poll(&pw, 1, 5000);
    if (rw != 1 || !(pw.revents & POLLOUT)) {
        say("LINUX_NBCONNECT: FAIL (no POLLOUT)\n");
        return 1;
    }
    int err = 0;
    socklen_t el = sizeof(err);
    if (getsockopt(fd, 1, 4, &err, &el) < 0 || err != 0) {
        say("LINUX_NBCONNECT: FAIL (SO_ERROR)\n");
        return 1;
    }
    long n = send(fd, REQ, strlen(REQ), 0);
    if (n != (long)strlen(REQ)) {
        say("LINUX_NBCONNECT: FAIL (send)\n");
        return 1;
    }
    char buf[4096];
    size_t total = 0;
    while (total + 1 < sizeof(buf)) {
        long r = recv(fd, buf + total, sizeof(buf) - total - 1, 0);
        if (r <= 0) break;
        total += (size_t)r;
        buf[total] = 0;
        if (strstr(buf, NEEDLE)) break;
    }
    buf[total] = 0;
    close(fd);
    if (!strstr(buf, NEEDLE)) {
        say("LINUX_NBCONNECT: FAIL (recv content)\n");
        return 1;
    }
    say("LINUX_NBCONNECT: PASS\n");
    return 0;
}
