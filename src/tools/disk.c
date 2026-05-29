#include "../block.h"
#include "../fs.h"
#include "../graphics/graphics.h"
#include "tool.h"

static void print_uint(uint32_t v) {
    char buf[16];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (n--) console_putchar(buf[n]);
}

static void print_mib(uint32_t bytes) {
    print_uint(bytes / (1024 * 1024));
    console_puts(" MiB");
}

static void usage_bar(uint32_t used, uint32_t cap) {
    const uint32_t width = 32;
    uint32_t filled = cap ? (used * width) / cap : 0;
    if (filled > width) filled = width;

    console_puts("\x1b[32m[");
    for (uint32_t i = 0; i < width; i++) {
        if (i < filled) console_putchar('#');
        else console_putchar('.');
    }
    console_puts("]\x1b[0m ");
    print_uint(cap ? (used * 100) / cap : 0);
    console_puts("%");
}

static void cmd_diskmgr(int argc, char **argv) {
    (void)argc;
    (void)argv;

    (void)block_init();
    console_puts("\n\x1b[33mHBOS Disk Manager\x1b[0m\n");
    console_puts("backend: \x1b[36m");
    console_puts(block_backend_name());
    console_puts("\x1b[0m\n");

    uint32_t sectors = block_sector_count();
    console_puts("disk:    ");
    if (sectors == 0) {
        console_puts("\x1b[31mnot found\x1b[0m\n");
    } else {
        print_uint(sectors);
        console_puts(" sectors, ");
        print_mib(sectors * BLOCK_SECTOR_SIZE);
        console_putchar('\n');
    }

    console_puts("fs:      \x1b[36m");
    console_puts(fs_backend_name());
    console_puts("\x1b[0m\n");
    console_puts("part:    start LBA ");
    print_uint(fs_disk_start_lba());
    console_puts(", ");
    print_uint(fs_disk_total_sectors());
    console_puts(" sectors\n");

    uint32_t used = fs_used_bytes();
    uint32_t cap = fs_capacity_bytes();
    console_puts("usage:   ");
    usage_bar(used, cap);
    console_puts("  ");
    print_mib(used);
    console_puts(" / ");
    print_mib(cap);
    console_putchar('\n');

    console_puts("files:   ");
    print_uint(fs_get_count());
    console_puts(" / ");
    print_uint(MAX_FILES);
    console_puts("\n\n");
}

static void cmd_install(int argc, char **argv) {
    (void)argc;
    (void)argv;
    console_puts("\x1b[33minstall:\x1b[0m writing MBR partition + formatting HBFS\n");
    if (fs_install_disk() < 0) {
        console_puts("\x1b[31minstall: failed, no writable disk\x1b[0m\n");
        return;
    }
    console_puts("\x1b[32minstall: HBFS partition ready\x1b[0m\n");
    console_puts("boot from ISO/UEFI media for now; files persist on disk.\n");
}

void tool_disk_init(void) {
    static const command_t cmds[] = {
        {"diskmgr", CMD_GROUP_FILE, "Show disk usage map", "diskmgr", cmd_diskmgr},
        {"install", CMD_GROUP_SYSTEM, "Partition disk and install HBFS", "install", cmd_install},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
