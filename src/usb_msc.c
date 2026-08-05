#include "usb_msc.h"
#include "xhci.h"
#include "core/heap.h"
#include "string.h"

static msc_device_t msc_devices[MSC_MAX_DEVICES];
static int msc_count;
static int msc_initialized;

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int msc_do_command(msc_device_t *dev, const uint8_t *cb, uint8_t cb_len,
                          void *data, uint32_t data_len, uint8_t dir_in) {
    if (!dev || !dev->active || !cb || !cb_len || cb_len > 16) return -1;
    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = MSC_CBW_SIGNATURE;
    cbw.dCBWTag = ++dev->tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir_in ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cb_len;
    memcpy(cbw.CBWCB, cb, cb_len);

    int ret = xhci_bulk_transfer_packet(
        dev->slot_id, dev->ep_out, &cbw, MSC_CBW_LENGTH,
        dev->ep_out_max_packet);
    if (ret != MSC_CBW_LENGTH) return -1;

    uint32_t transferred = data_len;
    if (data && data_len > 0) {
        if (dir_in) {
            ret = xhci_bulk_transfer_packet(
                dev->slot_id, dev->ep_in, data, data_len,
                dev->ep_in_max_packet);
        } else {
            ret = xhci_bulk_transfer_packet(
                dev->slot_id, dev->ep_out, data, data_len,
                dev->ep_out_max_packet);
        }
        if (ret < 0 || (uint32_t)ret > data_len) return -1;
        transferred = (uint32_t)ret;
    }

    msc_csw_t csw;
    ret = xhci_bulk_transfer_packet(
        dev->slot_id, dev->ep_in, &csw, MSC_CSW_LENGTH,
        dev->ep_in_max_packet);
    if (ret != MSC_CSW_LENGTH) return -1;

    if (csw.dCSWSignature != MSC_CSW_SIGNATURE) return -1;
    if (csw.dCSWTag != cbw.dCBWTag) return -1;
    if (csw.dCSWDataResidue > data_len) return -1;
    if (csw.bCSWStatus != 0) return -1;
    if (transferred != data_len - csw.dCSWDataResidue) return -1;

    return (int)transferred;
}

extern void console_puts(const char *s);
extern void console_putchar(char c);
static void dbg_print_uint(uint32_t v) {
    char buf[16];
    int n = 0;
    do { buf[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (n--) console_putchar(buf[n]);
}
static int msc_probe_config(int dev_idx, msc_device_t *dev) {
    int slot = xhci_device_slot(dev_idx);
    if (slot < 0 || !dev) return -1;

    uint8_t header[9];
    if (xhci_control_transfer(slot, 0x80, 6, 0x0200, 0,
                              header, sizeof(header)) < 0) return -1;
    uint16_t total = read_le16(header + 2);
    if (total < sizeof(header) || total > 4096U) return -1;

    uint8_t *config = (uint8_t *)kmalloc(total);
    if (!config) return -1;
    int ret = xhci_control_transfer(slot, 0x80, 6, 0x0200, 0,
                                    config, total);
    if (ret < (int)sizeof(header)) {
        kfree(config);
        return -1;
    }

    uint16_t got = ret < total ? (uint16_t)ret : total;
    uint8_t config_value = config[5];
    int current_msc = 0;
    int interface_num = -1;
    int ep_in = 0;
    int ep_out = 0;
    uint16_t ep_in_packet = 0;
    uint16_t ep_out_packet = 0;

    for (uint16_t off = 0; off + 2 <= got; ) {
        uint8_t len = config[off];
        uint8_t type = config[off + 1];
        if (len < 2 || off + len > got) break;

        if (type == 4 && len >= 9) {
            current_msc = config[off + 5] == 0x08 &&
                          config[off + 6] == 0x06 &&
                          config[off + 7] == 0x50;
            if (current_msc) interface_num = config[off + 2];
        } else if (type == 5 && len >= 7 && current_msc &&
                   (config[off + 3] & 0x03) == 0x02) {
            uint8_t address = config[off + 2];
            uint16_t packet = read_le16(config + off + 4) & 0x07FFU;
            if (address & 0x80) {
                ep_in = address;
                ep_in_packet = packet;
            } else {
                ep_out = address;
                ep_out_packet = packet;
            }
        }
        off += len;
    }

    if (!config_value || interface_num < 0 || !ep_in || !ep_out ||
        !ep_in_packet || !ep_out_packet ||
        xhci_control_transfer(slot, 0x00, 9, config_value, 0, 0, 0) < 0) {
        kfree(config);
        return -1;
    }
    kfree(config);

    memset(dev, 0, sizeof(*dev));
    dev->slot_id = slot;
    dev->interface_num = interface_num;
    dev->ep_in = ep_in;
    dev->ep_out = ep_out;
    dev->ep_in_max_packet = ep_in_packet;
    dev->ep_out_max_packet = ep_out_packet;
    dev->active = 1;
    dev->block_size = 512;

    uint8_t max_lun = 0;
    if (xhci_control_transfer(slot, 0xA1, 0xFE, 0,
                              (uint16_t)interface_num, &max_lun, 1) >= 0)
        dev->max_lun = max_lun;
    return 0;
}

int msc_init(void) {
    if (msc_initialized) return msc_count;
    msc_initialized = 1;
    msc_count = 0;
    memset(msc_devices, 0, sizeof(msc_devices));

    int dev_count = xhci_device_count();

    for (int i = 0; i < dev_count && msc_count < MSC_MAX_DEVICES; i++) {
        usb_device_desc_t desc;
        if (xhci_get_device_desc(i, &desc) < 0) continue;
        if (desc.bDeviceClass != 0x08 && desc.bDeviceClass != 0x00) continue;

        msc_device_t *dev = &msc_devices[msc_count];
        if (msc_probe_config(i, dev) < 0) continue;

        scsi_read_capacity_data_t cap_data;
        uint8_t cmd[10];
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x25;
        int ret = msc_do_command(dev, cmd, 10, (uint8_t *)&cap_data,
                                  sizeof(cap_data), 1);
        if (ret != (int)sizeof(cap_data)) {
            memset(dev, 0, sizeof(*dev));
            continue;
        }

        dev->block_size = read_be32((const uint8_t *)&cap_data.block_size);
        uint32_t last_lba = read_be32((const uint8_t *)&cap_data.lba);
        if (!dev->block_size || last_lba == 0xFFFFFFFFU) {
            memset(dev, 0, sizeof(*dev));
            continue;
        }
        dev->block_count = last_lba + 1U;

        msc_count++;
        console_puts("[MSC] storage ready: slot=");
        dbg_print_uint((uint32_t)dev->slot_id);
        console_puts(" blocks=");
        dbg_print_uint(dev->block_count);
        console_puts(" block_size=");
        dbg_print_uint(dev->block_size);
        console_puts("\n");
    }

    return msc_count;
}

int msc_device_count(void) {
    return msc_count;
}

int msc_read_sector(int dev_idx, uint32_t lba, uint8_t *buf, uint32_t count) {
    if (dev_idx < 0 || dev_idx >= msc_count) return -1;
    msc_device_t *dev = &msc_devices[dev_idx];
    if (!dev->active) return -1;
    if (!buf || !count || count > MSC_MAX_TRANSFER_SECTORS ||
        lba >= dev->block_count || count > dev->block_count - lba) return -1;

    uint64_t total64 = (uint64_t)count * dev->block_size;
    if (total64 > UINT32_MAX) return -1;
    uint32_t total = (uint32_t)total64;

    uint8_t cmd[10];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x28; /* READ(10) */
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)lba;
    cmd[7] = (uint8_t)(count >> 8);
    cmd[8] = (uint8_t)count;

    int ret = msc_do_command(dev, cmd, sizeof(cmd), buf, total, 1);
    return ret == (int)total ? ret : -1;
}

int msc_write_sector(int dev_idx, uint32_t lba, const uint8_t *buf, uint32_t count) {
    if (dev_idx < 0 || dev_idx >= msc_count) return -1;
    msc_device_t *dev = &msc_devices[dev_idx];
    if (!dev->active) return -1;
    if (!buf || !count || count > MSC_MAX_TRANSFER_SECTORS ||
        lba >= dev->block_count || count > dev->block_count - lba) return -1;

    uint64_t total64 = (uint64_t)count * dev->block_size;
    if (total64 > UINT32_MAX) return -1;
    uint32_t total = (uint32_t)total64;

    uint8_t cmd[10];
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x2A; /* WRITE(10) */
    cmd[2] = (uint8_t)(lba >> 24);
    cmd[3] = (uint8_t)(lba >> 16);
    cmd[4] = (uint8_t)(lba >> 8);
    cmd[5] = (uint8_t)lba;
    cmd[7] = (uint8_t)(count >> 8);
    cmd[8] = (uint8_t)count;

    int ret = msc_do_command(dev, cmd, sizeof(cmd), (void *)buf, total, 0);
    return ret == (int)total ? ret : -1;
}

uint32_t msc_get_block_count(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= msc_count) return 0;
    return msc_devices[dev_idx].block_count;
}

uint32_t msc_get_block_size(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= msc_count) return 0;
    return msc_devices[dev_idx].block_size;
}
