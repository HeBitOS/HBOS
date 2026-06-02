#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/syscall.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("Hello from HBOS ring3 user-space!\n");
    printf("pid=%d\n", (int)__syscall1(HBOS_SYS_GETPID, 0));

    if (argc > 1) {
        printf("args:");
        for (int i = 1; i < argc; i++) {
            printf(" %s", argv[i]);
        }
        printf("\n");
    }

    printf("malloc test: ");
    char *p = (char *)malloc(128);
    if (p) {
        memcpy(p, "Hello, heap!", 12);
        printf("%s\n", p);
        free(p);
    } else {
        printf("FAIL\n");
    }

    return 0;
}