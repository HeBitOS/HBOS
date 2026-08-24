#include <stdint.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>

/* G1 Linux 兼容层 DNS：getaddrinfo/gethostbyname → 内核 net_dns_resolve。
 * 先 dhcp（QEMU slirp DNS 10.0.2.3），再解析真实域名。 */

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

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : "example.com";
    (void)host;

    /* 1. gethostbyname */
    struct hostent *he = gethostbyname(host);
    if (!he || he->h_addrtype != 2 || !he->h_addr_list[0]) {
        say("LINUX_DNS: FAIL (gethostbyname)\n");
        return 1;
    }
    const unsigned char *ip = (const unsigned char *)he->h_addr_list[0];
    say_num("dns: ", (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

    /* 2. getaddrinfo with numeric port */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = 2;
    hints.ai_socktype = 1;
    struct addrinfo *res = 0;
    int rc = getaddrinfo(host, "8080", &hints, &res);
    say_num("ga rc ", rc);
    if (rc != 0 || !res) {
        say("LINUX_DNS: FAIL (getaddrinfo numeric)\n");
        return 1;
    }
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    say_num("port ", sa->sin_port);
    say_num("want ", htons(8080));
    if (sa->sin_family != 2 || sa->sin_port != htons(8080)) {
        say("LINUX_DNS: FAIL (getaddrinfo port)\n");
        freeaddrinfo(res);
        return 1;
    }
    freeaddrinfo(res);

    /* 3. getaddrinfo with service name "http" */
    res = 0;
    rc = getaddrinfo(host, "http", 0, &res);
    if (rc != 0 || !res) {
        say("LINUX_DNS: FAIL (getaddrinfo http)\n");
        return 1;
    }
    sa = (struct sockaddr_in *)res->ai_addr;
    if (sa->sin_port != htons(80)) {
        say("LINUX_DNS: FAIL (getaddrinfo port 80)\n");
        freeaddrinfo(res);
        return 1;
    }
    freeaddrinfo(res);

    /* 4. 未知域名应报 EAI_NONAME */
    rc = getaddrinfo("no-such-host.invalid.", "80", 0, &res);
    if (rc == 0) {
        say("LINUX_DNS: FAIL (unknown host resolved)\n");
        freeaddrinfo(res);
        return 1;
    }

    say("LINUX_DNS: PASS\n");
    return 0;
}
