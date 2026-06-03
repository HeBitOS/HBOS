#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/syscall.h"

#define MAX_ENTRIES 256

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";

    long ret = __syscall1(HBOS_SYS_OPENDIR, (long)path);
    if (ret < 0) {
        printf("ls: %s: cannot open directory\n", path);
        return 1;
    }

    char name[256];
    uint32_t type;
    int count = 0;

    while (count < MAX_ENTRIES) {
        long r = __syscall3(HBOS_SYS_READDIR, (long)path,
                            (long)name, (long)&type);
        if (r < 0) break;
        printf("%s%s  ", name, (type == 1) ? "/" : "");
        count++;
    }

    if (count > 0) printf("\n");

    __syscall1(HBOS_SYS_CLOSEDIR, (long)path);
    return 0;
}