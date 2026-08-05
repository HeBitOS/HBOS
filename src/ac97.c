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
#include "core/io.h"
#include "core/vmm.h"
#include "core/task.h"
#include "core/wait.h"
#include "string.h"

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
#define AC97_RESET_TIMEOUT_MS 100U
#define AC97_RESET_MAX_STALL_SPINS 100000U
#define AC97_PLAYBACK_MARGIN_MS 2000U
#define AC97_PLAYBACK_MAX_STALL_SPINS 20000000U

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
    io_out16(g_nam_base + NAM_RESET, 0);
    io_out16(g_nam_base + NAM_MASTER_VOL, 0x0000);
    io_out16(g_nam_base + NAM_PCM_VOL, 0x0000);

    /* 复位 PCM OUT bus master 通道，等复位位自动清零 */
    io_out8(g_nabm_base + NABM_PO_CR, PO_CR_RR);
    hw_deadline_t reset_deadline = hw_deadline_start();
    while (io_in8(g_nabm_base + NABM_PO_CR) & PO_CR_RR) {
        if (hw_deadline_expired_ms(&reset_deadline, AC97_RESET_TIMEOUT_MS,
                                   AC97_RESET_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }

    if (!g_bdl) {
        g_bdl = (ac97_bdl_entry_t *)kmalloc_aligned(
            sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES, 8);
        if (!g_bdl) return -1;
        g_bdl_phys = vmm_virt_to_phys((uint64_t)(uintptr_t)g_bdl);
        if (g_bdl_phys > UINT32_MAX) {
            kfree(g_bdl);
            g_bdl = 0;
            g_bdl_phys = 0;
            return -1;
        }
    }

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
        uint64_t buffer_phys = vmm_virt_to_phys((uint64_t)(uintptr_t)cursor);
        if (buffer_phys > UINT32_MAX) return -1;
        g_bdl[entry].buffer_ptr = (uint32_t)buffer_phys;
        g_bdl[entry].num_samples = (uint16_t)chunk;
        g_bdl[entry].flags = 0;
        cursor += chunk;
        remaining -= chunk;
        entry++;
    }
    if (remaining > 0) return -1; /* 理论上不会到这里，容量已在上面检查过 */

    int last_entry = entry - 1;
    io_out32(g_nabm_base + NABM_PO_BDBAR, (uint32_t)g_bdl_phys);
    io_out8(g_nabm_base + NABM_PO_LVI, (uint8_t)last_entry);
    io_out8(g_nabm_base + NABM_PO_CR, PO_CR_RPBM);

    /* 阻塞等播放完成（PO_SR 的 DCH 位在 DMA 引擎追到 LVI 边界后置位）——
     * 用 task_yield() 而不是纯自旋，至少不会独占整个协作式调度器。这里
     * 没有做成后台任务：真正的后台/非阻塞播放需要一个独立任务 + 状态
     * 机制，超出这次"能放出声音来"这个目标的范围。 */
    uint64_t expected_ms = ((uint64_t)frame_count * 1000U + 47999U) / 48000U;
    uint64_t timeout_ms64 = expected_ms + AC97_PLAYBACK_MARGIN_MS;
    uint32_t timeout_ms = timeout_ms64 > UINT32_MAX
                        ? UINT32_MAX : (uint32_t)timeout_ms64;
    hw_deadline_t playback_deadline = hw_deadline_start();
    int timed_out = 0;
    while (!(io_in16(g_nabm_base + NABM_PO_SR) & PO_SR_DCH)) {
        task_yield();
        if (hw_deadline_expired_ms(&playback_deadline, timeout_ms,
                                   AC97_PLAYBACK_MAX_STALL_SPINS)) {
            timed_out = 1;
            break;
        }
    }
    io_out8(g_nabm_base + NABM_PO_CR, 0); /* 停止 bus master */

    return timed_out ? -1 : 0;
}
