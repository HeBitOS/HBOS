#include "ahci.h"
#include "pci.h"
#include "string.h"
#include "core/vmm.h"

#define AHCI_CLASS_MASS_STORAGE 0x01
#define AHCI_SUBCLASS_SATA      0x06
#define AHCI_PROGIF_AHCI        0x01

#define HBA_GHC_AE   (1U << 31)
#define HBA_PxCMD_ST (1U << 0)
#define HBA_PxCMD_FRE (1U << 4)
#define HBA_PxCMD_FR (1U << 14)
#define HBA_PxCMD_CR (1U << 15)
#define HBA_PxIS_TFES (1U << 30)

#define SATA_SIG_ATA 0x00000101U

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

#define FIS_TYPE_REG_H2D 0x27

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t rsv[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    uint8_t cfl : 5;
    uint8_t a : 1;
    uint8_t w : 1;
    uint8_t p : 1;
    uint8_t r : 1;
    uint8_t b : 1;
    uint8_t c : 1;
    uint8_t rsv0 : 1;
    uint8_t pmp : 4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc : 22;
    uint32_t rsv1 : 9;
    uint32_t i : 1;
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[1];
} __attribute__((packed)) hba_cmd_table_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t rsv0 : 3;
    uint8_t c : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

static hba_mem_t *hba;
static hba_port_t *active_port;
static uint32_t active_port_index;
static uint32_t sector_count;
static char model[41];
static int initialized;

static uint8_t cmd_list[1024] __attribute__((aligned(1024)));
static uint8_t fis_area[256] __attribute__((aligned(256)));
static hba_cmd_table_t cmd_table __attribute__((aligned(128)));
static uint8_t identify_buf[512] __attribute__((aligned(2)));

static uint64_t ptr_phys(const void *p) {
    return (uint64_t)(uintptr_t)p;
}

static void map_abar(uint32_t abar) {
    uint64_t base = (uint64_t)(abar & ~0xFFFU);
    for (uint64_t off = 0; off < 0x2000; off += PAGE_SIZE)
        (void)vmm_map_page(base + off, base + off, VMM_W | VMM_CD);
}

static int port_present(hba_port_t *p) {
    uint32_t ssts = p->ssts;
    uint32_t det = ssts & 0x0F;
    uint32_t ipm = (ssts >> 8) & 0x0F;
    return det == 3 && ipm == 1 && p->sig == SATA_SIG_ATA;
}

static void stop_cmd(hba_port_t *p) {
    p->cmd &= ~HBA_PxCMD_ST;
    p->cmd &= ~HBA_PxCMD_FRE;
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(p->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR))) break;
    }
}

static void start_cmd(hba_port_t *p) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(p->cmd & HBA_PxCMD_CR)) break;
    }
    p->cmd |= HBA_PxCMD_FRE;
    p->cmd |= HBA_PxCMD_ST;
}

static void port_rebase(hba_port_t *p) {
    stop_cmd(p);
    memset(cmd_list, 0, sizeof(cmd_list));
    memset(fis_area, 0, sizeof(fis_area));
    memset(&cmd_table, 0, sizeof(cmd_table));

    uint64_t cl = ptr_phys(cmd_list);
    uint64_t fb = ptr_phys(fis_area);
    p->clb = (uint32_t)cl;
    p->clbu = (uint32_t)(cl >> 32);
    p->fb = (uint32_t)fb;
    p->fbu = (uint32_t)(fb >> 32);
    p->is = 0xFFFFFFFFU;
    p->serr = 0xFFFFFFFFU;
    start_cmd(p);
}

static int wait_not_busy(hba_port_t *p) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(p->tfd & 0x88)) return 0; // BSY | DRQ
    }
    return -1;
}

static int ahci_cmd(uint8_t command, uint32_t lba, uint8_t *buffer, int write) {
    if (!active_port || !buffer) return -1;
    hba_port_t *p = active_port;
    hba_cmd_header_t *hdr = (hba_cmd_header_t *)cmd_list;

    for (uint32_t i = 0; p->ci & 1; i++) {
        if (i > 5000000) return -1;
    }

    memset(cmd_list, 0, sizeof(cmd_list));
    memset(&cmd_table, 0, sizeof(cmd_table));

    hdr[0].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    hdr[0].w = write ? 1 : 0;
    hdr[0].prdtl = 1;
    uint64_t ct = ptr_phys(&cmd_table);
    hdr[0].ctba = (uint32_t)ct;
    hdr[0].ctbau = (uint32_t)(ct >> 32);

    uint64_t db = ptr_phys(buffer);
    cmd_table.prdt[0].dba = (uint32_t)db;
    cmd_table.prdt[0].dbau = (uint32_t)(db >> 32);
    cmd_table.prdt[0].dbc = 512 - 1;
    cmd_table.prdt[0].i = 1;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table.cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = command;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 1 << 6;
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = 0;
    fis->lba5 = 0;
    fis->countl = 1;
    fis->counth = 0;

    p->is = 0xFFFFFFFFU;
    if (wait_not_busy(p) < 0) return -1;
    p->ci = 1;

    for (uint32_t i = 0; i < 5000000; i++) {
        if (!(p->ci & 1)) {
            if (p->is & HBA_PxIS_TFES) return -1;
            return 0;
        }
    }
    return -1;
}

static int ahci_identify(void) {
    if (ahci_cmd(ATA_CMD_IDENTIFY, 0, identify_buf, 0) < 0) return -1;
    uint16_t *id = (uint16_t *)identify_buf;

    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        model[i * 2] = (char)(w >> 8);
        model[i * 2 + 1] = (char)(w & 0xFF);
    }
    model[40] = '\0';
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
    sector_count = ((uint32_t)id[61] << 16) | id[60];
    return sector_count ? 0 : -1;
}

int ahci_init(void) {
    if (initialized) return active_port ? 0 : -1;
    initialized = 1;

    pci_device_t dev;
    if (pci_find_class(AHCI_CLASS_MASS_STORAGE, AHCI_SUBCLASS_SATA, AHCI_PROGIF_AHCI, &dev) < 0)
        return -1;

    pci_enable_bus_master_mmio(&dev);
    uint32_t abar = pci_bar(dev.bus, dev.slot, dev.func, 5);
    if (!abar || (abar & 1)) return -1;
    abar &= ~0x0FU;
    map_abar(abar);

    hba = (hba_mem_t *)(uintptr_t)abar;
    hba->ghc |= HBA_GHC_AE;

    uint32_t pi = hba->pi;
    for (uint32_t i = 0; i < 32; i++) {
        if (!(pi & (1U << i))) continue;
        hba_port_t *p = (hba_port_t *)&hba->ports[i];
        if (!port_present(p)) continue;
        active_port = p;
        active_port_index = i;
        port_rebase(p);
        if (ahci_identify() == 0) return 0;
        active_port = 0;
    }
    (void)active_port_index;
    return -1;
}

int ahci_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!active_port || !buffer || lba >= sector_count) return -1;
    return ahci_cmd(ATA_CMD_READ_DMA_EXT, lba, buffer, 0);
}

int ahci_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!active_port || !buffer || lba >= sector_count) return -1;
    return ahci_cmd(ATA_CMD_WRITE_DMA_EXT, lba, (uint8_t *)buffer, 1);
}

uint32_t ahci_sector_count(void) {
    return sector_count;
}

const char *ahci_model(void) {
    return model;
}

int ahci_present(void) {
    return active_port != 0;
}
