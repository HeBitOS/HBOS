#ifndef HBOS_AHCI_H
#define HBOS_AHCI_H

#include <stdint.h>

int ahci_init(void);
int ahci_read_sector(uint32_t lba, uint8_t *buffer);
int ahci_write_sector(uint32_t lba, const uint8_t *buffer);
uint32_t ahci_sector_count(void);
const char *ahci_model(void);
int ahci_present(void);

#endif
