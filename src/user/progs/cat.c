#include "libc/stdio.h"
#include "libc/string.h"
#include "libc/stdlib.h"
#include "libc/syscall.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cat <file>\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        FILE *fp = fopen(argv[i], "r");
        if (!fp) {
            printf("cat: %s: cannot open\n", argv[i]);
            continue;
        }

        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf) - 1, fp)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
        }
        printf("\n");
        fclose(fp);
    }

    return 0;
}