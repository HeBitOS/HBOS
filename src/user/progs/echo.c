#include "libc/stdio.h"
#include "libc/string.h"

int main(int argc, char **argv) {
    int newline = 1;
    int start   = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start   = 2;
    }

    for (int i = start; i < argc; i++) {
        if (i > start) printf(" ");
        printf("%s", argv[i]);
    }

    if (newline) printf("\n");
    return 0;
}