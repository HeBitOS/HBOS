#include "xhci.h"
#include "pci.h"
#include "core/pmm.h"
#include "core/vmm.h"
#include "core/heap.h"
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

static xhci_t xhci;
static volatile uint32_t *xhci_op;

static uint32_t xhci_read32(uint32_t offset) {
    return xhci.mmio[offset / 4];
}

static void xhci_write32(uint32_t offset, uint32_t value) {
    xhci.mmio[offset / 4] = value;
}

static uint32_t xhci_op_read32(uint32_t offset) {
    return xhci_op[offset / 4];
}

static void xhci_op_write32(uint32_t offset, uint32_t value) {
    xhci_op[offset / 4] = value;
}

static uint32_t xhci_read_portsc(uint32_t port) {
    return xhci_op[(PORTSC_OFFSET + (port - 1) * 16) / 4];
}

static void xhci_write_portsc(uint32_t port, uint32_t value) {
    xhci_op[(PORTSC_OFFSET + (port - 1) * 16) / 4] = value;
}

static void xhci_ring_doorbell(uint32_t slot, uint32_t target) {
    if (xhci.doorbell) xhci.doorbell[slot] = target;
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
    return (evt->param3 & TRB_C) == xhci.event_ring_cycle;
}

static void xhci_advance_event(void) {
    xhci.event_ring_idx++;
    if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
        xhci.event_ring_idx = 0;
        xhci.event_ring_cycle ^= 1;
    }
    if (xhci.runtime) {
        uint64_t erdp = xhci.event_ring_phys + xhci.event_ring_idx * 16;
        xhci.runtime[0x38 / 4] = (uint32_t)(erdp & 0xFFFFFFFF) | (1 << 3);
        xhci.runtime[0x3C / 4] = (uint32_t)(erdp >> 32);
    }
}

static int xhci_post_cmd(uint32_t trb_type, uint64_t param, uint32_t status) {
    uint32_t idx = xhci.cmd_ring_idx;
    xhci_trb_t *trb = &((xhci_trb_t *)xhci.cmd_ring)[idx];

    /* Correct XHCI Command TRB specification layout:
     * Slot ID (and other control status parameters) belong to DWORD 3 (trb->param3),
     * not DWORD 2 (trb->param2). Setting DWORD 2 to zero and merging status into DWORD 3. */
    xhci_build_trb(trb, param, 0,
                   status | (trb_type << 10) | (xhci.cmd_ring_cycle ? TRB_C : 0));

    xhci.cmd_ring_idx++;
    if (xhci.cmd_ring_idx >= XHCI_CMD_RING_SIZE) {
        xhci.cmd_ring_idx = 0;
        xhci.cmd_ring_cycle ^= 1;
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
    dbg_print_uint(xhci.event_ring_idx);
    console_puts(", cycle: ");
    dbg_print_uint(xhci.event_ring_cycle);
    console_puts("\n");

    for (int timeout = 0; timeout < 5000000; timeout++) {
        uint32_t idx = xhci.event_ring_idx;
        xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[idx];
        
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

        if (!xhci_event_ready(evt)) continue;

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
            return code == 1 ? 0 : -code;
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

    for (int i = 0; i < 100000; i++) {
        if (xhci_op_read32(XHCI_USBSTS) & XHCI_STS_HCH) break;
        for (volatile int j = 0; j < 1000; j++);
    }

    uint32_t status = xhci_op_read32(XHCI_USBSTS);
    if (!(status & XHCI_STS_HCH)) return -1;

    xhci_op_write32(XHCI_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 100000; i++) {
        if (!(xhci_op_read32(XHCI_USBCMD) & XHCI_CMD_HCRST)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    if (xhci_op_read32(XHCI_USBCMD) & XHCI_CMD_HCRST) return -1;

    for (int i = 0; i < 100000; i++) {
        if (!(xhci_op_read32(XHCI_USBSTS) & XHCI_STS_CNR)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    return (xhci_op_read32(XHCI_USBSTS) & XHCI_STS_CNR) ? -1 : 0;
}

static int xhci_init_rings(void) {
    xhci.cmd_ring = (uint64_t *)xhci_alloc_aligned(
        XHCI_CMD_RING_SIZE * 16, &xhci.cmd_ring_phys);
    if (!xhci.cmd_ring) return -1;
    memset(xhci.cmd_ring, 0, XHCI_CMD_RING_SIZE * 16);
    xhci.cmd_ring_idx = 0;
    xhci.cmd_ring_cycle = 1;

    uint64_t event_ring_seg_phys = 0;
    xhci.event_ring_seg = (uint64_t *)xhci_alloc_aligned(16, &event_ring_seg_phys);
    if (!xhci.event_ring_seg) return -1;

    xhci.event_ring = (uint64_t *)xhci_alloc_aligned(
        XHCI_EVENT_RING_SIZE * 16, &xhci.event_ring_phys);
    if (!xhci.event_ring) return -1;
    memset(xhci.event_ring, 0, XHCI_EVENT_RING_SIZE * 16);
    xhci.event_ring_idx = 0;
    xhci.event_ring_cycle = 1;

    xhci.event_ring_seg[0] = xhci.event_ring_phys;
    xhci.event_ring_seg[1] = XHCI_EVENT_RING_SIZE;

    xhci_op_write32(XHCI_CRCR_LO, (uint32_t)(xhci.cmd_ring_phys & 0xFFFFFFFF) | TRB_C);
    xhci_op_write32(XHCI_CRCR_HI, (uint32_t)(xhci.cmd_ring_phys >> 32));

    if (!xhci.runtime) return -1;
    xhci.runtime[0x28 / 4] = 1; /* ERSTSZ */
    xhci.runtime[0x30 / 4] = (uint32_t)(event_ring_seg_phys & 0xFFFFFFFF);
    xhci.runtime[0x34 / 4] = (uint32_t)(event_ring_seg_phys >> 32);
    xhci.runtime[0x38 / 4] = (uint32_t)(xhci.event_ring_phys & 0xFFFFFFFF);
    xhci.runtime[0x3C / 4] = (uint32_t)(xhci.event_ring_phys >> 32);

    return 0;
}

static int xhci_init_scratchpad(void) {
    xhci.dcbaa = (uint64_t *)xhci_alloc_aligned(
        XHCI_MAX_SLOTS * 8 + 16, &xhci.dcbaa_phys);
    if (!xhci.dcbaa) return -1;
    memset(xhci.dcbaa, 0, XHCI_MAX_SLOTS * 8 + 16);

    if (xhci.max_scratchpad > 0 && xhci.max_scratchpad <= XHCI_MAX_SCRATCHPAD_BUFFERS) {
        uint64_t scratchpad_array_phys = 0;
        xhci.scratchpad_array = (uint64_t *)xhci_alloc_aligned(
            xhci.max_scratchpad * 8, &scratchpad_array_phys);
        if (xhci.scratchpad_array) {
            for (uint32_t i = 0; i < xhci.max_scratchpad; i++) {
                uint64_t scratchpad_phys = 0;
                xhci.scratchpad_buffers[i] = (uint64_t *)xhci_alloc_aligned(
                    4096, &scratchpad_phys);
                xhci.scratchpad_array[i] = scratchpad_phys;
            }
            xhci.dcbaa[0] = scratchpad_array_phys;
        }
    }

    xhci_op_write32(XHCI_DCBAAP_LO, (uint32_t)(xhci.dcbaa_phys & 0xFFFFFFFF));
    xhci_op_write32(XHCI_DCBAAP_HI, (uint32_t)(xhci.dcbaa_phys >> 32));

    return 0;
}

static int xhci_start_controller(void) {
    uint32_t cmd = xhci_op_read32(XHCI_USBCMD);
    cmd |= XHCI_CMD_RS;
    xhci_op_write32(XHCI_USBCMD, cmd);

    for (int i = 0; i < 100000; i++) {
        if (!(xhci_op_read32(XHCI_USBSTS) & XHCI_STS_HCH)) return 0;
        for (volatile int j = 0; j < 1000; j++);
    }
    return -1;
}

static int xhci_reset_port(uint32_t port) {
    uint32_t portsc = xhci_read_portsc(port);
    portsc |= XHCI_PORTSC_PR;
    xhci_write_portsc(port, portsc);

    for (int i = 0; i < 500000; i++) {
        portsc = xhci_read_portsc(port);
        if (!(portsc & XHCI_PORTSC_PR)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    if (xhci_read_portsc(port) & XHCI_PORTSC_PR) return -1;

    for (int i = 0; i < 100000; i++) {
        portsc = xhci_read_portsc(port);
        if (portsc & XHCI_PORTSC_PED) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    return (xhci_read_portsc(port) & XHCI_PORTSC_PED) ? 0 : -1;
}

static int xhci_setup_device_context(uint32_t slot_id) {
    uint64_t ctx_phys = 0;
    uint64_t *ctx = (uint64_t *)xhci_alloc_aligned(CTX_SIZE * 32, &ctx_phys);
    if (!ctx) return -1;
    memset(ctx, 0, CTX_SIZE * 32);
    xhci.device_context_base[slot_id] = ctx;
    xhci.dcbaa[slot_id] = ctx_phys;
    return 0;
}

static int xhci_enable_slot(uint32_t *slot_id) {
    int ret = xhci_send_cmd(TRB_ENABLE_SLOT, 0, 0);
    if (ret < 0) return ret;
    for (uint32_t i = 1; i <= xhci.max_slots; i++) {
        if (!xhci.slot_enabled[i]) {
            xhci.slot_enabled[i] = 1;
            *slot_id = i;
            if (xhci_setup_device_context(i) < 0) return -1;
            return 0;
        }
    }
    return -1;
}

static uint64_t *xhci_alloc_transfer_ring(uint64_t *ring_phys);

static int xhci_address_device(uint32_t slot_id, uint32_t port, uint32_t speed) {
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

    xhci.input_context[slot_id] = ep0_ring;
    xhci.ep_rings[slot_id][1] = ep0_ring;
    xhci.ep_rings_phys[slot_id][1] = ep0_ring_phys;
    xhci.ep_ring_idx[slot_id][1] = 0;
    xhci.ep_ring_cycle[slot_id][1] = 1;
    xhci.ep_configured[slot_id][1] = 1;

    uint32_t *ctx = (uint32_t *)input_ctx;
    ctx[1] = (1 << 0) | (1 << 1); /* Add slot + EP0 contexts */
    ctx[8] = (1 << 27) | (speed << 20);
    ctx[9] = (port << 16);

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
    ctx[8] = (ep_id << 27);

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

static int xhci_init_internal(void) {

    console_puts("[XHCI] Scanning PCI bus for USB controllers...\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_read16((uint8_t)bus, slot, 0, 0x00);
            if (vendor == 0xFFFF) continue;
            uint16_t device = pci_read16((uint8_t)bus, slot, 0, 0x02);
            uint8_t class_code = pci_read8((uint8_t)bus, slot, 0, 0x0B);
            uint8_t subclass = pci_read8((uint8_t)bus, slot, 0, 0x0A);
            uint8_t prog_if = pci_read8((uint8_t)bus, slot, 0, 0x09);
            console_puts("[PCI] ");
            dbg_print_uint(bus);
            console_putchar(':');
            dbg_print_uint(slot);
            console_puts(" - Vendor=");
            dbg_print_hex16(vendor);
            console_puts(" Device=");
            dbg_print_hex16(device);
            console_puts(" Class=");
            dbg_print_hex16(class_code);
            console_puts(" SubClass=");
            dbg_print_hex16(subclass);
            console_puts(" ProgIF=");
            dbg_print_hex16(prog_if);
            console_puts("\n");
        }
    }

    pci_device_t dev;
    if (pci_find_class(XHCI_PCI_CLASS, XHCI_PCI_SUBCLASS, XHCI_PCI_PROGIF, &dev) < 0) {
        console_puts("[XHCI] Failed to find xHCI controller on PCI!\n");
        return -1;
    }

    memset(&xhci, 0, sizeof(xhci));

    uint32_t bar0 = pci_bar(dev.bus, dev.slot, dev.func, 0);
    console_puts("[XHCI] BAR0 raw value: ");
    dbg_print_hex16((uint16_t)(bar0 >> 16));
    dbg_print_hex16((uint16_t)(bar0 & 0xFFFF));
    console_puts("\n");
    if (!(bar0 & 0xFFFFFFF0)) {
        console_puts("[XHCI] BAR0 is invalid!\n");
        return -1;
    }
    xhci.mmio_phys = bar0 & 0xFFFFFFF0;

    pci_enable_bus_master_mmio(&dev);

    xhci.mmio = (volatile uint32_t *)vmm_map_mmio(xhci.mmio_phys, 0x10000);
    if (!xhci.mmio) {
        console_puts("[XHCI] MMIO mapping failed!\n");
        return -1;
    }
    console_puts("[XHCI] MMIO mapped successfully.\n");

    uint8_t caplength = (uint8_t)(xhci_read32(XHCI_CAPLENGTH) & 0xFF);
    xhci_op = (volatile uint32_t *)((uint8_t *)xhci.mmio + caplength);

    uint32_t hcsparams1 = xhci_read32(XHCI_HCSPARAMS1);
    uint32_t hcsparams2 = xhci_read32(XHCI_HCSPARAMS2);
    uint32_t hccparams1 = xhci_read32(XHCI_HCCPARAMS1);

    // --- BIOS Legacy Handover ---
    uint32_t ext_cap_offset = (hccparams1 >> 16) << 2;
    while (ext_cap_offset) {
        uint32_t cap = xhci_read32(ext_cap_offset);
        uint32_t cap_id = cap & 0xFF;
        if (cap_id == 1) { // USB Legacy Support
            console_puts("[XHCI] Found USB Legacy Support capability.\n");
            uint32_t val = xhci_read32(ext_cap_offset);
            if (val & (1 << 24)) { // BIOS owned
                console_puts("[XHCI] Controller is BIOS-owned. Requesting handover...\n");
                val |= (1 << 16); // OS owned
                xhci_write32(ext_cap_offset, val);
                
                int timeout = 1000;
                while (timeout-- > 0) {
                    val = xhci_read32(ext_cap_offset);
                    if (!(val & (1 << 24))) {
                        break;
                    }
                    // Delay using volatile loop to be safe in early boot
                    for (volatile int j = 0; j < 10000; j++);
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

    xhci.max_slots = (hcsparams1 & 0xFF);
    xhci.max_ports = (hcsparams1 >> 24) & 0xFF;
    xhci.max_scratchpad = (hcsparams2 >> 21) & 0x1F;
    uint32_t db_offset = xhci_read32(XHCI_DBOFF) & 0xFFFFFFFF;
    uint32_t rts_offset = xhci_read32(XHCI_RTSOFF) & 0xFFFFFFFF;

    xhci.doorbell = (volatile uint32_t *)((uint8_t *)xhci.mmio + db_offset);
    xhci.runtime = (volatile uint32_t *)((uint8_t *)xhci.mmio + rts_offset);

    if (xhci_reset_controller() < 0) {
        console_puts("[XHCI] Controller reset failed!\n");
        return -1;
    }
    if (xhci_init_rings() < 0) {
        console_puts("[XHCI] Ring initialization failed!\n");
        return -1;
    }
    if (xhci_init_scratchpad() < 0) {
        console_puts("[XHCI] Scratchpad initialization failed!\n");
        return -1;
    }
    xhci_op_write32(XHCI_CONFIG, xhci.max_slots);
    if (xhci_start_controller() < 0) {
        console_puts("[XHCI] Controller start failed!\n");
        return -1;
    }

    console_puts("[XHCI] Controller initialized. Max slots: ");
    dbg_print_uint(xhci.max_slots);
    console_puts(", Max ports: ");
    dbg_print_uint(xhci.max_ports);
    console_puts("\n");

    // Wait 100ms for ports to stabilize
    usleep(100000);

    for (uint32_t port = 1; port <= xhci.max_ports; port++) {
        uint32_t portsc = xhci_read_portsc(port);
        if (!(portsc & XHCI_PORTSC_CCS)) continue;

        console_puts("[XHCI] Port ");
        dbg_print_uint(port);
        console_puts(" has device connected. Resetting...\n");

        if (xhci_reset_port(port) < 0) {
            console_puts("[XHCI] Port reset failed!\n");
            continue;
        }

        uint32_t slot;
        if (xhci_enable_slot(&slot) < 0) {
            console_puts("[XHCI] Enable slot failed!\n");
            continue;
        }

        uint32_t speed = (xhci_read_portsc(port) >> 10) & 0xF;
        console_puts("[XHCI] Slot ");
        dbg_print_uint(slot);
        console_puts(" enabled. Addressing device at speed ");
        dbg_print_uint(speed);
        console_puts("...\n");

        if (xhci_address_device(slot, port, speed) < 0) {
            console_puts("[XHCI] Address device failed!\n");
            xhci_send_cmd(TRB_DISABLE_SLOT, 0, (slot << 24));
            xhci.slot_enabled[slot] = 0;
            continue;
        }
        console_puts("[XHCI] Device addressed successfully!\n");
        usleep(10000); // 10ms recovery time after Set Address
    }

    return 0;
}

int xhci_init(void) {
    if (xhci.initialized) {
        return xhci.initialized == 1 ? 0 : -1;
    }
    int ret = xhci_init_internal();
    if (ret < 0) {
        xhci.initialized = 2; // Permanently mark as failed
    } else {
        xhci.initialized = 1; // Successfully initialized
    }
    return ret;
}

void xhci_poll(void) {
    if (!xhci.initialized) return;

    uint32_t usbsts = xhci_op_read32(XHCI_USBSTS);
    if (usbsts & XHCI_STS_EINT) {
        xhci_op_write32(XHCI_USBSTS, XHCI_STS_EINT);

        while (1) {
            uint32_t idx = xhci.event_ring_idx;
            xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[idx];
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

                if (slot > 0 && slot <= xhci.max_slots && ep > 0 && ep < XHCI_MAX_EP) {
                    if (code == 1 || code == 0x26) {
                        xhci.ep_has_data[slot][ep] = 1;
                    } else {
                        xhci.ep_has_data[slot][ep] = code;
                    }
                    
                    if (xhci.ep_data_buf[slot][ep]) {
                        // Re-enqueue the TRB to keep polling
                        uint32_t control = (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP;
                        xhci_post_transfer_trb(slot, ep, xhci.ep_data_buf_phys[slot][ep],
                                               xhci.ep_data_buf_len[slot][ep], control);
                        xhci_ring_doorbell(slot, ep);
                    }
                }
            }

            xhci_advance_event();
        }
    }
}

int xhci_device_count(void) {
    int count = 0;
    for (uint32_t i = 1; i <= xhci.max_slots; i++) {
        if (xhci.slot_enabled[i]) count++;
    }
    return count;
}

int xhci_device_slot(int idx) {
    for (uint32_t i = 1; i <= xhci.max_slots; i++) {
        if (xhci.slot_enabled[i]) {
            if (idx == 0) return (int)i;
            idx--;
        }
    }
    return -1;
}

int xhci_get_device_desc(int idx, usb_device_desc_t *desc) {
    if (!desc) return -1;
    int slot = xhci_device_slot(idx);
    if (slot < 0) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(64);
    if (!buf) return -1;

    int ret = xhci_control_transfer(slot,
        0x80, 6, 0x0100, 0, buf, 18);
    if (ret >= 0) memcpy(desc, buf, 18);
    kfree(buf);
    return ret;
}

extern void task_yield(void);

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static xhci_trb_t *xhci_get_or_create_ep_ring(uint32_t slot_id, uint32_t ep_id,
                                              uint32_t ep_type, uint32_t max_packet_size,
                                              uint32_t interval, uint64_t *out_phys) {
    if (slot_id <= 0 || slot_id > xhci.max_slots) return NULL;
    if (ep_id <= 0 || ep_id >= XHCI_MAX_EP) return NULL;

    if (xhci.ep_rings[slot_id][ep_id]) {
        if (out_phys) *out_phys = xhci.ep_rings_phys[slot_id][ep_id];
        return (xhci_trb_t *)xhci.ep_rings[slot_id][ep_id];
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

    xhci.ep_rings[slot_id][ep_id] = ring;
    xhci.ep_rings_phys[slot_id][ep_id] = ring_phys;
    xhci.ep_ring_idx[slot_id][ep_id] = 0;
    xhci.ep_ring_cycle[slot_id][ep_id] = 1;
    xhci.ep_configured[slot_id][ep_id] = 1;

    // For Interrupt IN endpoint, allocate persistent buffer and enqueue first TRB
    if (ep_id != 1 && ep_type == EP_CTX_INTERRUPT_IN) {
        uint64_t buf_phys = 0;
        // Allocate buffer of max_packet_size
        uint8_t *buf = (uint8_t *)xhci_alloc_aligned(max_packet_size, &buf_phys);
        if (buf) {
            memset(buf, 0, max_packet_size);
            xhci.ep_data_buf[slot_id][ep_id] = buf;
            xhci.ep_data_buf_phys[slot_id][ep_id] = buf_phys;
            xhci.ep_data_buf_len[slot_id][ep_id] = max_packet_size;
            xhci.ep_has_data[slot_id][ep_id] = 0;
            
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
    xhci_trb_t *ring = (xhci_trb_t *)xhci.ep_rings[slot_id][ep_id];
    uint64_t ring_phys = xhci.ep_rings_phys[slot_id][ep_id];
    uint32_t idx = xhci.ep_ring_idx[slot_id][ep_id];
    uint32_t cycle = xhci.ep_ring_cycle[slot_id][ep_id];

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
    xhci.ep_ring_idx[slot_id][ep_id] = idx;
    xhci.ep_ring_cycle[slot_id][ep_id] = cycle;
}

int xhci_control_transfer(int slot_id, uint8_t bmRequestType,
                          uint8_t bRequest, uint16_t wValue,
                          uint16_t wIndex, void *data, uint16_t wLength) {
    if (!xhci.initialized || slot_id <= 0 || (uint32_t)slot_id > xhci.max_slots) return -1;
    if (!xhci.slot_enabled[slot_id]) return -1;

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
    xhci.ep_has_data[slot_id][1] = 0;

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
    for (int i = 0; i < 5000000; i++) {
        xhci_poll();
        uint32_t code = xhci.ep_has_data[slot_id][1];
        if (code != 0) {
            xhci.ep_has_data[slot_id][1] = 0;
            if (code == 1 || code == 0x26) {
                if (data_buf && dir_in) memcpy(data, data_buf, wLength);
                ret_val = (int)wLength;
            } else {
                ret_val = -(int)code;
            }
            break;
        }
        task_yield();
    }

    if (data_buf) kfree(data_buf);
    return ret_val;
}

int xhci_bulk_transfer(int slot_id, int ep_addr, void *data, uint32_t len) {
    if (!xhci.initialized || !data || !len) return -1;
    if (slot_id <= 0 || (uint32_t)slot_id > xhci.max_slots) return -1;
    if (!xhci.slot_enabled[slot_id]) return -1;

    int ep_num = ep_addr & 0x0F;
    int is_in = (ep_addr & 0x80) ? 1 : 0;
    int ep_id = ep_num * 2 + is_in;

    uint32_t ep_type = is_in ? EP_CTX_BULK_IN : 2; /* EP_CTX_BULK_OUT = 2 */
    uint64_t ring_phys = 0;
    xhci_trb_t *ring = xhci_get_or_create_ep_ring((uint32_t)slot_id, ep_id, ep_type, 512, 0, &ring_phys);
    if (!ring) return -1;

    /* Data buffer for the transfer */
    uint64_t data_phys = 0;
    void *data_buf = xhci_alloc_aligned(len, &data_phys);
    if (!data_buf) return -1;

    if (is_in) {
        memset(data_buf, 0, len);
    } else {
        memcpy(data_buf, data, len);
    }

    // Clear completion flag
    xhci.ep_has_data[slot_id][ep_id] = 0;

    /* Build and post a Normal TRB pointing to the data buffer */
    uint32_t control = (TRB_NORMAL << 10) | TRB_IOC | TRB_ISP;
    xhci_post_transfer_trb((uint32_t)slot_id, ep_id, data_phys, len, control);

    /* Ring the doorbell to kick off the transfer */
    xhci_ring_doorbell((uint32_t)slot_id, ep_id);

    int ret_val = -1;
    uint64_t start_tsc = rdtsc();
    uint64_t timeout_ticks = 1000ULL * 1000000ULL; // 1-second timeout

    while (1) {
        xhci_poll();
        uint32_t code = xhci.ep_has_data[slot_id][ep_id];
        if (code != 0) {
            xhci.ep_has_data[slot_id][ep_id] = 0;
            if (code == 1 || code == 0x26) {
                if (is_in) {
                    memcpy(data, data_buf, len);
                }
                ret_val = (int)len;
            } else {
                ret_val = -(int)code;
            }
            break;
        }
        if (rdtsc() - start_tsc >= timeout_ticks) {
            break;
        }
        task_yield();
    }

    kfree(data_buf);
    return ret_val;
}

int xhci_interrupt_transfer(int slot_id, int ep_addr, void *data, uint32_t len, uint32_t interval) {
    if (!xhci.initialized || !data || !len) return -1;
    if (slot_id <= 0 || (uint32_t)slot_id > xhci.max_slots) return -1;
    if (!xhci.slot_enabled[slot_id]) return -1;

    int ep_num = ep_addr & 0x0F;
    int is_in = (ep_addr & 0x80) ? 1 : 0;
    int ep_id = ep_num * 2 + is_in;

    uint32_t max_packet = len < 8 ? 8 : len;
    if (max_packet > 64) max_packet = 64;

    uint64_t ring_phys = 0;
    xhci_trb_t *ring = xhci_get_or_create_ep_ring((uint32_t)slot_id, ep_id, EP_CTX_INTERRUPT_IN, max_packet, interval, &ring_phys);
    if (!ring) return -1;

    // Run xhci_poll to pull any new interrupt event
    xhci_poll();

    // Check if new data has arrived
    uint32_t code = xhci.ep_has_data[slot_id][ep_id];
    if (code != 0) {
        xhci.ep_has_data[slot_id][ep_id] = 0;
        if (code == 1 || code == 0x26) {
            uint32_t copy_len = len;
            if (copy_len > max_packet) copy_len = max_packet;
            memcpy(data, xhci.ep_data_buf[slot_id][ep_id], copy_len);
            return (int)copy_len;
        }
        return -(int)code;
    }

    return -1; // no data
}
