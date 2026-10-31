/**
 * @file ata.c
 * @brief IDE/ATA PIO模式硬盘驱动实现，探测主/从通道的主/从盘共4个槽位，
 *        使用第一个支持LBA28的硬盘，通过I/O端口PIO读写，LBA28寻址。
 */

#include "ata.h"
#include "string.h"
#include "core/io.h"
#include "core/wait.h"

#define ATA_CMD_READ_SECTORS  0x20  /**< 读扇区命令 */
#define ATA_CMD_WRITE_SECTORS 0x30  /**< 写扇区命令 */
#define ATA_CMD_IDENTIFY      0xEC  /**< 设备识别命令 */
#define ATA_CMD_CACHE_FLUSH   0xE7  /**< 刷新写缓存命令 */

#define ATA_IO_TIMEOUT_MS 1000U
#define ATA_MAX_STALL_SPINS 1000000U

static const uint16_t ch_io[2]  = { ATA_PRIMARY_IO_BASE, ATA_SECONDARY_IO_BASE };
static const uint16_t ch_alt[2] = { ATA_ALT_STATUS_P, ATA_ALT_STATUS_S };

static ata_device_t g_drives[ATA_MAX_DRIVES];
static int g_drive_count;            /* 已识别设备数 */
static int g_active = -1;            /* 用于读写活动设备下标（首个 LBA28 硬盘） */

static inline uint16_t d_io(const ata_device_t *d)  { return ch_io[d->channel]; }
static inline uint16_t d_alt(const ata_device_t *d) { return ch_alt[d->channel]; }

/**
 * @brief I/O延时等待，通过连续读取备用状态寄存器消耗约400ns
 */
static void ata_io_wait_alt(uint16_t alt) {
    (void)io_in8(alt);
    (void)io_in8(alt);
    (void)io_in8(alt);
    (void)io_in8(alt);
}

/**
 * @brief 等待ATA控制器就绪（BSY位清零）
 * @return 状态寄存器值（就绪时返回），超时返回-1
 */
static int wait_ready_io(uint16_t io) {
    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        uint8_t st = io_in8(io + 7);
        if (!(st & ATA_STATUS_BSY))
            return st;
        if (hw_deadline_expired_ms(&deadline, ATA_IO_TIMEOUT_MS,
                                   ATA_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

int ata_wait(void) {
    return wait_ready_io(ATA_PRIMARY_IO_BASE);
}

/**
 * @brief 等待数据请求就绪（BSY清零且DRQ置位）
 */
static int wait_drq_io(uint16_t io) {
    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        uint8_t st = io_in8(io + 7);
        if (st & ATA_STATUS_ERR) return -1;
        if (!(st & ATA_STATUS_BSY) && (st & ATA_STATUS_DRQ)) return st;
        if (hw_deadline_expired_ms(&deadline, ATA_IO_TIMEOUT_MS,
                                   ATA_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

/**
 * @brief 选择目标盘并设置LBA高4位（位24-27）
 */
static void select_drive(const ata_device_t *d, uint32_t lba) {
    uint16_t io = d_io(d);
    io_out8(io + 6, (uint8_t)((d->slave ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F)));
    ata_io_wait_alt(d_alt(d));
}

/**
 * @brief 对指定槽位执行IDENTIFY命令，获取设备信息
 * @return 成功返回0，槽位无设备返回-1
 */
static int identify_slot(ata_device_t *d) {
    uint16_t io = d_io(d);
    uint16_t id[256];
    uint8_t is_slave = d->slave;

    memset(d, 0, sizeof(*d));
    d->channel = (io == ATA_PRIMARY_IO_BASE) ? 0 : 1;
    d->slave = is_slave;

    io_out8(io + 6, is_slave ? 0xF0 : 0xE0);
    ata_io_wait_alt(d_alt(d));
    io_out8(io + 2, 0);
    io_out8(io + 3, 0);
    io_out8(io + 4, 0);
    io_out8(io + 5, 0);
    io_out8(io + 7, ATA_CMD_IDENTIFY);
    ata_io_wait_alt(d_alt(d));

    uint8_t st = io_in8(io + 7);
    if (st == 0 || st == 0xFF) return -1;   /* 悬空总线 */
    /* ATAPI 设备（光驱等）会把 LBA_MID/HIGH 置为 0x14/0xEB —— 跳过 */
    if (wait_ready_io(io) < 0) return -1;
    uint8_t mid = io_in8(io + 4), high = io_in8(io + 5);
    if ((mid == 0x14 && high == 0xEB) || mid != 0 || high != 0) return -1;
    if (wait_drq_io(io) < 0) return -1;

    for (int i = 0; i < 256; i++) id[i] = io_in16(io);

    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        d->model[i * 2] = (char)(w >> 8);
        d->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    d->model[40] = '\0';
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--) d->model[i] = '\0';

    d->present = 1;
    d->lba28 = (id[49] & (1 << 9)) ? 1 : 0;
    d->sectors = ((uint32_t)id[61] << 16) | id[60];
    return 0;
}

/**
 * @brief 探测全部4个槽位（主/从通道 × 主/从盘），记录活动盘
 * @return 找到至少一块可用硬盘返回0，否则-1
 */
int ata_init(void) {
    g_drive_count = 0;
    g_active = -1;
    for (int ch = 0; ch < 2; ch++) {
        for (int sl = 0; sl < 2; sl++) {
            ata_device_t *d = &g_drives[g_drive_count];
            d->channel = (uint8_t)ch;
            d->slave = (uint8_t)sl;
            if (identify_slot(d) != 0) {
                memset(d, 0, sizeof(*d));
                continue;
            }
            if (g_active < 0 && d->present && d->lba28 && d->sectors > 0)
                g_active = g_drive_count;
            g_drive_count++;
        }
    }
    return g_active >= 0 ? 0 : -1;
}

/**
 * @brief 获取首个可用的 LBA28 硬盘信息（无则返回 present=0 的占位）
 */
const ata_device_t *ata_primary(void) {
    static ata_device_t none;
    if (g_active < 0) { memset(&none, 0, sizeof(none)); return &none; }
    return &g_drives[g_active];
}

int ata_drive_count(void) {
    return g_drive_count;
}

const ata_device_t *ata_drive(int idx) {
    if (idx < 0 || idx >= g_drive_count) return 0;
    return &g_drives[idx];
}

/**
 * @brief 兼容旧接口：探测主通道主盘到 out
 */
int ata_identify(ata_device_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->channel = 0;
    out->slave = 0;
    return identify_slot(out);
}

/**
 * @brief 读取活动盘指定LBA扇区数据到缓冲区（PIO，LBA28）
 */
int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!buffer || g_active < 0) return -1;
    const ata_device_t *d = &g_drives[g_active];
    if (!d->present || !d->lba28) return -1;
    if (lba >= d->sectors) return -1;
    uint16_t io = d_io(d);

    if (wait_ready_io(io) < 0) return -1;
    select_drive(d, lba);
    io_out8(io + 2, 1);
    io_out8(io + 3, (uint8_t)(lba & 0xFF));
    io_out8(io + 4, (uint8_t)((lba >> 8) & 0xFF));
    io_out8(io + 5, (uint8_t)((lba >> 16) & 0xFF));
    io_out8(io + 7, ATA_CMD_READ_SECTORS);
    if (wait_drq_io(io) < 0) return -1;

    uint16_t *dst = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) dst[i] = io_in16(io);
    return 0;
}

/**
 * @brief 将缓冲区数据写入活动盘指定LBA扇区（PIO，LBA28），写后刷新缓存
 */
int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!buffer || g_active < 0) return -1;
    const ata_device_t *d = &g_drives[g_active];
    if (!d->present || !d->lba28) return -1;
    if (lba >= d->sectors) return -1;
    uint16_t io = d_io(d);

    if (wait_ready_io(io) < 0) return -1;
    select_drive(d, lba);
    io_out8(io + 2, 1);
    io_out8(io + 3, (uint8_t)(lba & 0xFF));
    io_out8(io + 4, (uint8_t)((lba >> 8) & 0xFF));
    io_out8(io + 5, (uint8_t)((lba >> 16) & 0xFF));
    io_out8(io + 7, ATA_CMD_WRITE_SECTORS);
    if (wait_drq_io(io) < 0) return -1;

    const uint16_t *src = (const uint16_t *)buffer;
    for (int i = 0; i < 256; i++) io_out16(io, src[i]);
    io_out8(io + 7, ATA_CMD_CACHE_FLUSH);
    return wait_ready_io(io) < 0 ? -1 : 0;
}
