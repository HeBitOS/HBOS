#include "xhci.h"
#include "pci.h"
#include "core/pmm.h"
#include "core/vmm.h"
#include "core/heap.h"
#include "string.h"

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
#define EP_CTX_CONTROL   1
#define EP_CTX_BULK      2
#define EP_CTX_INTERRUPT 3
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

static uint32_t xhci_read32(uint32_t offset) {
    return xhci.mmio[offset / 4];
}

static void xhci_write32(uint32_t offset, uint32_t value) {
    xhci.mmio[offset / 4] = value;
}

static uint32_t xhci_read_portsc(uint32_t port) {
    return xhci.mmio[(PORTSC_OFFSET + (port - 1) * 16) / 4];
}

static void xhci_write_portsc(uint32_t port, uint32_t value) {
    xhci.mmio[(PORTSC_OFFSET + (port - 1) * 16) / 4] = value;
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

static void xhci_build_trb(xhci_trb_t *trb, uint64_t param, uint32_t status, uint32_t cycle) {
    trb->param0 = (uint32_t)(param & 0xFFFFFFFF);
    trb->param1 = (uint32_t)(param >> 32);
    trb->param2 = status;
    trb->param3 = 0;
    trb->status = 0;
    trb->cycle = cycle;
    trb->flags = 0;
    trb->_reserved = 0;
}

static int xhci_post_cmd(uint32_t trb_type, uint64_t param, uint32_t status) {
    uint32_t idx = xhci.cmd_ring_idx;
    xhci_trb_t *trb = &((xhci_trb_t *)xhci.cmd_ring)[idx];

    xhci_build_trb(trb, param, status | (trb_type << 10), xhci.cmd_ring_cycle);

    xhci.cmd_ring_idx++;
    if (xhci.cmd_ring_idx >= XHCI_CMD_RING_SIZE) {
        xhci.cmd_ring_idx = 0;
        xhci.cmd_ring_cycle ^= 1;
    }

    xhci_ring_doorbell(0, 0);
    return 0;
}

static int xhci_wait_cmd_resp(void) {
    for (int timeout = 0; timeout < 5000000; timeout++) {
        uint32_t idx = xhci.event_ring_idx;
        xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[idx];
        if ((evt->cycle & 1) != xhci.event_ring_cycle) continue;

        uint32_t evt_type = (evt->param2 >> 10) & 0x3F;
        if (evt_type == TRB_EV_CMD_COMP) {
            int code = (evt->param1 >> 24) & 0xFF;
            xhci.event_ring_idx++;
            if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
                xhci.event_ring_idx = 0;
                xhci.event_ring_cycle ^= 1;
            }
            return code == 1 ? 0 : -code;
        }

        xhci.event_ring_idx++;
        if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
            xhci.event_ring_idx = 0;
            xhci.event_ring_cycle ^= 1;
        }
    }
    return -1;
}

static int xhci_send_cmd(uint32_t trb_type, uint64_t param, uint32_t status) {
    xhci_post_cmd(trb_type, param, status);
    return xhci_wait_cmd_resp();
}

static int xhci_reset_controller(void) {
    uint32_t cmd = xhci_read32(XHCI_USBCMD);
    cmd &= ~XHCI_CMD_RS;
    xhci_write32(XHCI_USBCMD, cmd);

    for (int i = 0; i < 100000; i++) {
        if (xhci_read32(XHCI_USBSTS) & XHCI_STS_HCH) break;
        for (volatile int j = 0; j < 1000; j++);
    }

    uint32_t status = xhci_read32(XHCI_USBSTS);
    if (!(status & XHCI_STS_HCH)) return -1;

    xhci_write32(XHCI_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 100000; i++) {
        if (!(xhci_read32(XHCI_USBCMD) & XHCI_CMD_HCRST)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    if (xhci_read32(XHCI_USBCMD) & XHCI_CMD_HCRST) return -1;

    for (int i = 0; i < 100000; i++) {
        if (!(xhci_read32(XHCI_USBSTS) & XHCI_STS_CNR)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    return (xhci_read32(XHCI_USBSTS) & XHCI_STS_CNR) ? -1 : 0;
}

static int xhci_init_rings(void) {
    xhci.cmd_ring = (uint64_t *)xhci_alloc_aligned(
        XHCI_CMD_RING_SIZE * 16, &xhci.cmd_ring_phys);
    if (!xhci.cmd_ring) return -1;
    memset(xhci.cmd_ring, 0, XHCI_CMD_RING_SIZE * 16);
    xhci.cmd_ring_idx = 0;
    xhci.cmd_ring_cycle = 1;

    xhci.event_ring_seg = (uint64_t *)xhci_alloc_aligned(16, &xhci.event_ring_phys);
    if (!xhci.event_ring_seg) return -1;

    xhci.event_ring = (uint64_t *)xhci_alloc_aligned(
        XHCI_EVENT_RING_SIZE * 16, &xhci.event_ring_phys);
    if (!xhci.event_ring) return -1;
    memset(xhci.event_ring, 0, XHCI_EVENT_RING_SIZE * 16);
    xhci.event_ring_idx = 0;
    xhci.event_ring_cycle = 1;

    xhci.event_ring_seg[0] = xhci.event_ring_phys;
    xhci.event_ring_seg[1] = XHCI_EVENT_RING_SIZE;

    xhci_write32(XHCI_CRCR_LO, (uint32_t)(xhci.cmd_ring_phys & 0xFFFFFFFF));
    xhci_write32(XHCI_CRCR_HI, (uint32_t)(xhci.cmd_ring_phys >> 32));
    xhci_write32(XHCI_CRCR_LO + 1, 1);

    xhci_write32(0x38, (uint32_t)((uint64_t)(uintptr_t)xhci.event_ring_seg & 0xFFFFFFFF));
    xhci_write32(0x3C, (uint32_t)((uint64_t)(uintptr_t)xhci.event_ring_seg >> 32));
    xhci_write32(0x20, 0);
    xhci_write32(0x24, 0);

    return 0;
}

static int xhci_init_scratchpad(void) {
    xhci.dcbaa = (uint64_t *)xhci_alloc_aligned(
        XHCI_MAX_SLOTS * 8 + 16, &xhci.dcbaa_phys);
    if (!xhci.dcbaa) return -1;
    memset(xhci.dcbaa, 0, XHCI_MAX_SLOTS * 8 + 16);

    if (xhci.max_scratchpad > 0 && xhci.max_scratchpad <= XHCI_MAX_SCRATCHPAD_BUFFERS) {
        xhci.scratchpad_array = (uint64_t *)xhci_alloc_aligned(
            xhci.max_scratchpad * 8, &xhci.dcbaa_phys);
        if (xhci.scratchpad_array) {
            for (uint32_t i = 0; i < xhci.max_scratchpad; i++) {
                xhci.scratchpad_buffers[i] = (uint64_t *)xhci_alloc_aligned(
                    4096, &xhci.scratchpad_array[i]);
            }
            xhci.dcbaa[0] = (uint64_t)(uintptr_t)xhci.scratchpad_array;
        }
    }

    xhci_write32(XHCI_DCBAAP_LO, (uint32_t)(xhci.dcbaa_phys & 0xFFFFFFFF));
    xhci_write32(XHCI_DCBAAP_HI, (uint32_t)(xhci.dcbaa_phys >> 32));

    return 0;
}

static int xhci_start_controller(void) {
    uint32_t cmd = xhci_read32(XHCI_USBCMD);
    cmd |= XHCI_CMD_RS;
    xhci_write32(XHCI_USBCMD, cmd);

    for (int i = 0; i < 100000; i++) {
        if (!(xhci_read32(XHCI_USBSTS) & XHCI_STS_HCH)) return 0;
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
    uint64_t *ctx = (uint64_t *)xhci_alloc_aligned(CTX_SIZE * 32, &xhci.dcbaa[slot_id]);
    if (!ctx) return -1;
    memset(ctx, 0, CTX_SIZE * 32);
    xhci.device_context_base[slot_id] = ctx;
    xhci.dcbaa[slot_id] = (uint64_t)(uintptr_t)ctx;
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

static int xhci_address_device(uint32_t slot_id, uint32_t port, uint32_t speed) {
    (void)port;
    uint64_t *input_ctx = (uint64_t *)xhci_alloc_aligned(CTX_SIZE * 32, &xhci.dcbaa[slot_id]);
    if (!input_ctx) return -1;
    memset(input_ctx, 0, CTX_SIZE * 32);

    input_ctx[0] = (1 << 0) | (1 << 1);
    input_ctx[1] = 0;
    input_ctx[2] = (1 << 27) | (speed << 20) | (8 << 16);

    uint64_t input_phys = (uint64_t)(uintptr_t)input_ctx;
    int ret = xhci_send_cmd(TRB_ADDRESS_DEV, input_phys, (slot_id << 24));
    kfree((void *)input_ctx);
    return ret;
}

static int xhci_configure_endpoint(uint32_t slot_id, uint32_t ep_type,
                                   uint32_t ep_id, uint32_t max_packet_size,
                                   uint32_t interval, uint64_t ring_phys) {
    uint64_t *input_ctx = (uint64_t *)xhci_alloc_aligned(CTX_SIZE * 32, &xhci.dcbaa[slot_id]);
    if (!input_ctx) return -1;
    memset(input_ctx, 0, CTX_SIZE * 32);

    input_ctx[0] = (1 << 0) | (1 << (ep_id + 1));
    input_ctx[1] = 0;

    uint32_t ep_off = (ep_id + 1) * 4;
    input_ctx[ep_off + 0] = 0;
    input_ctx[ep_off + 1] = (ep_type << 3) | (max_packet_size << 16) |
                            (interval << 0) | (1 << 0);
    input_ctx[ep_off + 2] = (ring_phys & 0xFFFFFFFF) | (1 << 0);
    input_ctx[ep_off + 3] = (ring_phys >> 32);

    uint64_t input_phys = (uint64_t)(uintptr_t)input_ctx;
    int ret = xhci_send_cmd(TRB_CONFIGURE_EP, input_phys, (slot_id << 24));
    kfree((void *)input_ctx);
    return ret;
}

static uint64_t *xhci_alloc_transfer_ring(uint64_t *ring_phys) {
    return xhci_alloc_aligned(XHCI_TRB_RING_SIZE * 16, ring_phys);
}

int xhci_init(void) {
    pci_device_t dev;
    if (pci_find_class(XHCI_PCI_CLASS, XHCI_PCI_SUBCLASS, XHCI_PCI_PROGIF, &dev) < 0) {
        return -1;
    }

    memset(&xhci, 0, sizeof(xhci));

    uint32_t bar0 = pci_bar(dev.bus, dev.slot, dev.func, 0);
    if (!(bar0 & 0xFFFFFFF0)) return -1;
    xhci.mmio_phys = bar0 & 0xFFFFFFF0;

    pci_enable_bus_master_mmio(&dev);

    xhci.mmio = (volatile uint32_t *)vmm_map_mmio(xhci.mmio_phys, 0x10000);
    if (!xhci.mmio) return -1;

    uint8_t caplength = (uint8_t)(xhci_read32(XHCI_CAPLENGTH) & 0xFF);
    uint32_t hcsparams1 = xhci_read32(XHCI_HCSPARAMS1);
    uint32_t hcsparams2 = xhci_read32(XHCI_HCSPARAMS2);
    uint32_t hccparams1 = xhci_read32(XHCI_HCCPARAMS1);
    (void)caplength;
    (void)hccparams1;

    xhci.max_slots = (hcsparams1 & 0xFF);
    xhci.max_ports = (hcsparams1 >> 24) & 0xFF;
    xhci.max_scratchpad = (hcsparams2 >> 21) & 0x1F;
    uint32_t db_offset = xhci_read32(XHCI_DBOFF) & 0xFFFFFFFF;
    uint32_t rts_offset = xhci_read32(XHCI_RTSOFF) & 0xFFFFFFFF;

    xhci.doorbell = (volatile uint32_t *)((uint8_t *)xhci.mmio + db_offset);
    xhci.runtime = (volatile uint32_t *)((uint8_t *)xhci.mmio + rts_offset);

    if (xhci_reset_controller() < 0) return -1;
    if (xhci_init_rings() < 0) return -1;
    if (xhci_init_scratchpad() < 0) return -1;
    if (xhci_start_controller() < 0) return -1;

    for (uint32_t port = 1; port <= xhci.max_ports; port++) {
        uint32_t portsc = xhci_read_portsc(port);
        if (!(portsc & XHCI_PORTSC_CCS)) continue;

        if (xhci_reset_port(port) < 0) continue;

        uint32_t slot;
        if (xhci_enable_slot(&slot) < 0) continue;

        uint32_t speed = (xhci_read_portsc(port) >> 10) & 0xF;
        if (xhci_address_device(slot, port, speed) < 0) {
            xhci_send_cmd(TRB_DISABLE_SLOT, 0, (slot << 24));
            xhci.slot_enabled[slot] = 0;
            continue;
        }
    }

    xhci.initialized = 1;
    return 0;
}

void xhci_poll(void) {
    if (!xhci.initialized) return;

    uint32_t usbsts = xhci_read32(XHCI_USBSTS);
    if (usbsts & XHCI_STS_EINT) {
        xhci_write32(XHCI_USBSTS, XHCI_STS_EINT);

        while (1) {
            uint32_t idx = xhci.event_ring_idx;
            xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[idx];
            if ((evt->cycle & 1) != xhci.event_ring_cycle) break;

            uint32_t evt_type = (evt->param2 >> 10) & 0x3F;
            if (evt_type == TRB_EV_PORT_SC) {
                uint32_t port = (evt->param0 >> 24) & 0xFF;
                uint32_t portsc = xhci_read_portsc(port);
                if ((portsc & XHCI_PORTSC_CCS) && !(portsc & XHCI_PORTSC_PED)) {
                    xhci_reset_port(port);
                }
            }

            xhci.event_ring_idx++;
            if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
                xhci.event_ring_idx = 0;
                xhci.event_ring_cycle ^= 1;
            }
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

int xhci_get_device_desc(int idx, usb_device_desc_t *desc) {
    if (!desc) return -1;
    int slot = 0;
    for (uint32_t i = 1; i <= xhci.max_slots; i++) {
        if (xhci.slot_enabled[i]) {
            if (idx == 0) { slot = (int)i; break; }
            idx--;
        }
    }
    if (!slot) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(64);
    if (!buf) return -1;

    int ret = xhci_control_transfer(slot,
        0x80, 6, 0x0100, 0, buf, 18);
    if (ret >= 0) memcpy(desc, buf, 18);
    kfree(buf);
    return ret;
}

int xhci_control_transfer(int slot_id, uint8_t bmRequestType,
                          uint8_t bRequest, uint16_t wValue,
                          uint16_t wIndex, void *data, uint16_t wLength) {
    (void)wIndex;
    (void)data;
    (void)wLength;
    if (!xhci.initialized || slot_id <= 0 || (uint32_t)slot_id > xhci.max_slots) return -1;
    if (!xhci.slot_enabled[slot_id]) return -1;

    uint64_t ring_phys;
    uint64_t *ring = xhci_alloc_transfer_ring(&ring_phys);
    if (!ring) return -1;

    uint32_t idx = 0;
    uint32_t cycle = 1;

    uint32_t dir_in = (bmRequestType & 0x80) ? 1 : 0;
    uint32_t trt = wLength > 0 ? (dir_in ? TRB_TRT_IN_DATA : TRB_TRT_OUT_DATA) : TRB_TRT_NO_DATA;

    uint32_t setup_word = ((uint32_t)bmRequestType << 0) |
                          ((uint32_t)bRequest << 8) |
                          ((uint32_t)wValue << 16);

    xhci_build_trb(&((xhci_trb_t *)ring)[idx++], setup_word, 0, cycle);
    ((xhci_trb_t *)ring)[idx-1].param2 = 8 | (TRB_SETUP_STAGE << 10) | (trt << 16) | TRB_IDT;
    xhci_build_trb(&((xhci_trb_t *)ring)[idx++], 0, 0, cycle);
    ((xhci_trb_t *)ring)[idx-1].param2 = 0 | (TRB_STATUS_STAGE << 10) | TRB_IOC;

    int ret = xhci_configure_endpoint(slot_id, EP_CTX_CONTROL, 1, 8, 0, ring_phys);
    if (ret < 0) { kfree((void *)ring); return ret; }

    xhci_ring_doorbell(slot_id, 1);

    for (int i = 0; i < 5000000; i++) {
        uint32_t evt_idx = xhci.event_ring_idx;
        xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[evt_idx];
        if ((evt->cycle & 1) != xhci.event_ring_cycle) continue;
        uint32_t evt_type = (evt->param2 >> 10) & 0x3F;
        if (evt_type == TRB_EV_TRANSFER) {
            int code = (evt->param1 >> 24) & 0xFF;
            xhci.event_ring_idx++;
            if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
                xhci.event_ring_idx = 0;
                xhci.event_ring_cycle ^= 1;
            }
            kfree((void *)ring);
            return code == 1 ? (int)wLength : (code == 0x26 ? 0 : -code);
        }
        xhci.event_ring_idx++;
        if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
            xhci.event_ring_idx = 0;
            xhci.event_ring_cycle ^= 1;
        }
    }

    kfree((void *)ring);
    return -1;
}

int xhci_bulk_transfer(int slot_id, int ep_addr, void *data, uint32_t len) {
    if (!xhci.initialized) return -1;
    (void)slot_id;
    (void)ep_addr;
    (void)data;
    (void)len;
    return -1;
}

int xhci_interrupt_transfer(int slot_id, int ep_addr, void *data, uint32_t len) {
    if (!xhci.initialized || !data || !len) return -1;
    if (slot_id <= 0 || (uint32_t)slot_id > xhci.max_slots) return -1;
    if (!xhci.slot_enabled[slot_id]) return -1;

    int ep_id = (ep_addr & 0x0F) * 2 + 1; /* EP index: ep_num*2 + dir (IN=1) */

    uint64_t ring_phys;
    uint64_t *ring = xhci_alloc_transfer_ring(&ring_phys);
    if (!ring) return -1;

    /* Data buffer for the transfer */
    uint64_t data_phys;
    void *data_buf = xhci_alloc_aligned(len, &data_phys);
    if (!data_buf) { kfree((void *)ring); return -1; }
    memset(data_buf, 0, len);

    /* Build a Normal TRB pointing to the data buffer */
    xhci_trb_t *trb = (xhci_trb_t *)ring;
    trb->param0 = (uint32_t)(data_phys & 0xFFFFFFFF);
    trb->param1 = (uint32_t)(data_phys >> 32);
    trb->param2 = len | (TRB_NORMAL << 10) | TRB_IOC;
    trb->cycle = 1;

    /* Configure the interrupt endpoint with this transfer ring */
    int ret = xhci_configure_endpoint(slot_id, ep_id, 1, 64, 0, ring_phys);
    if (ret < 0) { kfree(data_buf); kfree((void *)ring); return -1; }

    /* Ring the doorbell to kick off the transfer */
    xhci_ring_doorbell(slot_id, ep_id);

    /* Poll for completion event */
    for (int i = 0; i < 2000000; i++) {
        uint32_t evt_idx = xhci.event_ring_idx;
        xhci_trb_t *evt = &((xhci_trb_t *)xhci.event_ring)[evt_idx];
        if ((evt->cycle & 1) != xhci.event_ring_cycle) continue;
        uint32_t evt_type = (evt->param2 >> 10) & 0x3F;
        if (evt_type == TRB_EV_TRANSFER) {
            int code = (evt->param1 >> 24) & 0xFF;
            /* Advance event ring */
            xhci.event_ring_idx++;
            if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
                xhci.event_ring_idx = 0;
                xhci.event_ring_cycle ^= 1;
            }
            if (code == 1 || code == 0x26) {
                memcpy(data, data_buf, len);
                kfree(data_buf);
                kfree((void *)ring);
                return (int)len;
            }
            kfree(data_buf);
            kfree((void *)ring);
            return -code;
        }
        /* Consume non-matching events */
        xhci.event_ring_idx++;
        if (xhci.event_ring_idx >= XHCI_EVENT_RING_SIZE) {
            xhci.event_ring_idx = 0;
            xhci.event_ring_cycle ^= 1;
        }
    }

    kfree(data_buf);
    kfree((void *)ring);
    return -1;
}