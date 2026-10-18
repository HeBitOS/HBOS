/**
 * @file    nvme.c
 * @brief   NVMe 主机控制器驱动（轮询 + 单队列，最小实现）
 *
 * 只做四件事：找到控制器、建 admin 队列、Identify、建一个 I/O 队列并读写。
 * 不做中断、不做多队列、不做 PRP list（单次传输限制在一页内，按扇区循环）。
 *
 * 与 Linux 的关系：初始化顺序与寄存器语义按 NVMe 规范，参考了 Linux
 * drivers/nvme/host/pci.c；代码本身是重写，不是拷贝（Linux 版是
 * GPL-2.0-only，与 HBOS 的 GPLv3 不兼容）。
 */

#include "nvme.h"
#include "block.h"
#include "pci.h"
#include "string.h"
#include "core/pmm.h"
#include "core/vmm.h"
#include "graphics/graphics.h"

/* ── PCI 定位 ─────────────────────────────────────────────────────── */
#define NVME_CLASS       0x01   /* Mass storage */
#define NVME_SUBCLASS    0x08   /* Non-volatile memory */
#define NVME_PROGIF      0x02   /* NVMHCI */

/* ── 寄存器（BAR0，字节偏移）───────────────────────────────────────── */
#define REG_CAP     0x00
#define REG_VS      0x08
#define REG_CC      0x14
#define REG_CSTS    0x1C
#define REG_AQA     0x24
#define REG_ASQ     0x28
#define REG_ACQ     0x30
#define REG_DBS     0x1000      /* doorbell base */

#define CC_EN         (1u << 0)
#define CC_IOSQES     (6u << 16)   /* 64B submission entry */
#define CC_IOCQES     (4u << 20)   /* 16B completion entry */
#define CSTS_RDY      (1u << 0)
#define CSTS_CFS      (1u << 1)

/* ── 命令 opcode ──────────────────────────────────────────────────── */
#define OP_ADMIN_CREATE_SQ  0x01
#define OP_ADMIN_CREATE_CQ  0x05
#define OP_ADMIN_IDENTIFY   0x06
#define OP_ADMIN_SET_FEAT   0x09
#define OP_IO_WRITE         0x01
#define OP_IO_READ          0x02

#define FID_NUM_QUEUES      0x07
#define IDENTIFY_NSID       0x00
#define IDENTIFY_CTRL       0x01

#define NVME_QD   32            /* 队列深度（两个队列一样，方便算） */
#define NVME_TIMEOUT 20000000u  /* 轮询上限，够慢机器握手了 */

typedef struct __attribute__((packed)) {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cdw2, cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_cmd_t;

typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} nvme_cqe_t;

static volatile uint32_t *g_regs;
static uint32_t  g_dstrd;          /* doorbell stride: 4 << dstrd */
static uint8_t  *g_asq, *g_acq;    /* admin 队列（恒等映射，指针即物理地址） */
static uint8_t  *g_sq,  *g_cq;     /* I/O 队列 */
static uint8_t  *g_buf;            /* 单页 bounce buffer */
static uint16_t  g_asq_tail, g_acq_head;
static uint16_t  g_sq_tail,  g_cq_head;
static uint8_t   g_a_phase = 1, g_i_phase = 1;
static uint16_t  g_cid;
static uint32_t  g_nsid;
static uint32_t  g_sectors;
static int       g_ready;
static int       g_tried;   /* 失败后不再重复探测：每次探测都会占 5 个物理页 */

static void nvme_put_u32(uint32_t v) {
    char s[12];
    int i = 0;
    if (!v) { console_write("0", 1); return; }
    while (v && i < (int)sizeof(s)) { s[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) console_write(&s[i], 1);
}

static inline uint32_t reg_rd(uint32_t off) {
    return *(volatile uint32_t *)((uint8_t *)g_regs + off);
}
static inline void reg_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)((uint8_t *)g_regs + off) = v;
}
static inline void mem_barrier(void) { __asm__ volatile("" ::: "memory"); }

/* doorbell: qid*2 + (1 if completion) */
static volatile uint32_t *doorbell(uint16_t qid, int cq) {
    uint32_t stride = 4u << g_dstrd;
    return (volatile uint32_t *)((uint8_t *)g_regs + REG_DBS +
                                 (uint32_t)(2 * qid + (cq ? 1 : 0)) * stride);
}

static int wait_csts(uint32_t mask, int want) {
    for (uint32_t i = 0; i < NVME_TIMEOUT; i++) {
        uint32_t csts = reg_rd(REG_CSTS);
        if (csts & CSTS_CFS) return -1;
        if (!!(csts & mask) == want) return 0;
    }
    return -1;
}

/* 提交一条 admin 命令并轮询完成队列 */
static int admin_cmd(nvme_cmd_t *cmd) {
    cmd->cid = ++g_cid;
    memcpy(g_asq + (size_t)g_asq_tail * 64, cmd, 64);
    mem_barrier();
    g_asq_tail = (uint16_t)((g_asq_tail + 1) % NVME_QD);
    *doorbell(0, 0) = g_asq_tail;

    for (uint32_t spin = 0; spin < NVME_TIMEOUT; spin++) {
        nvme_cqe_t *cqe = (nvme_cqe_t *)(g_acq + (size_t)g_acq_head * 16);
        uint16_t st = *(volatile uint16_t *)&cqe->status;
        if ((uint8_t)(st & 1) == g_a_phase) {
            uint16_t code = (uint16_t)((st >> 1) & 0x7FFFu);
            g_acq_head = (uint16_t)((g_acq_head + 1) % NVME_QD);
            if (g_acq_head == 0) g_a_phase ^= 1;
            *doorbell(0, 1) = g_acq_head;
            return code ? -1 : 0;
        }
    }
    return -1;
}

/* 提交一条 I/O 命令并轮询 */
static int io_cmd(nvme_cmd_t *cmd) {
    cmd->cid = ++g_cid;
    memcpy(g_sq + (size_t)g_sq_tail * 64, cmd, 64);
    mem_barrier();
    g_sq_tail = (uint16_t)((g_sq_tail + 1) % NVME_QD);
    *doorbell(1, 0) = g_sq_tail;

    for (uint32_t spin = 0; spin < NVME_TIMEOUT; spin++) {
        nvme_cqe_t *cqe = (nvme_cqe_t *)(g_cq + (size_t)g_cq_head * 16);
        uint16_t st = *(volatile uint16_t *)&cqe->status;
        if ((uint8_t)(st & 1) == g_i_phase) {
            uint16_t code = (uint16_t)((st >> 1) & 0x7FFFu);
            g_cq_head = (uint16_t)((g_cq_head + 1) % NVME_QD);
            if (g_cq_head == 0) g_i_phase ^= 1;
            *doorbell(1, 1) = g_cq_head;
            return code ? -1 : 0;
        }
    }
    return -1;
}

static int identify(uint32_t nsid, uint32_t cns) {
    nvme_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = OP_ADMIN_IDENTIFY;
    c.nsid = nsid;
    c.prp1 = (uint64_t)(uintptr_t)g_buf;
    c.cdw10 = cns;
    return admin_cmd(&c);
}

int nvme_init(void) {
    if (g_ready) return 0;
    if (g_tried) return -1;
    g_tried = 1;

    pci_device_t dev;
    if (pci_find_class(NVME_CLASS, NVME_SUBCLASS, NVME_PROGIF, &dev) < 0)
        return -1;

    int is_io = 0;
    uint64_t bar = pci_bar_address(dev.bus, dev.slot, dev.func, 0, &is_io);
    if (!bar || is_io) return -1;
    pci_enable_bus_master_mmio(&dev);

    /* 恒等映射 BAR：NVMe 的寄存器与 doorbell 都在 0x1000 之后，映射 16 页够用 */
    for (uint64_t off = 0; off < 0x4000; off += 0x1000)
        (void)vmm_map_page(bar + off, bar + off, VMM_W | VMM_CD);
    g_regs = (volatile uint32_t *)(uintptr_t)bar;

    uint64_t cap = (uint64_t)reg_rd(REG_CAP) | ((uint64_t)reg_rd(REG_CAP + 4) << 32);
    uint32_t mqes = (uint32_t)(cap & 0xFFFFu);
    g_dstrd = (uint32_t)((cap >> 32) & 0xFu);
    uint32_t qsize = NVME_QD;
    if (qsize > mqes + 1) qsize = mqes + 1;
    if (qsize < 2) return -1;

    /* 控制器复位：CC.EN=0 → 等 RDY=0 */
    reg_wr(REG_CC, 0);
    if (wait_csts(CSTS_RDY, 0) < 0) return -1;

    uint64_t asq = pmm_alloc_page();
    uint64_t acq = pmm_alloc_page();
    uint64_t sq  = pmm_alloc_page();
    uint64_t cq  = pmm_alloc_page();
    uint64_t buf = pmm_alloc_page();
    if (!asq || !acq || !sq || !cq || !buf) return -1;
    g_asq = (uint8_t *)(uintptr_t)asq;
    g_acq = (uint8_t *)(uintptr_t)acq;
    g_sq  = (uint8_t *)(uintptr_t)sq;
    g_cq  = (uint8_t *)(uintptr_t)cq;
    g_buf = (uint8_t *)(uintptr_t)buf;
    memset(g_asq, 0, 0x1000); memset(g_acq, 0, 0x1000);
    memset(g_sq, 0, 0x1000);  memset(g_cq, 0, 0x1000);

    reg_wr(REG_AQA, ((uint32_t)(qsize - 1) << 16) | (uint32_t)(qsize - 1));
    reg_wr(REG_ASQ, (uint32_t)asq);
    reg_wr(REG_ASQ + 4, (uint32_t)(asq >> 32));
    reg_wr(REG_ACQ, (uint32_t)acq);
    reg_wr(REG_ACQ + 4, (uint32_t)(acq >> 32));
    reg_wr(REG_CC, CC_EN | CC_IOSQES | CC_IOCQES);
    if (wait_csts(CSTS_RDY, 1) < 0) return -1;

    /* 控制器信息：拿命名空间数量 */
    if (identify(0, IDENTIFY_CTRL) < 0) return -1;
    uint32_t nn = *(uint32_t *)(g_buf + 516);
    if (!nn) return -1;

    /* 只要一个 I/O 队列对：先设置队列数，再建 CQ，再建 SQ */
    nvme_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = OP_ADMIN_SET_FEAT;
    c.cdw10 = FID_NUM_QUEUES | (1u << 16);   /* 0-based: 1 个队列 */
    (void)admin_cmd(&c);                     /* 不支持就用默认的 1 个 */

    memset(&c, 0, sizeof(c));
    c.opcode = OP_ADMIN_CREATE_CQ;
    c.prp1 = cq;
    c.cdw10 = 1u | ((qsize - 1) << 16);
    c.cdw11 = 1u;                            /* PC=1 */
    if (admin_cmd(&c) < 0) return -1;

    memset(&c, 0, sizeof(c));
    c.opcode = OP_ADMIN_CREATE_SQ;
    c.prp1 = sq;
    c.cdw10 = 1u | ((qsize - 1) << 16);
    c.cdw11 = 1u | (1u << 16);               /* PC=1, CQID=1 */
    if (admin_cmd(&c) < 0) return -1;

    /* 命名空间 1：大小与 LBA 格式 */
    if (identify(1, IDENTIFY_NSID) < 0) return -1;
    uint64_t nsze = *(uint64_t *)g_buf;
    uint8_t flbas = g_buf[26];
    uint8_t lbaf = flbas & 0x0F;
    uint32_t lbads = (*(uint32_t *)(g_buf + 128 + lbaf * 4) >> 16) & 0xFF;
    if (!nsze || lbads != 9) return -1;      /* 只支持 512B 扇区 */

    g_nsid = 1;
    g_sectors = nsze > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)nsze;
    g_ready = 1;

    console_write("nvme: ", 6);
    nvme_put_u32(g_sectors);
    console_write(" sectors\n", 9);
    return 0;
}

int nvme_ready(void) { return g_ready; }
uint32_t nvme_sector_count(void) { return g_sectors; }

static int rw_one(uint8_t opcode, uint32_t lba, uint8_t *buf, int write) {
    if (!g_ready) return -1;
    if (write) memcpy(g_buf, buf, 512);

    nvme_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = opcode;
    c.nsid = g_nsid;
    c.prp1 = (uint64_t)(uintptr_t)g_buf;
    c.cdw10 = lba;
    c.cdw11 = 0;
    c.cdw12 = 0;                 /* NLB 0-based: 1 个扇区 */
    if (io_cmd(&c) < 0) return -1;

    if (!write) memcpy(buf, g_buf, 512);
    return 0;
}

int nvme_read_sectors(uint32_t lba, uint8_t *buffer, uint32_t count) {
    if (!buffer || !count) return -1;
    if (lba >= g_sectors || count > g_sectors - lba) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (rw_one(OP_IO_READ, lba + i, buffer + (size_t)i * 512, 0) < 0)
            return -1;
    }
    return 0;
}

int nvme_write_sectors(uint32_t lba, const uint8_t *buffer, uint32_t count) {
    if (!buffer || !count) return -1;
    if (lba >= g_sectors || count > g_sectors - lba) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (rw_one(OP_IO_WRITE, lba + i, (uint8_t *)(buffer + (size_t)i * 512), 1) < 0)
            return -1;
    }
    return 0;
}
