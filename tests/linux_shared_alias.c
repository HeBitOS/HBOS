#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int check_alias(int fd, const char *label) {
    if (ftruncate(fd, 8192) != 0) { perror("ftruncate"); return 1; }
    unsigned char *a = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    unsigned char *b = mmap(NULL, 8192, PROT_READ, MAP_SHARED, fd, 0);
    unsigned char *c = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
    if (a == MAP_FAILED || b == MAP_FAILED || c == MAP_FAILED) {
        perror("mmap"); return 2;
    }
    a[7] = 0x5a;
    a[4103] = 0xa5;
    __sync_synchronize();
    int failed = b[7] != 0x5a || b[4103] != 0xa5 || c[7] != 0xa5;
    c[23] = 0x37;
    __sync_synchronize();
    failed |= a[4119] != 0x37 || b[4119] != 0x37;
    printf("SHARED-ALIAS %s: %s read=%02x/%02x offset=%02x reverse=%02x\n",
           label, failed ? "FAIL" : "PASS", b[7], b[4103], c[7], a[4119]);
    munmap(c, 4096); munmap(b, 8192); munmap(a, 8192);
    return failed;
}
int main(void) {
    setbuf(stdout, NULL);
    int m = memfd_create("alias-regression", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (m < 0) { perror("memfd_create"); return 3; }
    int result = check_alias(m, "memfd"); close(m);
    int f = open("/tmp/shared-alias-regression", O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (f < 0) { perror("open"); return 4; }
    result |= check_alias(f, "regular-file"); close(f);
    return result;
}
