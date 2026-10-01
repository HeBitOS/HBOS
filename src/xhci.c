#include "xhci.h"
#include "pci.h"
#include "core/pmm.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "core/io.h"
#include "core/task.h"
#include "core/wait.h"
#include "string.h"
#include "unistd.h"

#define XHCI_CAPLENGTH    0x00
#define XHCI_HCSPARAMS1   0x04
#define XHCI_HCSPARAMS2   0x08
#define XHCI_HCSPARAMS3   0x0C
#define XHCI_HCCPARAMS1   0x10
#define XHCI_DBOFF         0x14
#define XHCI_RTSOFF        0x18

#define XHCI_USBCMD        0x00
#define XHCI_USBSTS        0x04
#define XHCI_DNCTRL        0x14
#define XHCI_CRCR_LO       0x18
#define XHCI_CRCR_HI       0x1C
#define XHCI_DCBAAP_LO     0x30
#define XHCI_DCBAAP_HI     0x34
#define XHCI_CONFIG         0x38
#define XHCI_PAGESIZE       0x08

#define XHCI_CMD_RS         (1 << 0)
#define XHCI_CMD_HCRST      (1 << 1)
#define XHCI_CMD_INTE       (1 << 2)
#define XHCI_CMD_HSEE        (1 << 3)

#define XHCI_STS_HCH         (1 << 0)
#define XHCI_STS_HSE         (1 << 2)
#define XHCI_STS_EINT        (1 << 3)
#define XHCI_STS_PCD         (1 << 4)
#define XHCI_STS_CNR         (1 << 11)

#define XHCI_PORTSC_CCS      (1 << 0)
#define XHCI_PORTSC_PED      (1 << 1)
#define XHCI_PORTSC_OCA      (1 << 3)
#define XHCI_PORTSC_PR       (1 << 4)
#define XHCI_PORTSC_PLS      (0xF << 5)
#define XHCI_PORTSC_PP       (1 << 9)
#define XHCI_PORTSC_SPEED    (0xF << 10)
#define XHCI_PORTSC_PIC      (0x3 << 14)
#define XHCI_PORTSC_LWS      (1 << 16)
#define XHCI_PORTSC_CSC      (1 << 17)
#define XHCI_PORTSC_PEC      (1 << 18)
#define XHCI_PORTSC_WRC      (1 << 19)
#define XHCI_PORTSC_OCC      (1 << 20)
#define XHCI_PORTSC_PRC      (1 << 21)
#define XHCI_PORTSC_PLC      (1 << 22)
#define XHCI_PORTSC_CEC      (1 << 23)
#define XHCI_PORTSC_CAS      (1 << 24)
#define XHCI_PORTSC_WCE      (1 << 25)
#define XHCI_PORTSC_WDE      (1 << 26)
#define XHCI_PORTSC_WOE      (1 << 27)
#define XHCI_PORTSC_DR       (1 << 30)
#define XHCI_PORTSC_WPR      (1 << 31)

/*
 * PORTSC 混合了 RO、RWS、RW1S 和 RW1C 位。按 Linux xHCI 驱动的 neutral
 * 写法，只回写 RO 与“0 清/1 置”的 RWS 位，避免无意清除变化位或禁用端口。
 */
#define XHCI_PORTSC_RO       ((1U << 0) | (1U << 3) | \
                              (0xFU << 10) | (1U << 30))
#define XHCI_PORTSC_RWS      ((0xFU << 5) | (1U << 9) | \
                              (0x3U << 14) | (0x7U << 25))

#define XHCI_CONTROLLER_TIMEOUT_MS 1000U
#define XHCI_PORT_TIMEOUT_MS       1000U
#define XHCI_COMMAND_TIMEOUT_MS    1000U
#define XHCI_TRANSFER_TIMEOUT_MS   1000U
#define XHCI_HANDOVER_TIMEOUT_MS   1000U
#define XHCI_INIT_MAX_STALL_SPINS  10000000U
#define XHCI_IO_MAX_STALL_SPINS     5000000U
#define XHCI_MMIO_MAP_SIZE             0x10000U
#define XHCI_TRB_MAX_BUFFER             65536U

#define XHCI_STATE_UNINITIALIZED 0U
#define XHCI_STATE_READY         1U
#define XHCI_STATE_FAILED        2U

#define XHCI_COMP_SUCCESS       1U
#define XHCI_COMP_SHORT_PACKET 13U

#define TRB_NORMAL        1
#define TRB_SETUP_STAGE   2
#define TRB_DATA_STAGE    3
#define TRB_STATUS_STAGE  4
#define TRB_LINK          6
#define TRB_EVENT_DATA    7
#define TRB_NOOP          8
#define TRB_ENABLE_SLOT   9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV  11
#define TRB_CONFIGURE_EP 12
#define TRB_EVALUATE_CTX 13
#define TRB_RESET_EP     14
#define TRB_STOP_EP      15
#define TRB_SET_TR_DEQUEUE 16
#define TRB_RESET_DEV    17
#define TRB_FORCE_EVENT  18
#define TRB_NEGOTIATE_BW 19
#define TRB_SET_LATENCY  20
#define TRB_GET_PORT_BW  21
#define TRB_FORCE_HEADER 22
#define TRB_NOOP_CMD     23

#define TRB_EV_TRANSFER  32
#define TRB_EV_CMD_COMP  33
#define TRB_EV_PORT_SC   34
#define TRB_EV_BW_REQ    35
#define TRB_EV_DOORBELL  36
#define TRB_EV_HOST_CTRL 37
#define TRB_EV_DEV_NOTIFY 38
#define TRB_EV_MFINDEX   39

#define TRB_C   (1 << 0)
#define TRB_TC  (1 << 1)
#define TRB_CH  (1 << 2)
#define TRB_B   (1 << 4)
#define TRB_BSR (1 << 9)

#define TRB_TRT_NONE 0
#define TRB_TRT_IN   2
#define TRB_TRT_OUT  1

#define TRB_TRT_NO_DATA     0
#define TRB_TRT_RESERVED    1
#define TRB_TRT_OUT_DATA    2
#define TRB_TRT_IN_DATA     3

#define TRB_DIR_IN          (1 << 16)

#define TRB_IDT  (1 << 6)
#define TRB_IOC  (1 << 5)
#define TRB_ISP  (1 << 2)

#define TRB_SPD  (1 << 3)

#define EP_CTX_DISABLED  0
#define EP_CTX_CONTROL      4
#define EP_CTX_BULK_IN      6
#define EP_CTX_INTERRUPT_IN 7
#define EP_CTX_ISOCH     4

#define EP_STATE_DISABLED    0
#define EP_STATE_RUNNING     1
#define EP_STATE_HALTED      2
#define EP_STATE_STOPPED     3
#define EP_STATE_ERROR       4

#define SLOT_STATE_DISABLED  0
#define SLOT_STATE_DEFAULT   1
#define SLOT_STATE_ADDRESSED 2
#define SLOT_STATE_CONFIGURED 3

#define CTX_SIZE 32

#define PORTSC_OFFSET 0x400

/******************************************************************************/

/** @brief 控制器实例池：一台机器上可能有多个 xHCI（PCH 一个 + 板载 ASMedia 等） */
static xhci_t xhci_pool[XHCI_MAX_CONTROLLERS];
/**
 * @brief 当前操作上下文（控制器）
 *
 * 不变量：所有公开 API 在入口处用 xhci_select_controller()/xhci_select_dev()
 * 设置 hc，之后到返回为止不再切换。HBOS 的 USB 传输是同步、协作式调度的，
 * 同一时刻只有一个传输在进行，因此"环境指针"是安全的；**不要**在持有 hc
 * 的路径里递归调用另一个控制器的 API。
 */
static xhci_t *hc;
/** @brief 当前控制器的操作寄存器窗口（HCSPARAMS 之后的 operational registers） */
static volatile uint32_t *hc_op;

/**
 * @brief 全局设备表条目：设备属于哪个控制器实例、占用该实例的哪个 slot
 *
 * 设备下标（dev_idx）是跨控制器的全局编号：HID/存储层只认这个编号，由
 * xhci_select_dev() 换算成 (控制器, slot)。真机上一台机器可能有两个 xHCI
 * （PCH 一个 + 板载第三方一个），鼠标挂在第二个上时，只认第一个控制器的
 * 旧实现就完全看不到设备。
 */
typedef struct {
    uint8_t ctrl;  /**< 控制器在 xhci_pool 中的下标 */
    uint32_t slot; /**< 该控制器内的 slot id */
} xhci_dev_ref_t;

/** @brief 跨控制器的全局设备表 */
static xhci_dev_ref_t xhci_dev_refs[XHCI_MAX_DEVICES];
/** @brief 全局设备表中的有效条目数 */
static int xhci_dev_total;
/** @brief 已成功初始化的控制器数量 */
static int xhci_ctrl_count;

static uint32_t xhci_read32(uint32_t offset) {
    return hc->mmio[offset / 4];
}

static void xhci_write32(uint32_t offset, uint32_t value) {
    hc->mmio[offset / 4] = value;
}

static uint32_t xhci_op_read32(uint32_t offset) {
    return hc_op[offset / 4];
}

static void xhci_op_write32(uint32_t offset, uint32_t value) {
    hc_op[offset / 4] = value;
}

static uint32_t xhci_read_portsc(uint32_t port) {
    return hc_op[(PORTSC_OFFSET + (port - 1) * 16) / 4];
}

static void xhci_write_portsc(uint32_t port, uint32_t value) {
    hc_op[(PORTSC_OFFSET + (port - 1) * 16) / 4] = value;
}

static uint32_t xhci_port_state_to_neutral(uint32_t portsc) {
    return portsc & (XHCI_PORTSC_RO | XHCI_PORTSC_RWS);
}

static void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    if (hc->doorbell) hc->doorbell[slot] = target;
}

static uint64_t *xhci_alloc_aligned(size_t size, uint64_t *phys) {
    uint64_t p = (uint64_t)(uintptr_t)kmalloc_aligned(size, 64);
    if (!p) return 0;
    *phys = vmm_virt_to_phys(p);
    return (uint64_t *)p;
}

static void xhci_build_trb(xhci_trb_t *trb, uint64_t param, uint32_t status, uint32_t control) {
    trb->param0 = (uint32_t)(param & 0xFFFFFFFF);
    trb->param1 = (uint32_t)(param >> 32);
    trb->param2 = status;
    trb->param3 = control;
}

static int xhci_event_ready(const xhci_trb_t *evt) {
    return (evt->param3 & TRB_C) == hc->event_ring_cycle;
}

static void xhci_advance_event(void) {
    hc->event_ring_idx++;
    if (hc->event_ring_idx >= XHCI_EVENT_RING_SIZE) {
        hc->event_ring_idx = 0;
        hc->event_ring_cycle ^= 1;
    }
    if (hc->runtime) {
        uint64_t erdp = hc->event_ring_phys + hc->event_ring_idx * 16;
        hc->runtime[0x38 / 4] = (uint32_t)(erdp & 0xFFFFFFFF) | (1 << 3);
        hc->runtime[0x3C / 4] = (uint32_t)(erdp >> 32);
    }
}

static int xhci_post_cmd(uint32_t trb_type, uint64_t param, uint32_t status) {
    uint32_t idx = hc->cmd_ring_idx;
    xhci_trb_t *trb = &((xhci_trb_t *)hc->cmd_ring)[idx];

    /* Correct XHCI Command TRB specification layout:
     * Slot ID (and other control status parameters) belong to DWORD 3 (trb->param3),
     * not DWORD 2 (trb->param2). Setting DWORD 2 to zero and merging status into DWORD 3. */
    xhci_build_trb(trb, param, 0,
                   status | (trb_type << 10) | (hc->cmd_ring_cycle ? TRB_C : 0));

    hc->cmd_ring_idx++;
    if (hc->cmd_ring_idx >= XHCI_CMD_RING_SIZE) {
        hc->cmd_ring_idx = 0;
        hc->cmd_ring_cycle ^= 1;
    }

    xhci_ring_doorbell(0, 0);
    return 0;
}

static void xhci_post_transfer_trb(uint32_t slot_id, uint32_t ep_id,
                                    uint64_t param, uint32_t status, uint32_t control);
static xhci_trb_t *xhci_get_or_create_ep_ring(uint32_t slot_id, uint32_t ep_id,
                                              uint32_t ep_type, uint32_t max_packet_size,
                                              uint32_t interval, uint64_t *out_phys);

static int xhci_wait_cmd_resp(void);

static int xhci_send_cmd(uint32_t trb_type, uint64_t param, uint32_t status) {
    xhci_post_cmd(trb_type, param, status);
    return xhci_wait_cmd_resp();
}

extern void console_puts(const char *s);
extern void console_putchar(char c);
static void dbg_print_uint(uint32_t v);
static void dbg_print_hex16(uint16_t v);

static int xhci_wait_cmd_resp(void) {
    console_puts("[XHCI] Waiting for command response. Event ring idx: ");
    dbg_print_uint(hc->event_ring_idx);
    console_puts(", cycle: ");
    dbg_print_uint(hc->event_ring_cycle);
    console_puts("\n");

    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        uint32_t idx = hc->event_ring_idx;
        xhci_trb_t *evt = &((xhci_trb_t *)hc->event_ring)[idx];
        
        static uint32_t last_p3 = 0xFFFFFFFF;
        if (evt->param3 != last_p3) {
            last_p3 = evt->param3;
            console_puts("[XHCI] Event TRB changed: param3=");
            dbg_print_hex16(evt->param3 >> 16);
            dbg_print_hex16(evt->param3 & 0xFFFF);
            console_puts(", param2=");
            dbg_print_hex16(evt->param2 >> 16);
            dbg_print_hex16(evt->param2 & 0xFFFF);
            console_puts("\n");
        }

        if (!xhci_event_ready(evt)) {
            if (hw_deadline_expired_ms(&deadline, XHCI_COMMAND_TIMEOUT_MS,
                                       XHCI_IO_MAX_STALL_SPINS)) break;
            cpu_relax();
            continue;
        }

        uint32_t evt_type = (evt->param3 >> 10) & 0x3F;
        console_puts("[XHCI] Event ready! Type: ");
        dbg_print_uint(evt_type);
        console_puts("\n");

        if (evt_type == TRB_EV_CMD_COMP) {
            int code = (evt->param2 >> 24) & 0xFF;
            console_puts("[XHCI] Command completion code: ");
            dbg_print_uint(code);
            console_puts("\n");
            xhci_advance_event();
            return code == XHCI_COMP_SUCCESS ? 0 : -code;
        }

        xhci_advance_event();
    }
    console_puts("[XHCI] Command response timeout!\n");
    return -1;
}

static int xhci_reset_controller(void) {
    uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
    cmd &= ~XHCI_CMD_RS;
    xhci_op_write32(XHCI_USBCMD, cmd);

    hw_deadline_t deadline = hw_deadline_start();
    while (!(xhci_op_read32(XHCI_USBSTS) & XHCI_STS_HCH)) {
        if (hw_deadline_expired_ms(&deadline, XHCI_CONTROLLER_TIMEOUT_MS,
                                   XHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }

    xhci_op_write32(XHCI_USBCMD, XHCI_CMD_HCRST);
    deadline = hw_deadline_start();
    while (xhci_op_read32(XHCI_USBCMD) & XHCI_CMD_HCRST) {
        if (hw_deadline_expired_ms(&deadline, XHCI_CONTROLLER_TIMEOUT_MS,
                                   XHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }

    deadline = hw_deadline_start();
    while (xhci_op_read32(XHCI_USBSTS) & XHCI_STS_CNR) {
        if (hw_deadline_expired_ms(&deadline, XHCI_CONTROLLER_TIMEOUT_MS,
                                   XHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
    return 0;
}

static int xhci_init_rings(void) {
    hc->cmd_ring = (uint64_t *)xhci_alloc_aligned(
        XHCI_CMD_RING_SIZE * 16, &hc->cmd_ring_phys);
    if (!hc->cmd_ring) return -1;
    memset(hc->cmd_ring, 0, XHCI_CMD_RING_SIZE * 16);
    hc->cmd_ring_idx = 0;
    hc->cmd_ring_cycle = 1;

    uint64_t event_ring_seg_phys = 0;
    hc->event_ring_seg = (uint64_t *)xhci_alloc_aligned(16, &event_ring_seg_phys);
    if (!hc->event_ring_seg) return -1;

    hc->event_ring = (uint64_t *)xhci_alloc_aligned(
        XHCI_EVENT_RING_SIZE * 16, &hc->event_ring_phys);
    if (!hc->event_ring) return -1;
    memset(hc->event_ring, 0, XHCI_EVENT_RING_SIZE * 16);
    hc->event_ring_idx = 0;
    hc->event_ring_cycle = 1;

    hc->event_ring_seg[0] = hc->event_ring_phys;
    hc->event_ring_seg[1] = XHCI_EVENT_RING_SIZE;

    xhci_op_write32(XHCI_CRCR_LO, (uint32_t)(hc->cmd_ring_phys & 0xFFFFFFFF) | TRB_C);
    xhci_op_write32(XHCI_CRCR_HI, (uint32_t)(hc->cmd_ring_phys >> 32));

    if (!hc->runtime) return -1;
    hc->runtime[0x28 / 4] = 1; /* ERSTSZ */
    hc->runtime[0x30 / 4] = (uint32_t)(event_ring_seg_phys & 0xFFFFFFFF);
    hc->runtime[0x34 / 4] = (uint32_t)(event_ring_seg_phys >> 32);
    hc->runtime[0x38 / 4] = (uint32_t)(hc->event_ring_phys & 0xFFFFFFFF);
    hc->runtime[0x3C / 4] = (uint32_t)(hc->event_ring_phys >> 32);

    return 0;
}

static int xhci_init_scratchpad(void) {
    hc->dcbaa = (uint64_t *)xhci_alloc_aligned(
        XHCI_MAX_SLOTS * 8 + 16, &hc->dcbaa_phys);
    if (!hc->dcbaa) return -1;
    memset(hc->dcbaa, 0, XHCI_MAX_SLOTS * 8 + 16);

    if (hc->max_scratchpad > XHCI_MAX_SCRATCHPAD_BUFFERS) return -1;
    if (hc->max_scratchpad > 0) {
        uint64_t scratchpad_array_phys = 0;
        hc->scratchpad_array = (uint64_t *)xhci_alloc_aligned(
            hc->max_scratchpad * 8, &scratchpad_array_phys);
        if (!hc->scratchpad_array) return -1;
        memset(hc->scratchpad_array, 0, hc->max_scratchpad * 8);
        for (uint32_t i = 0; i < hc->max_scratchpad; i++) {
            uint64_t scratchpad_phys = 0;
            hc->scratchpad_buffers[i] = (uint64_t *)xhci_alloc_aligned(
                4096, &scratchpad_phys);
            if (!hc->scratchpad_buffers[i]) return -1;
            memset(hc->scratchpad_buffers[i], 0, 4096);
            hc->scratchpad_array[i] = scratchpad_phys;
        }
        hc->dcbaa[0] = scratchpad_array_phys;
    }

    xhci_op_write32(XHCI_DCBAAP_LO, (uint32_t)(hc->dcbaa_phys & 0xFFFFFFFF));
    xhci_op_write32(XHCI_DCBAAP_HI, (uint32_t)(hc->dcbaa_phys >> 32));

    return 0;
}

static int xhci_start_controller(void) {
    uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
    cmd |= XHCI_CMD_RS;
    xhci_op_write32(XHCI_USBCMD, cmd);

    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        if (!(xhci_op_read32(XHCI_USBSTS) & XHCI_STS_HCH)) return 0;
        if (hw_deadline_expired_ms(&deadline, XHCI_CONTROLLER_TIMEOUT_MS,
                                   XHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

static int xhci_reset_port(uint32_t port) {
    uint32_t portsc = xhci_read_portsc(port);
    xhci_write_portsc(port, xhci_port_state_to_neutral(portsc) |
                      XHCI_PORTSC_PR);

    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        portsc = xhci_read_portsc(port);
        if (!(portsc & XHCI_PORTSC_PR)) break;
        if (hw_deadline_expired_ms(&deadline, XHCI_PORT_TIMEOUT_MS,
                                   XHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }

    deadline = hw_deadline_start();
    for (;;) {
        portsc = xhci_read_portsc(port);
        if (portsc & XHCI_PORTSC_PED) return 0;
        if (hw_deadline_expired_ms(&deadline, XHCI_PORT_TIMEOUT_MS,
                                   XHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

static int xhci_setup_device_context(uint32_t slot_id) {
    uint64_t ctx_phys = 0;
    uint64_t *ctx = (uint64_t *)xhci_alloc_aligned(CTX_SIZE * 32, &ctx_phys);
    if (!ctx) return -1;
    memset(ctx, 0, CTX_SIZE * 32);
    hc->device_context_base[slot_id] = ctx;
    hc->dcbaa[slot_id] = ctx_phys;
    return 0;
}

static int xhci_enable_slot(uint32_t *slot_id) {
    int ret = xhci_send_cmd(TRB_ENABLE_SLOT, 0, 0);
    if (ret < 0) return ret;
    for (uint32_t i = 1; i <= hc->max_slots; i++) {
        if (!hc->slot_enabled[i]) {
            hc->slot_enabled[i] = 1;
            *slot_id = i;
            if (xhci_setup_device_context(i) < 0) return -1;
            return 0;
        }
    }
    return -1;
}

static uint64_t *xhci_alloc_transfer_ring(uint64_t *ring_phys);

/**
 * @brief 给一个设备分配地址（Address Device）
 *
 * @param slot_id 已 enable 的 slot
 * @param port    根端口号（1 起算），写入 slot context 的 ROOT_HUB_PORT
 * @param speed   设备速度（PORTSC 的 speed 域）
 * @param tt_slot 父 hub 的 slot id；0 表示设备直连根端口（不需要 TT）
 * @param tt_port 父 hub 上的下行端口号；仅 tt_slot != 0 时有意义
 *
 * 关键点（真机上"鼠标插在 hub/扩展坞后面就不认"的根因）：设备挂在 **USB 2.0
 * hub** 后面时，xHC 需要知道 split 事务要发给哪个 hub 的哪个端口，也就是
 * slot context 的 `tt_info`：
 *    bits 0-7  = TT hub 的 slot id
 *    bits 8-15 = TT hub 上的端口号
 * 不填的话 xHC 会把全速/低速设备的事务直接发到根端口，设备永远收不到，表现
 * 就是控制传输超时、枚举失败。参考 Linux `xhci-mem.c:
 * xhci_setup_addressable_virt_dev()` 的 `slot_ctx->tt_info`。
 */
static int xhci_address_device(uint32_t slot_id, uint32_t port, uint32_t speed,
                               uint32_t tt_slot, uint32_t tt_port) {
    uint64_t input_phys = 0;
    uint64_t *input_ctx = (uint64_t *)xhci_alloc_aligned(CTX_SIZE * 32, &input_phys);
    if (!input_ctx) return -1;
    memset(input_ctx, 0, CTX_SIZE * 32);

    uint64_t ep0_ring_phys = 0;
    uint64_t *ep0_ring = xhci_alloc_transfer_ring(&ep0_ring_phys);
    if (!ep0_ring) { kfree((void *)input_ctx); return -1; }
    memset(ep0_ring, 0, XHCI_TRB_RING_SIZE * 16);

    // Set up Link TRB at the end of ep0_ring (index XHCI_TRB_RING_SIZE - 1)
    xhci_trb_t *trb_ring = (xhci_trb_t *)ep0_ring;
    uint32_t link_ctrl = (TRB_LINK << 10) | TRB_TC;
    xhci_build_trb(&trb_ring[XHCI_TRB_RING_SIZE - 1], ep0_ring_phys, 0, link_ctrl);

    hc->input_context[slot_id] = ep0_ring;
    hc->ep_rings[slot_id][1] = ep0_ring;
    hc->ep_rings_phys[slot_id][1] = ep0_ring_phys;
    hc->ep_ring_idx[slot_id][1] = 0;
    hc->ep_ring_cycle[slot_id][1] = 1;
    hc->ep_configured[slot_id][1] = 1;

    uint32_t *ctx = (uint32_t *)input_ctx;
    ctx[1] = (1 << 0) | (1 << 1); /* Add slot + EP0 contexts */
    ctx[8] = (1 << 27) | (speed << 20);
    ctx[9] = (port << 16);
    /* Route string 为 0（设备在根端口下第一层或更深层由 tt_info 描述）。
     * hub 后面的设备：填 TT slot/port，让 xHC 生成 split 事务。 */
    if (tt_slot) {
        ctx[8 + 2] = (tt_slot & 0xFFU) | ((tt_port & 0xFFU) << 8);
    }

    uint32_t ep0 = 16;
    ctx[ep0 + 1] = (EP_CTX_CONTROL << 3) | (8 << 16) | (3 << 1);
    ctx[ep0 + 2] = (uint32_t)(ep0_ring_phys & 0xFFFFFFF0) | TRB_C;
    ctx[ep0 + 3] = (uint32_t)(ep0_ring_phys >> 32);

    int ret = xhci_send_cmd(TRB_ADDRESS_DEV, input_phys, (slot_id << 24));
    kfree((void *)input_ctx);
    return ret;
}

static int xhci_configure_endpoint(uint32_t slot_id, uint32_t ep_type,
                                   uint32_t ep_id, uint32_t max_packet_size,
                                   uint32_t interval, uint64_t ring_phys) {
    uint64_t input_phys = 0;
    uint64_t *input_ctx = (uint64_t *)xhci_alloc_aligned(CTX_SIZE * 32, &input_phys);
    if (!input_ctx) return -1;
    memset(input_ctx, 0, CTX_SIZE * 32);

    uint32_t *ctx = (uint32_t *)input_ctx;
    ctx[1] = (1 << 0) | (1 << ep_id); /* Add slot + target endpoint */
    uint32_t context_entries = ep_id;
    for (uint32_t i = ep_id + 1; i < XHCI_MAX_EP; i++) {
        if (hc->ep_configured[slot_id][i]) context_entries = i;
    }
    ctx[8] = context_entries << 27;

    uint32_t ep_off = 16 + (ep_id - 1) * 8;
    ctx[ep_off + 0] = (interval & 0xFF) << 16;
    ctx[ep_off + 1] = (ep_type << 3) | (max_packet_size << 16) | (3 << 1);
    ctx[ep_off + 2] = (uint32_t)(ring_phys & 0xFFFFFFF0) | TRB_C;
    ctx[ep_off + 3] = (uint32_t)(ring_phys >> 32);

    int ret = xhci_send_cmd(TRB_CONFIGURE_EP, input_phys, (slot_id << 24));
    kfree((void *)input_ctx);
    return ret;
}

static uint64_t *xhci_alloc_transfer_ring(uint64_t *ring_phys) {
    return xhci_alloc_aligned(XHCI_TRB_RING_SIZE * 16, ring_phys);
}

extern void console_puts(const char *s);
extern void console_putchar(char c);
static void dbg_print_uint(uint32_t v) {
    char buf[16];
    int n = 0;
    do { buf[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n--) console_putchar(buf[n]);
}
static void dbg_print_hex16(uint16_t v) {
    static const char hex[] = "0123456789ABCDEF";
    console_putchar(hex[(v >> 12) & 0xF]);
    console_putchar(hex[(v >> 8) & 0xF]);
    console_putchar(hex[(v >> 4) & 0xF]);
    console_putchar(hex[v & 0xF]);
}

static void xhci_dump_pci_device(const pci_device_t *dev, void *ctx) {
    (void)ctx;
    console_puts("[PCI] ");
    dbg_print_uint(dev->bus);
    console_putchar(':');
    dbg_print_uint(dev->slot);
    console_puts(" - Vendor=");
    dbg_print_hex16(dev->vendor_id);
    console_puts(" Device=");
    dbg_print_hex16(dev->device_id);
    console_puts(" Class=");
    dbg_print_hex16(dev->class_code);
    console_puts(" SubClass=");
    dbg_print_hex16(dev->subclass);
    console_puts(" ProgIF=");
    dbg_print_hex16(dev->prog_if);
    console_puts("\n");
}

/*
 * USB hub 支持
 * ════════════
 * 真机上鼠标/键盘经常挂在**内置 hub** 后面（笔记本键盘上方的扩展口、显示器
 * 自带 hub、扩展坞）。这类拓扑下根端口上看到的是 hub 类设备（bDeviceClass=9），
 * 真正的键鼠在 hub 的下行端口上。只扫根端口就永远找不到它们。
 *
 * 这里实现最小闭环（与 Linux `usb/core/hub.c` 的 hub 枚举一致）：
 *   1. 识别 bDeviceClass=9 的设备；
 *   2. GET_DESCRIPTOR(hub class descriptor) 拿 bNbrPorts；
 *   3. 对每个下行端口 SetPortFeature(PORT_POWER) 上电 → 读 port status；
 *   4. 有连接的端口 SetPortFeature(PORT_RESET) 复位，等 C_PORT_RESET；
 *   5. 清变化位（wPortChange 各位是粘滞的，不清会反复上报）；
 *   6. 递归调用寻址（设备在 hub 后 → 填 TT slot/port）。
 *
 * 上层（HID）不需要知道这层：hub 后的设备与前排设备一样出现在全局设备表里。
 *
 * ⚠ 验证状态：**QEMU 无法构造"设备挂在 hub 后面"的拓扑**（QEMU 的 usb-hub
 * 不暴露可寻址的下游 bus，`-device usb-mouse,bus=hubX` 会直接报 Bus not
 * found），因此这段代码只做了编译与"直连设备不受影响"的回归验证，枚举时序
 * 是照 Linux `usb/core/hub.c` 写的、未在真机/模拟器上端到端跑过。真机上若
 * hub 后的键鼠仍不认，排查顺序建议：
 *   1) 串口里有没有 `[XHCI] Hub on port N with M downstream ports`——
 *      没有说明 hub 设备本身没被识别为 hub（bDeviceClass 不是 9？）；
 *   2) 有没有 `Hub downstream device addressed`——没有则看是端口复位失败
 *      还是 Address Device 失败（后者多半是 tt_info 没生效）；
 *   3) 有 addressed 但 HID 层没注册：看设备描述符/配置描述符读取是否成功。
 */

/** @brief hub 类请求：bmRequestType / bRequest / feature 编号（USB 2.0 §11.24） */
#define USB_RT_HUB          0x20U
#define USB_RT_PORT         0x23U
#define USB_REQ_GET_STATUS  0x00U
#define USB_REQ_SET_FEATURE 0x03U
#define USB_REQ_CLEAR_FEATURE 0x01U
/** @brief 端口特性：连接变化 / 复位 / 端口电源 */
#define USB_PORT_FEAT_C_RESET     16U
#define USB_PORT_FEAT_RESET        4U
#define USB_PORT_FEAT_POWER        8U
/** @brief wPortStatus 位：当前连接 / 已使能 / 低速 / 高速 */
#define USB_PORT_STAT_CONNECTION  0x0001U
#define USB_PORT_STAT_ENABLE      0x0002U
#define USB_PORT_STAT_LOW_SPEED   0x0200U
#define USB_PORT_STAT_HIGH_SPEED  0x0400U
/** @brief 单个 hub 的下行端口上限（USB 2.0 规范 ≤ 31） */
#define USB_HUB_MAX_PORTS 31
/** @brief hub 类描述符长度上限（7 + 端口位图） */
#define USB_HUB_DESC_MAX  15

/**
 * @brief 判断一个设备描述符是否是 hub（bDeviceClass = 9）
 *
 * 注意：有些复合设备把 hub 类信息放在接口描述符里（bDeviceClass = 0），本
 * 驱动只处理设备级声明的 hub —— 这覆盖了绝大多数真机内置 hub。
 */
static int xhci_is_hub(const usb_device_desc_t *desc) {
    return desc && desc->bDeviceClass == 0x09;
}

/**
 * @brief 给 hub 的一个下行端口上电
 *
 * 即使 hub 声明不支持电源切换（wHubCharacteristics 的 LPSM=2）也要发这条
 * 请求：Linux `hub.c` 的注释说明"有些 hub 只是模拟它，不发端口就不工作"。
 */
static int xhci_hub_port_power(uint32_t hub_slot, uint32_t port) {
    return xhci_control_transfer(hub_slot, USB_RT_PORT, USB_REQ_SET_FEATURE,
                                 USB_PORT_FEAT_POWER, (uint16_t)port, 0, 0);
}

/** @brief 清除端口变化位（粘滞位，必须显式清，否则会反复上报） */
static int xhci_hub_clear_change(uint32_t hub_slot, uint32_t port, uint16_t feature) {
    return xhci_control_transfer(hub_slot, USB_RT_PORT, USB_REQ_CLEAR_FEATURE,
                                 feature, (uint16_t)port, 0, 0);
}

/**
 * @brief 读一个下行端口的状态
 * @param port_status 输出 4 字节：低 2 字节 wPortStatus、高 2 字节 wPortChange
 * @return 0 成功，-1 失败
 */
static int xhci_hub_port_status(uint32_t hub_slot, uint32_t port,
                                uint16_t *port_status, uint16_t *port_change) {
    uint8_t buf[4];
    int ret = xhci_control_transfer(hub_slot, USB_RT_PORT | 0x80U,
                                    USB_REQ_GET_STATUS, 0, (uint16_t)port,
                                    buf, sizeof(buf));
    if (ret < 0) return -1;
    *port_status = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    *port_change = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    return 0;
}

/**
 * @brief 复位 hub 的一个下行端口并等复位完成
 *
 * 判定条件与 Linux `hub.c: hub_port_wait_reset()` 一致：轮询 GetPortStatus，
 * 直到 `PORT_RESET` 清零**且**连接位仍为 1；成功后清 `C_PORT_RESET`。
 */
static int xhci_hub_reset_port(uint32_t hub_slot, uint32_t port) {
    if (xhci_control_transfer(hub_slot, USB_RT_PORT, USB_REQ_SET_FEATURE,
                              USB_PORT_FEAT_RESET, (uint16_t)port, 0, 0) < 0)
        return -1;

    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        uint16_t st = 0, chg = 0;
        if (xhci_hub_port_status(hub_slot, port, &st, &chg) < 0) return -1;
        if (!(st & 0x10U) && (st & USB_PORT_STAT_CONNECTION)) {
            /* 复位完成：清掉 C_PORT_RESET，否则该位会一直粘着。 */
            (void)xhci_hub_clear_change(hub_slot, port, USB_PORT_FEAT_C_RESET);
            return 0;
        }
        if (hw_deadline_expired_ms(&deadline, XHCI_PORT_TIMEOUT_MS,
                                   XHCI_INIT_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

/**
 * @brief 探测并寻址一个已连接的 USB 设备（根端口或 hub 下行端口通用）
 *
 * @param port    根端口号（1 起算），写进 slot context 的 ROOT_HUB_PORT
 * @param speed   设备速度
 * @param tt_slot 父 hub 的 slot（0 = 直连根端口）
 * @param tt_port 父 hub 的下行端口号
 * @param out_slot 输出分配到的 slot id
 * @param out_is_hub 输出该设备是否是 hub（调用方据此决定是否继续下钻）
 * @return 0 成功，-1 失败
 */
static int xhci_probe_device(uint32_t port, uint32_t speed, uint32_t tt_slot,
                             uint32_t tt_port, uint32_t *out_slot,
                             int *out_is_hub) {
    uint32_t slot;
    if (xhci_enable_slot(&slot) < 0) return -1;
    if (xhci_address_device(slot, port, speed, tt_slot, tt_port) < 0) {
        xhci_send_cmd(TRB_DISABLE_SLOT, 0, (slot << 24));
        hc->slot_enabled[slot] = 0;
        return -1;
    }

    /* 读设备描述符：判断是不是 hub（hub 的下行端口还要继续枚举）。 */
    usb_device_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    uint8_t buf[18];
    int is_hub = 0;
    if (xhci_control_transfer((int)slot, 0x80, 6, 0x0100, 0, buf, sizeof(buf)) >= 18) {
        memcpy(&desc, buf, 18);
        is_hub = xhci_is_hub(&desc);
    }
    if (out_slot) *out_slot = slot;
    if (out_is_hub) *out_is_hub = is_hub;
    return 0;
}

/** @brief 递归深度上限，防止 hub 环或异常拓扑导致无限递归 */
#define XHCI_HUB_MAX_DEPTH 4
/** @brief 单次探测里寻址的设备总数上限 */
#define XHCI_HUB_MAX_DEVICES 12

/**
 * @brief 枚举一个 hub 的所有下行端口，并对有连接的端口递归寻址
 *
 * @param hub_slot   hub 自身的 slot
 * @param n_ports    hub 报告的下行端口数
 * @param root_port  该 hub 所在的根端口（透传给下游设备的 slot context）
 * @param depth      当前递归深度
 * @param budget     剩余可寻址设备数（调用方保证不会无限枚举）
 * @return 本次新寻址的设备数
 */
static int xhci_scan_hub_ports(uint32_t hub_slot, uint32_t n_ports,
                               uint32_t root_port, int depth, int *budget) {
    if (depth > XHCI_HUB_MAX_DEPTH) return 0;
    int found = 0;

    for (uint32_t p = 1; p <= n_ports && *budget > 0; p++) {
        if (xhci_hub_port_power(hub_slot, p) < 0) continue;
        /* 上电后给端口一点稳定时间（USB 2.0 §7.1.7.3 要求 ≥100ms，
         * 这里按 Linux HUB_DEBOUNCE 的思路给 50ms 的折中）。 */
        usleep(50000);

        uint16_t st = 0, chg = 0;
        if (xhci_hub_port_status(hub_slot, p, &st, &chg) < 0) continue;
        if (!(st & USB_PORT_STAT_CONNECTION)) continue;
        if (chg) {
            /* 清掉所有变化位，避免下次读到陈旧的连接事件。 */
            for (uint16_t b = 16; b <= 23; b++) {
                if (chg & (1U << b)) (void)xhci_hub_clear_change(hub_slot, p, b);
            }
        }

        console_puts("[XHCI] Hub slot ");
        dbg_print_uint(hub_slot);
        console_puts(" port ");
        dbg_print_uint(p);
        console_puts(" connected, resetting...\n");

        if (xhci_hub_reset_port(hub_slot, p) < 0) {
            console_puts("[XHCI] Hub port reset failed.\n");
            continue;
        }

        /* 速度判定（USB 2.0 §11.24.2.7.1）：两位都 0 表示全速。
         * 全速/低速设备走在高速 hub 后面时需要 TT，xHC 会按 tt_info 生成
         * split 事务，因此这里把父 hub 的 slot/端口一并传下去。 */
        uint32_t speed = 1; /* 默认全速 */
        if (st & USB_PORT_STAT_HIGH_SPEED) speed = 3;
        else if (st & USB_PORT_STAT_LOW_SPEED) speed = 2;

        uint32_t child_slot = 0;
        int child_is_hub = 0;
        if (xhci_probe_device(root_port, speed, hub_slot, p, &child_slot,
                              &child_is_hub) < 0) {
            console_puts("[XHCI] Hub downstream device address failed.\n");
            continue;
        }
        (*budget)--;
        found++;
        console_puts("[XHCI] Hub downstream device addressed.\n");
        usleep(10000);

        if (child_is_hub) {
            /* 下级 hub：读它的端口数继续下钻。 */
            uint8_t hd[USB_HUB_DESC_MAX];
            int r = xhci_control_transfer((int)child_slot,
                                          USB_RT_HUB | 0x80U, 6, 0x2900, 0,
                                          hd, sizeof(hd));
            if (r >= 3 && hd[2] > 0 && hd[2] <= USB_HUB_MAX_PORTS) {
                found += xhci_scan_hub_ports(child_slot, hd[2], root_port,
                                             depth + 1, budget);
            }
        }
    }
    return found;
}

/**
 * @brief 读 hub 的下行端口数（hub class descriptor 的 bNbrPorts）
 * @return 端口数（1..31），失败返回 0
 */
static uint32_t xhci_hub_port_count(uint32_t hub_slot) {
    uint8_t hd[USB_HUB_DESC_MAX];
    /* GET_DESCRIPTOR 的 wValue 高字节 = 描述符类型 0x29（hub class）。 */
    int r = xhci_control_transfer((int)hub_slot, USB_RT_HUB | 0x80U, 6, 0x2900,
                                  0, hd, sizeof(hd));
    if (r < 3) return 0;
    uint32_t n = hd[2];
    if (n == 0 || n > USB_HUB_MAX_PORTS) return 0;
    return n;
}

/**
 * @brief 初始化一个 xHCI 控制器（调用方已选定 hc）
 *
 * @param ctrl_idx 该控制器在 xhci_pool 中的下标（登记全局设备表用）
 * @param dev      PCI 上找到的这台 xHCI 控制器
 * @return 0 成功（控制器进入 READY 并完成根端口扫描），-1 失败
 */
static int xhci_init_controller(int ctrl_idx, const pci_device_t *dev) {
    uint32_t bar0 = pci_bar(dev->bus, dev->slot, dev->func, 0);
    console_puts("[XHCI] BAR0 raw value: ");
    dbg_print_hex16((uint16_t)(bar0 >> 16));
    dbg_print_hex16((uint16_t)(bar0 & 0xFFFF));
    console_puts("\n");
    int bar_is_io = 0;
    uint64_t bar_address = pci_bar_address(dev->bus, dev->slot, dev->func, 0,
                                           &bar_is_io);
    if (!bar_address || bar_is_io) {
        console_puts("[XHCI] BAR0 is invalid!\n");
        return -1;
    }
    hc->mmio_phys = bar_address;

    pci_enable_bus_master_mmio(dev);

    hc->mmio = (volatile uint32_t *)vmm_map_mmio(
        hc->mmio_phys, XHCI_MMIO_MAP_SIZE);
    if (!hc->mmio) {
        console_puts("[XHCI] MMIO mapping failed!\n");
        return -1;
    }
    console_puts("[XHCI] MMIO mapped successfully.\n");

    uint8_t caplength = (uint8_t)(xhci_read32(XHCI_CAPLENGTH) & 0xFF);
    if (caplength < 0x20U) {
        console_puts("[XHCI] Invalid capability length!\n");
        return -1;
    }
    hc_op = (volatile uint32_t *)((uint8_t *)hc->mmio + caplength);

    uint32_t hcsparams1 = xhci_read32(XHCI_HCSPARAMS1);
    uint32_t hcsparams2 = xhci_read32(XHCI_HCSPARAMS2);
    uint32_t hccparams1 = xhci_read32(XHCI_HCCPARAMS1);

    // --- BIOS Legacy Handover ---
    uint32_t ext_cap_offset = (hccparams1 >> 16) << 2;
    uint32_t ext_cap_hops = 0;
    while (ext_cap_offset && ext_cap_offset + 8U <= 0x10000U &&
           ext_cap_hops++ < 64U) {
        uint32_t cap = xhci_read32(ext_cap_offset);
        uint32_t cap_id = cap & 0xFF;
        if (cap_id == 1) { // USB Legacy Support
            console_puts("[XHCI] Found USB Legacy Support capability.\n");
            uint32_t val = xhci_read32(ext_cap_offset);
            if (val & (1 << 24)) { // BIOS owned
                console_puts("[XHCI] Controller is BIOS-owned. Requesting handover...\n");
                val |= (1 << 16); // OS owned
                xhci_write32(ext_cap_offset, val);
                
                hw_deadline_t handover_deadline = hw_deadline_start();
                while (val & (1U << 24)) {
                    val = xhci_read32(ext_cap_offset);
                    if (!(val & (1U << 24))) break;
                    if (hw_deadline_expired_ms(
                            &handover_deadline, XHCI_HANDOVER_TIMEOUT_MS,
                            XHCI_INIT_MAX_STALL_SPINS)) break;
                    cpu_relax();
                }
                if (val & (1 << 24)) {
                     console_puts("[XHCI] Handover timed out! Forcing OS ownership...\n");
                     val &= ~(1 << 24);
                     val |= (1 << 16);
                     xhci_write32(ext_cap_offset, val);
                } else {
                     console_puts("[XHCI] Handover successful.\n");
                }
            } else {
                console_puts("[XHCI] Controller is not BIOS-owned.\n");
            }
            
            // Disable SMIs
            uint32_t legctl = xhci_read32(ext_cap_offset + 4);
            legctl &= ~(0x1F | (1 << 15)); // clear SMI enables
            legctl |= 0xE0000000; // clear status
            xhci_write32(ext_cap_offset + 4, legctl);
            break;
        }
        uint32_t next = (cap >> 8) & 0xFF;
        if (next == 0) break;
        ext_cap_offset += next << 2;
    }

    hc->max_slots = hcsparams1 & 0xFFU;
    if (hc->max_slots > XHCI_MAX_SLOTS) hc->max_slots = XHCI_MAX_SLOTS;
    hc->max_ports = (hcsparams1 >> 24) & 0xFFU;
    if (hc->max_ports > XHCI_MAX_PORTS) hc->max_ports = XHCI_MAX_PORTS;
    hc->max_scratchpad = (((hcsparams2 >> 27) & 0x1FU) << 5) |
                          ((hcsparams2 >> 21) & 0x1FU);
    if (!hc->max_slots || !hc->max_ports) return -1;

    uint32_t db_offset = xhci_read32(XHCI_DBOFF) & ~0x3U;
    uint32_t rts_offset = xhci_read32(XHCI_RTSOFF) & ~0x1FU;
    uint64_t doorbell_end = (uint64_t)db_offset +
                            ((uint64_t)hc->max_slots + 1U) * sizeof(uint32_t);
    if (doorbell_end > XHCI_MMIO_MAP_SIZE ||
        (uint64_t)rts_offset + 0x40U > XHCI_MMIO_MAP_SIZE) {
        console_puts("[XHCI] MMIO register offsets exceed mapped BAR!\n");
        return -1;
    }

    hc->doorbell = (volatile uint32_t *)((uint8_t *)hc->mmio + db_offset);
    hc->runtime = (volatile uint32_t *)((uint8_t *)hc->mmio + rts_offset);

    if (xhci_reset_controller() < 0) {
        console_puts("[XHCI] Controller reset failed!\n");
        return -1;
    }
    if (!(xhci_op_read32(XHCI_PAGESIZE) & 1U)) {
        console_puts("[XHCI] Controller does not support 4KiB pages!\n");
        return -1;
    }
    hc->page_size = 4096U;
    if (xhci_init_rings() < 0) {
        console_puts("[XHCI] Ring initialization failed!\n");
        return -1;
    }
    if (xhci_init_scratchpad() < 0) {
        console_puts("[XHCI] Scratchpad initialization failed!\n");
        return -1;
    }
    xhci_op_write32(XHCI_CONFIG, hc->max_slots);
    if (xhci_start_controller() < 0) {
        console_puts("[XHCI] Controller start failed!\n");
        return -1;
    }

    console_puts("[XHCI] Controller initialized. Max slots: ");
    dbg_print_uint(hc->max_slots);
    console_puts(", Max ports: ");
    dbg_print_uint(hc->max_ports);
    console_puts("\n");

    // Wait 100ms for ports to stabilize
    usleep(100000);

    int hub_budget = XHCI_HUB_MAX_DEVICES;
    for (uint32_t port = 1; port <= hc->max_ports; port++) {
        uint32_t portsc = xhci_read_portsc(port);
        if (!(portsc & XHCI_PORTSC_CCS)) continue;

        console_puts("[XHCI] Port ");
        dbg_print_uint(port);
        console_puts(" has device connected. Resetting...\n");

        if (xhci_reset_port(port) < 0) {
            console_puts("[XHCI] Port reset failed!\n");
            continue;
        }

        uint32_t speed = (xhci_read_portsc(port) >> 10) & 0xF;
        uint32_t slot = 0;
        int is_hub = 0;
        if (xhci_probe_device(port, speed, 0, 0, &slot, &is_hub) < 0) {
            console_puts("[XHCI] Address device failed!\n");
            continue;
        }
        console_puts("[XHCI] Device addressed successfully!\n");
        usleep(10000); // 10ms recovery time after Set Address

        if (is_hub) {
            /* 根端口上是 hub：真机鼠标很可能就在它的下行端口上。 */
            uint32_t n = xhci_hub_port_count(slot);
            if (n) {
                console_puts("[XHCI] Hub on port ");
                dbg_print_uint(port);
                console_puts(" with ");
                dbg_print_uint(n);
                console_puts(" downstream ports.\n");
                (void)xhci_scan_hub_ports(slot, n, port, 1, &hub_budget);
            }
        }
    }

    /* 把本控制器上已寻址的设备登记进全局设备表：设备下标从此跨控制器唯一。 */
    for (uint32_t slot = 1; slot <= hc->max_slots; slot++) {
        if (!hc->slot_enabled[slot]) continue;
        if (xhci_dev_total >= XHCI_MAX_DEVICES) {
            console_puts("[XHCI] Global device table full, extra devices ignored.\n");
            break;
        }
        xhci_dev_refs[xhci_dev_total].ctrl = (uint8_t)ctrl_idx;
        xhci_dev_refs[xhci_dev_total].slot = slot;
        xhci_dev_total++;
    }

    return 0;
}

/**
 * @brief xhci_init() 的控制器扫描上下文：收集所有可达的 xHCI 控制器
 */
typedef struct {
    pci_device_t list[XHCI_MAX_CONTROLLERS]; /**< 已收集的控制器 */
    int count;                               /**< 已收集数量 */
} xhci_scan_ctx_t;

/**
 * @brief pci_enumerate_all() 回调：把 xHCI 控制器收进扫描上下文
 *
 * 不再用 pci_find_class() 只取第一台：真机上常有第二个 xHCI（板载第三方
 * 控制器，或 CPU 直连的那一个），键鼠插在它上面时只初始化第一台就等于
 * "没有 USB 输入"。
 */
static void xhci_collect_controller(const pci_device_t *dev, void *ctx) {
    xhci_scan_ctx_t *scan = (xhci_scan_ctx_t *)ctx;
    if (dev->class_code != XHCI_PCI_CLASS ||
        dev->subclass != XHCI_PCI_SUBCLASS ||
        dev->prog_if != XHCI_PCI_PROGIF) return;
    if (scan->count >= XHCI_MAX_CONTROLLERS) return;
    scan->list[scan->count++] = *dev;
}

int xhci_init(void) {
    static int initialized;
    static int ready;
    if (initialized) return ready > 0 ? 0 : -1;
    initialized = 1;

    console_puts("[XHCI] Scanning PCI bus for USB controllers...\n");
    /* 递归、桥感知的枚举（跟进 PCI-to-PCI 桥的 Secondary Bus），而不是
     * 各自维护一份平坦的 bus 0-255 扫描——见 pci.c pci_enumerate_all()。 */
    pci_enumerate_all(xhci_dump_pci_device, NULL);

    xhci_scan_ctx_t scan;
    scan.count = 0;
    pci_enumerate_all(xhci_collect_controller, &scan);
    if (scan.count == 0) {
        console_puts("[XHCI] Failed to find xHCI controller on PCI!\n");
        return -1;
    }
    console_puts("[XHCI] Controllers found: ");
    dbg_print_uint((uint32_t)scan.count);
    console_puts("\n");

    xhci_ctrl_count = 0;
    for (int i = 0; i < scan.count; i++) {
        hc = &xhci_pool[i];
        memset(hc, 0, sizeof(*hc));
        if (xhci_init_controller(i, &scan.list[i]) < 0) {
            hc->initialized = XHCI_STATE_FAILED;
            console_puts("[XHCI] Controller init failed; trying next one.\n");
            continue;
        }
        hc->initialized = XHCI_STATE_READY;
        xhci_ctrl_count++;
    }
    hc = 0;
    ready = xhci_ctrl_count;
    return ready > 0 ? 0 : -1;
}

/**
 * @brief 推进**一个**控制器的事件处理（调用方已选定 hc）
 *
 * 传输路径（xhci_control_transfer 等）在等待完成时调用本函数，因此它必须
 * 只动当前 hc：如果这里改成遍历所有控制器，正在进行的传输上下文会被打断。
 */
static void xhci_poll_hc(void) {
    if (hc->initialized != XHCI_STATE_READY) return;

    uint32_t usbsts = xhci_op_read32(XHCI_USBSTS);
    if (usbsts & XHCI_STS_EINT) {
        xhci_op_write32(XHCI_USBSTS, XHCI_STS_EINT);

        while (1) {
            uint32_t idx = hc->event_ring_idx;
            xhci_trb_t *evt = &((xhci_trb_t *)hc->event_ring)[idx];
            if (!xhci_event_ready(evt)) break;

            uint32_t evt_type = (evt->param3 >> 10) & 0x3F;
            if (evt_type == TRB_EV_PORT_SC) {
                uint32_t port = (evt->param0 >> 24) & 0xFF;
                uint32_t portsc = xhci_read_portsc(port);
                if ((portsc & XHCI_PORTSC_CCS) && !(portsc & XHCI_PORTSC_PED)) {
                    xhci_reset_port(port);
                }
            } else if (evt_type == TRB_EV_TRANSFER) {
                uint32_t slot = (evt->param3 >> 24) & 0xFF;
                uint32_t ep = (evt->param3 >> 16) & 0x1F;
                int code = (evt->param2 >> 24) & 0xFF;

                if (slot > 0 && slot <= hc->max_slots && ep > 0 && ep < XHCI_MAX_EP) {
                    hc->ep_transfer_residue[slot][ep] =
                        evt->param2 & 0x00FFFFFFU;
                    hc->ep_has_data[slot][ep] = (uint32_t)code;

                    if (hc->ep_data_buf[slot][ep]) {
                        // Re-enqueue the TRB to keep polling
                        uint32_t control = (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP;
                        xhci_post_transfer_trb(slot, ep, hc->ep_data_buf_phys[slot][ep],
                                               hc->ep_data_buf_len[slot][ep], control);
                        xhci_ring_doorbell(slot, ep);
                    }
                }
            }

            xhci_advance_event();
        }
    }
}

void xhci_poll(void) {
    /* 任务上下文里的轮询：把所有就绪控制器都推进一遍，否则鼠标挂在第二个
     * 控制器上时它的完成事件永远不会被处理。hc 用完必须恢复，避免把调用方
     * （可能正处于某次传输中）的上下文改掉。 */
    xhci_t *saved = hc;
    for (int i = 0; i < XHCI_MAX_CONTROLLERS; i++) {
        if (xhci_pool[i].initialized != XHCI_STATE_READY) continue;
        hc = &xhci_pool[i];
        xhci_poll_hc();
    }
    hc = saved;
}

/**
 * @brief 按全局设备下标选中对应的控制器与 slot
 *
 * 设备下标跨控制器唯一（见 xhci_dev_refs），这是真机多控制器场景能工作的
 * 前提。成功时把 hc 切到该设备所属控制器，调用方随后的 slot 级访问才有意义。
 *
 * @param dev_idx 全局设备下标
 * @param out_slot 输出该设备在所属控制器内的 slot id；可为 NULL
 * @return 0 成功，-1 下标越界
 */
static int xhci_select_dev(int dev_idx, uint32_t *out_slot) {
    if (dev_idx < 0 || dev_idx >= xhci_dev_total) return -1;
    xhci_dev_ref_t *ref = &xhci_dev_refs[dev_idx];
    if (ref->ctrl >= XHCI_MAX_CONTROLLERS) return -1;
    hc = &xhci_pool[ref->ctrl];
    if (hc->initialized != XHCI_STATE_READY) return -1;
    if (out_slot) *out_slot = ref->slot;
    return 0;
}

int xhci_device_count(void) {
    return xhci_dev_total;
}

int xhci_device_slot(int idx) {
    if (idx < 0 || idx >= xhci_dev_total) return -1;
    return (int)xhci_dev_refs[idx].slot;
}

/*
 * 设备下标版传输 API —— HID/存储层只认全局设备下标，控制器与 slot 的换算
 * 封在 xhci_select_dev() 里。旧的 slot 版接口保留给控制器内部与诊断使用。
 */

int xhci_dev_control_transfer(int dev_idx, uint8_t bmRequestType,
                              uint8_t bRequest, uint16_t wValue,
                              uint16_t wIndex, void *data, uint16_t wLength) {
    uint32_t slot = 0;
    if (xhci_select_dev(dev_idx, &slot) < 0) return -1;
    return xhci_control_transfer((int)slot, bmRequestType, bRequest,
                                 wValue, wIndex, data, wLength);
}

int xhci_dev_interrupt_transfer(int dev_idx, int ep_addr, void *data,
                                uint32_t len, uint32_t interval) {
    uint32_t slot = 0;
    if (xhci_select_dev(dev_idx, &slot) < 0) return -1;
    return xhci_interrupt_transfer((int)slot, ep_addr, data, len, interval);
}

int xhci_dev_bulk_transfer_packet(int dev_idx, int ep_addr, void *data,
                                  uint32_t len, uint32_t max_packet) {
    uint32_t slot = 0;
    if (xhci_select_dev(dev_idx, &slot) < 0) return -1;
    return xhci_bulk_transfer_packet((int)slot, ep_addr, data, len, max_packet);
}

int xhci_dev_bulk_transfer(int dev_idx, int ep_addr, void *data, uint32_t len) {
    uint32_t slot = 0;
    if (xhci_select_dev(dev_idx, &slot) < 0) return -1;
    return xhci_bulk_transfer((int)slot, ep_addr, data, len);
}

int xhci_get_device_desc(int idx, usb_device_desc_t *desc) {
    if (!desc) return -1;
    if (idx < 0 || idx >= xhci_dev_total) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(64);
    if (!buf) return -1;

    int ret = xhci_dev_control_transfer(idx, 0x80, 6, 0x0100, 0, buf, 18);
    if (ret >= 0) memcpy(desc, buf, 18);
    kfree(buf);
    return ret;
}

static xhci_trb_t *xhci_get_or_create_ep_ring(uint32_t slot_id, uint32_t ep_id,
                                              uint32_t ep_type, uint32_t max_packet_size,
                                              uint32_t interval, uint64_t *out_phys) {
    if (slot_id <= 0 || slot_id > hc->max_slots) return NULL;
    if (ep_id <= 0 || ep_id >= XHCI_MAX_EP) return NULL;

    if (hc->ep_rings[slot_id][ep_id]) {
        if (out_phys) *out_phys = hc->ep_rings_phys[slot_id][ep_id];
        return (xhci_trb_t *)hc->ep_rings[slot_id][ep_id];
    }

    uint64_t ring_phys = 0;
    uint64_t *ring = xhci_alloc_transfer_ring(&ring_phys);
    if (!ring) return NULL;
    memset(ring, 0, XHCI_TRB_RING_SIZE * 16);

    // Initialize Link TRB at the end of the ring (index XHCI_TRB_RING_SIZE - 1)
    xhci_trb_t *trb_ring = (xhci_trb_t *)ring;
    uint32_t link_ctrl = (TRB_LINK << 10) | TRB_TC;
    xhci_build_trb(&trb_ring[XHCI_TRB_RING_SIZE - 1], ring_phys, 0, link_ctrl);

    // For non-control endpoints, configure the endpoint in the controller
    if (ep_id != 1) {
        int ret = xhci_configure_endpoint(slot_id, ep_type, ep_id,
                                          max_packet_size, interval, ring_phys);
        if (ret < 0) {
            kfree(ring);
            return NULL;
        }
    }

    hc->ep_rings[slot_id][ep_id] = ring;
    hc->ep_rings_phys[slot_id][ep_id] = ring_phys;
    hc->ep_ring_idx[slot_id][ep_id] = 0;
    hc->ep_ring_cycle[slot_id][ep_id] = 1;
    hc->ep_configured[slot_id][ep_id] = 1;

    // For Interrupt IN endpoint, allocate persistent buffer and enqueue first TRB
    if (ep_id != 1 && ep_type == EP_CTX_INTERRUPT_IN) {
        uint64_t buf_phys = 0;
        // Allocate buffer of max_packet_size
        uint8_t *buf = (uint8_t *)xhci_alloc_aligned(max_packet_size, &buf_phys);
        if (buf) {
            memset(buf, 0, max_packet_size);
            hc->ep_data_buf[slot_id][ep_id] = buf;
            hc->ep_data_buf_phys[slot_id][ep_id] = buf_phys;
            hc->ep_data_buf_len[slot_id][ep_id] = max_packet_size;
            hc->ep_has_data[slot_id][ep_id] = 0;
            
            // Post first TRB pointing to this buffer
            uint32_t control = (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP;
            xhci_post_transfer_trb(slot_id, ep_id, buf_phys, max_packet_size, control);
            
            // Ring the doorbell to start polling
            xhci_ring_doorbell(slot_id, ep_id);
        }
    }

    if (out_phys) *out_phys = ring_phys;
    return trb_ring;
}

static void xhci_post_transfer_trb(uint32_t slot_id, uint32_t ep_id,
                                    uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *ring = (xhci_trb_t *)hc->ep_rings[slot_id][ep_id];
    uint64_t ring_phys = hc->ep_rings_phys[slot_id][ep_id];
    uint32_t idx = hc->ep_ring_idx[slot_id][ep_id];
    uint32_t cycle = hc->ep_ring_cycle[slot_id][ep_id];

    // If the next slot is the Link TRB, execute toggle cycle and wrap around
    if (idx == XHCI_TRB_RING_SIZE - 1) {
        uint32_t link_ctrl = (TRB_LINK << 10) | TRB_TC | (cycle ? TRB_C : 0);
        xhci_build_trb(&ring[idx], ring_phys, 0, link_ctrl);
        idx = 0;
        cycle ^= 1;
    }

    uint32_t final_ctrl = control | (cycle ? TRB_C : 0);
    xhci_build_trb(&ring[idx], param, status, final_ctrl);

    idx++;
    hc->ep_ring_idx[slot_id][ep_id] = idx;
    hc->ep_ring_cycle[slot_id][ep_id] = cycle;
}

int xhci_control_transfer(int slot_id, uint8_t bmRequestType,
                          uint8_t bRequest, uint16_t wValue,
                          uint16_t wIndex, void *data, uint16_t wLength) {
    if (hc->initialized != XHCI_STATE_READY || slot_id <= 0 ||
        (uint32_t)slot_id > hc->max_slots) return -1;
    if (!hc->slot_enabled[slot_id]) return -1;

    uint64_t ring_phys;
    xhci_trb_t *ring = xhci_get_or_create_ep_ring((uint32_t)slot_id, 1, EP_CTX_CONTROL, 8, 0, &ring_phys);
    if (!ring) return -1;

    uint64_t data_phys = 0;
    void *data_buf = 0;
    if (wLength > 0) {
        if (!data) return -1;
        data_buf = xhci_alloc_aligned(wLength, &data_phys);
        if (!data_buf) return -1;
        if (bmRequestType & 0x80) memset(data_buf, 0, wLength);
        else memcpy(data_buf, data, wLength);
    }

    uint32_t dir_in = (bmRequestType & 0x80) ? 1 : 0;
    uint32_t trt = wLength > 0 ? (dir_in ? TRB_TRT_IN_DATA : TRB_TRT_OUT_DATA) : TRB_TRT_NO_DATA;

    uint64_t setup_word = ((uint64_t)bmRequestType << 0) |
                          ((uint64_t)bRequest << 8) |
                          ((uint64_t)wValue << 16) |
                          ((uint64_t)wIndex << 32) |
                          ((uint64_t)wLength << 48);

    // Clear completion flag
    hc->ep_has_data[slot_id][1] = 0;

    // We post Setup Stage TRB
    xhci_post_transfer_trb((uint32_t)slot_id, 1, setup_word, 8,
                           (TRB_SETUP_STAGE << 10) | (trt << 16) | TRB_IDT);

    if (wLength > 0) {
        uint32_t data_ctrl = (TRB_DATA_STAGE << 10);
        if (dir_in) data_ctrl |= TRB_DIR_IN;
        
        xhci_post_transfer_trb((uint32_t)slot_id, 1, data_phys, wLength, data_ctrl);
    }

    uint32_t status_ctrl = (TRB_STATUS_STAGE << 10) | TRB_IOC;
    if (wLength == 0 || !dir_in) status_ctrl |= TRB_DIR_IN;

    xhci_post_transfer_trb((uint32_t)slot_id, 1, 0, 0, status_ctrl);

    xhci_ring_doorbell((uint32_t)slot_id, 1);

    int ret_val = -1;
    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        xhci_poll_hc();
        uint32_t code = hc->ep_has_data[slot_id][1];
        if (code != 0) {
            hc->ep_has_data[slot_id][1] = 0;
            if (code == XHCI_COMP_SUCCESS ||
                code == XHCI_COMP_SHORT_PACKET) {
                if (data_buf && dir_in) memcpy(data, data_buf, wLength);
                ret_val = (int)wLength;
            } else {
                ret_val = -(int)code;
            }
            break;
        }
        if (hw_deadline_expired_ms(&deadline, XHCI_TRANSFER_TIMEOUT_MS,
                                   XHCI_IO_MAX_STALL_SPINS)) break;
        task_yield();
    }

    if (data_buf) kfree(data_buf);
    return ret_val;
}

int xhci_bulk_transfer_packet(int slot_id, int ep_addr, void *data,
                              uint32_t len, uint32_t max_packet) {
    if (hc->initialized != XHCI_STATE_READY || !data || !len) return -1;
    if (len > XHCI_TRB_MAX_BUFFER) return -1;
    if (slot_id <= 0 || (uint32_t)slot_id > hc->max_slots) return -1;
    if (!hc->slot_enabled[slot_id]) return -1;
    if (!max_packet || max_packet > 1024U) return -1;

    int ep_num = ep_addr & 0x0F;
    int is_in = (ep_addr & 0x80) ? 1 : 0;
    int ep_id = ep_num * 2 + is_in;

    uint32_t ep_type = is_in ? EP_CTX_BULK_IN : 2; /* EP_CTX_BULK_OUT = 2 */
    uint64_t ring_phys = 0;
    xhci_trb_t *ring = xhci_get_or_create_ep_ring(
        (uint32_t)slot_id, ep_id, ep_type, max_packet, 0, &ring_phys);
    if (!ring) return -1;

    /* Data buffer for the transfer */
    void *data_buf = kmalloc_aligned(len, XHCI_TRB_MAX_BUFFER);
    if (!data_buf) return -1;
    uint64_t data_phys =
        vmm_virt_to_phys((uint64_t)(uintptr_t)data_buf);
    if ((data_phys & (XHCI_TRB_MAX_BUFFER - 1U)) != 0) {
        kfree(data_buf);
        return -1;
    }

    if (is_in) {
        memset(data_buf, 0, len);
    } else {
        memcpy(data_buf, data, len);
    }

    // Clear completion flag
    hc->ep_has_data[slot_id][ep_id] = 0;

    /* Build and post a Normal TRB pointing to the data buffer */
    uint32_t control = (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP;
    xhci_post_transfer_trb((uint32_t)slot_id, ep_id, data_phys, len, control);

    /* Ring the doorbell to kick off the transfer */
    xhci_ring_doorbell((uint32_t)slot_id, ep_id);

    int ret_val = -1;
    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        xhci_poll_hc();
        uint32_t code = hc->ep_has_data[slot_id][ep_id];
        if (code != 0) {
            hc->ep_has_data[slot_id][ep_id] = 0;
            if (code == XHCI_COMP_SUCCESS ||
                code == XHCI_COMP_SHORT_PACKET) {
                uint32_t residue = hc->ep_transfer_residue[slot_id][ep_id];
                uint32_t actual = residue <= len ? len - residue : 0;
                if (is_in) {
                    memcpy(data, data_buf, actual);
                }
                ret_val = (int)actual;
            } else {
                ret_val = -(int)code;
            }
            break;
        }
        if (hw_deadline_expired_ms(&deadline, XHCI_TRANSFER_TIMEOUT_MS,
                                   XHCI_IO_MAX_STALL_SPINS)) break;
        task_yield();
    }

    kfree(data_buf);
    return ret_val;
}

int xhci_bulk_transfer(int slot_id, int ep_addr, void *data, uint32_t len) {
    return xhci_bulk_transfer_packet(slot_id, ep_addr, data, len, 512U);
}

int xhci_interrupt_transfer(int slot_id, int ep_addr, void *data, uint32_t len, uint32_t interval) {
    if (hc->initialized != XHCI_STATE_READY || !data || !len) return -1;
    if (slot_id <= 0 || (uint32_t)slot_id > hc->max_slots) return -1;
    if (!hc->slot_enabled[slot_id]) return -1;

    int ep_num = ep_addr & 0x0F;
    int is_in = (ep_addr & 0x80) ? 1 : 0;
    int ep_id = ep_num * 2 + is_in;

    uint32_t max_packet = len < 8 ? 8 : len;
    if (max_packet > 64) max_packet = 64;

    uint64_t ring_phys = 0;
    xhci_trb_t *ring = xhci_get_or_create_ep_ring((uint32_t)slot_id, ep_id, EP_CTX_INTERRUPT_IN, max_packet, interval, &ring_phys);
    if (!ring) return -1;

    // 只推进当前控制器：这会是一台设备的一次中断 IN 传输
    xhci_poll_hc();

    // Check if new data has arrived
    uint32_t code = hc->ep_has_data[slot_id][ep_id];
    if (code != 0) {
        hc->ep_has_data[slot_id][ep_id] = 0;
        if (code == XHCI_COMP_SUCCESS ||
            code == XHCI_COMP_SHORT_PACKET) {
            uint32_t residue = hc->ep_transfer_residue[slot_id][ep_id];
            uint32_t copy_len = residue <= max_packet
                              ? max_packet - residue : 0;
            if (copy_len > len) copy_len = len;
            memcpy(data, hc->ep_data_buf[slot_id][ep_id], copy_len);
            return (int)copy_len;
        }
        return -(int)code;
    }

    return -1; // no data
}
