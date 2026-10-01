/**
 * @file usb_ehci.c
 * @brief EHCI（USB 2.0）主机控制器驱动 —— 纯轮询实现
 *
 * 为什么需要这个文件：真机上 USB 2.0 端口大量挂在 EHCI 控制器上（2012 年
 * 前后的主板/笔记本尤其如此），它与 xHCI 是两条独立总线。只做 xHCI 时，
 * 这类机器"有 USB 控制器却看不到鼠标"。本驱动把 EHCI 接进 `usb_host.h`
 * 的控制器抽象，HID 层因此不需要区分控制器类型。
 *
 * 数据结构和调度逻辑照抄 Linux `drivers/usb/host/ehci-q.c` /
 * `ehci-sched.c` / `ehci-hcd.c`，不自己发明时序：
 *  - QH（Queue Head，48 字节）承载端点属性（`ehci-q.c: qh_make()`），
 *  - qTD（Queue Element Transfer Descriptor，32 字节）承载一次传输的
 *    buffer/PID/长度（`ehci-q.c: qtd_fill()`），
 *  - 异步调度用"锚点 QH + 环形链表"（`ehci-hcd.c: ehci_init()` 里
 *    `hw->hw_next = QH_NEXT(ehci->async->qh_dma)` 的自环），
 *  - 周期调度用 1024 项 frame list，中断 QH 按 interval 插到
 *    `frame % period == phase` 的那些帧上（`ehci-sched.c:
 *    qh_link_periodic()`）。
 *
 * ## 性能要点 / 不变式
 *
 * 1. **纯轮询**：`USBINTR` 全程为 0，完成判定只读 qTD 的 Active 位，
 *    并带"时间 + 自旋次数"双重截止（`core/wait.h` 的 `hw_deadline_*`）。
 *    因此本驱动可以在中断上下文之外、甚至在启动早期（PIT tick 还没开始
 *    递增）安全使用：tick 不前进时自旋上限兜底。
 * 2. **DMA 结构的对齐是硬约束**：QH 与 qTD 必须 32 字节对齐，frame list
 *    必须 4 KiB 对齐。全部静态分配并显式 `aligned`，不依赖 `kmalloc`
 *    的偶然对齐；指针值就是物理地址（恒等映射），不需要转换。
 * 3. **控制器一次只服务一个同步传输**（`g_ehci_port_busy`）：EHCI 没有
 *    xHCI 的 ring 机制，逐个端口的 QH 我们自己拥有，串行化比"并发 + 复杂
 *    完成回调"简单得多，也符合 HID 层"探测/取报告都是同步调用"的用法。
 * 4. **失败路径逆序释放**：探测失败时先把端口 QH 从调度上摘掉（如果已经
 *    挂上 frame list），再释放设备表项，保证控制器不会再 DMA 到被复用的
 *    端口槽位。
 * 5. **低速/全速设备不归 EHCI 管**：USB 2.0 规范里它们必须由 companion
 *    controller（UHCI/OHCI）服务；EHCI 端口复位后 `PORT_PE` 不为 1 就说明
 *    设备不是高速的，此时按 Linux `ehci-hub.c: check_reset_complete()` 把
 *    `PORT_OWNER` 置 1 释放给 companion（而不是硬用 `PORT_TEST`/TT 去带）。
 *    取舍见 `usb_ehci_port_reset()` 的注释。
 */

#include "usb_ehci.h"

#include "usb_host.h"
#include "xhci.h"
#include "pci.h"
#include "string.h"
#include "core/heap.h"
#include "core/io.h"
#include "core/vmm.h"
#include "core/wait.h"
#include "core/cpu.h"

/* ── PCI 识别 ────────────────────────────────────────────────
 * EHCI 的 PCI 编程接口是 0x20（USB 2.0 主机控制器）；0x10 是 UHCI/OHCI
 * 的 companion，0x30 是 xHCI，不能混。 */
#define EHCI_PCI_CLASS     0x0C
#define EHCI_PCI_SUBCLASS  0x03
#define EHCI_PCI_PROGIF    0x20

/* ── 容量与规模上限 ────────────────────────────────────────── */
#define EHCI_MAX_CONTROLLERS   4U
#define EHCI_MAX_PORTS        15U  /**< HCSPARAMS.N_PORTS 位宽只有 4 bit */
#define EHCI_MAX_DEVICES      (EHCI_MAX_CONTROLLERS * EHCI_MAX_PORTS)
#define EHCI_MAX_EPS           4U  /**< 每个设备缓存的（非 EP0）中断端点 QH 数 */
#define EHCI_QTD_PER_DEVICE    8U  /**< EP0 链（SETUP+DATA+STATUS）+ 余量 */

/* ── 寄存器布局（EHCI 0.95 规范 §2.2/§2.3，Linux include/linux/usb/ehci_def.h） ── */
#define EHCI_CAP_CAPBASE     0x00U /**< CAPLENGTH(7:0) + HCIVERSION(31:16) */
#define EHCI_CAP_HCSPARAMS   0x04U
#define EHCI_CAP_HCCPARAMS   0x08U
#define EHCI_CAP_PORTROUTE   0x0CU

#define EHCI_OP_USBCMD       0x00U
#define EHCI_OP_USBSTS       0x04U
#define EHCI_OP_USBINTR      0x08U
#define EHCI_OP_FRINDEX      0x0CU
#define EHCI_OP_CTRLDSSEG    0x10U
#define EHCI_OP_PERIODICBASE 0x14U
#define EHCI_OP_ASYNCLISTADDR 0x18U
#define EHCI_OP_CONFIGFLAG   0x40U
#define EHCI_OP_PORTSC       0x44U /**< PORTSC[0]，每个端口间隔 4 字节 */

#define EHCI_CMD_RUN         (1U << 0)
#define EHCI_CMD_HCRESET     (1U << 1)
#define EHCI_CMD_ASE         (1U << 5) /**< async schedule enable */
#define EHCI_CMD_PSE         (1U << 4) /**< periodic schedule enable */
#define EHCI_CMD_IAAD        (1U << 6)

#define EHCI_STS_INT         (1U << 0)
#define EHCI_STS_ERR         (1U << 1)
#define EHCI_STS_PCD         (1U << 2)
#define EHCI_STS_FLR         (1U << 3)
#define EHCI_STS_FATAL       (1U << 4)
#define EHCI_STS_IAA         (1U << 5)
#define EHCI_STS_HALT        (1U << 12)
#define EHCI_STS_PSS         (1U << 14)
#define EHCI_STS_ASS         (1U << 15)
/** 一次写清全部状态位：EHCI 的 USBSTS 是 RW1C（除 HALT/ASS/PSS 等只读位）。 */
#define EHCI_STS_W1C_CLEAR   (EHCI_STS_INT | EHCI_STS_ERR | EHCI_STS_PCD | \
                              EHCI_STS_FLR | EHCI_STS_FATAL | EHCI_STS_IAA)

#define EHCI_HCS_N_PORTS(p)   ((p) & 0x0FU)
#define EHCI_HCS_PPC(p)       ((p) & (1U << 4))
#define EHCI_HCS_N_CC(p)      (((p) >> 12) & 0x0FU)
#define EHCI_HCS_N_PCC(p)     (((p) >> 8) & 0x0FU)
#define EHCI_HCS_PORTROUTED(p) ((p) & (1U << 7))
#define EHCI_HCC_EECP(p)      (((p) >> 8) & 0xFFU)
#define EHCI_HCC_64BIT(p)     ((p) & 0x1U)

#define EHCI_PORT_CONNECT    (1U << 0)
#define EHCI_PORT_CSC        (1U << 1)
#define EHCI_PORT_PE         (1U << 2)
#define EHCI_PORT_PEC        (1U << 3)
#define EHCI_PORT_OC         (1U << 4)
#define EHCI_PORT_OCC        (1U << 5)
#define EHCI_PORT_RESUME     (1U << 6)
#define EHCI_PORT_SUSPEND    (1U << 7)
#define EHCI_PORT_RESET      (1U << 8)
#define EHCI_PORT_LS_MASK    (3U << 10)
#define EHCI_PORT_LS_USB11   (1U << 10) /**< PORT_USB11(x)：LS==01 即 1.1 设备 */
#define EHCI_PORT_POWER      (1U << 12)
#define EHCI_PORT_OWNER      (1U << 13)
/** PORTSC 的 RW1C 位（Linux ehci_def.h 的 PORT_RWC_BITS）：回写时必须置 1 才清除。 */
#define EHCI_PORT_RWC_BITS   (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC)
/** GetPortStatus 的"端口变化"位（Linux ehci-hub.c: ehci_hub_status_data()）。 */
#define EHCI_PORT_CHANGE_BITS (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC)

/* ── BIOS/OS 交接（PCI 配置空间里的 EHCI extended capability） ──
 * Linux 出处：drivers/usb/host/pci-quirks.c: ehci_bios_handoff() /
 * quirk_usb_disable_ehci()。cap id 1 = USBLEGSUP，位 16 = BIOS owned，
 * 位 24 = OS owned；U SLEGCTLSTS 是 cap 偏移 +4。 */
#define EHCI_EXT_CAP_USBLEGSUP  1U
#define EHCI_LEGSUP_BIOS_OWNED  (1U << 16)
#define EHCI_LEGSUP_OS_OWNED    (1U << 24)
#define EHCI_LEGSUP_CTLSTS_OFF  4U
/** 写配置空间 cap 基址 +3 的那个字节：触发 BIOS 开始交接（pci-quirks.c:883）。 */
#define EHCI_LEGSUP_BIOS_SEM_OFF 3U
#define EHCI_LEGSUP_OS_SEM_OFF   2U

/* ── 超时与自旋上限 ── */
#define EHCI_MMIO_MAP_SIZE        0x1000U
#define EHCI_RESET_TIMEOUT_MS        250U /**< ehci-hcd.c: ehci_reset() 用 250ms */
#define EHCI_START_TIMEOUT_MS        100U
#define EHCI_HANDOVER_TIMEOUT_MS    1000U
#define EHCI_PORT_POWER_TIMEOUT_MS    20U
#define EHCI_PORT_RESET_TIMEOUT_MS   1000U
/** @brief 保持 PORT_RESET=1 的最短时间（USB 2.0 §7.1.7.5 要求 ≥10ms） */
#define EHCI_PORT_RESET_HOLD_MS       12U
/** root hub 复位后规范要求至少 50ms 才能回读端口状态（usb 2.0 §11.24.2.7）。 */
#define EHCI_PORT_RESET_SETTLE_MS     50U
#define EHCI_SET_ADDRESS_RECOVER_MS    2U
#define EHCI_TRANSFER_TIMEOUT_MS    1000U
#define EHCI_INTR_SAMPLE_TIMEOUT_MS    40U
/** 自旋上限：PIT tick 尚未推进（启动早期）时兜底，避免永久挂死。 */
#define EHCI_INIT_MAX_STALL_SPINS  10000000U
#define EHCI_IO_MAX_STALL_SPINS     5000000U

/* ── 描述符/调度相关的常量 ── */
#define EHCI_FRAME_LIST_ENTRIES  1024U
#define EHCI_CONTROL_MAX_PACKET     64U /**< 高速控制端点固定 64（ehci-q.c:893） */
#define EHCI_TUNE_CERR               3U /**< ehci-hcd.c: EHCI_TUNE_CERR */
#define EHCI_TUNE_RL_HS              4U /**< ehci-hcd.c: EHCI_TUNE_RL_HS */
#define EHCI_TUNE_MULT_HS            1U
#define EHCI_INTR_SCRATCH_BYTES     64U

/** @brief 设备速度（EHCI 只有高速设备直连；全速/低速走 companion） */
#define EHCI_SPEED_FULL 0U
#define EHCI_SPEED_LOW  1U
#define EHCI_SPEED_HIGH 2U

#define EHCI_STATE_UNINITIALIZED 0U
#define EHCI_STATE_READY         1U
#define EHCI_STATE_FAILED        2U
#define EHCI_STATE_NONE          3U /**< 系统里没有 EHCI 控制器 */

/* ── QH 的硬件部分（EHCI §3.6，48 字节，必须 32 字节对齐） ── */
#define QH_INFO1_RL_HS(devaddr, epnum) \
    (((devaddr) & 0x7FU) | (((epnum) & 0x0FU) << 8) | (EHCI_TUNE_RL_HS << 28))
#define QH_INFO1_HEAD        (1U << 15) /**< 异步调度锚点（reclamation head） */
#define QH_INFO1_TOGGLE_CTL  (1U << 14) /**< 数据翻转由 qTD 提供（控制端点） */
#define QH_INFO1_HIGH_SPEED  (2U << 12)
#define QH_INFO1_LOW_SPEED   (1U << 12)
#define QH_INFO2_CMASK       0x0000FF00U
#define QH_INFO2_SMASK       0x000000FFU
#define QH_INFO2_MULT_HS     (EHCI_TUNE_MULT_HS << 30)

/* ── qTD 的 token 位（EHCI §3.5.3，Linux ehci.h: QTD_*） ── */
#define QTD_TOGGLE      (1U << 31)
#define QTD_LENGTH_MASK 0x7FFFU
#define QTD_LENGTH_SHIFT 16
#define QTD_IOC         (1U << 15)
#define QTD_CERR_SHIFT  10
#define QTD_PID_SHIFT   8
#define QTD_STS_ACTIVE  (1U << 7)
#define QTD_STS_HALT    (1U << 6)
#define QTD_STS_DBE     (1U << 5)
#define QTD_STS_BABBLE  (1U << 4)
#define QTD_STS_XACT    (1U << 3)
#define QTD_STS_MMF     (1U << 2)
#define QTD_STS_STS     (1U << 1)
#define QTD_STS_PING    (1U << 0)

#define PID_CODE_OUT   0U
#define PID_CODE_IN    1U
#define PID_CODE_SETUP 2U

#define Q_TYPE_QH      2U  /**< QH 链接指针的类型标签（bit1:0 = 01b << 1） */
#define EHCI_LIST_END  1U  /**< qTD.next / QH.next 的"链表结束"标记（T 位） */

typedef struct {
    uint32_t hw_next;
    uint32_t hw_info1;
    uint32_t hw_info2;
    uint32_t hw_current;
    uint32_t hw_qtd_next;
    uint32_t hw_alt_next;
    uint32_t hw_token;
    uint32_t hw_buf[5];
    uint32_t hw_buf_hi[5];
} __attribute__((aligned(32))) ehci_qh_hw_t;

/** @brief QH + 软件侧簿记；`hw` 必须放在首位（Linux ehci.h struct ehci_qh 同构） */
typedef struct {
    ehci_qh_hw_t hw;
    uint8_t in_use;
    uint8_t is_interrupt;
    uint8_t ep_addr;   /**< 非 EP0 端点地址（含方向位） */
    uint8_t period;    /**< 周期调度间隔，单位 frame；0 = 未挂周期表 */
} ehci_qh_t;

/** @brief qTD 的硬件部分（EHCI §3.5，32 字节，必须 32 字节对齐） */
typedef struct {
    uint32_t hw_next;
    uint32_t hw_alt_next;
    uint32_t hw_token;
    uint32_t hw_buf[5];
    uint32_t hw_buf_hi[5];
} __attribute__((aligned(32))) ehci_qtd_t;

/** @brief 一个已被寻址（SET_ADDRESS 成功）的设备 */
typedef struct {
    uint8_t in_use;
    uint8_t speed;       /**< EHCI_SPEED_* */
    uint8_t port;        /**< 1 起算的 root port 号 */
    uint8_t addr;        /**< USB 地址（1..127） */
    uint8_t max_packet0; /**< EP0 wMaxPacketSize */
    uint8_t controller;  /**< 所属控制器下标 */
    /// EP0 的 QH（静态，随控制器一起分配）
    ehci_qh_t *qh_control;
    /** EP0 的 qTD 链：SETUP + 可选 DATA 链 + STATUS，8 个够用 */
    ehci_qtd_t *qtd;
    int qtd_count;
    /** 中断端点缓存（[]索引 = ep_addr & 0x0F，方向由 ep_addr 的 bit7 区分） */
    ehci_qh_t *qh_intr[EHCI_MAX_EPS];
    ehci_qtd_t *intr_qtd[EHCI_MAX_EPS];
} ehci_device_t;

/** @brief 一个控制器实例 */
typedef struct {
    volatile uint8_t *mmio;
    uint64_t mmio_phys;
    uint32_t caplength;
    uint32_t n_ports;
    uint32_t hcs_params;
    uint32_t hcc_params;
    uint8_t has_ppc;      /**< HCSPARAMS.PPC：需要软件开端口电源 */
    uint8_t has_64bit;    /**< HCCPARAMS.64BIT：可以用 CTRLDSSEGMENT=0 */
    uint8_t pci_bus;
    uint8_t pci_slot;
    uint8_t pci_func;
    uint8_t ready;
    uint32_t *frame_list;             /**< 周期调度表，4 KiB 对齐 */
    uint64_t frame_list_phys;
    volatile uint32_t *portsc_base;
    /** 每个端口一个端口级簿记（端口槽位自身在设备表里） */
    uint8_t port_addressed[EHCI_MAX_PORTS + 1U];
    ehci_qh_t async_head;             /**< 异步调度锚点 QH（自环） */
    ehci_qtd_t async_dummy;           /**< 锚点的 dummy qTD（EHCI §3.6.4 要求 current 有效） */
    uint8_t intr_scratch[EHCI_INTR_SCRATCH_BYTES] __attribute__((aligned(64)));
    /**
     * @brief 常驻的 SETUP 包缓冲（8 字节，DMA 源）
     *
     * 必须是**静态存储**：控制传输的 SETUP 阶段由硬件通过 DMA 去读这块内存，
     * 如果放在 `ehci_control_build()` 的栈上，函数返回后栈帧就可能被后续调用
     * 覆盖（本驱动是同步等待，但等待期间调用的 `ehci_wait_qtd()` 自己也会压栈，
     * 更不用说编译器优化），硬件读到的就是垃圾，表现为 SETUP 交易 XactErr/HALT。
     * Linux 对应的是 `urb->setup_dma`（urb 生命周期内的持久缓冲，见
     * `ehci-q.c: qh_urb_transaction()` 的 `qtd_fill(..., urb->setup_dma, ...)`）。
     */
    uint8_t setup_buf[8] __attribute__((aligned(16)));
} ehci_controller_t;

static ehci_controller_t g_ehci[EHCI_MAX_CONTROLLERS];
static ehci_device_t g_ehci_dev[EHCI_MAX_DEVICES];
static int g_ehci_controller_count;
static int g_ehci_device_count;
static int g_ehci_state = EHCI_STATE_UNINITIALIZED;
/** 串行化闸门：同一时刻只让一个端口的传输占用调度（见文件头不变式 3） */
static int g_ehci_port_busy = -1;

extern void console_puts(const char *s);
extern void console_putchar(char c);

/* ══════════════════════════════════════════════════════════════
 * 诊断输出（与 xhci.c 同风格；只在慢路径调用）
 * ══════════════════════════════════════════════════════════════ */

/** @brief 打印一个十进制无符号数（避免引入 printf） */
static void ehci_dbg_uint(uint32_t value) {
    char buf[11];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value && n < (int)sizeof(buf));
    while (n--) console_putchar(buf[n]);
}

/** @brief 打印一个小写十六进制字节 */
static void ehci_dbg_hex8(uint8_t value) {
    static const char hex[] = "0123456789abcdef";
    console_putchar(hex[(value >> 4) & 0xFU]);
    console_putchar(hex[value & 0xFU]);
}

/** @brief 打印 `bus:slot.func`，用于把控制器和 PCI 位置对上 */
static void ehci_dbg_bdf(uint8_t bus, uint8_t slot, uint8_t func) {
    ehci_dbg_uint(bus);
    console_putchar(':');
    ehci_dbg_uint(slot);
    console_putchar('.');
    ehci_dbg_uint(func);
}

/* ══════════════════════════════════════════════════════════════
 * MMIO 寄存器访问
 * ══════════════════════════════════════════════════════════════ */

/** @brief 读 32 位操作寄存器；`offset` 是相对操作寄存器基址的字节偏移 */
static uint32_t ehci_op_read32(const ehci_controller_t *hc, uint32_t offset) {
    return *(volatile uint32_t *)(hc->mmio + hc->caplength + offset);
}

/** @brief 写 32 位操作寄存器 */
static void ehci_op_write32(const ehci_controller_t *hc, uint32_t offset,
                            uint32_t value) {
    *(volatile uint32_t *)(hc->mmio + hc->caplength + offset) = value;
}

/** @brief 读 32 位能力寄存器（相对 MMIO 基址） */
static uint32_t ehci_cap_read32(const ehci_controller_t *hc, uint32_t offset) {
    return *(volatile uint32_t *)(hc->mmio + offset);
}

/** @brief 读端口 n（1 起算）的 PORTSC */
static uint32_t ehci_read_portsc(const ehci_controller_t *hc, uint32_t port) {
    return hc->portsc_base[port - 1U];
}

/** @brief 写端口 n（1 起算）的 PORTSC */
static void ehci_write_portsc(const ehci_controller_t *hc, uint32_t port,
                              uint32_t value) {
    hc->portsc_base[port - 1U] = value;
}

/**
 * @brief 把 PORTSC 归一化到可以安全回写的形式
 *
 * PORTSC 混了 RO、RWS、RW1C 三种位。RW1C 位（CSC/PEC/OCC）**写 1 才清除**，
 * 若原样回写会把历史变化位清掉，后续判断"端口变化"就永远看不到东西。
 * Linux ehci-hub.c 用 `status & ~PORT_RWC_BITS` 表达同一件事。
 */
static uint32_t ehci_portsc_to_neutral(uint32_t portsc) {
    return portsc & ~EHCI_PORT_RWC_BITS;
}

/* ══════════════════════════════════════════════════════════════
 * QH / qTD 构造
 * ══════════════════════════════════════════════════════════════ */

/** @brief QH 链接指针：地址（清低 5 位）+ QH 类型标签 */
static uint32_t ehci_qh_next(const ehci_qh_t *qh) {
    return ((uint32_t)(uintptr_t)&qh->hw & ~0x1FU) | Q_TYPE_QH;
}

/** @brief 设备地址/速度取整：低 4 位保留给端点号 */
static uint32_t ehci_qh_info1_base(uint8_t addr, uint8_t speed, uint8_t maxp) {
    uint32_t info1 = (uint32_t)(addr & 0x7FU);
    if (speed >= EHCI_SPEED_HIGH) {
        /* Linux ehci-q.c:903：高速控制端点固定 64 字节 + RL=4，翻转交给 qTD */
        info1 |= QH_INFO1_HIGH_SPEED | (EHCI_TUNE_RL_HS << 28) |
                 (EHCI_CONTROL_MAX_PACKET << 16) | QH_INFO1_TOGGLE_CTL;
    } else {
        /* 全速设备（EHCI 只应当看到高速设备；保留此分支以便直接挂 FS 的
         * 集成 TT 控制器可用）信息里写真实 wMaxPacketSize。 */
        if (speed == EHCI_SPEED_LOW) info1 |= QH_INFO1_LOW_SPEED;
        info1 |= ((uint32_t)maxp << 16) | QH_INFO1_TOGGLE_CTL;
    }
    return info1;
}

/**
 * @brief 初始化一个控制端点 QH（EP0）
 *
 * 不变式：调用方负责保证 `qh->hw` 已清零、地址与 maxp 与设备当前状态一致。
 * `hw_current` 指向自带的 dummy qTD 且该 qTD 的 token 为 HALT —— EHCI §3.6.4
 * 规定队列头进入调度时 current 必须指向有效 qTD，Linux ehci-hcd.c:544 在
 * 锚点 QH 上做的也是这件事；之后 `ehci_qh_prepare_submit()` 会把
 * `hw_qtd_next` 直接指向真实 qTD 链（不用 dummy 换位技巧，见那里的注释）。
 */
static void ehci_qh_init_control(ehci_qh_t *qh, ehci_qtd_t *dummy,
                                 uint8_t addr, uint8_t speed, uint8_t maxp) {
    qh->hw.hw_next = EHCI_LIST_END; /* EP0 不进异步环形链表，由提交时显式挂载 */
    qh->hw.hw_info1 = ehci_qh_info1_base(addr, speed, maxp);
    qh->hw.hw_info2 = QH_INFO2_MULT_HS;
    qh->hw.hw_current = (uint32_t)(uintptr_t)dummy;
    qh->hw.hw_qtd_next = EHCI_LIST_END;
    qh->hw.hw_alt_next = EHCI_LIST_END;
    qh->hw.hw_token = QTD_STS_HALT;
    qh->in_use = 1;
}

/**
 * @brief 组装一个 qTD 的 token
 * @param pid  PID_CODE_{OUT,IN,SETUP}
 * @param toggle 数据翻转（0 = DATA0）
 * @param bytes 本次传输的字节数（0 表示零长度阶段）
 * @param ioc 是否在完成时请求 IAA 中断（本驱动轮询，恒传 0）
 */
static uint32_t ehci_qtd_token(uint32_t pid, uint32_t toggle, uint32_t bytes,
                               uint32_t ioc) {
    return QTD_STS_ACTIVE | (EHCI_TUNE_CERR << QTD_CERR_SHIFT) |
           ((bytes & QTD_LENGTH_MASK) << QTD_LENGTH_SHIFT) |
           ((pid & 0x3U) << QTD_PID_SHIFT) |
           (ioc ? QTD_IOC : 0U) |
           (toggle ? QTD_TOGGLE : 0U);
}

/**
 * @brief 填充一个 qTD 的 buffer 列表与长度
 *
 * 照抄 Linux `ehci-q.c: qtd_fill()`：一个 qTD 最多 5 个 4 KiB buffer 项，
 * 首项从当前页的剩余空间开始；qTD 的 16 位长度域最多 0x7FFF，所以本驱动
 * 只在单页以内做填充（控制传输最大 64 KiB 由调用方的 wp_length 限制，
 * 我们额外限制为不超过 1 页，避免出现跨页链）。
 *
 * @return 实际填充进本 qTD 的字节数
 */
static uint32_t ehci_qtd_fill(ehci_qtd_t *qtd, uintptr_t buf, uint32_t len,
                              uint32_t token, uint32_t maxpacket) {
    uint32_t count = 0x1000U - (uint32_t)(buf & 0xFFFU);
    if (len < count) {
        count = len;
    } else {
        /* 首项只到页边界，其余项按 4 KiB 步进；总长控制在 16 KiB 内
         * （5 项），超过部分由调用方切成多个 qTD。 */
        uintptr_t page = (buf + 0x1000U) & ~(uintptr_t)0xFFFU;
        uint32_t filled = count;
        for (int i = 1; i < 5 && filled < len; i++) {
            qtd->hw_buf[i] = (uint32_t)page;
            qtd->hw_buf_hi[i] = (uint32_t)(page >> 32);
            page += 0x1000U;
            filled += (filled + 0x1000U < len) ? 0x1000U : (len - filled);
        }
        count = filled;
        /* 短包只能出现在传输末尾：中间阶段按 maxpacket 向下取整
         * （Linux qtd_fill(): `count -= count % maxpacket`）。 */
        if (count != len && maxpacket) count -= count % maxpacket;
    }

    qtd->hw_buf[0] = (uint32_t)buf;
    qtd->hw_buf_hi[0] = (uint32_t)(buf >> 32);
    qtd->hw_token = token | ((count & QTD_LENGTH_MASK) << QTD_LENGTH_SHIFT);
    return count;
}

/** @brief qTD 的 token 里剩余的字节数（硬件完成后回写的"没传成功"的数量） */
static uint32_t ehci_qtd_residual(const ehci_qtd_t *qtd) {
    return (qtd->hw_token >> QTD_LENGTH_SHIFT) & QTD_LENGTH_MASK;
}

/** @brief 把 qTD 链清成"全部未激活、next 结束" */
static void ehci_qtd_chain_reset(ehci_qtd_t *qtds, int count) {
    for (int i = 0; i < count; i++) {
        qtds[i].hw_next = EHCI_LIST_END;
        qtds[i].hw_alt_next = EHCI_LIST_END;
        qtds[i].hw_token = 0;
        for (int b = 0; b < 5; b++) {
            qtds[i].hw_buf[b] = 0;
            qtds[i].hw_buf_hi[b] = 0;
        }
    }
}

/* ══════════════════════════════════════════════════════════════
 * 设备表
 * ══════════════════════════════════════════════════════════════ */

/** @brief 在设备表里找一个空闲槽；失败返回 -1（-1 不可与合法下标混淆） */
static int ehci_dev_alloc(void) {
    for (int i = 0; i < (int)EHCI_MAX_DEVICES; i++) {
        if (!g_ehci_dev[i].in_use) return i;
    }
    return -1;
}

/** @brief 取设备记录；下标越界或未使用返回 NULL */
static ehci_device_t *ehci_dev_get(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= (int)EHCI_MAX_DEVICES) return NULL;
    if (!g_ehci_dev[dev_idx].in_use) return NULL;
    return &g_ehci_dev[dev_idx];
}

/** @brief 该设备所属控制器 */
static ehci_controller_t *ehci_dev_controller(ehci_device_t *dev) {
    return &g_ehci[dev->controller];
}

/**
 * @brief 释放设备表槽及其 DMA 结构（枚举失败路径用）
 *
 * 分配顺序是 qh_control → qtd → 中断端点结构（见 ehci_scan_ports），
 * 这里按**逆序**释放，并把 in_use 清 0 归还表槽；否则一次端口复位失败
 * 就会永久吃掉一个设备槽与两块 32 字节对齐内存。
 *
 * @param dev_idx 设备下标；越界或未使用则什么都不做
 */
static void ehci_dev_release(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= (int)EHCI_MAX_DEVICES) return;
    ehci_device_t *dev = &g_ehci_dev[dev_idx];
    if (!dev->in_use) return;

    for (int i = 0; i < (int)EHCI_MAX_EPS; i++) {
        if (dev->intr_qtd[i]) { kfree(dev->intr_qtd[i]); dev->intr_qtd[i] = 0; }
        if (dev->qh_intr[i]) { kfree(dev->qh_intr[i]); dev->qh_intr[i] = 0; }
    }
    if (dev->qtd) { kfree(dev->qtd); dev->qtd = 0; }
    if (dev->qh_control) { kfree(dev->qh_control); dev->qh_control = 0; }
    dev->qtd_count = 0;
    dev->in_use = 0;
}

/* ══════════════════════════════════════════════════════════════
 * 异步 / 周期调度挂载
 * ══════════════════════════════════════════════════════════════ */

/** @brief 开异步调度（ASE），并等 ASS 生效 */
static int ehci_enable_async(ehci_controller_t *hc) {
    uint32_t cmd = ehci_op_read32(hc, EHCI_OP_USBCMD);
    if (!(cmd & EHCI_CMD_ASE)) {
        ehci_op_write32(hc, EHCI_OP_USBCMD, cmd | EHCI_CMD_ASE);
    }
    hw_deadline_t deadline = hw_deadline_start();
    while (!(ehci_op_read32(hc, EHCI_OP_USBSTS) & EHCI_STS_ASS)) {
        if (hw_deadline_expired_ms(&deadline, EHCI_START_TIMEOUT_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
    return 0;
}

/** @brief 关异步调度（ASE）并等 ASS 清零；失败只记录，不阻塞后续流程 */
static int ehci_disable_async(ehci_controller_t *hc) {
    uint32_t cmd = ehci_op_read32(hc, EHCI_OP_USBCMD);
    if (cmd & EHCI_CMD_ASE) {
        ehci_op_write32(hc, EHCI_OP_USBCMD, cmd & ~EHCI_CMD_ASE);
    }
    hw_deadline_t deadline = hw_deadline_start();
    while (ehci_op_read32(hc, EHCI_OP_USBSTS) & EHCI_STS_ASS) {
        if (hw_deadline_expired_ms(&deadline, EHCI_START_TIMEOUT_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
    return 0;
}

/** @brief 开周期调度（PSE），并等 PSS 生效 */
static int ehci_enable_periodic(ehci_controller_t *hc) {
    uint32_t cmd = ehci_op_read32(hc, EHCI_OP_USBCMD);
    if (!(cmd & EHCI_CMD_PSE)) {
        ehci_op_write32(hc, EHCI_OP_USBCMD, cmd | EHCI_CMD_PSE);
    }
    hw_deadline_t deadline = hw_deadline_start();
    while (!(ehci_op_read32(hc, EHCI_OP_USBSTS) & EHCI_STS_PSS)) {
        if (hw_deadline_expired_ms(&deadline, EHCI_START_TIMEOUT_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
    return 0;
}

/**
 * @brief 把控制 QH 挂进异步调度，并指向给定的 qTD 链
 *
 * 挂载方式与 Linux `ehci-q.c: qh_link_async()` 一致：插在锚点之后
 * （`qH->next = head->next; head->next = qh`），锚点自环保证环形链表不断。
 * 与 Linux 的差异：不用 "dummy TD 换位" 技巧（`ehci-q.c: qh_append_tds()`
 * 用 dummy 承接旧的首 qTD，避免和硬件赛跑），而是**只在调度关闭时**改链：
 * 我们的提交/回收是同步的，`ehci_disable_async()` 之后硬件不会再抓取 QH，
 * 因此可以安全地直接重写 `hw_qtd_next`。这样少一个必须与硬件同步的
 * 交换步骤，也避免了 overlay 残留导致的偶发 HALT。
 */
static void ehci_qh_prepare_submit(ehci_controller_t *hc, ehci_qh_t *qh,
                                   ehci_qtd_t *first) {
    /* 完整重写 QH 的 overlay 区，等价于 Linux `ehci-q.c: qh_update()`
     * （:76-103）+ `qh_refresh()`（:110-131）：
     *   - hw_qtd_next 指向本次链首；hw_alt_next = LIST_END 禁止队列头在短读后
     *     自动前进（控制传输必须按 SETUP/DATA/STATUS 严格顺序执行）；
     *   - hw_token 只保留 TOGGLE/PING，其余位清零。**不能只清 token 而不写
     *     qtd_next/alt_next**：硬件在链路挂载时会先把 overlay 读进自己的
     *     状态机，overlay 里残留旧 qTD 指针就会去执行一块已经回收的内存，
     *     表现就是每次控制传输都超时、一个描述符都读不回来。
     *   - 链路改动放在最后：先改 qh->hw_next 与锚点 next，再让硬件看到。 */
    qh->hw.hw_qtd_next = (uint32_t)(uintptr_t)first;
    qh->hw.hw_alt_next = EHCI_LIST_END;
    qh->hw.hw_token &= (QTD_TOGGLE | QTD_STS_PING);
    qh->hw.hw_current = (uint32_t)(uintptr_t)first;
    qh->hw.hw_next = ehci_qh_next(qh);
    __sync_synchronize();
    hc->async_head.hw.hw_next = ehci_qh_next(qh);
}

/**
 * @brief 把控制 QH 从异步调度摘掉（只改锚点的 next）
 *
 * 不变式：调用前异步调度已关闭（否则硬件可能正抓着这个 QH）。
 */
static void ehci_qh_unlink_async(ehci_controller_t *hc, ehci_qh_t *qh) {
    if (hc->async_head.hw.hw_next != ehci_qh_next(qh)) return;
    hc->async_head.hw.hw_next = ehci_qh_next(&hc->async_head);
    qh->hw.hw_next = EHCI_LIST_END;
}

/**
 * @brief 周期调度摘链：从所有 frame list 项里摘掉 qh
 *
 * 只处理"frame list 直接指向本 QH，且 QH 的 next 是结束标记"这一种形态
 * （`ehci_qh_link_periodic()` 建立的就是这种），与 Linux
 * `ehci-sched.c: qh_unlink_periodic()` 的意图一致，但不需要影子表：
 * 我们每个端点只挂一个 QH，不会出现共享的中间节点。
 */
static void ehci_qh_unlink_periodic(ehci_controller_t *hc, ehci_qh_t *qh) {
    if (!qh->period) return;
    uint32_t self = ehci_qh_next(qh);
    for (uint32_t i = 0; i < EHCI_FRAME_LIST_ENTRIES; i++) {
        if (hc->frame_list[i] == self) hc->frame_list[i] = EHCI_LIST_END;
    }
    qh->period = 0;
    qh->hw.hw_next = EHCI_LIST_END;
}

/**
 * @brief 把中断 QH 挂进周期调度
 *
 * 照抄 Linux `ehci-sched.c: qh_link_periodic()`：对 `phase = 0` 的端点，
 * 从第 0 帧开始每 `period` 帧把该 QH 的指针写进 frame list；高速设备一个
 * 微帧就能完成一次中断事务，所以 S-mask 取 `1 << uframe`、uframe 取 0。
 * 这里不支持 high-bandwidth（MULT>1）与 split 事务（全速/低速设备已被
 * 交给 companion，见 `usb_ehci_port_reset()`）。
 *
 * @param period 周期（帧）；调用方已保证是 2 的幂且 <= 1024
 */
static void ehci_qh_link_periodic(ehci_controller_t *hc, ehci_qh_t *qh,
                                  uint32_t period) {
    uint32_t self = ehci_qh_next(qh);
    qh->hw.hw_info2 &= ~(QH_INFO2_CMASK | QH_INFO2_SMASK);
    qh->hw.hw_info2 |= (QH_INFO2_SMASK); /* S-mask = 0x01：只用微帧 0 */
    qh->hw.hw_next = EHCI_LIST_END;      /* 每个 frame list 项到此为止 */
    for (uint32_t i = 0; i < EHCI_FRAME_LIST_ENTRIES; i += period) {
        hc->frame_list[i] = self;
    }
    qh->period = (uint8_t)(period > 255U ? 255U : period);
}

/* ══════════════════════════════════════════════════════════════
 * 传输等待
 * ══════════════════════════════════════════════════════════════ */

/**
 * @brief 轮询 qTD 直到 Active 位清零
 *
 * 不变式：调用前 qTD 已被挂进调度且硬件可能正在写它（DMA），因此只读
 * token 一个 32 位字。返回后 qTD 归软件所有，其 hw_token 里带有硬件回写的
 * 长度与错误位。
 *
 * @return 0 已完成（可能带错误位，由调用方检查），-1 超时
 */
static int ehci_wait_qtd(ehci_qtd_t *qtd, uint32_t timeout_ms) {
    hw_deadline_t deadline = hw_deadline_start();
    while (qtd->hw_token & QTD_STS_ACTIVE) {
        if (hw_deadline_expired_ms(&deadline, timeout_ms,
                                   EHCI_IO_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
    return 0;
}

/** @brief qTD 是否以错误结束（HALT 或数据/事务错误位） */
static int ehci_qtd_has_error(const ehci_qtd_t *qtd) {
    return (qtd->hw_token &
            (QTD_STS_HALT | QTD_STS_DBE | QTD_STS_BABBLE | QTD_STS_XACT)) != 0;
}

/* ══════════════════════════════════════════════════════════════
 * 控制传输
 * ══════════════════════════════════════════════════════════════ */

/** @brief POST/GET_DESCRIPTOR 等标准请求的 8 字节 setup 包 */
typedef struct {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) ehci_setup_packet_t;

/** @brief 把本设备 QH 的地址域改成新地址（SET_ADDRESS 之后立即生效） */
static void ehci_dev_set_address(ehci_device_t *dev, uint8_t addr) {
    uint32_t info1 = dev->qh_control->hw.hw_info1 & ~0x7FU;
    dev->qh_control->hw.hw_info1 = info1 | (uint32_t)(addr & 0x7FU);
    dev->addr = addr;
}

/**
 * @brief 在已挂载的控制 QH + qTD 链上等完成并回收
 *
 * @param chain_len  链上 qTD 个数，用于重置
 * @param out_bytes  输出：DATA 阶段实际字节数（失败时不写出）
 * @return 0 成功，-1 失败（STALL/超时/控制器错误）
 */
static int ehci_control_finish(ehci_controller_t *hc, ehci_device_t *dev,
                               int chain_len, ehci_qtd_t *data_qtd,
                               uint32_t data_len, int *out_bytes) {
    ehci_qtd_t *last = &dev->qtd[chain_len - 1];
    int result = -1;

    if (ehci_wait_qtd(last, EHCI_TRANSFER_TIMEOUT_MS) < 0) {
        console_puts("[EHCI] EP0 timeout\n");
    } else if (ehci_qtd_has_error(last)) {
        console_puts("[EHCI] EP0 halted, token=0x");
        ehci_dbg_hex8((uint8_t)(last->hw_token >> 24));
        ehci_dbg_hex8((uint8_t)(last->hw_token >> 16));
        ehci_dbg_hex8((uint8_t)(last->hw_token >> 8));
        ehci_dbg_hex8((uint8_t)last->hw_token);
        console_puts("\n");
    } else if (data_qtd && data_len) {
        uint32_t residual = ehci_qtd_residual(data_qtd);
        if (residual > data_len) residual = data_len;
        if (out_bytes) *out_bytes = (int)(data_len - residual);
        result = 0;
    } else {
        result = 0;
    }

    (void)ehci_disable_async(hc);
    ehci_qh_unlink_async(hc, dev->qh_control);
    ehci_qtd_chain_reset(dev->qtd, chain_len);
    return result;
}

/**
 * @brief 生成一次控制传输的 qTD 链并挂到调度上
 *
 * 阶段顺序照抄 Linux `ehci-q.c: qh_urb_transaction()`：
 *   1. SETUP（PID=SETUP，DT=0，8 字节）；
 *   2. 若 wLength > 0：DATA，方向由 bmRequestType.bit7 决定，DT=1；
 *      wLength == 0 时这一阶段就是 STATUS，且**方向固定为 IN**
 *      （`token |= (PID_CODE_IN << 8)`，ehci-q.c:614）；
 *   3. 若 wLength > 0：STATUS，方向与 DATA 相反，DT 强制 DATA1
 *      （`token ^= PID_CODE_IN; token |= QTD_TOGGLE`，ehci-q.c:706）。
 *
 * @return 0 成功（qTD 已挂上调度），-1 参数非法
 */
static int ehci_control_build(ehci_controller_t *hc, ehci_device_t *dev,
                              uint8_t bmRequestType,
                              uint8_t bRequest, uint16_t wValue,
                              uint16_t wIndex, uintptr_t buf, uint16_t wLength,
                              int *chain_len, ehci_qtd_t **data_qtd) {
    if (wLength > 0 && !buf) return -1;

    /* SETUP 包写进控制器常驻缓冲，而不是本函数的栈帧：硬件要 DMA 读它。 */
    ehci_setup_packet_t *setup_p = (ehci_setup_packet_t *)hc->setup_buf;
    ehci_setup_packet_t setup;
    setup.bmRequestType = bmRequestType;
    setup.bRequest = bRequest;
    setup.wValue = wValue;
    setup.wIndex = wIndex;
    setup.wLength = wLength;

    *setup_p = setup;
    __sync_synchronize();

    ehci_qtd_t *chain = dev->qtd;
    uint8_t maxp = dev->max_packet0 ? dev->max_packet0 : 8U;
    int n = 0;

    /* 阶段 1：SETUP。IOC 恒 0 —— 本驱动纯轮询，不产生 IAA 中断。 */
    uint32_t token = ehci_qtd_token(PID_CODE_SETUP, 0, 8U, 0);
    ehci_qtd_fill(&chain[n], (uintptr_t)setup_p, 8U, token, 8U);
    n++;

    /* 阶段 2：DATA（wLength > 0 时），否则直接进 STATUS */
    int is_in = (bmRequestType & 0x80U) != 0;
    if (wLength > 0) {
        token = ehci_qtd_token(is_in ? PID_CODE_IN : PID_CODE_OUT, 1U,
                               wLength, 0);
        ehci_qtd_fill(&chain[n], buf, wLength, token, maxp);
        if (data_qtd) *data_qtd = &chain[n];
        n++;
    }

    if (n < (int)EHCI_QTD_PER_DEVICE) {
        /* 阶段 3：STATUS。长度为 0、DT 强制 DATA1；方向规则分两种，必须区别对待，
         * 抄错方向会让设备永不回 ACK，qTD 一直停在 Active（表现就是"每次控制
         * 传输都超时"）：
         *   - wLength == 0（没有任何 DATA 阶段）：STATUS 固定为 **IN**
         *     —— Linux ehci-q.c:614 "for zero length DATA stages, STATUS is
         *     always IN"（`token |= (PID_CODE_IN << 8)`）。SET_ADDRESS、
         *     SET_CONFIGURATION、SET_PROTOCOL、SET_IDLE 全走这条路。
         *   - wLength > 0：STATUS 与 DATA 方向**相反**
         *     —— Linux ehci-q.c:706 `token ^= (PID_CODE_IN << 8)`。
         * 旧实现把第一种也当成"DATA 的反方向"，于是零长度请求的 STATUS 发成了
         * OUT，设备侧等的是 STATUS IN，整条链就永远完不成。 */
        uint32_t status_pid = (wLength == 0) ? PID_CODE_IN
                            : (is_in ? PID_CODE_OUT : PID_CODE_IN);
        token = ehci_qtd_token(status_pid, 1U, 0U, 0);
        ehci_qtd_fill(&chain[n], 0, 0U, token, 0U);
        n++;
    }

    for (int i = 0; i + 1 < n; i++) {
        chain[i].hw_next = (uint32_t)(uintptr_t)&chain[i + 1];
    }
    chain[n - 1].hw_next = EHCI_LIST_END;
    /* 短读时队列头不自动前进（Linux 把 control 的 alt_next 设为 LIST_END，
     * ehci-q.c:693）；我们的成链方式保证两个阶段都会执行。 */
    for (int i = 0; i < n; i++) chain[i].hw_alt_next = EHCI_LIST_END;

    if (chain_len) *chain_len = n;
    return 0;
}

/**
 * @brief 控制传输（EP0），见 usb_ehci.h 契约
 *
 * 失败时的部分状态：qTD 链被完整回收、QH 已从异步调度摘除、调度关闭。
 * 缓冲区在失败时不被写入（DATA 阶段只在成功后按实际长度回填，见
 * `ehci_control_finish()`）。
 */
int usb_ehci_control_transfer(int dev_idx, uint8_t bmRequestType,
                              uint8_t bRequest, uint16_t wValue,
                              uint16_t wIndex, void *data, uint16_t wLength) {
    ehci_device_t *dev = ehci_dev_get(dev_idx);
    if (!dev || !dev->qh_control) return -1;
    if (g_ehci_port_busy >= 0) return -1; /* 另一个端口的传输未收尾 */

    ehci_controller_t *hc = ehci_dev_controller(dev);
    if (!hc->ready) return -1;

    int chain_len = 0;
    ehci_qtd_t *data_qtd = NULL;
    if (ehci_control_build(hc, dev, bmRequestType, bRequest, wValue, wIndex,
                           (uintptr_t)data, wLength, &chain_len,
                           &data_qtd) < 0) {
        return -1;
    }

    g_ehci_port_busy = dev_idx;
    ehci_qh_prepare_submit(hc, dev->qh_control, &dev->qtd[0]);
    if (ehci_enable_async(hc) < 0) {
        ehci_qh_unlink_async(hc, dev->qh_control);
        g_ehci_port_busy = -1;
        ehci_qtd_chain_reset(dev->qtd, chain_len);
        return -1;
    }

    int bytes = 0;
    int result = ehci_control_finish(hc, dev, chain_len, data_qtd, wLength,
                                     &bytes);
    g_ehci_port_busy = -1;
    return (result < 0) ? -1 : bytes;
}

/* ══════════════════════════════════════════════════════════════
 * 中断传输（周期调度）
 * ══════════════════════════════════════════════════════════════ */

/** @brief 取中断端点缓存槽下标；端点号超出 0..3 返回 -1 */
static int ehci_intr_slot(int ep_addr) {
    int epnum = ep_addr & 0x0F;
    if (epnum < 1 || epnum > (int)EHCI_MAX_EPS) return -1;
    return epnum - 1;
}

/**
 * @brief 取得（必要时创建）某中断端点的 QH 与常驻 qTD
 *
 * QH 的 maxpacket 用**保守值**而非端点描述符：HID 层没有把 wMaxPacketSize
 * 传进来，而传输长度上限是 64 字节（`len` 由 HID 层的 64 字节报告缓冲限
 * 定），所以控制每个 qTD 只传 8 字节、最多 8 个 qTD 完成一次 64 字节报告，
 * 既不会越界也不会因为 maxpacket 猜错导致设备端错误。
 */
static int ehci_intr_alloc(ehci_controller_t *hc, ehci_device_t *dev,
                           int slot, int ep_addr, ehci_qh_t **out_qh,
                           ehci_qtd_t **out_qtd) {
    if (!dev->qh_intr[slot]) {
        dev->qh_intr[slot] = (ehci_qh_t *)kmalloc_aligned(sizeof(ehci_qh_t), 32);
        if (!dev->qh_intr[slot]) return -1;
        memset(dev->qh_intr[slot], 0, sizeof(ehci_qh_t));
    }
    if (!dev->intr_qtd[slot]) {
        dev->intr_qtd[slot] = (ehci_qtd_t *)kmalloc_aligned(sizeof(ehci_qtd_t), 32);
        if (!dev->intr_qtd[slot]) return -1;
        memset(dev->intr_qtd[slot], 0, sizeof(ehci_qtd_t));
    }

    ehci_qh_t *qh = dev->qh_intr[slot];
    if (!qh->in_use) {
        int epnum = ep_addr & 0x0F;
        int is_in = (ep_addr & 0x80) != 0;
        uint32_t info1 = ((uint32_t)(dev->addr & 0x7FU)) |
                         ((uint32_t)epnum << 8) | QH_INFO1_HIGH_SPEED |
                         (8U << 16);
        memset(qh, 0, sizeof(*qh));
        qh->hw.hw_info1 = info1;
        qh->hw.hw_info2 = QH_INFO2_MULT_HS;
        qh->hw.hw_next = EHCI_LIST_END;
        qh->hw.hw_qtd_next = EHCI_LIST_END;
        qh->hw.hw_current = (uint32_t)(uintptr_t)dev->intr_qtd[slot];
        qh->hw.hw_token = QTD_STS_HALT;
        qh->in_use = 1;
        qh->is_interrupt = 1;
        qh->ep_addr = (uint8_t)ep_addr;
        /* 端点方向写进 QH 之外没地方放，用高 8 位之外的 bit 记录：
         * 直接复用 hw_info1 的端点号即可，方向由 qTD 的 PID 决定。 */
        (void)is_in;
    }

    *out_qh = qh;
    *out_qtd = dev->intr_qtd[slot];
    (void)hc;
    return 0;
}

/**
 * @brief 给中断 QH 装上一个指向控制器私有缓冲的 qTD
 *
 * 不变式：调用前 QH 已从周期表摘掉（`ehci_qh_unlink_periodic()`），否则
 * 硬件可能正在执行旧 qTD，改写它会破坏正在进行的 DMA。
 */
static void ehci_intr_arm(ehci_controller_t *hc, ehci_qh_t *qh, ehci_qtd_t *qtd,
                          uintptr_t buf, uint32_t bytes) {
    memset(qtd, 0, sizeof(*qtd));
    uint32_t token = ehci_qtd_token(PID_CODE_IN, 0, bytes, 0);
    ehci_qtd_fill(qtd, buf, bytes, token, 8U);
    qtd->hw_next = EHCI_LIST_END;
    qtd->hw_alt_next = EHCI_LIST_END;

    qh->hw.hw_qtd_next = (uint32_t)(uintptr_t)qtd;
    qh->hw.hw_alt_next = EHCI_LIST_END;
    qh->hw.hw_token = 0;
    qh->hw.hw_current = (uint32_t)(uintptr_t)qtd;
    (void)hc;
}

/**
 * @brief 中断 IN 传输（轮询式"取一次报告"），见 usb_ehci.h 契约
 *
 * 实现取舍：**每次调用才把 QH 挂进周期表，取完就摘掉**。
 *  - 好处：设备空闲时不会每帧都产生一次 NAK 轮询（省总线带宽、也避免
 *    某些芯片在 QH 常驻但 qTD 已完成时行为不一致）；QH 只在函数执行期间
 *    属于硬件，软件随时可以安全改写 qTD。
 *  - 代价：每次调用要等一个采样窗口（`EHCI_INTR_SAMPLE_TIMEOUT_MS`）。
 *    HID 层本来就在 1ms 级的循环里轮询，可接受。
 *  - "没有数据时不阻塞"：窗口内 qTD 仍 Active 就返回 -1；"有数据时能取到"：
 *    qTD 完成即拷贝并按实际长度返回。
 */
int usb_ehci_interrupt_transfer(int dev_idx, int ep_addr, void *data,
                                uint32_t len, uint32_t interval) {
    if (!data || len == 0) return -1;
    if (!(ep_addr & 0x80)) return -1; /* 本驱动只做中断 IN（HID 报告） */
    if (interval == 0) interval = 1;

    int slot = ehci_intr_slot(ep_addr);
    if (slot < 0) return -1;

    ehci_device_t *dev = ehci_dev_get(dev_idx);
    if (!dev || !dev->qh_control) return -1;
    if (g_ehci_port_busy >= 0) return -1;

    ehci_controller_t *hc = ehci_dev_controller(dev);
    if (!hc->ready) return -1;

    ehci_qh_t *qh = NULL;
    ehci_qtd_t *qtd = NULL;
    if (ehci_intr_alloc(hc, dev, slot, ep_addr, &qh, &qtd) < 0) return -1;

    /* 缓冲区上限：控制器私有 64 字节 scratch（DMA 目标）与调用方缓冲取小，
     * 保证硬件永远不会越过两者中较小的那一个。 */
    uint32_t bytes = len < EHCI_INTR_SCRATCH_BYTES ? len : EHCI_INTR_SCRATCH_BYTES;
    uint8_t *scratch = hc->intr_scratch;

    /* period 必须落在 frame list 的合法范围内（Linux ehci-q.c 把
     * interval 钳到 periodic_size）。 */
    uint32_t period = 1;
    while (period < EHCI_FRAME_LIST_ENTRIES && period < interval) period <<= 1;
    if (period > EHCI_FRAME_LIST_ENTRIES) period = EHCI_FRAME_LIST_ENTRIES;

    g_ehci_port_busy = dev_idx;
    ehci_intr_arm(hc, qh, qtd, (uintptr_t)scratch, bytes);
    ehci_qh_link_periodic(hc, qh, period);

    int result = -1;
    if (ehci_enable_periodic(hc) == 0) {
        if (ehci_wait_qtd(qtd, EHCI_INTR_SAMPLE_TIMEOUT_MS) == 0) {
            if (!ehci_qtd_has_error(qtd)) {
                uint32_t residual = ehci_qtd_residual(qtd);
                if (residual > bytes) residual = bytes;
                uint32_t got = bytes - residual;
                if (got > 0) {
                    memcpy(data, scratch, got);
                    result = (int)got;
                }
            } else {
                console_puts("[EHCI] interrupt endpoint halted, token=0x");
                ehci_dbg_hex8((uint8_t)(qtd->hw_token >> 24));
                ehci_dbg_hex8((uint8_t)(qtd->hw_token >> 16));
                ehci_dbg_hex8((uint8_t)(qtd->hw_token >> 8));
                ehci_dbg_hex8((uint8_t)qtd->hw_token);
                console_puts("\n");
            }
        }
    }

    /* 摘链顺序（失败路径逆序释放）：先摘周期表，再关周期调度，最后清 qTD。 */
    ehci_qh_unlink_periodic(hc, qh);
    (void)ehci_disable_async(hc);
    ehci_qtd_chain_reset(qtd, 1);
    g_ehci_port_busy = -1;
    return result;
}

/* ══════════════════════════════════════════════════════════════
 * 描述符读取
 * ══════════════════════════════════════════════════════════════ */

/** @brief 读 18 字节设备描述符（不改变调用方缓冲的内容，除非成功） */
int usb_ehci_get_device_desc(int dev_idx, usb_device_desc_t *desc) {
    if (!desc) return -1;
    if (!ehci_dev_get(dev_idx)) return -1;

    uint8_t buf[18];
    int ret = usb_ehci_control_transfer(dev_idx, 0x80, 6, 0x0100, 0, buf,
                                        sizeof(buf));
    if (ret < 0 || ret < (int)sizeof(buf)) return -1;
    memcpy(desc, buf, sizeof(buf));
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * 端口（root hub）操作
 * ══════════════════════════════════════════════════════════════ */

/** @brief 有 PPC 的控制器需要软件给端口上电（Linux ehci-hub.c: ehci_port_power） */
static void ehci_port_power_on(ehci_controller_t *hc, uint32_t port) {
    if (!hc->has_ppc) return;
    uint32_t portsc = ehci_read_portsc(hc, port);
    if (portsc & EHCI_PORT_POWER) return;
    ehci_write_portsc(hc, port, ehci_portsc_to_neutral(portsc) | EHCI_PORT_POWER);
}

/**
 * @brief 结束一次端口复位并回读端口状态（Linux ehci-hub.c:944-961）
 *
 * ★ 关键点：**复位位不是等它自己清零，而是由发起者写 0 清掉**。
 * Linux 的注释写得很直白："whoever resets must GetPortStatus to complete it!!"，
 * 紧接着就是 `writel(temp & ~(PORT_RWC_BITS | PORT_RESET), status_reg)` 再
 * 握手等 `PORT_RESET == 0`。只轮询、不写 0 的话 PR 位会一直停在 1，表现为
 * "端口复位超时、一个设备都枚举不到"——这正是真机（与 QEMU）上 EHCI 不动
 * 的原因。
 *
 * 顺序不能换：
 *  1. 保持 RESET=1 至少 EHCI_PORT_RESET_HOLD_MS（USB 2.0 §7.1.7.5 要求
 *     10ms 以上；Linux 由 hub 层的定时器承担这段保持时间）；
 *  2. 清 PR（同时保留 RWC 位——它们写 1 才清，这里不能顺带清掉变化位）；
 *  3. 握手等 PR 归零，再等 EHCI_PORT_RESET_SETTLE_MS（≥50ms，规范要求
 *     复位结束后至少 50ms 才能可靠读端口状态）。
 *
 * @param out_portsc 输出复位结束后的 PORTSC；可为 NULL
 * @return 0 成功，-1 复位位始终不清（端口异常）
 */
static int ehci_port_reset_settle(ehci_controller_t *hc, uint32_t port,
                                  uint32_t *out_portsc) {
    hw_deadline_t deadline = hw_deadline_start();
    while (!hw_deadline_expired_ms(&deadline, EHCI_PORT_RESET_HOLD_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) {
        cpu_relax();
    }

    /* 第 1 步：软件清 PR。PORT_RWC_BITS 是写 1 才清的位，必须原样保留，
     * 否则会把 CSC 变化位一起吃掉。 */
    uint32_t portsc = ehci_read_portsc(hc, port);
    ehci_write_portsc(hc, port, (portsc & ~EHCI_PORT_RESET));

    /* 第 2 步：握手等 PR 归零。少数硬件要 550us 以上，给足 1s。 */
    deadline = hw_deadline_start();
    for (;;) {
        portsc = ehci_read_portsc(hc, port);
        if (!(portsc & EHCI_PORT_RESET)) break;
        if (hw_deadline_expired_ms(&deadline, EHCI_PORT_RESET_TIMEOUT_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }

    /* 第 3 步：复位后稳定窗口。 */
    deadline = hw_deadline_start();
    while (!hw_deadline_expired_ms(&deadline, EHCI_PORT_RESET_SETTLE_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) {
        cpu_relax();
    }

    portsc = ehci_read_portsc(hc, port);
    if (out_portsc) *out_portsc = portsc;
    return 0;
}

/**
 * @brief 复位一个 root port，并把"复位后仍未使能"的端口交给 companion
 *
 * 流程照抄 Linux：`ehci-hub.c` 的 SetPortFeature(RESET) 分支 + 50ms 等待，
 * 再由 `ehci-hub.c: check_reset_complete()` 判定结果。
 *
 * **低速/全速端口的取舍**：EHCI（root hub 无集成 TT 时）根本不能直接驱动
 * 低速设备；USB 2.0 规范要求把它们交给 companion controller（UHCI/OHCI）。
 * Linux 的做法是：
 *   - 复位**前**若发现 `LS == 01b`（USB 1.1 设备）就直接置 `PORT_OWNER`
 *     （ehci-hub.c:1100 "port %d low speed --> companion"），根本不启动复位；
 *   - 复位**后**若 `PORT_PE` 仍为 0（全速设备复位后不会使能高速端口）也置
 *     `PORT_OWNER`（ehci-hub.c:585 "port %d full speed --> companion"）。
 * 本驱动完全照做：释放而不是硬撑。结果是"低速/全速键鼠由 companion 驱动
 * 负责"，而这正是真机上的实际情况（早期机器的键鼠多半挂在 UHCI/OHCI 上，
 * 由 BIOS 或后续 companion 驱动处理）。取舍的代价是：本驱动只枚举出高速
 * 设备；若某台机器没有 companion 驱动，低速设备就仍然不可用。
 *
 * @return 0 高速设备且端口已使能；-1 端口无效/复位失败/已交给 companion
 */
static int ehci_port_reset(ehci_controller_t *hc, uint32_t port) {
    uint32_t portsc = ehci_read_portsc(hc, port);
    if (!(portsc & EHCI_PORT_CONNECT)) return -1;

    /* 复位前就是 USB 1.1 设备（LS 位报告 SE0/K/J 状态）→ 直接交给 companion，
     * 不要对它发起复位：复位一个不属于自己的端口会把 companion 的总线搞乱。 */
    if ((portsc & EHCI_PORT_LS_MASK) == EHCI_PORT_LS_USB11) {
        console_puts("[EHCI] port ");
        ehci_dbg_uint(port);
        console_puts(" low/full speed, releasing to companion\n");
        ehci_write_portsc(hc, port, ehci_portsc_to_neutral(portsc) |
                                   EHCI_PORT_OWNER);
        return -1;
    }

    /* 启动复位：清 PE、把 CSC/PEC/OCC 这些 RW1C 位保持原值（写 1 才清），
     * 只额外置 PORT_RESET。 */
    ehci_write_portsc(hc, port, ehci_portsc_to_neutral(portsc) |
                               EHCI_PORT_RESET);

    uint32_t after = 0;
    if (ehci_port_reset_settle(hc, port, &after) < 0) {
        console_puts("[EHCI] port reset did not complete\n");
        return -1;
    }

    if (!(after & EHCI_PORT_PE)) {
        /* 复位完成仍未使能 → 不是高速设备，交给 companion（含低全速两种情况：
         * 上面 LS 位可能因为"复位前设备还没稳定"而漏判）。 */
        console_puts("[EHCI] port ");
        ehci_dbg_uint(port);
        console_puts(" not high speed after reset, releasing to companion\n");
        ehci_write_portsc(hc, port, ehci_portsc_to_neutral(after) |
                                   EHCI_PORT_OWNER);
        return -1;
    }

    /* 顺利使能：清变化位 + 清掉可能残留的 SUSPEND/RESUME。 */
    ehci_write_portsc(hc, port,
                      ehci_portsc_to_neutral(after) | EHCI_PORT_RWC_BITS);
    return 0;
}

/** @brief 读端口变化位并清除（清完再读一次，避免丢事件） */
static uint32_t ehci_port_take_changes(ehci_controller_t *hc, uint32_t port) {
    uint32_t portsc = ehci_read_portsc(hc, port);
    uint32_t changes = portsc & EHCI_PORT_CHANGE_BITS;
    if (changes) {
        ehci_write_portsc(hc, port, ehci_portsc_to_neutral(portsc) | changes);
    }
    return changes;
}

/* ══════════════════════════════════════════════════════════════
 * 设备寻址
 * ══════════════════════════════════════════════════════════════ */

/**
 * @brief 对一个刚复位成功的高速设备执行 SET_ADDRESS
 *
 * 步骤（与 Linux usb core 的枚举顺序一致）：
 *   1. 在地址 0、EP0 maxpacket=64（高速固定值）下发 SET_ADDRESS，
 *      此时设备进入 Default 状态；
 *   2. 立刻把 QH 的地址域改成新地址，后续所有传输直接寻址新地址；
 *   3. 发 GET_DESCRIPTOR(DEVICE) 取真实的 bMaxPacketSize0 并回填 QH。
 *
 * 失败路径：设备记录保持 in_use=0（由调用方释放），QH 已经被挂过异步调度
 * 的会由 `usb_ehci_control_transfer()` 自己摘掉。
 *
 * @return 0 成功，-1 失败
 */
static int ehci_device_address(ehci_controller_t *hc, int dev_idx,
                               uint8_t port, uint8_t new_addr) {
    ehci_device_t *dev = &g_ehci_dev[dev_idx];

    int ret = usb_ehci_control_transfer(dev_idx, 0x00, 5, new_addr, 0, NULL, 0);
    if (ret < 0) {
        console_puts("[EHCI] SET_ADDRESS failed\n");
        return -1;
    }
    ehci_dev_set_address(dev, new_addr);
    (void)hc;

    /* USB 2.0 §9.2.6.3：SetAddress 之后设备有 2ms 的恢复窗口。 */
    hw_deadline_t deadline = hw_deadline_start();
    while (!hw_deadline_expired_ms(&deadline, EHCI_SET_ADDRESS_RECOVER_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) {
        cpu_relax();
    }

    /* 读设备描述符拿真实 EP0 包长；失败不致命（保留 64 继续）。 */
    uint8_t buf[18];
    ret = usb_ehci_control_transfer(dev_idx, 0x80, 6, 0x0100, 0, buf,
                                    sizeof(buf));
    if (ret >= 8 && buf[7]) {
        dev->max_packet0 = buf[7];
        uint32_t info1 = dev->qh_control->hw.hw_info1 & ~(0xFFU << 16);
        dev->qh_control->hw.hw_info1 = info1 | ((uint32_t)buf[7] << 16);
    }
    (void)port;
    return 0;
}

/**
 * @brief 扫描一个控制器的所有 root port，复位并寻址其上的高速设备
 *
 * @return 本控制器新寻址成功的设备数
 */
static int ehci_scan_ports(ehci_controller_t *hc, int *next_addr) {
    int found = 0;

    for (uint32_t port = 1; port <= hc->n_ports; port++) {
        ehci_port_power_on(hc, port);
    }

    /* 上电后给端口一点稳定时间（Linux ehci-hcd.c:733 在 ehci_run 之后
     * 干脆 `msleep` 等端口稳定）。这里用一个短窗口，不阻塞启动太久。 */
    hw_deadline_t settle = hw_deadline_start();
    while (!hw_deadline_expired_ms(&settle, EHCI_PORT_POWER_TIMEOUT_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) {
        cpu_relax();
    }

    for (uint32_t port = 1; port <= hc->n_ports; port++) {
        uint32_t portsc = ehci_read_portsc(hc, port);
        if (!(portsc & EHCI_PORT_CONNECT)) continue;
        if (portsc & EHCI_PORT_OWNER) continue; /* 已属于 companion */

        int dev_idx = ehci_dev_alloc();
        if (dev_idx < 0) {
            console_puts("[EHCI] device table full\n");
            break;
        }

        /* 先建立 QH/qTD（此时地址域还是 0），QH 初始化需要速度参数，
         * 而速度要等复位完成才知道 —— 所以先按高速准备，复位后再校正。 */
        ehci_device_t *dev = &g_ehci_dev[dev_idx];
        memset(dev, 0, sizeof(*dev));
        dev->controller = (uint8_t)(hc - g_ehci);
        dev->port = (uint8_t)port;

        dev->qh_control = (ehci_qh_t *)kmalloc_aligned(sizeof(ehci_qh_t), 32);
        if (!dev->qh_control) break;
        memset(dev->qh_control, 0, sizeof(ehci_qh_t));

        dev->qtd = (ehci_qtd_t *)kmalloc_aligned(
            sizeof(ehci_qtd_t) * EHCI_QTD_PER_DEVICE, 32);
        if (!dev->qtd) {
            kfree(dev->qh_control);
            dev->qh_control = NULL;
            break;
        }
        memset(dev->qtd, 0, sizeof(ehci_qtd_t) * EHCI_QTD_PER_DEVICE);
        dev->qtd_count = (int)EHCI_QTD_PER_DEVICE;
        ehci_qtd_chain_reset(dev->qtd, dev->qtd_count);
        dev->in_use = 1; /* 允许 control_transfer 通过 ehci_dev_get() 校验 */

        if (ehci_port_reset(hc, port) < 0) {
            ehci_dev_release(dev_idx);
            continue;
        }

        dev->speed = EHCI_SPEED_HIGH;
        dev->max_packet0 = (uint8_t)EHCI_CONTROL_MAX_PACKET;
        ehci_qh_init_control(dev->qh_control, &dev->qtd[EHCI_QTD_PER_DEVICE - 1U],
                            0, dev->speed, dev->max_packet0);

        if (ehci_device_address(hc, dev_idx, (uint8_t)port,
                                (uint8_t)(*next_addr)) < 0) {
            ehci_dev_release(dev_idx);
            continue;
        }

        console_puts("[EHCI] port ");
        ehci_dbg_uint(port);
        console_puts(" device addressed, addr=");
        ehci_dbg_uint(dev->addr);
        console_puts(" maxp=");
        ehci_dbg_uint(dev->max_packet0);
        console_puts("\n");

        hc->port_addressed[port] = 1;
        (*next_addr)++;
        if (*next_addr > 127) *next_addr = 1;
        found++;
    }
    return found;
}

/* ══════════════════════════════════════════════════════════════
 * 控制器初始化
 * ══════════════════════════════════════════════════════════════ */

/** @brief BIOS/OS 所有权交接：在 PCI 配置空间的 extended capability 链里找 USBLEGSUP */
static void ehci_bios_handoff(const pci_device_t *pci, uint32_t hcc_params) {
    uint8_t offset = (uint8_t)EHCI_HCC_EECP(hcc_params);
    int hops = 0;

    while (offset && hops++ < 64) {
        uint32_t cap = pci_read32(pci->bus, pci->slot, pci->func, offset);
        uint8_t id = (uint8_t)(cap & 0xFFU);

        if (id == EHCI_EXT_CAP_USBLEGSUP) {
            console_puts("[EHCI] USBLEGSUP found at 0x");
            ehci_dbg_hex8(offset);
            console_puts("\n");

            if (cap & EHCI_LEGSUP_BIOS_OWNED) {
                console_puts("[EHCI] BIOS owned, requesting handover\n");
                /* 写 cap+3 的一个字节，通知 BIOS 我们要接管
                 * （pci-quirks.c:904 的 pci_write_config_byte(offset+3, 1)）。 */
                pci_write32(pci->bus, pci->slot, pci->func,
                            (uint8_t)(offset + EHCI_LEGSUP_BIOS_SEM_OFF),
                            cap | EHCI_LEGSUP_OS_OWNED);
                /* 同时置 OS Owned 位（cap+2 字节位置更精确，但整字写回也
                 * 只影响我们关心的两个信号量位）。 */
                pci_write32(pci->bus, pci->slot, pci->func, offset,
                            cap | EHCI_LEGSUP_OS_OWNED);

                hw_deadline_t deadline = hw_deadline_start();
                for (;;) {
                    cap = pci_read32(pci->bus, pci->slot, pci->func, offset);
                    if (!(cap & EHCI_LEGSUP_BIOS_OWNED)) break;
                    if (hw_deadline_expired_ms(&deadline,
                                               EHCI_HANDOVER_TIMEOUT_MS,
                                               EHCI_INIT_MAX_STALL_SPINS)) {
                        console_puts("[EHCI] handover timeout, forcing\n");
                        break;
                    }
                }
                if (cap & EHCI_LEGSUP_BIOS_OWNED) {
                    /* BIOS 赖着不放（已知的坏 BIOS）：Linux 会强清 BIOS Owned
                     * 再希望没事（pci-quirks.c:923）。照做。 */
                    pci_write32(pci->bus, pci->slot, pci->func,
                                (uint8_t)(offset + EHCI_LEGSUP_OS_SEM_OFF), 0);
                } else {
                    console_puts("[EHCI] handover OK\n");
                }
            } else {
                console_puts("[EHCI] not BIOS owned\n");
            }

            /* 无论交接是否成功都关掉 EHCI 的 SMI（pci-quirks.c:930）。 */
            pci_write32(pci->bus, pci->slot, pci->func,
                        (uint8_t)(offset + EHCI_LEGSUP_CTLSTS_OFF), 0);
            break;
        }
        if (id == 0) break; /* 保留 id：链到此为止 */
        uint8_t next = (uint8_t)((cap >> 8) & 0xFFU);
        if (!next) break;
        offset = (uint8_t)(offset + next);
    }
}

/** @brief 控制器复位：HCRESET 置位后轮询自清（Linux ehci-hcd.c: ehci_reset()） */
static int ehci_reset(ehci_controller_t *hc) {
    uint32_t cmd = ehci_op_read32(hc, EHCI_OP_USBCMD);
    ehci_op_write32(hc, EHCI_OP_USBCMD, cmd | EHCI_CMD_HCRESET);

    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        if (!(ehci_op_read32(hc, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) break;
        if (hw_deadline_expired_ms(&deadline, EHCI_RESET_TIMEOUT_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) {
            console_puts("[EHCI] HCRESET did not self-clear\n");
            return -1;
        }
        cpu_relax();
    }
    return 0;
}

/** @brief 建立周期表与异步锚点（必须在复位之后，寄存器语义才有效） */
static int ehci_setup_schedules(ehci_controller_t *hc) {
    hc->frame_list = (uint32_t *)kmalloc_aligned(
        EHCI_FRAME_LIST_ENTRIES * sizeof(uint32_t), 4096);
    if (!hc->frame_list) {
        console_puts("[EHCI] frame list allocation failed\n");
        return -1;
    }
    hc->frame_list_phys = (uint64_t)(uintptr_t)hc->frame_list;

    /* 锚点 QH（Linux ehci-hcd.c:544）：自环 + QH_HEAD + token=HALT，
     * 保证异步环形链表永远闭合，且空闲时不会去执行任何 qTD。 */
    memset(&hc->async_head, 0, sizeof(hc->async_head));
    memset(&hc->async_dummy, 0, sizeof(hc->async_dummy));
    hc->async_head.hw.hw_next = ehci_qh_next(&hc->async_head);
    hc->async_head.hw.hw_info1 = QH_INFO1_HEAD;
    hc->async_head.hw.hw_token = QTD_STS_HALT;
    hc->async_head.hw.hw_qtd_next = EHCI_LIST_END;
    hc->async_head.hw.hw_alt_next = (uint32_t)(uintptr_t)&hc->async_dummy;
    hc->async_head.hw.hw_current = (uint32_t)(uintptr_t)&hc->async_dummy;
    hc->async_dummy.hw_token = QTD_STS_HALT;
    hc->async_dummy.hw_next = EHCI_LIST_END;

    /* 空周期表：每项都指向"结束"，PSE 打开后控制器不会执行任何事务。 */
    for (uint32_t i = 0; i < EHCI_FRAME_LIST_ENTRIES; i++) {
        hc->frame_list[i] = EHCI_LIST_END;
    }

    ehci_op_write32(hc, EHCI_OP_PERIODICBASE,
                    (uint32_t)(hc->frame_list_phys & 0xFFFFFFF0U));
    ehci_op_write32(hc, EHCI_OP_ASYNCLISTADDR,
                    (uint32_t)(uintptr_t)&hc->async_head.hw);
    return 0;
}

/**
 * @brief 让控制器进入运行态（Linux ehci-hcd.c: ehci_run()）
 *
 * 顺序：先放好 frame list / async list 基址，再置 CMD_RUN，最后置
 * CONFIGFLAG=1 把端口从 companion 抢过来（"Configure Flag"）。CF 之后必须
 * 给硬件几毫秒完成端口切换（Linux 在这里 `msleep(5)`）。
 */
static int ehci_run(ehci_controller_t *hc) {
    if (hc->has_64bit) {
        ehci_op_write32(hc, EHCI_OP_CTRLDSSEG, 0);
    }

    uint32_t cmd = ehci_op_read32(hc, EHCI_OP_USBCMD);
    cmd &= ~(EHCI_CMD_HCRESET | EHCI_CMD_IAAD | EHCI_CMD_PSE | EHCI_CMD_ASE);
    cmd |= EHCI_CMD_RUN;
    ehci_op_write32(hc, EHCI_OP_USBCMD, cmd);

    ehci_op_write32(hc, EHCI_OP_CONFIGFLAG, 1U);

    /* 读一次 USBCMD 把 posted write 冲下去，再等 5ms 让端口切换完成。 */
    (void)ehci_op_read32(hc, EHCI_OP_USBCMD);
    hw_deadline_t deadline = hw_deadline_start();
    while (!hw_deadline_expired_ms(&deadline, 5U, EHCI_INIT_MAX_STALL_SPINS)) {
        cpu_relax();
    }

    deadline = hw_deadline_start();
    while (ehci_op_read32(hc, EHCI_OP_USBSTS) & EHCI_STS_HALT) {
        if (hw_deadline_expired_ms(&deadline, EHCI_START_TIMEOUT_MS,
                                   EHCI_INIT_MAX_STALL_SPINS)) {
            console_puts("[EHCI] controller refused to start\n");
            return -1;
        }
        cpu_relax();
    }

    /* 纯轮询：明确关掉中断使能，避免控制器发 SMI/IRQ（Linux 在这里打开中断）。 */
    ehci_op_write32(hc, EHCI_OP_USBINTR, 0);
    return 0;
}

/**
 * @brief 初始化单个 EHCI 控制器
 *
 * 失败路径：调用方负责递增失败计数；本函数在失败前只写本控制器的状态，
 * 不碰全局设备表（设备是在 `ehci_scan_ports()` 里才建立的）。
 */
static int ehci_init_one(const pci_device_t *pci) {
    if (g_ehci_controller_count >= (int)EHCI_MAX_CONTROLLERS) return -1;

    int is_io = 0;
    uint64_t bar = pci_bar_address(pci->bus, pci->slot, pci->func, 0, &is_io);
    if (!bar || is_io) {
        console_puts("[EHCI] BAR0 invalid\n");
        return -1;
    }

    ehci_controller_t *hc = &g_ehci[g_ehci_controller_count];
    memset(hc, 0, sizeof(*hc));
    hc->mmio_phys = bar;
    hc->pci_bus = pci->bus;
    hc->pci_slot = pci->slot;
    hc->pci_func = pci->func;

    console_puts("[EHCI] controller at ");
    ehci_dbg_bdf(pci->bus, pci->slot, pci->func);
    console_puts(" BAR0=0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        ehci_dbg_hex8((uint8_t)((bar >> shift) & 0xFU));
    }
    console_puts("\n");

    pci_enable_bus_master_mmio(pci);

    hc->mmio = (volatile uint8_t *)vmm_map_mmio(bar, EHCI_MMIO_MAP_SIZE);
    if (!hc->mmio) {
        console_puts("[EHCI] MMIO mapping failed\n");
        return -1;
    }

    uint32_t capbase = ehci_cap_read32(hc, EHCI_CAP_CAPBASE);
    hc->caplength = capbase & 0xFFU;
    if (hc->caplength < 0x10U || hc->caplength > 0x80U) {
        console_puts("[EHCI] bogus CAPLENGTH\n");
        return -1;
    }
    hc->hcs_params = ehci_cap_read32(hc, EHCI_CAP_HCSPARAMS);
    hc->hcc_params = ehci_cap_read32(hc, EHCI_CAP_HCCPARAMS);

    hc->n_ports = EHCI_HCS_N_PORTS(hc->hcs_params);
    if (!hc->n_ports || hc->n_ports > EHCI_MAX_PORTS) {
        console_puts("[EHCI] bogus port count\n");
        return -1;
    }
    /* PORTSC 数组必须在映射窗口内：CAPLENGTH + 0x44 + n_ports*4。 */
    if (hc->caplength + EHCI_OP_PORTSC + hc->n_ports * 4U > EHCI_MMIO_MAP_SIZE) {
        console_puts("[EHCI] PORTSC array outside mapped BAR\n");
        return -1;
    }
    hc->portsc_base = (volatile uint32_t *)(hc->mmio + hc->caplength +
                                            EHCI_OP_PORTSC);
    hc->has_ppc = (uint8_t)(EHCI_HCS_PPC(hc->hcs_params) ? 1 : 0);
    hc->has_64bit = (uint8_t)(EHCI_HCC_64BIT(hc->hcc_params) ? 1 : 0);

    console_puts("[EHCI] cap ver 0x");
    ehci_dbg_hex8((uint8_t)((capbase >> 24) & 0xFFU));
    ehci_dbg_hex8((uint8_t)((capbase >> 16) & 0xFFU));
    console_puts(" ports=");
    ehci_dbg_uint(hc->n_ports);
    console_puts(" cc=");
    ehci_dbg_uint(EHCI_HCS_N_CC(hc->hcs_params));
    console_puts(" ppc=");
    ehci_dbg_uint(hc->has_ppc);
    console_puts("\n");

    /* BIOS 交接必须在复位之前：BIOS 可能正握着控制器做 USB legacy 仿真。 */
    ehci_bios_handoff(pci, hc->hcc_params);

    if (ehci_reset(hc) < 0) return -1;
    if (ehci_setup_schedules(hc) < 0) return -1;
    if (ehci_run(hc) < 0) return -1;

    hc->ready = 1;
    g_ehci_controller_count++;

    int next_addr = 1;
    int found = ehci_scan_ports(hc, &next_addr);
    console_puts("[EHCI] controller ready, devices found=");
    ehci_dbg_uint((uint32_t)found);
    console_puts("\n");

    if (found == 0) {
        /* 空闲控制器：推进到"无事务"状态，省掉每帧的 NAK 轮询。 */
        (void)ehci_disable_async(hc);
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * 对外接口
 * ══════════════════════════════════════════════════════════════ */

/** @brief pci_enumerate_all() 回调：收下所有 EHCI 控制器 */
static void ehci_pci_probe_cb(const pci_device_t *dev, void *ctx) {
    int *found = (int *)ctx;
    if (dev->class_code != EHCI_PCI_CLASS ||
        dev->subclass != EHCI_PCI_SUBCLASS ||
        dev->prog_if != EHCI_PCI_PROGIF) {
        return;
    }
    if (ehci_init_one(dev) == 0) {
        (*found)++;
    } else {
        /* 已占用的控制器槽位必须回退，否则下一个控制器会被跳过。 */
        /* ehci_init_one() 只在最后一步才递增 g_ehci_controller_count，
         * 失败时槽位未被占用，这里不需要额外处理。 */
        console_puts("[EHCI] controller init failed, skipping\n");
    }
}

/**
 * @brief 探测并初始化所有 EHCI 控制器，见 usb_ehci.h 契约
 *
 * 幂等：状态机只在第一次调用时真正探测，其余调用直接返回缓存结果
 * （与 xhci_init() 相同的处理）。
 */
int usb_ehci_init(void) {
    if (g_ehci_state != EHCI_STATE_UNINITIALIZED) {
        return g_ehci_state == EHCI_STATE_READY ? 0 : -1;
    }

    console_puts("[EHCI] scanning PCI for EHCI controllers\n");
    int found = 0;
    pci_enumerate_all(ehci_pci_probe_cb, &found);

    /* 逐个把设备表里已建立记录的设备算总数（控制器在扫描时建立的设备都
     * 已经算过一次，这里统一重算，避免扫描过程中的中间状态泄漏出去）。 */
    g_ehci_device_count = 0;
    for (int i = 0; i < (int)EHCI_MAX_DEVICES; i++) {
        if (g_ehci_dev[i].in_use) g_ehci_device_count++;
    }

    if (found == 0) {
        console_puts("[EHCI] no controller found\n");
        g_ehci_state = EHCI_STATE_NONE;
        return -1;
    }
    if (g_ehci_device_count == 0) {
        /* 有控制器但没有可用高速设备：注册成功、init 失败，符合契约
         * （"至少一个控制器就绪且 EP0 可用"才算 0）。 */
        console_puts("[EHCI] controller(s) up but no device addressed\n");
        g_ehci_state = EHCI_STATE_FAILED;
        return -1;
    }

    console_puts("[EHCI] init done, devices=");
    ehci_dbg_uint((uint32_t)g_ehci_device_count);
    console_puts("\n");
    g_ehci_state = EHCI_STATE_READY;
    return 0;
}

/** @brief 已枚举设备数量，见 usb_ehci.h 契约 */
int usb_ehci_device_count(void) {
    return g_ehci_device_count;
}

/**
 * @brief 推进控制器事件处理，见 usb_ehci.h 契约
 *
 * 本驱动的完成判定完全在 `usb_ehci_control_transfer()` /
 * `usb_ehci_interrupt_transfer()` 内部通过轮询 qTD 的 Active 位同步完成，
 * 没有任何"已完成但尚未报告"的状态需要在这里回收，所以 poll 只做两件事：
 *   1. 清 USBSTS 的 RW1C 错误位（ERR/PCD/FLR/FATAL/INT），否则它们会一直
 *      累积；PCD 会在拔插设备时置位，属于正常事件，不需要额外动作；
 *   2. 若控制器被意外 halt（USBSTS.HALT=1），清一次 USBSTS 帮助它恢复
 *      （真正的重启留给下一次 `usb_ehci_init()`，因为复位会销毁所有 QH）。
 *
 * 之所以"可以近似为空"：`USBINTR` 恒为 0，没有中断需要确认；也没有异步
 * 回调要跑。保留这个函数是为了满足 ops 契约（`usb_host_poll_all()` 会调）。
 */
void usb_ehci_poll(void) {
    if (g_ehci_state != EHCI_STATE_READY) return;

    for (int i = 0; i < g_ehci_controller_count; i++) {
        ehci_controller_t *hc = &g_ehci[i];
        if (!hc->ready) continue;

        uint32_t status = ehci_op_read32(hc, EHCI_OP_USBSTS);
        if (status & EHCI_STS_W1C_CLEAR) {
            ehci_op_write32(hc, EHCI_OP_USBSTS, status & EHCI_STS_W1C_CLEAR);
        }
        if (status & EHCI_STS_HALT) {
            console_puts("[EHCI] controller halted, clearing status\n");
            ehci_op_write32(hc, EHCI_OP_USBSTS, EHCI_STS_W1C_CLEAR);
        }
        /* 顺手清掉端口变化位，防止下次扫描时把旧事件当成新插入。 */
        for (uint32_t port = 1; port <= hc->n_ports; port++) {
            (void)ehci_port_take_changes(hc, port);
        }
    }
}

/** @brief 本驱动的控制器接口表（静态常量，注册表只保存指针） */
static const usb_host_ops_t ehci_host_ops = {
    "EHCI",
    usb_ehci_init,
    usb_ehci_device_count,
    usb_ehci_get_device_desc,
    usb_ehci_control_transfer,
    usb_ehci_interrupt_transfer,
    usb_ehci_poll,
};

/** @brief 把 EHCI 驱动注册进 USB 控制器注册表，见 usb_ehci.h 契约 */
int usb_ehci_register(void) {
    return usb_host_register(&ehci_host_ops);
}
