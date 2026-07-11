/**
 * @file ac97.c
 * @brief AC97 (Intel 82801AA 兼容) 声卡驱动——PCI 探测 + codec 初始化 +
 *        PCM OUT bus master DMA，阻塞式播放 16 位 PCM。
 *
 * AC97 控制器暴露两个 I/O 空间 BAR：BAR0 是 Native Audio Mixer（codec
 * 寄存器，音量等），BAR1 是 Native Audio Bus Master（DMA 缓冲区描述符
 * 列表控制）。播放走 PCM OUT 通道（NABM 偏移 0x10 起），用一个最多 32
 * 条描述符的 Buffer Descriptor List，每条指向一段物理连续的 16 位 PCM
 * 数据。没有实现变采样率协商（VRA）/重采样，固定按 AC97 默认的
 * 48000Hz 输出——非 48000Hz 的 WAV 会以错误的速度/音调播放，这是有意
 * 接受的限制（真正的重采样是独立的工作量，不在这次范围内）。
 */

#include "ac97.h"
#include "pci.h"
#include "core/heap.h"
#include "core/vmm.h"
#include "core/task.h"
#include "string.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* NABM (Native Audio Bus Master) 寄存器偏移——PCM OUT 通道 */
#define NABM_PO_BDBAR 0x10 /**< u32: buffer descriptor list 物理基址 */
#define NABM_PO_CIV   0x14 /**< u8:  当前正在播放的描述符下标（只读） */
#define NABM_PO_LVI   0x15 /**< u8:  最后一条有效描述符下标 */
#define NABM_PO_SR    0x16 /**< u16: 状态寄存器 */
#define NABM_PO_PICB  0x18 /**< u16: 当前缓冲区剩余采样数（只读） */
#define NABM_PO_CR    0x1B /**< u8:  控制寄存器 */

#define PO_SR_DCH   0x0001 /**< DMA controller halted */
#define PO_CR_RPBM  0x01   /**< run/pause bus master：1=运行 */
#define PO_CR_RR    0x02   /**< reset regs */

/* NAM (Native Audio Mixer / codec) 寄存器偏移 */
#define NAM_RESET      0x00
#define NAM_MASTER_VOL 0x02
#define NAM_PCM_VOL    0x18

#define AC97_BDL_ENTRIES   32
#define AC97_MAX_SAMPLES_PER_ENTRY 0xFFFE /**< 每条描述符最多的 16 位采样点数 */

typedef struct {
    uint32_t buffer_ptr;
    uint16_t num_samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

static int g_present = 0;
static uint16_t g_nam_base = 0;
static uint16_t g_nabm_base = 0;
static ac97_bdl_entry_t *g_bdl = 0;
static uint64_t g_bdl_phys = 0;

/* PCI BAR 是 I/O 空间时 bit0=1，端口地址在 bit2 起（bit1 保留为 0） */
static uint16_t io_bar_port(uint32_t bar) {
    if (!(bar & 0x1)) return 0; /* 不是 I/O 空间 BAR */
    return (uint16_t)(bar & 0xFFFC);
}

int ac97_present(void) { return g_present; }

int ac97_init(void) {
    g_present = 0;

    pci_device_t dev;
    if (pci_find_class(0x04, 0x01, 0xFF, &dev) < 0) return -1;

    /* 使能 I/O 空间访问 + 总线主控（AC97 用 I/O BAR，不是 MMIO，所以不能
     * 直接用 pci_enable_bus_master_mmio()——那个只置 Memory Space 位）。 */
    uint32_t cmd = pci_read32(dev.bus, dev.slot, dev.func, 0x04);
    cmd |= 0x00000005U; /* bit0 I/O space + bit2 bus master */
    pci_write32(dev.bus, dev.slot, dev.func, 0x04, cmd);

    uint16_t nam = io_bar_port(pci_bar(dev.bus, dev.slot, dev.func, 0));
    uint16_t nabm = io_bar_port(pci_bar(dev.bus, dev.slot, dev.func, 1));
    if (!nam || !nabm) return -1;
    g_nam_base = nam;
    g_nabm_base = nabm;

    /* codec 冷复位 + 音量设为最大（0x0000 = 0dB 衰减，未静音） */
    outw(g_nam_base + NAM_RESET, 0);
    outw(g_nam_base + NAM_MASTER_VOL, 0x0000);
    outw(g_nam_base + NAM_PCM_VOL, 0x0000);

    /* 复位 PCM OUT bus master 通道，等复位位自动清零 */
    outb(g_nabm_base + NABM_PO_CR, PO_CR_RR);
    for (int i = 0; i < 100000 && (inb(g_nabm_base + NABM_PO_CR) & PO_CR_RR); i++) {
        __asm__ volatile ("pause");
    }

    g_bdl = (ac97_bdl_entry_t *)kmalloc_aligned(sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES, 8);
    if (!g_bdl) return -1;
    g_bdl_phys = vmm_virt_to_phys((uint64_t)(uintptr_t)g_bdl);

    g_present = 1;
    return 0;
}

int ac97_play_pcm16(const int16_t *samples, uint32_t frame_count, int channels) {
    if (!g_present || !samples || frame_count == 0) return -1;
    if (channels != 1 && channels != 2) return -1;

    uint64_t total_samples = (uint64_t)frame_count * (uint32_t)channels;
    uint64_t max_total = (uint64_t)AC97_BDL_ENTRIES * AC97_MAX_SAMPLES_PER_ENTRY;
    if (total_samples > max_total) return -1; /* 太长，一次播放不下（约 44 秒立体声） */

    memset(g_bdl, 0, sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES);

    const int16_t *cursor = samples;
    uint64_t remaining = total_samples;
    int entry = 0;
    while (remaining > 0 && entry < AC97_BDL_ENTRIES) {
        uint32_t chunk = (remaining > AC97_MAX_SAMPLES_PER_ENTRY) ? AC97_MAX_SAMPLES_PER_ENTRY : (uint32_t)remaining;
        g_bdl[entry].buffer_ptr = (uint32_t)vmm_virt_to_phys((uint64_t)(uintptr_t)cursor);
        g_bdl[entry].num_samples = (uint16_t)chunk;
        g_bdl[entry].flags = 0;
        cursor += chunk;
        remaining -= chunk;
        entry++;
    }
    if (remaining > 0) return -1; /* 理论上不会到这里，容量已在上面检查过 */

    int last_entry = entry - 1;
    outl(g_nabm_base + NABM_PO_BDBAR, (uint32_t)g_bdl_phys);
    outb(g_nabm_base + NABM_PO_LVI, (uint8_t)last_entry);
    outb(g_nabm_base + NABM_PO_CR, PO_CR_RPBM);

    /* 阻塞等播放完成（PO_SR 的 DCH 位在 DMA 引擎追到 LVI 边界后置位）——
     * 用 task_yield() 而不是纯自旋，至少不会独占整个协作式调度器。这里
     * 没有做成后台任务：真正的后台/非阻塞播放需要一个独立任务 + 状态
     * 机制，超出这次"能放出声音来"这个目标的范围。 */
    uint32_t guard = 0;
    while (!(inw(g_nabm_base + NABM_PO_SR) & PO_SR_DCH)) {
        task_yield();
        if (++guard > 20000000U) break; /* 防止 codec 异常时永久挂起 */
    }
    outb(g_nabm_base + NABM_PO_CR, 0); /* 停止 bus master */

    return 0;
}
