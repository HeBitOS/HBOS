/**
 * @file net_virtio.c
 * @brief VirtIO network driver — modern (virtio 1.0) PCI transport, polled.
 *
 * QEMU's virtio-net-pci exposes either device id 0x1041 (modern) or
 * 0x1000 (transitional legacy id) with VIRTIO_PCI_CAP_* vendor
 * capabilities.  Both expose the modern transport; this driver negotiates
 * VIRTIO_F_VERSION_1 + VIRTIO_NET_F_MAC + VIRTIO_NET_F_STATUS, sets up
 * the two default virtqueues (rx = 0, tx = 1) and drives them by polling
 * the used rings — the same model as the E1000/RTL8139/PCnet backends in
 * net.c, so no MSI-X or INTx service path is required.  A pure legacy
 * device without modern capabilities (no VIRTIO_F_VERSION_1) is rejected.
 */

#include "net_virtio.h"
#include "pci.h"
#include "string.h"
#include "core/vmm.h"
#include "core/heap.h"

/* ---- virtio PCI transport constants (virtio 1.0) ---- */
#define VIRTIO_PCI_CAP_ID_VNDR      0x09
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4

/* device status bits */
#define V_STATUS_ACK               0x01
#define V_STATUS_DRIVER            0x02
#define V_STATUS_DRIVER_OK         0x04
#define V_STATUS_FEATURES_OK       0x08
#define V_STATUS_FAILED            0x80

/* feature bits */
#define VIRTIO_NET_F_CSUM          0
#define VIRTIO_NET_F_GUEST_CSUM    1
#define VIRTIO_NET_F_MTU           3
#define VIRTIO_NET_F_MAC           5
#define VIRTIO_NET_F_STATUS        16
#define VIRTIO_NET_F_CTRL_VQ       17
#define VIRTIO_NET_F_MQ            22
#define VIRTIO_F_VERSION_1         32

#define VIRTIO_NET_S_LINK_UP       1u

/* virtqueue descriptor flags */
#define VIRTQ_DESC_F_NEXT          1
#define VIRTQ_DESC_F_WRITE         2

#define VQ_RX                      0
#define VQ_TX                      1
#define VIRTIO_NET_VQS             2

#define VIRTIO_MAX_RING            256u   /* QEMU default ring size */
#define VIRTIO_BUF_COUNT           32u    /* buffers installed per queue */
#define VIRTIO_PKT_SIZE            2048u  /* matches PKT_SIZE in net.c */
#define VIRTIO_NET_HDR_SIZE        12u    /* struct virtio_net_hdr */

/* common config register offsets (virtio 1.0) */
#define CFG_DEVICE_FEATURE_SEL     0
#define CFG_DEVICE_FEATURE         4
#define CFG_DRIVER_FEATURE_SEL     8
#define CFG_DRIVER_FEATURE         12
#define CFG_MSIX_CONFIG            16
#define CFG_NUM_QUEUES             18
#define CFG_DEVICE_STATUS          20
#define CFG_CONFIG_GENERATION      21
#define CFG_QUEUE_SEL              22
#define CFG_QUEUE_SIZE             24
#define CFG_QUEUE_MSIX_VECTOR      26
#define CFG_QUEUE_ENABLE           28
#define CFG_QUEUE_NOTIFY_OFF       30
#define CFG_QUEUE_DESC             32
#define CFG_QUEUE_DRIVER           40
#define CFG_QUEUE_DEVICE           48

/* device config offsets when MAC + STATUS are negotiated */
#define DEVCFG_MAC                 0
#define DEVCFG_STATUS              6

#define VIRTIO_MSI_NO_VECTOR       0xFFFF

/* virtqueue memory layouts (little-endian; x86 host) */
struct vq_desc {
    volatile uint64_t addr;
    volatile uint32_t len;
    volatile uint16_t flags;
    volatile uint16_t next;
};
struct vq_avail {
    volatile uint16_t flags;
    volatile uint16_t idx;
    volatile uint16_t ring[];
};
struct vq_used_elem {
    volatile uint32_t id;
    volatile uint32_t len;
};
struct vq_used {
    volatile uint16_t flags;
    volatile uint16_t idx;
    volatile struct vq_used_elem ring[];
};

typedef struct {
    /* mapped transport regions */
    volatile uint8_t *common;
    volatile uint8_t *devcfg;
    volatile uint8_t *notify;          /* notify cap bar + cap offset */
    uint32_t notify_off_multiplier;
    uint64_t features;                 /* negotiated features */
    uint16_t qsize;                    /* device ring size (power of two) */

    struct vq_desc *desc[VIRTIO_NET_VQS];
    struct vq_avail *avail[VIRTIO_NET_VQS];
    struct vq_used  *used[VIRTIO_NET_VQS];
    uint16_t notify_off[VIRTIO_NET_VQS];

    /* rx bookkeeping */
    uint16_t rx_avail_shadow;          /* descriptors handed to device */
    uint16_t rx_last_used;             /* used entries consumed */

    /* tx bookkeeping */
    uint16_t tx_free[VIRTIO_BUF_COUNT]; /* free descriptor id stack */
    uint16_t tx_free_count;
    uint16_t tx_avail_shadow;
    uint16_t tx_last_used;

    /* driver-owned packet buffers (identity-mapped for DMA) */
    uint8_t rx_buf[VIRTIO_BUF_COUNT][VIRTIO_PKT_SIZE]
        __attribute__((aligned(16)));
    uint8_t tx_buf[VIRTIO_BUF_COUNT][VIRTIO_PKT_SIZE]
        __attribute__((aligned(16)));

    int ready;
} virtio_net_t;

static virtio_net_t vnet;

static inline void virtio_mb(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/* ---- common config accessors ---- */
static inline uint8_t  vc_read8(uint32_t off)  { return *(volatile uint8_t  *)(vnet.common + off); }
static inline void     vc_write8(uint32_t off, uint8_t v)  { *(volatile uint8_t  *)(vnet.common + off) = v; }
static inline uint16_t vc_read16(uint32_t off) { return *(volatile uint16_t *)(vnet.common + off); }
static inline void     vc_write16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(vnet.common + off) = v; }
static inline uint32_t vc_read32(uint32_t off) { return *(volatile uint32_t *)(vnet.common + off); }
static inline void     vc_write32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(vnet.common + off) = v; }
static inline void     vc_write64(uint32_t off, uint64_t v) { *(volatile uint64_t *)(vnet.common + off) = v; }

/**
 * @brief Walk the PCI vendor-specific capability chain.
 *
 * Unaligned 32-bit fields (offset/length/notify multiplier) are assembled
 * from 16-bit reads because pci_read32() is dword-aligned only.
 */
static int cap_locate(const pci_device_t *dev, uint8_t want_type,
                      uint8_t *bar, uint32_t *off, uint32_t *len,
                      uint32_t *notify_multiplier) {
    uint8_t cap = pci_read8(dev->bus, dev->slot, dev->func, 0x34);
    while (cap != 0 && cap != 0xFF) {
        uint8_t id = pci_read8(dev->bus, dev->slot, dev->func, cap);
        if (id == VIRTIO_PCI_CAP_ID_VNDR) {
            uint8_t type = pci_read8(dev->bus, dev->slot, dev->func, cap + 3);
            if (type == want_type) {
                if (bar) *bar = pci_read8(dev->bus, dev->slot, dev->func, cap + 4);
                /* QEMU (observed on 11.1 transitional virtio-pci) stores the
                 * offset/length/multiplier fields at +8/+12/+16: two padding
                 * bytes follow the bar field.  Field values were verified
                 * against raw config-space dumps (notify offset 0x3000,
                 * length 0x1000, multiplier 4). */
                if (off) {
                    *off = (uint32_t)pci_read16(dev->bus, dev->slot, dev->func, cap + 8) |
                           ((uint32_t)pci_read16(dev->bus, dev->slot, dev->func, cap + 10) << 16);
                }
                if (len) {
                    *len = (uint32_t)pci_read16(dev->bus, dev->slot, dev->func, cap + 12) |
                           ((uint32_t)pci_read16(dev->bus, dev->slot, dev->func, cap + 14) << 16);
                }
                if (notify_multiplier && type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                    *notify_multiplier =
                        (uint32_t)pci_read16(dev->bus, dev->slot, dev->func, cap + 16) |
                        ((uint32_t)pci_read16(dev->bus, dev->slot, dev->func, cap + 18) << 16);
                }
                return 0;
            }
        }
        cap = pci_read8(dev->bus, dev->slot, dev->func, cap + 1);
    }
    return -1;
}

/* cached virtual mappings per BAR (64-bit BARs live above 4 GiB and are
 * not identity-mapped; vmm_map_mmio picks kernel virtual addresses) */
static volatile uint8_t *bar_virt[6];
static int bar_mapped[6];

/** @brief Map a memory BAR once and return its virtual base. */
static volatile uint8_t *map_bar(const pci_device_t *dev, uint8_t bar_idx) {
    if (bar_idx > 5) return 0;
    if (bar_mapped[bar_idx]) return bar_virt[bar_idx];
    int is_io = 0;
    uint64_t base = pci_bar_address(dev->bus, dev->slot, dev->func, bar_idx, &is_io);
    if (is_io || base == 0) return 0;
    volatile uint8_t *v =
        (volatile uint8_t *)vmm_map_mmio(base, 0x10000u);
    if (!v) return 0;
    bar_virt[bar_idx] = v;
    bar_mapped[bar_idx] = 1;
    return v;
}

/** @brief Write a queue-notify (kick) word for the given virtqueue. */
static void virtio_kick(uint16_t q) {
    uint32_t off = (uint32_t)vnet.notify_off[q] * vnet.notify_off_multiplier;
    *(volatile uint16_t *)(vnet.notify + off) = q;
}

/** @brief Configure one virtqueue and enable it. */
static int virtio_setup_queue(uint16_t q) {
    vc_write16(CFG_QUEUE_SEL, q);
    uint16_t qsize = vc_read16(CFG_QUEUE_SIZE);
    if (qsize < 2 || qsize > VIRTIO_MAX_RING || (qsize & (qsize - 1)) != 0)
        return -1;
    vnet.qsize = qsize;

    size_t desc_bytes  = (size_t)qsize * sizeof(struct vq_desc);
    size_t avail_bytes = sizeof(uint16_t) * 2 + (size_t)qsize * sizeof(uint16_t);
    size_t used_bytes  = sizeof(uint16_t) * 2 + (size_t)qsize * sizeof(struct vq_used_elem);

    struct vq_desc *desc  = (struct vq_desc *)kmalloc_aligned(desc_bytes, 16);
    struct vq_avail *avail = (struct vq_avail *)kmalloc_aligned(avail_bytes, 2);
    struct vq_used *used  = (struct vq_used *)kmalloc_aligned(used_bytes, 4);
    if (!desc || !avail || !used) return -1;
    memset(desc, 0, desc_bytes);
    memset(avail, 0, avail_bytes);
    memset(used, 0, used_bytes);

    vnet.desc[q]  = desc;
    vnet.avail[q] = avail;
    vnet.used[q]  = used;

    vc_write64(CFG_QUEUE_DESC, (uint64_t)(uintptr_t)desc);
    vc_write64(CFG_QUEUE_DRIVER, (uint64_t)(uintptr_t)avail);
    vc_write64(CFG_QUEUE_DEVICE, (uint64_t)(uintptr_t)used);
    vc_write16(CFG_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
    virtio_mb();
    vc_write16(CFG_QUEUE_ENABLE, 1);
    vnet.notify_off[q] = vc_read16(CFG_QUEUE_NOTIFY_OFF);
    return 0;
}

/** @brief Hand one receive descriptor back to the device. */
static void virtio_refill_rx(uint16_t id) {
    struct vq_desc *d = &vnet.desc[VQ_RX][id];
    d->addr  = (uint64_t)(uintptr_t)vnet.rx_buf[id];
    d->len   = VIRTIO_PKT_SIZE;
    d->flags = VIRTQ_DESC_F_WRITE;
    d->next  = 0;
    virtio_mb();
    uint16_t slot = (uint16_t)(vnet.rx_avail_shadow & (vnet.qsize - 1));
    vnet.avail[VQ_RX]->ring[slot] = id;
    virtio_mb();
    vnet.avail[VQ_RX]->idx = (uint16_t)(vnet.rx_avail_shadow + 1);
    vnet.rx_avail_shadow++;
    virtio_mb();
    virtio_kick(VQ_RX);
}

/** @brief Pre-fill the whole receive ring. */
static void virtio_rx_fill_all(void) {
    for (uint16_t i = 0; i < VIRTIO_BUF_COUNT; i++) {
        vnet.desc[VQ_RX][i].addr  = (uint64_t)(uintptr_t)vnet.rx_buf[i];
        vnet.desc[VQ_RX][i].len   = VIRTIO_PKT_SIZE;
        vnet.desc[VQ_RX][i].flags = VIRTQ_DESC_F_WRITE;
        vnet.desc[VQ_RX][i].next  = 0;
        vnet.avail[VQ_RX]->ring[i] = i;
    }
    virtio_mb();
    vnet.avail[VQ_RX]->idx = VIRTIO_BUF_COUNT;
    vnet.rx_avail_shadow = VIRTIO_BUF_COUNT;
    vnet.rx_last_used = 0;
    virtio_mb();
    virtio_kick(VQ_RX);
}

/** @brief Reclaim transmit descriptors whose used entries have arrived. */
static void virtio_reap_tx(void) {
    while (vnet.tx_last_used != vnet.used[VQ_TX]->idx) {
        uint16_t i = (uint16_t)(vnet.tx_last_used & (vnet.qsize - 1));
        uint32_t id = vnet.used[VQ_TX]->ring[i].id;
        vnet.tx_last_used++;
        if (id < VIRTIO_BUF_COUNT && vnet.tx_free_count < VIRTIO_BUF_COUNT)
            vnet.tx_free[vnet.tx_free_count++] = (uint16_t)id;
    }
}

int virtio_net_init_hw(const pci_device_t *pdev, uint8_t mac[6],
                       bool *mac_valid, bool *link) {
    if (!pdev) return -1;
    memset(&vnet, 0, sizeof(vnet));
    if (mac_valid) *mac_valid = false;
    if (link) *link = false;

    /* 1. locate and map the four modern-transport capabilities */
    uint8_t bar;
    uint32_t off, len, mult = 0;
    if (cap_locate(pdev, VIRTIO_PCI_CAP_COMMON_CFG, &bar, &off, &len, 0) < 0)
        return -1;
    volatile uint8_t *common = map_bar(pdev, bar);
    if (!common) return -1;
    vnet.common = common + off;

    if (cap_locate(pdev, VIRTIO_PCI_CAP_NOTIFY_CFG, &bar, &off, &len, &mult) < 0)
        return -1;
    volatile uint8_t *nb = map_bar(pdev, bar);
    if (!nb) return -1;
    vnet.notify = nb + off;
    vnet.notify_off_multiplier = mult ? mult : 2u;

    if (cap_locate(pdev, VIRTIO_PCI_CAP_DEVICE_CFG, &bar, &off, &len, 0) < 0)
        return -1;
    volatile uint8_t *dc = map_bar(pdev, bar);
    if (!dc) return -1;
    vnet.devcfg = dc + off;

    pci_enable_bus_master_mmio(pdev);

    /* 2. reset, acknowledge, enter driver mode */
    vc_write8(CFG_DEVICE_STATUS, 0);
    virtio_mb();
    uint32_t t = 0;
    while (vc_read8(CFG_DEVICE_STATUS) != 0 && t++ < 100000) { }
    vc_write8(CFG_DEVICE_STATUS, V_STATUS_ACK);
    vc_write8(CFG_DEVICE_STATUS, V_STATUS_ACK | V_STATUS_DRIVER);
    virtio_mb();

    /* 3. feature negotiation */
    vc_write32(CFG_DEVICE_FEATURE_SEL, 0);
    uint64_t devf = vc_read32(CFG_DEVICE_FEATURE);
    vc_write32(CFG_DEVICE_FEATURE_SEL, 1);
    devf |= (uint64_t)vc_read32(CFG_DEVICE_FEATURE) << 32;
    if (!(devf & (1ULL << VIRTIO_F_VERSION_1))) return -1; /* legacy transport */

    uint64_t want = (1ULL << VIRTIO_F_VERSION_1) |
                    (1ULL << VIRTIO_NET_F_MAC) |
                    (1ULL << VIRTIO_NET_F_STATUS);
    uint64_t got = devf & want;
    vnet.features = got;
    vc_write32(CFG_DRIVER_FEATURE_SEL, 0);
    vc_write32(CFG_DRIVER_FEATURE, (uint32_t)got);
    vc_write32(CFG_DRIVER_FEATURE_SEL, 1);
    vc_write32(CFG_DRIVER_FEATURE, (uint32_t)(got >> 32));
    virtio_mb();

    vc_write8(CFG_DEVICE_STATUS,
              V_STATUS_ACK | V_STATUS_DRIVER | V_STATUS_FEATURES_OK);
    virtio_mb();
    if (!(vc_read8(CFG_DEVICE_STATUS) & V_STATUS_FEATURES_OK)) return -1;

    /* 4. virtqueues: rx = 0, tx = 1 */
    if (virtio_setup_queue(VQ_RX) < 0) return -1;
    if (virtio_setup_queue(VQ_TX) < 0) return -1;
    for (uint16_t i = 0; i < VIRTIO_BUF_COUNT; i++) vnet.tx_free[i] = i;
    vnet.tx_free_count = VIRTIO_BUF_COUNT;
    vnet.tx_avail_shadow = 0;
    vnet.tx_last_used = 0;
    virtio_rx_fill_all();

    /* 5. driver ready */
    vc_write8(CFG_DEVICE_STATUS,
              V_STATUS_ACK | V_STATUS_DRIVER | V_STATUS_FEATURES_OK |
              V_STATUS_DRIVER_OK);
    virtio_mb();

    /* 6. MAC address and link state */
    if (got & (1ULL << VIRTIO_NET_F_MAC)) {
        for (int i = 0; i < 6; i++)
            mac[i] = *(volatile uint8_t *)(vnet.devcfg + DEVCFG_MAC + i);
        if (mac_valid) {
            *mac_valid = (mac[0] | mac[1] | mac[2] |
                          mac[3] | mac[4] | mac[5]) != 0;
        }
    }
    if (link) {
        if (got & (1ULL << VIRTIO_NET_F_STATUS)) {
            uint16_t st = *(volatile uint16_t *)(vnet.devcfg + DEVCFG_STATUS);
            *link = (st & VIRTIO_NET_S_LINK_UP) != 0;
        }
        /* Polled driver mirrors net.c's e1000 behavior: allow traffic
         * immediately instead of waiting for a link-status interrupt. */
        *link = true;
    }

    vnet.ready = 1;
    return 0;
}

int virtio_net_send(const void *frame, uint16_t len) {
    if (!vnet.ready || !frame || len == 0 || len > VIRTIO_PKT_SIZE)
        return -1;

    /* Wait briefly for a transmit descriptor (drains used ring). */
    uint32_t wait = 0;
    while (vnet.tx_free_count == 0 && wait++ < 1000000) virtio_reap_tx();
    if (vnet.tx_free_count == 0) return -1;

    uint16_t id = vnet.tx_free[--vnet.tx_free_count];
    /* QEMU's virtio-net transport consumes a 12-byte virtio_net_hdr from
     * the front of every TX buffer in virtio-1 mode (verified empirically
     * against QEMU 11.1 / slirp; mirrored on RX below).  An all-zero header
     * means "no checksum/segmentation offloads". */
    memset(vnet.tx_buf[id], 0, VIRTIO_NET_HDR_SIZE);
    memcpy(vnet.tx_buf[id] + VIRTIO_NET_HDR_SIZE, frame, len);

    struct vq_desc *d = &vnet.desc[VQ_TX][id];
    d->addr  = (uint64_t)(uintptr_t)vnet.tx_buf[id];
    d->len   = (uint32_t)len + VIRTIO_NET_HDR_SIZE;
    d->flags = 0;             /* device reads this single descriptor */
    d->next  = 0;
    virtio_mb();
    uint16_t slot = (uint16_t)(vnet.tx_avail_shadow & (vnet.qsize - 1));
    vnet.avail[VQ_TX]->ring[slot] = id;
    virtio_mb();
    vnet.avail[VQ_TX]->idx = (uint16_t)(vnet.tx_avail_shadow + 1);
    vnet.tx_avail_shadow++;
    virtio_mb();
    virtio_kick(VQ_TX);
    return 0;
}

int virtio_net_poll(int (*cb)(const uint8_t *pkt, uint16_t len, void *arg),
                    void *arg, uint32_t spins,
                    uint64_t *rx_packets, uint64_t *rx_bytes,
                    uint64_t *rx_dropped) {
    if (!vnet.ready) return -1;
    int result = 0;
    for (uint32_t s = 0; s < spins; s++) {
        if (vnet.rx_last_used == vnet.used[VQ_RX]->idx) break;

        uint16_t i = (uint16_t)(vnet.rx_last_used & (vnet.qsize - 1));
        uint32_t id = vnet.used[VQ_RX]->ring[i].id;
        uint32_t plen = vnet.used[VQ_RX]->ring[i].len;
        vnet.rx_last_used++;

        /* virtio-1 mode prepends a 12-byte virtio_net_hdr to received
         * packets; strip it before handing the frame to the stack. */
        uint32_t frame_len = plen > VIRTIO_NET_HDR_SIZE
                                 ? plen - VIRTIO_NET_HDR_SIZE : 0;
        const uint8_t *frame = vnet.rx_buf[id] + VIRTIO_NET_HDR_SIZE;
        if (id >= VIRTIO_BUF_COUNT || frame_len == 0 ||
            frame_len > VIRTIO_PKT_SIZE) {
            if (rx_dropped) (*rx_dropped)++;
        } else if (cb) {
            if (rx_packets) (*rx_packets)++;
            if (rx_bytes) (*rx_bytes) += frame_len;
            result = cb(frame, (uint16_t)frame_len, arg);
        } else {
            if (rx_dropped) (*rx_dropped)++;
        }

        /* Always hand the buffer back to the device. */
        virtio_refill_rx((uint16_t)id);
        if (result) return result;
    }
    return 0;
}
