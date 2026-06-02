/**
 * @file ata.c
 * @brief IDE/ATA PIO模式硬盘驱动实现，通过I/O端口操作主通道主盘，支持LBA28寻址
 */

#include "ata.h"
#include "string.h"

#define ATA_ALT_STATUS 0x3F6     /**< 备用状态寄存器端口，读取不清除中断 */
#define ATA_DEVICE_MASTER 0xE0   /**< 主盘选择标志，含LBA模式位（位6置1） */

#define ATA_CMD_READ_SECTORS  0x20  /**< 读扇区命令 */
#define ATA_CMD_WRITE_SECTORS 0x30  /**< 写扇区命令 */
#define ATA_CMD_IDENTIFY      0xEC  /**< 设台识别命令 */
#define ATA_CMD_CACHE_FLUSH   0xE7  /**< 刷新写缓存命令 */

static ata_device_t primary;     /**< 主通道主盘设备信息 */

/**
 * @brief 向指定I/O端口写入一个字节
 * @param port 端口号
 * @param val  要写入的字节值
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief 从指定I/O端口读取一个字节
 * @param port 端口号
 * @return 读取到的字节值
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief 向指定I/O端口写入一个字（16位）
 * @param port 端口号
 * @param val  要写入的字值
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief 从指定I/O端口读取一个字（16位）
 * @param port 端口号
 * @return 读取到的字值
 */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief I/O延时等待，通过连续读取备用状态寄存器消耗约400ns
 *
 * ATA PIO模式下某些操作需要短暂延时，读取备用状态寄存器
 * 是标准延时方式，每次读取约100ns。
 */
static void ata_io_wait(void) {
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
}

/**
 * @brief 等待ATA控制器就绪（BSY位清零）
 * @return 状态寄存器值（就绪时返回），超时返回-1
 */
int ata_wait(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (!(st & ATA_STATUS_BSY))
            return st;
    }
    return -1;
}

/**
 * @brief 等待数据请求就绪（BSY清零且DRQ置位）
 * @return 状态寄存器值（就绪时返回），出错或超时返回-1
 */
static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & ATA_STATUS_ERR) return -1;
        if (!(st & ATA_STATUS_BSY) && (st & ATA_STATUS_DRQ)) return st;
    }
    return -1;
}

/**
 * @brief 选择主盘并设置LBA高4位（位24-27）
 * @param lba LBA扇区地址
 */
static void ata_select(uint32_t lba) {
    outb(ATA_DRIVE_HEAD, (uint8_t)(ATA_DEVICE_MASTER | ((lba >> 24) & 0x0F)));
    ata_io_wait();
}

/**
 * @brief 执行ATA IDENTIFY命令，获取设备信息
 * @param out 输出设备信息结构体指针
 * @return 成功返回0，失败返回-1
 *
 * 通过IDENTIFY命令读取设备256个字（512字节）的识别数据，
 * 从中提取型号、LBA28支持情况和总扇区数等信息。
 */
int ata_identify(ata_device_t *out) {
    uint16_t id[256];
    memset(out, 0, sizeof(*out));

    outb(ATA_DRIVE_HEAD, ATA_DEVICE_MASTER);
    ata_io_wait();
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_wait();

    uint8_t st = inb(ATA_STATUS);
    if (st == 0) return -1;
    if (ata_wait() < 0) return -1;
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) return -1;
    if (ata_wait_drq() < 0) return -1;

    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);

    for (int i = 0; i < 20; i++) {
        uint16_t w = id[27 + i];
        out->model[i * 2] = (char)(w >> 8);
        out->model[i * 2 + 1] = (char)(w & 0xFF);
    }
    out->model[40] = '\0';
    for (int i = 39; i >= 0 && out->model[i] == ' '; i--) out->model[i] = '\0';

    out->present = 1;
    out->lba28 = (id[49] & (1 << 9)) ? 1 : 0;
    out->sectors = ((uint32_t)id[61] << 16) | id[60];
    return 0;
}

/**
 * @brief 初始化ATA驱动，识别主通道主盘
 * @return 成功返回0，失败返回-1
 */
int ata_init(void) {
    return ata_identify(&primary);
}

/**
 * @brief 获取主通道主盘设备信息
 * @return 指向主盘设备信息结构体的指针
 */
const ata_device_t *ata_primary(void) {
    return &primary;
}

/**
 * @brief 读取指定LBA扇区数据到缓冲区
 * @param lba    要读取的扇区LBA地址
 * @param buffer 输出缓冲区，至少ATA_SECTOR_SIZE字节
 * @return 成功返回0，失败返回-1
 *
 * 使用LBA28寻址模式读取一个扇区（512字节），通过PIO方式
 * 从数据端口逐字（16位）读取256次完成传输。
 */
int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!buffer || !primary.present || !primary.lba28) return -1;
    if (lba >= primary.sectors) return -1;

    if (ata_wait() < 0) return -1;
    ata_select(lba);
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    if (ata_wait_drq() < 0) return -1;

    uint16_t *dst = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) dst[i] = inw(ATA_DATA);
    return 0;
}

/**
 * @brief 将缓冲区数据写入指定LBA扇区
 * @param lba    要写入的扇区LBA地址
 * @param buffer 输入缓冲区，至少ATA_SECTOR_SIZE字节
 * @return 成功返回0，失败返回-1
 *
 * 使用LBA28寻址模式写入一个扇区（512字节），通过PIO方式
 * 向数据端口逐字（16位）写入256次完成传输，写入后刷新缓存。
 */
int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!buffer || !primary.present || !primary.lba28) return -1;
    if (lba >= primary.sectors) return -1;

    if (ata_wait() < 0) return -1;
    ata_select(lba);
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    if (ata_wait_drq() < 0) return -1;

    const uint16_t *src = (const uint16_t *)buffer;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, src[i]);
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait() < 0 ? -1 : 0;
}
