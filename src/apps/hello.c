#include "../string.h"
#include "../sys/types.h"
#include "../unistd.h"
#include "../user/app.h"
#include "../user/syscall.h"

static void put_uint(uint32_t v) {
    char buf[16];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n--) hbos_write(STDOUT_FILENO, &buf[n], 1);
}

static int app_hello_main(int argc, char **argv) {
    hbos_puts("hello from hbos app, pid=");
    put_uint((uint32_t)hbos_getpid());
    hbos_puts("\n");
    if (argc > 1) {
        hbos_puts("args:");
        for (int i = 1; i < argc; i++) {
            hbos_puts(" ");
            hbos_puts(argv[i]);
        }
        hbos_puts("\n");
    }
    return 0;
}

HBOS_APP("hello", "Minimal syscall demo app", app_hello_main);
