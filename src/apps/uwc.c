#include "../errno.h"
#include "../fcntl.h"
#include "../string.h"
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

static int app_uwc_main(int argc, char **argv) {
    const char *name = "-";
    int fd = STDIN_FILENO;
    if (argc >= 2) {
        name = argv[1];
        fd = hbos_open(argv[1], O_RDONLY, 0);
        if (fd < 0) {
            hbos_puts("uwc: open failed\n");
            return errno;
        }
    }

    char buf[128];
    uint32_t bytes = 0;
    uint32_t lines = 0;
    ssize_t n;
    while ((n = hbos_read(fd, buf, sizeof(buf))) > 0) {
        bytes += (uint32_t)n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
        }
    }
    if (fd != STDIN_FILENO) hbos_close(fd);

    put_uint(lines);
    hbos_puts(" lines ");
    put_uint(bytes);
    hbos_puts(" bytes ");
    hbos_puts(name);
    hbos_puts("\n");
    return 0;
}

HBOS_APP("uwc", "Count bytes and lines through syscall fd API", app_uwc_main);
