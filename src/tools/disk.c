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

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int parse_u32(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (!s || !*s || !out) return -1;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t n = v * 10 + (uint32_t)(*s - '0');
        if (n < v) return -1;
        v = n;
        s++;
    }
    *out = v;
    return 0;
}

static void print_hex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    console_puts("0x");
    console_putchar(hex[(v >> 4) & 0xF]);
    console_putchar(hex[v & 0xF]);
}

static void print_mib(uint32_t bytes) {
    print_uint(bytes / (1024 * 1024));
    console_puts(" MiB");
}

static void print_sectors_mib(uint32_t sectors) {
    print_uint(sectors);
    console_puts(" sectors, ");
    print_mib(sectors * BLOCK_SECTOR_SIZE);
}

static void usage_bar(uint32_t used, uint32_t cap, uint32_t width) {
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

static void print_partition_line(uint32_t idx, const fs_partition_info_t *p, uint32_t disk_sectors) {
    console_puts("  p");
    print_uint(idx + 1);
    console_puts(": ");
    if (!p->present) {
        console_puts("\x1b[90mempty\x1b[0m\n");
        return;
    }
    if (p->bootable) console_puts("\x1b[32m*\x1b[0m ");
    else console_puts("  ");
    print_hex8(p->type);
    if (p->type == 0x0C) console_puts(" FAT32 ");
    else if (p->type == 0xEF) console_puts(" ESP   ");
    else if (p->type == 0xEB) console_puts(" HBFS  ");
    else if (p->type == 0x83) console_puts(" EXT2  ");
    else console_puts("       ");
    console_puts(" start=");
    print_uint(p->start_lba);
    console_puts(" size=");
    print_sectors_mib(p->sectors);
    console_puts(" ");
    usage_bar(p->sectors, disk_sectors, 18);
    if ((p->type == 0xEB || p->type == 0x83 || p->type == 0x0B || p->type == 0x0C) && p->start_lba == fs_disk_start_lba()) console_puts(" \x1b[36mmounted\x1b[0m");
    console_putchar('\n');
}

static void print_partitions(uint32_t disk_sectors) {
    fs_partition_info_t parts[4];
    console_puts("partitions:\n");
    if (fs_read_partitions(parts) < 0) {
        console_puts("  \x1b[31munavailable\x1b[0m\n");
        return;
    }
    for (uint32_t i = 0; i < 4; i++) print_partition_line(i, &parts[i], disk_sectors);
}

static void print_file_usage(void) {
    console_puts("files:\n");
    if (fs_get_count() == 0) {
        console_puts("  \x1b[90mempty\x1b[0m\n");
        return;
    }
    for (uint32_t i = 0; i < fs_get_count(); i++) {
        file_t *f = fs_get_file(i);
        if (!f) continue;
        console_puts("  ");
        console_puts(f->name);
        console_puts(" ");
        usage_bar(f->size, RAMFS_MAX_FILE_SIZE, 18);
        console_puts(" ");
        print_uint(f->size);
        console_puts(" B\n");
    }
}

static void print_next_step(void) {
    console_puts("next:    ");
    if (block_sector_count() == 0) {
        console_puts("\x1b[33mattach a writable disk, then run diskmgr\x1b[0m\n");
    } else if (fs_is_disk()) {
        console_puts("\x1b[32mpersistent storage ready\x1b[0m; try writefile a hello\n");
    } else {
        console_puts("\x1b[36mrun install auto\x1b[0m to prepare persistent HBFS\n");
    }
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
        print_sectors_mib(sectors);
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
    usage_bar(used, cap, 32);
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

    if (sectors != 0) print_partitions(sectors);
    print_file_usage();
    print_next_step();
    console_putchar('\n');
}

static void cmd_install(int argc, char **argv) {
    if (argc == 1) {
        console_puts("\n\x1b[33mHBOS Installer\x1b[0m\n");
        console_puts("No disk changes were made.\n");
        console_puts("Recommended path:\n");
        console_puts("  1. diskmgr        inspect disk and partition status\n");
        console_puts("  2. install auto   prepare persistent HBFS storage\n");
        console_puts("  3. diskmgr        confirm storage is ready\n\n");
        console_puts("Advanced:\n");
        console_puts("  install format                 erase the current HBFS filesystem\n");
        console_puts("  install part <start> <sectors> create HBFS at an exact LBA range\n\n");
        cmd_diskmgr(0, 0);
        return;
    }

    if (argc > 1 && streq(argv[1], "status")) {
        cmd_diskmgr(0, 0);
        return;
    }

    if (argc > 1 && streq(argv[1], "format")) {
        console_puts("\x1b[33minstall:\x1b[0m formatting current HBFS partition\n");
        if (fs_format_disk() < 0) console_puts("\x1b[31minstall: format failed\x1b[0m\n");
        else console_puts("\x1b[32minstall: formatted\x1b[0m\n");
        return;
    }

    if (argc > 1 && streq(argv[1], "part")) {
        uint32_t start = 0, sectors = 0;
        if (argc < 4 || parse_u32(argv[2], &start) < 0 || parse_u32(argv[3], &sectors) < 0) {
            console_puts("usage: install part <start_lba> <sectors>\n");
            return;
        }
        console_puts("\x1b[33minstall:\x1b[0m creating HBFS partition at LBA ");
        print_uint(start);
        console_puts(" size ");
        print_uint(sectors);
        console_puts(" sectors\n");
        if (fs_install_disk_at(start, sectors) < 0) {
            console_puts("\x1b[31minstall: failed, range overlaps or disk is too small\x1b[0m\n");
            return;
        }
        console_puts("\x1b[32minstall: HBFS partition ready\x1b[0m\n");
        return;
    }

    if (!streq(argv[1], "auto")) {
        console_puts("usage: install [auto|status|format|part <start_lba> <sectors>]\n");
        return;
    }

    console_puts("\x1b[33minstall:\x1b[0m creating/formatting HBFS partition\n");
    if (fs_install_disk() < 0) {
        console_puts("\x1b[31minstall: failed, no writable space for HBFS\x1b[0m\n");
        return;
    }
    console_puts("\x1b[32minstall: HBFS partition ready\x1b[0m\n");
    cmd_diskmgr(0, 0);
}

void tool_disk_init(void) {
    static const command_t cmds[] = {
        {"diskmgr", CMD_GROUP_FILE, "Show disk usage map", "diskmgr", cmd_diskmgr},
        {"disk", CMD_GROUP_FILE, "Show disk usage map", "disk", cmd_diskmgr},
        {"install", CMD_GROUP_SYSTEM, "Show installer or prepare HBFS", "install [auto|status|format|part <start_lba> <sectors>]", cmd_install},
        {"setup", CMD_GROUP_SYSTEM, "Show installer or prepare HBFS", "setup [auto|status|format|part <start_lba> <sectors>]", cmd_install},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
