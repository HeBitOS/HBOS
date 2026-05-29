#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return 0x80000000U |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           (offset & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, slot, func, offset);
    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t v = pci_read32(bus, slot, func, offset);
    return (uint8_t)((v >> ((offset & 3) * 8)) & 0xFF);
}

static int pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t *out) {
    uint16_t vendor = pci_read16(bus, slot, func, 0x00);
    if (vendor == 0xFFFF) return -1;
    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor_id = vendor;
    out->device_id = pci_read16(bus, slot, func, 0x02);
    out->prog_if = pci_read8(bus, slot, func, 0x09);
    out->subclass = pci_read8(bus, slot, func, 0x0A);
    out->class_code = pci_read8(bus, slot, func, 0x0B);
    return 0;
}

int pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out) {
    pci_device_t dev;
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint8_t funcs = 1;
            if (pci_read16((uint8_t)bus, slot, 0, 0x00) == 0xFFFF) continue;
            if (pci_read8((uint8_t)bus, slot, 0, 0x0E) & 0x80) funcs = 8;
            for (uint8_t func = 0; func < funcs; func++) {
                if (pci_read_device((uint8_t)bus, slot, func, &dev) < 0) continue;
                if (dev.class_code == class_code &&
                    dev.subclass == subclass &&
                    (prog_if == 0xFF || dev.prog_if == prog_if)) {
                    if (out) *out = dev;
                    return 0;
                }
            }
        }
    }
    return -1;
}

uint32_t pci_bar(uint8_t bus, uint8_t slot, uint8_t func, uint8_t bar_index) {
    if (bar_index >= 6) return 0;
    return pci_read32(bus, slot, func, (uint8_t)(0x10 + bar_index * 4));
}

void pci_enable_bus_master_mmio(const pci_device_t *dev) {
    if (!dev) return;
    uint32_t cmd = pci_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x00000006U; // memory space + bus master
    pci_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
