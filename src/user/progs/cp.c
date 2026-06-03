#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/syscall.h"

#define BUFSIZE 4096

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: cp <src> <dst>\n");
        return 1;
    }

    long sfd = __syscall3(HBOS_SYS_OPEN, (long)argv[1], O_RDONLY, 0);
    if (sfd < 0) {
        printf("cp: %s: cannot open\n", argv[1]);
        return 1;
    }

    long dfd = __syscall3(HBOS_SYS_OPEN, (long)argv[2],
                          O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (dfd < 0) {
        printf("cp: %s: cannot create\n", argv[2]);
        __syscall1(HBOS_SYS_CLOSE, sfd);
        return 1;
    }

    char *buf = (char *)malloc(BUFSIZE);
    if (!buf) {
        printf("cp: out of memory\n");
        __syscall1(HBOS_SYS_CLOSE, sfd);
        __syscall1(HBOS_SYS_CLOSE, dfd);
        return 1;
    }

    long n;
    while ((n = __syscall3(HBOS_SYS_READ, sfd, (long)buf, BUFSIZE)) > 0) {
        long written = __syscall3(HBOS_SYS_WRITE, dfd, (long)buf, n);
        if (written < 0) {
            printf("cp: write error\n");
            break;
        }
    }

    free(buf);
    __syscall1(HBOS_SYS_CLOSE, sfd);
    __syscall1(HBOS_SYS_CLOSE, dfd);
    return 0;
}