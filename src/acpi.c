/**
 * @file    acpi.c
 * @brief   ACPI 高级配置与电源接口 — 支持系统关机 (S5 睡眠状态)
 *
 * 通过 Multiboot2 信息结构定位 RSDP (Root System Description Pointer)，
 * 遍历 RSDT/XSDT 查找 FACP (FADT)，再从 DSDT 中解析 _S5 包获取
 * 睡眠类型值，最终通过写入 PM1a/PM1b 控制寄存器实现关机。
 *
 * 关机流程:
 *   1. acpi_init(): 解析 ACPI 表，提取 SLP_TYPa/b 和 PM1 控制寄存器地址
 *   2. acpi_poweroff(): 写入 SLP_EN | (SLP_TYP << 10) 到 PM1 控制寄存器
 */

#include "acpi.h"

#include <stddef.h>
#include <stdint.h>

#include "string.h"

/** Multiboot2 ACPI 旧版标签类型 (RSDP v1) */
#define MB2_TAG_ACPI_OLD 14
/** Multiboot2 ACPI 新版标签类型 (RSDP v2+) */
#define MB2_TAG_ACPI_NEW 15

/** SLP_EN 位: 写入 PM1 控制寄存器时置此位以触发睡眠 */
#define ACPI_SLP_EN (1U << 13)

/**
 * RSDP (Root System Description Pointer) 结构
 * 定位 RSDT/XSDT 的入口，由 BIOS 在低内存中放置
 */
typedef struct {
    char signature[8];       /**< 签名: "RSD PTR " */
    uint8_t checksum;        /**< 校验和（v1 仅覆盖前 20 字节） */
    char oemid[6];           /**< OEM 标识 */
    uint8_t revision;        /**< 修订版本 (0=v1, 2=v2+) */
    uint32_t rsdt_address;   /**< RSDT 物理地址（32 位） */
    uint32_t length;         /**< RSDP v2+ 整体长度 */
    uint64_t xsdt_address;   /**< XSDT 物理地址（64 位，v2+） */
} __attribute__((packed)) rsdp_t;

/**
 * SDT 通用头部 — RSDT, XSDT, FADT, DSDT 等共享此头部
 */
typedef struct {
    char signature[4];       /**< 表签名 (如 "FACP", "DSDT") */
    uint32_t length;         /**< 整表长度（字节） */
    uint8_t revision;        /**< 修订版本 */
    uint8_t checksum;        /**< 校验和 */
    char oemid[6];           /**< OEM 标识 */
    char oem_table_id[8];    /**< OEM 表标识 */
    uint32_t oem_revision;   /**< OEM 修订版本 */
    uint32_t creator_id;     /**< 创建者 ID */
    uint32_t creator_revision; /**< 创建者修订版本 */
} __attribute__((packed)) sdt_header_t;

/**
 * FADT (Fixed ACPI Description Table) — 包含电源管理寄存器信息
 * 签名为 "FACP"，包含 PM1 事件/控制寄存器块地址
 */
typedef struct {
    sdt_header_t h;          /**< 通用 SDT 头部 */
    uint32_t firmware_ctrl;  /**< FACS 地址 */
    uint32_t dsdt;           /**< DSDT 地址 */
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;        /**< SCI 中断号 */
    uint32_t smi_cmd;        /**< SMI 命令端口 */
    uint8_t acpi_enable;     /**< 启用 ACPI 的命令值 */
    uint8_t acpi_disable;    /**< 禁用 ACPI 的命令值 */
    uint8_t s4bios_req;      /**< S4BIOS 请求值 */
    uint8_t pstate_cnt;      /**< P-State 控制值 */
    uint32_t pm1a_evt_blk;   /**< PM1a 事件寄存器块地址 */
    uint32_t pm1b_evt_blk;   /**< PM1b 事件寄存器块地址 */
    uint32_t pm1a_cnt_blk;   /**< PM1a 控制寄存器块地址 */
    uint32_t pm1b_cnt_blk;   /**< PM1b 控制寄存器块地址 */
} __attribute__((packed)) fadt_t;

/** PM1a 控制寄存器 I/O 端口地址 */
static uint16_t g_pm1a_cnt;
/** PM1b 控制寄存器 I/O 端口地址 */
static uint16_t g_pm1b_cnt;
/** S5 睡眠类型的 PM1a 值 */
static uint8_t g_slp_typa;
/** S5 睡眠类型的 PM1b 值 */
static uint8_t g_slp_typb;
/** ACPI 初始化是否成功 */
static int g_ready;

/** 向 I/O 端口写入 16 位值 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * 在 Multiboot2 信息结构中查找指定类型的标签
 * @param mbi   Multiboot2 信息结构指针
 * @param type  要查找的标签类型
 * @return 标签地址，未找到返回 NULL
 */
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

/** 比较 4 字节签名是否相等 */
static int sig_eq(const char *a, const char *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/**
 * 在 RSDT (32 位条目) 中查找指定签名的 ACPI 表
 * @param rsdt  RSDT 指针
 * @param sig   要查找的 4 字节签名
 * @return SDT 头部指针，未找到返回 NULL
 */
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

/**
 * 在 XSDT (64 位条目) 中查找指定签名的 ACPI 表
 * @param xsdt  XSDT 指针
 * @param sig   要查找的 4 字节签名
 * @return SDT 头部指针，未找到返回 NULL
 */
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

/**
 * 解析 AML 整数编码 (用于 _S5 包中的睡眠类型值)
 * 支持 Zero(0x00), One(0x01), Byte(0x0a), Word(0x0b), DWord(0x0c) 编码
 * @param p    指向 AML 字节流的指针（会被前移）
 * @param end  AML 字节流结束地址
 * @param out  输出解析后的整数值
 * @return 0 成功, -1 失败
 */
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

/**
 * 从 DSDT 中解析 _S5 包，提取 S5 睡眠类型值
 * _S5 包定义了系统关机时需要写入 PM1 控制寄存器的睡眠类型
 * @param dsdt  DSDT 表指针
 * @param typa  输出 SLP_TYPa 值
 * @param typb  输出 SLP_TYPb 值
 * @return 0 成功, -1 失败
 */
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

/**
 * 初始化 ACPI 子系统
 * 解析 RSDP → RSDT/XSDT → FADT → DSDT → _S5，提取关机所需的寄存器地址和睡眠类型值
 * @param mbi  Multiboot2 信息结构指针
 */
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

/**
 * 通过 ACPI S5 睡眠状态关机
 * 写入 PM1a/PM1b 控制寄存器: SLP_TYP << 10 | SLP_EN
 * @return 0 成功, -1 失败（ACPI 未初始化）
 */
int acpi_poweroff(void) {
    if (!g_ready || !g_pm1a_cnt) return -1;
    outw(g_pm1a_cnt, (uint16_t)((g_slp_typa << 10) | ACPI_SLP_EN));
    if (g_pm1b_cnt)
        outw(g_pm1b_cnt, (uint16_t)((g_slp_typb << 10) | ACPI_SLP_EN));
    return 0;
}
