#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

/* G1 AF_INET 就绪查询验证：poll/epoll 对 TCP socket 返回真实 POLLIN/
 * POLLOUT（旧实现一律"双向就绪"）。连接内核 httpd（回环）后发 GET，
 * 不 recv 先 poll——响应到达时应报 POLLIN；未 connect 的 socket 应报
 * POLLOUT。用法：run linux_sockpoll <ip> <port> */

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

    /* 1. 未 connect 的 socket：poll POLLOUT 应立即就绪 */
    int raw = socket(2, 1, 0);
    if (raw < 0) { say("LINUX_SOCKPOLL: FAIL (socket)\n"); return 1; }
    struct pollfd pw = { raw, POLLOUT, 0 };
    int rw = poll(&pw, 1, 0);
    if (rw != 1 || !(pw.revents & POLLOUT)) {
        say("LINUX_SOCKPOLL: FAIL (unconnected POLLOUT)\n");
        return 1;
    }
    close(raw);

    /* 2. 连接 httpd，发 GET，不 recv 先 poll POLLIN */
    int fd = socket(2, 1, 0);
    if (fd < 0) { say("LINUX_SOCKPOLL: FAIL (socket2)\n"); return 1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = 2;
    addr.sin_port = htons(port);
    addr.sin_addr = inet_addr(ip);
    if (connect(fd, (void *)&addr, sizeof(addr)) < 0) {
        say("LINUX_SOCKPOLL: FAIL (connect)\n");
        return 1;
    }
    long n = send(fd, REQ, strlen(REQ), 0);
    if (n != (long)strlen(REQ)) {
        say("LINUX_SOCKPOLL: FAIL (send)\n");
        return 1;
    }

    struct pollfd pr = { fd, POLLIN, 0 };
    int rr = poll(&pr, 1, 5000);
    if (rr != 1 || !(pr.revents & POLLIN)) {
        say("LINUX_SOCKPOLL: FAIL (no POLLIN after response)\n");
        return 1;
    }

    /* 3. epoll 同样应报就绪（epoll_wait 走同一 fd_ready_mask） */
    int ep = epoll_create1(0);
    if (ep < 0) {
        say("LINUX_SOCKPOLL: FAIL (epoll_create1)\n");
        return 1;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
        say("LINUX_SOCKPOLL: FAIL (epoll_ctl)\n");
        return 1;
    }
    struct epoll_event evs[2];
    int ew = epoll_wait(ep, evs, 2, 5000);
    close(ep);
    if (ew != 1 || !(evs[0].events & EPOLLIN)) {
        say("LINUX_SOCKPOLL: FAIL (epoll no POLLIN)\n");
        return 1;
    }
    /* 2b. recv 应能拿到完整响应 */
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
        say("LINUX_SOCKPOLL: FAIL (recv content)\n");
        return 1;
    }
    say("LINUX_SOCKPOLL: PASS\n");
    return 0;
}
