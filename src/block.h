#ifndef HBOS_BLOCK_H
#define HBOS_BLOCK_H

#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512

typedef enum {
    BLOCK_BACKEND_NONE = 0,
    BLOCK_BACKEND_AHCI,
    BLOCK_BACKEND_ATA,
} block_backend_t;

int block_init(void);
int block_read_sector(uint32_t lba, uint8_t *buffer);
int block_write_sector(uint32_t lba, const uint8_t *buffer);
uint32_t block_sector_count(void);
block_backend_t block_backend(void);
const char *block_backend_name(void);

#endif
