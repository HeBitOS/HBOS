#ifndef HBOS_PCI_H
#define HBOS_PCI_H

#include <stdint.h>

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
} pci_device_t;

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out);
uint32_t pci_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index);
void pci_enable_bus_master_mmio(const pci_device_t *dev);

#endif
