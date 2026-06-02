#include "usb_msc.h"
#include "xhci.h"
#include "core/heap.h"
#include "string.h"

static msc_device_t msc_devices[MSC_MAX_DEVICES];
static int msc_count;

static int msc_do_command(msc_device_t *dev, uint8_t *cb, uint8_t cb_len,
                          uint8_t *data, uint32_t data_len, uint8_t dir_in) {
    msc_cbw_t cbw;
    memset(&cbw, 0, sizeof(cbw));
    cbw.dCBWSignature = MSC_CBW_SIGNATURE;
    cbw.dCBWTag = ++dev->tag;
    cbw.dCBWDataTransferLength = data_len;
    cbw.bmCBWFlags = dir_in ? 0x80 : 0x00;
    cbw.bCBWLUN = 0;
    cbw.bCBWCBLength = cb_len;
    if (cb_len <= 16) memcpy(cbw.CBWCB, cb, cb_len);

    int ret = xhci_bulk_transfer(dev->slot_id, dev->ep_out, &cbw, MSC_CBW_LENGTH);
    if (ret < 0) return -1;

    if (data && data_len > 0) {
        if (dir_in) {
            ret = xhci_bulk_transfer(dev->slot_id, dev->ep_in, data, data_len);
        } else {
            ret = xhci_bulk_transfer(dev->slot_id, dev->ep_out, data, data_len);
        }
        if (ret < 0) return -1;
    }

    msc_csw_t csw;
    ret = xhci_bulk_transfer(dev->slot_id, dev->ep_in, &csw, MSC_CSW_LENGTH);
    if (ret < 0) return -1;

    if (csw.dCSWSignature != MSC_CSW_SIGNATURE) return -1;
    if (csw.bCSWStatus != 0) return -1;

    return (int)data_len;
}

int msc_init(void) {
    msc_count = 0;
    memset(msc_devices, 0, sizeof(msc_devices));

    int dev_count = xhci_device_count();
    for (int i = 0; i < dev_count && msc_count < MSC_MAX_DEVICES; i++) {
        usb_device_desc_t desc;
        if (xhci_get_device_desc(i, &desc) < 0) continue;

        if (desc.bDeviceClass != 0x08) continue;

        msc_device_t *dev = &msc_devices[msc_count];
        dev->slot_id = i + 1;
        dev->ep_in = 0x81;
        dev->ep_out = 0x02;
        dev->tag = 0;
        dev->active = 1;
        dev->max_lun = 0;
        dev->block_size = 512;
        dev->block_count = 0;

        scsi_read_capacity_data_t cap_data;
        uint8_t cmd[10];
        memset(cmd, 0, sizeof(cmd));
        cmd[0] = 0x25;
        int ret = msc_do_command(dev, cmd, 10, (uint8_t *)&cap_data,
                                  sizeof(cap_data), 1);
        if (ret >= 0) {
            dev->block_size = (cap_data.block_size >> 24) |
                              ((cap_data.block_size >> 8) & 0xFF00) |
                              ((cap_data.block_size << 8) & 0xFF0000) |
                              (cap_data.block_size << 24);
            if (dev->block_size == 0) dev->block_size = 512;
            dev->block_count = (cap_data.lba >> 24) |
                               ((cap_data.lba >> 8) & 0xFF00) |
                               ((cap_data.lba << 8) & 0xFF0000) |
                               (cap_data.lba << 24);
        }

        msc_count++;
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
    if (!buf) return -1;

    uint32_t total = count * dev->block_size;
    uint8_t *cmd_buf = (uint8_t *)kmalloc(16);
    if (!cmd_buf) return -1;

    memset(cmd_buf, 0, 16);
    if (count <= 0xFFFF) {
        cmd_buf[0] = 0x28;
        cmd_buf[2] = (uint8_t)(lba >> 24);
        cmd_buf[3] = (uint8_t)(lba >> 16);
        cmd_buf[4] = (uint8_t)(lba >> 8);
        cmd_buf[5] = (uint8_t)(lba);
        cmd_buf[7] = (uint8_t)(count >> 8);
        cmd_buf[8] = (uint8_t)(count);
    } else {
        cmd_buf[0] = 0x88;
        cmd_buf[2] = (uint8_t)(lba >> 24);
        cmd_buf[3] = (uint8_t)(lba >> 16);
        cmd_buf[4] = (uint8_t)(lba >> 8);
        cmd_buf[5] = (uint8_t)(lba);
        cmd_buf[6] = (uint8_t)(count >> 24);
        cmd_buf[7] = (uint8_t)(count >> 16);
        cmd_buf[8] = (uint8_t)(count >> 8);
        cmd_buf[9] = (uint8_t)(count);
    }

    int ret = msc_do_command(dev, cmd_buf, 16, buf, total, 1);
    kfree(cmd_buf);
    return ret;
}

int msc_write_sector(int dev_idx, uint32_t lba, const uint8_t *buf, uint32_t count) {
    if (dev_idx < 0 || dev_idx >= msc_count) return -1;
    msc_device_t *dev = &msc_devices[dev_idx];
    if (!dev->active) return -1;
    if (!buf) return -1;

    uint32_t total = count * dev->block_size;
    uint8_t *cmd_buf = (uint8_t *)kmalloc(16);
    if (!cmd_buf) return -1;

    memset(cmd_buf, 0, 16);
    if (count <= 0xFFFF) {
        cmd_buf[0] = 0x2A;
        cmd_buf[2] = (uint8_t)(lba >> 24);
        cmd_buf[3] = (uint8_t)(lba >> 16);
        cmd_buf[4] = (uint8_t)(lba >> 8);
        cmd_buf[5] = (uint8_t)(lba);
        cmd_buf[7] = (uint8_t)(count >> 8);
        cmd_buf[8] = (uint8_t)(count);
    } else {
        cmd_buf[0] = 0x8A;
        cmd_buf[2] = (uint8_t)(lba >> 24);
        cmd_buf[3] = (uint8_t)(lba >> 16);
        cmd_buf[4] = (uint8_t)(lba >> 8);
        cmd_buf[5] = (uint8_t)(lba);
        cmd_buf[6] = (uint8_t)(count >> 24);
        cmd_buf[7] = (uint8_t)(count >> 16);
        cmd_buf[8] = (uint8_t)(count >> 8);
        cmd_buf[9] = (uint8_t)(count);
    }

    uint8_t *wr_buf = (uint8_t *)kmalloc(total);
    if (!wr_buf) { kfree(cmd_buf); return -1; }
    memcpy(wr_buf, buf, total);

    int ret = msc_do_command(dev, cmd_buf, 16, wr_buf, total, 0);
    kfree(wr_buf);
    kfree(cmd_buf);
    return ret;
}

uint32_t msc_get_block_count(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= msc_count) return 0;
    return msc_devices[dev_idx].block_count;
}

uint32_t msc_get_block_size(int dev_idx) {
    if (dev_idx < 0 || dev_idx >= msc_count) return 0;
    return msc_devices[dev_idx].block_size;
}