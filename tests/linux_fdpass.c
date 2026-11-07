#define _GNU_SOURCE
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
    setbuf(stdout, NULL);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 2; }

    int fd = memfd_create("fdpass", MFD_CLOEXEC);
    if (fd < 0) { perror("memfd_create"); return 2; }
    if (ftruncate(fd, 4096) != 0) { perror("ftruncate"); return 2; }
    const char magic[] = "FD-PASS-MAGIC-42";
    if (pwrite(fd, magic, sizeof(magic), 64) != (ssize_t)sizeof(magic)) {
        printf("FDPASS: FAIL pwrite\n"); return 3;
    }

    char byte = 'X';
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    char ctrl[CMSG_SPACE(sizeof(int))];
    memset(ctrl, 0, sizeof(ctrl));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = ctrl; msg.msg_controllen = sizeof(ctrl);
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    if (sendmsg(sv[0], &msg, 0) != 1) { printf("FDPASS: FAIL sendmsg\n"); return 3; }

    char got_byte = 0;
    struct iovec riov = { .iov_base = &got_byte, .iov_len = 1 };
    char rctrl[CMSG_SPACE(sizeof(int) * 4)];
    memset(rctrl, 0, sizeof(rctrl));
    struct msghdr rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.msg_iov = &riov; rmsg.msg_iovlen = 1;
    rmsg.msg_control = rctrl; rmsg.msg_controllen = sizeof(rctrl);
    ssize_t n = recvmsg(sv[1], &rmsg, 0);
    if (n != 1) { printf("FDPASS: FAIL recvmsg n=%ld\n", (long)n); return 3; }

    int received = -1;
    for (struct cmsghdr *rc = CMSG_FIRSTHDR(&rmsg); rc; rc = CMSG_NXTHDR(&rmsg, rc)) {
        if (rc->cmsg_level == SOL_SOCKET && rc->cmsg_type == SCM_RIGHTS)
            memcpy(&received, CMSG_DATA(rc), sizeof(int));
    }
    if (received < 0) { printf("FDPASS: FAIL no fd in cmsg\n"); return 4; }

    char back[32];
    memset(back, 0, sizeof(back));
    ssize_t r = pread(received, back, sizeof(magic), 64);
    int ok = r == (ssize_t)sizeof(magic) && memcmp(back, magic, sizeof(magic)) == 0;
    printf("FDPASS fd=%d received=%d pread=%ld content=%s : %s\n",
           fd, received, (long)r, ok ? "OK" : "MISMATCH", ok ? "PASS" : "FAIL");
    return ok ? 0 : 5;
}
