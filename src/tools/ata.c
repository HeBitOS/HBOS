#include "../ata.h"
#include "../graphics/graphics.h"
#include "../stdlib.h"
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

static void print_hex8(uint8_t v) {
    const char *hex = "0123456789ABCDEF";
    console_putchar(hex[v >> 4]);
    console_putchar(hex[v & 0x0F]);
}

static void dump_sector(uint8_t *buf) {
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            print_hex8(buf[row * 16 + col]);
            console_putchar(' ');
        }
        console_putchar('\n');
    }
}

static void cmd_ata(int argc, char **argv) {
    if (argc < 2 || argv[1][0] == 'i') {
        const ata_device_t *dev = ata_primary();
        if (!dev->present) ata_init();
        dev = ata_primary();
        if (!dev->present) {
            console_puts("ATA primary master: not found\n");
            return;
        }
        console_puts("ATA primary master: ");
        console_puts(dev->model[0] ? dev->model : "unknown");
        console_puts("\nsectors: ");
        print_uint(dev->sectors);
        console_puts("\nlba28: ");
        console_puts(dev->lba28 ? "yes\n" : "no\n");
        return;
    }

    if (argc >= 3 && argv[1][0] == 'r') {
        uint32_t lba = (uint32_t)strtol(argv[2], 0, 0);
        static uint8_t sector[ATA_SECTOR_SIZE] __attribute__((aligned(2)));
        if (ata_read_sector(lba, sector) < 0) {
            console_puts("ata: read failed\n");
            return;
        }
        dump_sector(sector);
        return;
    }

    console_puts("Usage: ata info | ata read <lba>\n");
}

void tool_ata_init(void) {
    static const command_t cmds[] = {
        {"ata", CMD_GROUP_DEBUG, "ATA PIO diagnostics", "ata info | ata read <lba>", cmd_ata},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
