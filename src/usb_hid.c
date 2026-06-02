#include "usb_hid.h"
#include "xhci.h"
#include "core/heap.h"
#include "string.h"

static hid_device_t hid_devices[HID_MAX_DEVICES];
static int hid_count;

int hid_init(void) {
    hid_count = 0;
    memset(hid_devices, 0, sizeof(hid_devices));

    int dev_count = xhci_device_count();
    for (int i = 0; i < dev_count && hid_count < HID_MAX_DEVICES; i++) {
        usb_device_desc_t desc;
        if (xhci_get_device_desc(i, &desc) < 0) continue;

        if (desc.bDeviceClass == 0x03 || desc.bDeviceClass == 0x00) {
            hid_device_t *dev = &hid_devices[hid_count];
            dev->slot_id = i + 1;
            dev->ep_addr = 0x81;
            dev->max_packet_size = desc.bMaxPacketSize0;
            dev->active = 1;

            if (desc.bDeviceClass == 0x03) {
                dev->type = HID_KEYBOARD;
            } else {
                dev->type = HID_MOUSE;
            }
            dev->report_len = (dev->type == HID_MOUSE) ? 8 : 8;
            hid_count++;
        }
    }

    return hid_count;
}

int hid_device_count(void) {
    return hid_count;
}

int hid_get_keyboard_report(int idx, hid_kbd_report_t *report) {
    if (idx < 0 || idx >= hid_count) return -1;
    hid_device_t *dev = &hid_devices[idx];
    if (!dev->active || dev->type != HID_KEYBOARD) return -1;
    if (!report) return -1;

    int ret = xhci_interrupt_transfer(dev->slot_id, dev->ep_addr,
                                       report, sizeof(hid_kbd_report_t));
    return ret;
}

int hid_get_mouse_report(int idx, hid_mouse_report_t *report) {
    if (idx < 0 || idx >= hid_count) return -1;
    hid_device_t *dev = &hid_devices[idx];
    if (!dev->active || dev->type != HID_MOUSE) return -1;
    if (!report) return -1;

    int ret = xhci_interrupt_transfer(dev->slot_id, dev->ep_addr,
                                       report, sizeof(hid_mouse_report_t));
    return ret;
}

void hid_poll(void) {
    xhci_poll();
}