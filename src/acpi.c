#include "acpi.h"

#include <stddef.h>
#include <stdint.h>

#include "string.h"

#define MB2_TAG_ACPI_OLD 14
#define MB2_TAG_ACPI_NEW 15

#define ACPI_SLP_EN (1U << 13)

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
} __attribute__((packed)) rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;

typedef struct {
    sdt_header_t h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
} __attribute__((packed)) fadt_t;

static uint16_t g_pm1a_cnt;
static uint16_t g_pm1b_cnt;
static uint8_t g_slp_typa;
static uint8_t g_slp_typb;
static int g_ready;

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static void *mb2_find_tag(void *mbi, uint32_t type) {
    if (!mbi) return NULL;
    uint32_t total = *(uint32_t *)mbi;
    uintptr_t addr = (uintptr_t)mbi + 8;
    while (addr < (uintptr_t)mbi + total) {
        uint32_t tag_type = *(uint32_t *)addr;
        uint32_t tag_size = *(uint32_t *)(addr + 4);
        if (tag_type == 0) break;
        if (tag_type == type) return (void *)addr;
        addr += tag_size;
        if (addr & 7) addr = (addr + 7) & ~7;
    }
    return NULL;
}

static int sig_eq(const char *a, const char *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static sdt_header_t *find_table_rsdt(sdt_header_t *rsdt, const char *sig) {
    if (!rsdt || !sig_eq(rsdt->signature, "RSDT") || rsdt->length < sizeof(*rsdt)) return NULL;
    uint32_t count = (rsdt->length - sizeof(*rsdt)) / 4;
    uint32_t *entry = (uint32_t *)((uint8_t *)rsdt + sizeof(*rsdt));
    for (uint32_t i = 0; i < count; i++) {
        sdt_header_t *h = (sdt_header_t *)(uintptr_t)entry[i];
        if (h && sig_eq(h->signature, sig)) return h;
    }
    return NULL;
}

static sdt_header_t *find_table_xsdt(sdt_header_t *xsdt, const char *sig) {
    if (!xsdt || !sig_eq(xsdt->signature, "XSDT") || xsdt->length < sizeof(*xsdt)) return NULL;
    uint32_t count = (xsdt->length - sizeof(*xsdt)) / 8;
    uint64_t *entry = (uint64_t *)((uint8_t *)xsdt + sizeof(*xsdt));
    for (uint32_t i = 0; i < count; i++) {
        if (entry[i] > 0xffffffffULL) continue;
        sdt_header_t *h = (sdt_header_t *)(uintptr_t)entry[i];
        if (h && sig_eq(h->signature, sig)) return h;
    }
    return NULL;
}

static int aml_int(const uint8_t **p, const uint8_t *end, uint8_t *out) {
    const uint8_t *s = *p;
    if (s >= end) return -1;
    if (*s == 0x00) { *out = 0; *p = s + 1; return 0; }
    if (*s == 0x01) { *out = 1; *p = s + 1; return 0; }
    if (*s == 0x0a && s + 1 < end) { *out = s[1]; *p = s + 2; return 0; }
    if (*s == 0x0b && s + 2 < end) { *out = s[1]; *p = s + 3; return 0; }
    if (*s == 0x0c && s + 4 < end) { *out = s[1]; *p = s + 5; return 0; }
    *out = *s;
    *p = s + 1;
    return 0;
}

static int parse_s5(sdt_header_t *dsdt, uint8_t *typa, uint8_t *typb) {
    if (!dsdt || dsdt->length <= sizeof(*dsdt)) return -1;
    uint8_t *base = (uint8_t *)dsdt;
    uint8_t *end = base + dsdt->length;

    for (uint8_t *s = base + sizeof(*dsdt); s + 4 < end; s++) {
        if (s[0] != '_' || s[1] != 'S' || s[2] != '5' || s[3] != '_') continue;

        for (uint8_t *pkg = s + 4; pkg < s + 36 && pkg < end; pkg++) {
            if (*pkg != 0x12) continue;
            if (pkg + 2 >= end) return -1;
            uint8_t len_bytes = (uint8_t)((pkg[1] >> 6) + 1);
            const uint8_t *p = pkg + 1 + len_bytes;
            if (p >= end) return -1;
            p++; /* element count */

            uint8_t a = 0, b = 0;
            if (aml_int(&p, end, &a) < 0) return -1;
            if (aml_int(&p, end, &b) < 0) return -1;
            *typa = a;
            *typb = b;
            return 0;
        }
    }
    return -1;
}

void acpi_init(void *mbi) {
    g_ready = 0;
    g_pm1a_cnt = 0;
    g_pm1b_cnt = 0;
    g_slp_typa = 0;
    g_slp_typb = 0;

    uint8_t *tag = (uint8_t *)mb2_find_tag(mbi, MB2_TAG_ACPI_NEW);
    if (!tag) tag = (uint8_t *)mb2_find_tag(mbi, MB2_TAG_ACPI_OLD);
    if (!tag) return;

    rsdp_t *rsdp = (rsdp_t *)(tag + 8);
    sdt_header_t *fadt = NULL;
    if (rsdp->revision >= 2 && rsdp->xsdt_address && rsdp->xsdt_address <= 0xffffffffULL)
        fadt = find_table_xsdt((sdt_header_t *)(uintptr_t)rsdp->xsdt_address, "FACP");
    if (!fadt && rsdp->rsdt_address)
        fadt = find_table_rsdt((sdt_header_t *)(uintptr_t)rsdp->rsdt_address, "FACP");
    if (!fadt || fadt->length < 72) return;

    fadt_t *facp = (fadt_t *)fadt;
    sdt_header_t *dsdt = (sdt_header_t *)(uintptr_t)facp->dsdt;
    if (!dsdt || !sig_eq(dsdt->signature, "DSDT")) return;
    if (parse_s5(dsdt, &g_slp_typa, &g_slp_typb) < 0) return;

    g_pm1a_cnt = (uint16_t)facp->pm1a_cnt_blk;
    g_pm1b_cnt = (uint16_t)facp->pm1b_cnt_blk;
    if (!g_pm1a_cnt) return;
    g_ready = 1;
}

int acpi_poweroff(void) {
    if (!g_ready || !g_pm1a_cnt) return -1;
    outw(g_pm1a_cnt, (uint16_t)((g_slp_typa << 10) | ACPI_SLP_EN));
    if (g_pm1b_cnt)
        outw(g_pm1b_cnt, (uint16_t)((g_slp_typb << 10) | ACPI_SLP_EN));
    return 0;
}
