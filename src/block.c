#include "ahci.h"
#include "ata.h"
#include "block.h"

static block_backend_t backend = BLOCK_BACKEND_NONE;
static uint32_t sectors = 0;

int block_init(void) {
    if (ahci_init() == 0 && ahci_sector_count() > 0) {
        backend = BLOCK_BACKEND_AHCI;
        sectors = ahci_sector_count();
        return 0;
    }

    if (ata_init() == 0) {
        const ata_device_t *dev = ata_primary();
        if (dev->present && dev->lba28 && dev->sectors > 0) {
            backend = BLOCK_BACKEND_ATA;
            sectors = dev->sectors;
            return 0;
        }
    }

    backend = BLOCK_BACKEND_NONE;
    sectors = 0;
    return -1;
}

int block_read_sector(uint32_t lba, uint8_t *buffer) {
    if (backend == BLOCK_BACKEND_AHCI) return ahci_read_sector(lba, buffer);
    if (backend == BLOCK_BACKEND_ATA) return ata_read_sector(lba, buffer);
    return -1;
}

int block_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (backend == BLOCK_BACKEND_AHCI) return ahci_write_sector(lba, buffer);
    if (backend == BLOCK_BACKEND_ATA) return ata_write_sector(lba, buffer);
    return -1;
}

uint32_t block_sector_count(void) {
    return sectors;
}

block_backend_t block_backend(void) {
    return backend;
}

const char *block_backend_name(void) {
    if (backend == BLOCK_BACKEND_AHCI) return "ahci";
    if (backend == BLOCK_BACKEND_ATA) return "ata";
    return "none";
}
