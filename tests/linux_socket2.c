#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* G1 多连接 TCP 验证（全内核路径，无 slirp）：两个 AF_INET 流 socket
 * 同时保持打开，连到内核 httpd（或任意 TCP 服务器）。先都连接并发送，
 * 然后先读第二个连接——第一个连接的响应必须在轮询第二个连接时被内核
 * 分发器缓冲进自己的槽（旧单连接轮询会丢掉）。
 * 用法：run linux_socket2 <ip> <port> [<port2>]  */

static void say(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    while (n) {
        long w = write(1, s, n);
        if (w <= 0) break;
        s += w; n -= (size_t)w;
    }
}

static void say_num(const char *tag, long v) {
    char b[48];
    int i = 0;
    while (tag[i]) { b[i] = tag[i]; i++; }
    if (v < 0) { b[i++] = '-'; v = -v; }
    char tmp[16]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t) b[i++] = tmp[--t];
    b[i++] = '\n'; b[i] = 0;
    say(b);
}

static int parse_port(const char *s) {
    int v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; s++; }
    return v;
}

#define REQ "GET /a.txt HTTP/1.0\r\nHost: hbos\r\n\r\n"
#define NEEDLE "hello single-conn"

static int open_and_send(int idx, const char *ip, uint16_t port) {
    int fd = socket(2, 1, 0); /* AF_INET, SOCK_STREAM */
    if (fd < 0) {
        say("sock: socket failed\n");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = 2;
    addr.sin_port = htons(port);
    addr.sin_addr = inet_addr(ip);
    if (connect(fd, (void *)&addr, sizeof(addr)) < 0) {
        say("sock: connect failed\n");
        close(fd);
        return -1;
    }
    long n = send(fd, REQ, strlen(REQ), 0);
    if (n != (long)strlen(REQ)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int read_all(int idx, int fd) {
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
        say("sock: bad response\n");
        return -1;
    }
    say_num("sock: ok bytes ", (long)total);
    return 0;
}

int main(int argc, char **argv) {
    const char *ip = (argc > 1) ? argv[1] : "10.0.2.15";
    uint16_t p1 = (argc > 2) ? (uint16_t)parse_port(argv[2]) : 18080;
    uint16_t p2 = (argc > 3) ? (uint16_t)parse_port(argv[3]) : p1;
    int fd1 = open_and_send(1, ip, p1);
    int fd2 = open_and_send(2, ip, p2);
    if (fd1 < 0 || fd2 < 0) {
        if (fd1 >= 0) close(fd1);
        if (fd2 >= 0) close(fd2);
        say("LINUX_SOCKET2: FAIL (setup)\n");
        return 1;
    }
    /* 先读 fd2：fd1 的响应必须在轮询 fd2 期间由分发器缓冲进 fd1 槽 */
    int r2 = read_all(2, fd2);
    int r1 = read_all(1, fd1);
    if (r1 == 0 && r2 == 0) {
        say("LINUX_SOCKET2: PASS\n");
        return 0;
    }
    say_num("LINUX_SOCKET2: FAIL r1=", r1);
    say_num(" r2=", r2);
    return 1;
}
