#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SZ (64 * 1024)
int main(void) {
    setbuf(stdout, NULL);
    int fd = memfd_create("coherence", MFD_CLOEXEC);
    if (fd < 0) { perror("memfd_create"); return 2; }
    if (ftruncate(fd, SZ) != 0) { perror("ftruncate"); return 2; }
    unsigned char *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }

    int fail = 0;
    memset(m, 0, SZ);

    /* 1) pwrite 写 -> mmap 读 */
    const char payload[] = "PWRITE-VISIBLE-VIA-MMAP";
    if (pwrite(fd, payload, sizeof(payload), 100) != (ssize_t)sizeof(payload)) {
        printf("MEMFD-COH: FAIL pwrite returned error\n"); return 3;
    }
    int ok1 = memcmp(m + 100, payload, sizeof(payload)) == 0;
    printf("MEMFD-COH pwrite->mmap: %s\n", ok1 ? "PASS" : "FAIL");
    fail |= !ok1;

    /* 2) mmap 写 -> pread 读 */
    const char payload2[] = "MMAP-VISIBLE-VIA-PREAD";
    memcpy(m + 4096, payload2, sizeof(payload2));
    __sync_synchronize();
    char back[64];
    memset(back, 0, sizeof(back));
    ssize_t got = pread(fd, back, sizeof(payload2), 4096);
    int ok2 = got == (ssize_t)sizeof(payload2) && memcmp(back, payload2, sizeof(payload2)) == 0;
    printf("MEMFD-COH mmap->pread: %s (got=%ld)\n", ok2 ? "PASS" : "FAIL", (long)got);
    fail |= !ok2;

    /* 3) 扩容后旧映射仍可见新写（截图位图常先 mmap 再 ftruncate 增长） */
    if (ftruncate(fd, SZ * 2) != 0) { printf("MEMFD-COH grow: FAIL ftruncate\n"); return 4; }
    const char payload3[] = "AFTER-GROW";
    if (pwrite(fd, payload3, sizeof(payload3), SZ - 32) != (ssize_t)sizeof(payload3)) {
        printf("MEMFD-COH grow-write: FAIL\n"); return 4;
    }
    int ok3 = memcmp(m + SZ - 32, payload3, sizeof(payload3)) == 0;
    printf("MEMFD-COH grow->mmap: %s\n", ok3 ? "PASS" : "FAIL");
    fail |= !ok3;

    munmap(m, SZ); close(fd);
    printf("MEMFD-COH: %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
