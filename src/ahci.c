/**
 * @file ahci.c
 * @brief AHCI (Advanced Host Controller Interface) SATA 驱动实现
 *
 * 通过 PCI 总线查找 AHCI 控制器，初始化端口，
 * 执行 ATA IDENTIFY / READ / WRITE 命令。
 */

#include "ahci.h"
#include "pci.h"
#include "string.h"
#include "core/vmm.h"

/** PCI 大容量存储设备类代码 */
#define AHCI_CLASS_MASS_STORAGE 0x01
/** PCI SATA 子类代码 */
#define AHCI_SUBCLASS_SATA      0x06
/** PCI AHCI 编程接口代码 */
#define AHCI_PROGIF_AHCI        0x01

/** 全局 HBA 控制器使能位 (GHC.AE) */
#define HBA_GHC_AE   (1U << 31)
/** 端口命令启动位 (PxCMD.ST) */
#define HBA_PxCMD_ST (1U << 0)
/** 端口 FIS 接收使能位 (PxCMD.FRE) */
#define HBA_PxCMD_FRE (1U << 4)
/** 端口 FIS 接收运行位 (PxCMD.FR) */
#define HBA_PxCMD_FR (1U << 14)
/** 端口命令引擎运行位 (PxCMD.CR) */
#define HBA_PxCMD_CR (1U << 15)
/** 端口任务文件错误状态位 (PxIS.TFES) */
#define HBA_PxIS_TFES (1U << 30)

/** SATA 设备签名：ATA 设备 */
#define SATA_SIG_ATA 0x00000101U

/** ATA IDENTIFY DEVICE 命令 */
#define ATA_CMD_IDENTIFY      0xEC
/** ATA READ DMA EXT 命令（LBA48 读取） */
#define ATA_CMD_READ_DMA_EXT  0x25
/** ATA WRITE DMA EXT 命令（LBA48 写入） */
#define ATA_CMD_WRITE_DMA_EXT 0x35

/** FIS 类型：寄存器 - 主机到设备 */
#define FIS_TYPE_REG_H2D 0x27

/**
 * @brief AHCI 端口寄存器结构
 */
typedef volatile struct {
    uint32_t clb;     /**< 命令列表基地址低 32 位 */
    uint32_t clbu;    /**< 命令列表基地址高 32 位 */
    uint32_t fb;      /**< FIS 基地址低 32 位 */
    uint32_t fbu;     /**< FIS 基地址高 32 位 */
    uint32_t is;      /**< 中断状态 */
    uint32_t ie;      /**< 中断使能 */
    uint32_t cmd;     /**< 命令与状态 */
    uint32_t rsv0;    /**< 保留 */
    uint32_t tfd;     /**< 任务文件数据 */
    uint32_t sig;     /**< 设备签名 */
    uint32_t ssts;    /**< SATA 状态 */
    uint32_t sctl;    /**< SATA 控制 */
    uint32_t serr;    /**< SATA 错误 */
    uint32_t sact;    /**< SATA 活动位 */
    uint32_t ci;      /**< 命令发布位 */
    uint32_t sntf;    /**< SNotification */
    uint32_t fbs;     /**< 基于帧的发送控制 */
    uint32_t rsv1[11]; /**< 保留 */
    uint32_t vendor[4]; /**< 厂商专用 */
} hba_port_t;

/**
 * @brief AHCI HBA 内存映射寄存器结构
 */
typedef volatile struct {
    uint32_t cap;     /**< HBA 能力 */
    uint32_t ghc;     /**< 全局 HBA 控制 */
    uint32_t is;      /**< 中断状态 */
    uint32_t pi;      /**< 端口实现位图 */
    uint32_t vs;      /**< AHCI 版本 */
    uint32_t ccc_ctl; /**< 命令完成协商标识 */
    uint32_t ccc_pts; /**< 命令完成协商端口 */
    uint32_t em_loc;  /**< 封装管理位置 */
    uint32_t em_ctl;  /**< 封装管理控制 */
    uint32_t cap2;    /**< HBA 能力扩展 */
    uint32_t bohc;    /**< BIOS/OS 握手控制 */
    uint8_t rsv[0xA0 - 0x2C]; /**< 保留 */
    uint8_t vendor[0x100 - 0xA0]; /**< 厂商专用 */
    hba_port_t ports[32]; /**< 端口寄存器数组 */
} hba_mem_t;

/**
 * @brief AHCI 命令表头
 */
typedef struct {
    uint8_t cfl : 5;    /**< FIS 长度（以 DWORD 为单位） */
    uint8_t a : 1;      /**< ATAPI 命令标志 */
    uint8_t w : 1;      /**< 写操作标志 */
    uint8_t p : 1;      /**< 优先级标志 */
    uint8_t r : 1;      /**< 复位标志 */
    uint8_t b : 1;      /**< BIST 标志 */
    uint8_t c : 1;      /**< 清除 Busy 标志 */
    uint8_t rsv0 : 1;   /**< 保留 */
    uint8_t pmp : 4;    /**< 端口复用器端口号 */
    uint16_t prdtl;     /**< PRDT 条目数量 */
    volatile uint32_t prdbc; /**< 已传输的 PRD 字节计数 */
    uint32_t ctba;      /**< 命令表基地址低 32 位 */
    uint32_t ctbau;     /**< 命令表基地址高 32 位 */
    uint32_t rsv1[4];   /**< 保留 */
} __attribute__((packed)) hba_cmd_header_t;

/**
 * @brief 物理区域描述表 (PRDT) 条目
 */
typedef struct {
    uint32_t dba;      /**< 数据基地址低 32 位 */
    uint32_t dbau;     /**< 数据基地址高 32 位 */
    uint32_t rsv0;     /**< 保留 */
    uint32_t dbc : 22; /**< 数据字节计数（0 表示 4MB，实际为 dbc+1） */
    uint32_t rsv1 : 9; /**< 保留 */
    uint32_t i : 1;    /**< 完成时中断标志 */
} __attribute__((packed)) hba_prdt_entry_t;

/**
 * @brief AHCI 命令表（含 FIS、ACMD 和 PRDT）
 */
typedef struct {
    uint8_t cfis[64];          /**< 命令 FIS */
    uint8_t acmd[16];          /**< ATAPI 命令字节 */
    uint8_t rsv[48];           /**< 保留 */
    hba_prdt_entry_t prdt[1];  /**< PRDT 条目（可扩展） */
} __attribute__((packed)) hba_cmd_table_t;

/**
 * @brief 寄存器 FIS - 主机到设备 (H2D)
 */
typedef struct {
    uint8_t fis_type;   /**< FIS 类型 */
    uint8_t pmport : 4; /**< 端口复用器端口号 */
    uint8_t rsv0 : 3;   /**< 保留 */
    uint8_t c : 1;      /**< 命令标志（1 表示命令，0 表示控制） */
    uint8_t command;    /**< ATA 命令码 */
    uint8_t featurel;   /**< 特征字段低 8 位 */
    uint8_t lba0;       /**< LBA 低 8 位 [7:0] */
    uint8_t lba1;       /**< LBA [15:8] */
    uint8_t lba2;       /**< LBA [23:16] */
    uint8_t device;     /**< 设备/磁头寄存器 */
    uint8_t lba3;       /**< LBA [31:24] */
    uint8_t lba4;       /**< LBA [39:32] */
    uint8_t lba5;       /**< LBA [47:40] */
    uint8_t featureh;   /**< 特征字段高 8 位 */
    uint8_t countl;     /**< 扇区计数低 8 位 */
    uint8_t counth;     /**< 扇区计数高 8 位 */
    uint8_t icc;        /**< 接口电源状态 */
    uint8_t control;    /**< 设备控制 */
    uint8_t rsv1[4];    /**< 保留 */
} __attribute__((packed)) fis_reg_h2d_t;

/** HBA 内存映射寄存器基地址 */
static hba_mem_t *hba;
/** 当前活动的端口指针 */
static hba_port_t *active_port;
/** 当前活动端口的索引号 */
static uint32_t active_port_index;
/** 磁盘总扇区数 */
static uint32_t sector_count;
/** 磁盘型号字符串 */
static char model[41];
/** 初始化标志，防止重复初始化 */
static int initialized;

/** 命令列表缓冲区，1KB 对齐 */
static uint8_t cmd_list[1024] __attribute__((aligned(1024)));
/** FIS 接收区域，256 字节对齐 */
static uint8_t fis_area[256] __attribute__((aligned(256)));
/** 命令表，128 字节对齐 */
static hba_cmd_table_t cmd_table __attribute__((aligned(128)));
/** IDENTIFY 命令数据缓冲区 */
static uint8_t identify_buf[512] __attribute__((aligned(2)));

/**
 * @brief 将虚拟地址转换为物理地址（恒等映射场景下直接转换）
 * @param p 虚拟地址指针
 * @return 对应的物理地址
 */
static uint64_t ptr_phys(const void *p) {
    return (uint64_t)(uintptr_t)p;
}

/**
 * @brief 将 ABAR（AHCI 基地址寄存器）映射到虚拟地址空间
 * @param abar ABAR 的物理地址值
 */
static void map_abar(uint32_t abar) {
    uint64_t base = (uint64_t)(abar & ~0xFFFU);
    for (uint64_t off = 0; off < 0x2000; off += PAGE_SIZE)
        (void)vmm_map_page(base + off, base + off, VMM_W | VMM_CD);
}

/**
 * @brief 检查端口是否连接了 SATA 设备
 * @param p 端口寄存器指针
 * @return 设备存在返回非 0，否则返回 0
 */
static int port_present(hba_port_t *p) {
    uint32_t ssts = p->ssts;
    uint32_t det = ssts & 0x0F;
    uint32_t ipm = (ssts >> 8) & 0x0F;
    return det == 3 && ipm == 1 && p->sig == SATA_SIG_ATA;
}

/**
 * @brief 停止端口的命令引擎和 FIS 接收引擎
 * @param p 端口寄存器指针
 */
static void stop_cmd(hba_port_t *p) {
    p->cmd &= ~HBA_PxCMD_ST;
    p->cmd &= ~HBA_PxCMD_FRE;
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(p->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR))) break;
    }
}

/**
 * @brief 启动端口的 FIS 接收引擎和命令引擎
 * @param p 端口寄存器指针
 */
static void start_cmd(hba_port_t *p) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(p->cmd & HBA_PxCMD_CR)) break;
    }
    p->cmd |= HBA_PxCMD_FRE;
    p->cmd |= HBA_PxCMD_ST;
}

/**
 * @brief 重新初始化端口：停止引擎、清零缓冲区、设置基地址、重启引擎
 * @param p 端口寄存器指针
 */
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

/**
 * @brief 等待端口不再忙碌（BSY 和 DRQ 清零）
 * @param p 端口寄存器指针
 * @return 成功返回 0，超时返回 -1
 */
static int wait_not_busy(hba_port_t *p) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(p->tfd & 0x88)) return 0; // BSY | DRQ
    }
    return -1;
}

/**
 * @brief 向活动端口发送 ATA 命令
 * @param command ATA 命令码
 * @param lba     逻辑块地址
 * @param buffer  数据缓冲区
 * @param write   0 表示读，1 表示写
 * @return 成功返回 0，失败返回 -1
 */
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

/**
 * @brief 执行 ATA IDENTIFY DEVICE 命令，获取磁盘信息
 * @return 成功返回 0，失败返回 -1
 */
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

/**
 * @brief 初始化 AHCI 控制器，查找并配置可用端口
 * @return 成功返回 0，失败返回 -1
 */
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

/**
 * @brief 从指定 LBA 读取一个扇区（512 字节）
 * @param lba    逻辑块地址
 * @param buffer 输出缓冲区，至少 512 字节
 * @return 成功返回 0，失败返回 -1
 */
int ahci_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!active_port || !buffer || lba >= sector_count) return -1;
    return ahci_cmd(ATA_CMD_READ_DMA_EXT, lba, buffer, 0);
}

/**
 * @brief 向指定 LBA 写入一个扇区（512 字节）
 * @param lba    逻辑块地址
 * @param buffer 输入缓冲区，至少 512 字节
 * @return 成功返回 0，失败返回 -1
 */
int ahci_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!active_port || !buffer || lba >= sector_count) return -1;
    return ahci_cmd(ATA_CMD_WRITE_DMA_EXT, lba, (uint8_t *)buffer, 1);
}

/**
 * @brief 获取磁盘的总扇区数
 * @return 扇区数量
 */
uint32_t ahci_sector_count(void) {
    return sector_count;
}

/**
 * @brief 获取磁盘型号字符串
 * @return 型号名称（以 '\0' 结尾）
 */
const char *ahci_model(void) {
    return model;
}

/**
 * @brief 检查 AHCI 设备是否已初始化并可用
 * @return 可用返回非 0，不可用返回 0
 */
int ahci_present(void) {
    return active_port != 0;
}
