#include "usb_hid.h"
#include "xhci.h"
#include "core/heap.h"
#include "string.h"

static hid_device_t hid_devices[HID_MAX_DEVICES];
static int hid_count;
static hid_kbd_report_t last_report;
static int usb_kbd_initialized;
static int usb_kbd_index = -1;

#define USB_KEY_UP      0x100
#define USB_KEY_DOWN    0x101
#define USB_KEY_LEFT    0x102
#define USB_KEY_RIGHT   0x103
#define USB_KEY_PGUP    0x104
#define USB_KEY_PGDWN   0x105
#define USB_KEY_HOME    0x106
#define USB_KEY_END     0x107
#define USB_KEY_INSERT  0x108
#define USB_KEY_DELETE  0x109

/* USB HID keycode → ASCII (0x04..0x57 → a-z, 1-9, 0, Enter, etc.) */
static int usb_keycode_to_key(uint8_t kc, int shift, int ctrl) {
    if (kc >= 0x04 && kc <= 0x1D && ctrl) return (kc - 0x04) + 1;
    if (kc >= 0x04 && kc <= 0x1D) return shift ? 'A' + (kc - 0x04) : 'a' + (kc - 0x04);
    if (kc >= 0x1E && kc <= 0x26) return shift ? "!@#$%^&*("[kc - 0x1E] : '1' + (kc - 0x1E);
    if (kc == 0x27) return shift ? ')' : '0';
    if (kc == 0x28) return '\n';
    if (kc == 0x29) return 27;
    if (kc == 0x2A) return '\b';
    if (kc == 0x2C) return ' ';
    if (kc == 0x2D) return shift ? '_' : '-';
    if (kc == 0x2E) return shift ? '+' : '=';
    if (kc == 0x2F) return shift ? '{' : '[';
    if (kc == 0x30) return shift ? '}' : ']';
    if (kc == 0x31) return shift ? '|' : '\\';
    if (kc == 0x33) return shift ? ':' : ';';
    if (kc == 0x34) return shift ? '"' : '\'';
    if (kc == 0x35) return shift ? '~' : '`';
    if (kc == 0x36) return shift ? '<' : ',';
    if (kc == 0x37) return shift ? '>' : '.';
    if (kc == 0x38) return shift ? '?' : '/';
    if (kc == 0x49) return USB_KEY_INSERT;
    if (kc == 0x4A) return USB_KEY_HOME;
    if (kc == 0x4B) return USB_KEY_PGUP;
    if (kc == 0x4C) return USB_KEY_DELETE;
    if (kc == 0x4D) return USB_KEY_END;
    if (kc == 0x4E) return USB_KEY_PGDWN;
    if (kc == 0x4F) return USB_KEY_RIGHT;
    if (kc == 0x50) return USB_KEY_LEFT;
    if (kc == 0x51) return USB_KEY_DOWN;
    if (kc == 0x52) return USB_KEY_UP;
    return 0;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int hid_add_device(int type, int slot_id, int interface_num,
                          int ep_addr, uint16_t max_packet, uint8_t interval) {
    if (hid_count >= HID_MAX_DEVICES) return 0;
    hid_device_t *dev = &hid_devices[hid_count++];
    memset(dev, 0, sizeof(*dev));
    dev->type = type;
    dev->slot_id = slot_id;
    dev->interface_num = interface_num;
    dev->ep_addr = ep_addr;
    dev->max_packet_size = max_packet ? max_packet : 8;
    dev->interval = interval;
    dev->report_len = type == HID_MOUSE ? (int)sizeof(hid_mouse_report_t) : (int)sizeof(hid_kbd_report_t);
    dev->active = 1;
    return 1;
}

static void hid_set_boot_protocol(int slot_id, int interface_num) {
    (void)xhci_control_transfer(slot_id, 0x21, 11, 0, (uint16_t)interface_num, 0, 0);
}

static int hid_probe_config(int dev_idx) {
    int slot = xhci_device_slot(dev_idx);
    if (slot < 0) return 0;

    uint8_t hdr[9];
    if (xhci_control_transfer(slot, 0x80, 6, 0x0200, 0, hdr, sizeof(hdr)) < 0)
        return 0;

    uint16_t total = rd16(hdr + 2);
    if (total < sizeof(hdr)) return 0;
    if (total > 256) total = 256;

    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) return 0;
    int ret = xhci_control_transfer(slot, 0x80, 6, 0x0200, 0, buf, total);
    if (ret < 0) {
        kfree(buf);
        return 0;
    }

    int added = 0;
    uint16_t got = ret > 0 && ret < total ? (uint16_t)ret : total;
    uint8_t cfg_value = got >= 6 ? buf[5] : 0;
    if (cfg_value) (void)xhci_control_transfer(slot, 0x00, 9, cfg_value, 0, 0, 0);

    int current_type = 0;
    int current_if = -1;
    for (uint16_t off = 0; off + 2 <= got; ) {
        uint8_t len = buf[off];
        uint8_t dtype = buf[off + 1];
        if (len < 2 || off + len > got) break;

        if (dtype == 4 && len >= 9) {
            uint8_t if_class = buf[off + 5];
            uint8_t if_protocol = buf[off + 7];
            current_if = buf[off + 2];
            current_type = 0;
            if (if_class == 0x03 && if_protocol == 1) current_type = HID_KEYBOARD;
            else if (if_class == 0x03 && if_protocol == 2) current_type = HID_MOUSE;
        } else if (dtype == 5 && len >= 7 && current_type) {
            uint8_t ep_addr = buf[off + 2];
            uint8_t attrs = buf[off + 3] & 0x03;
            if ((ep_addr & 0x80) && attrs == 0x03) {
                uint16_t max_packet = rd16(buf + off + 4) & 0x07FF;
                uint8_t interval = buf[off + 6];
                added += hid_add_device(current_type, slot, current_if,
                                        ep_addr, max_packet, interval);
                hid_set_boot_protocol(slot, current_if);
                current_type = 0;
            }
        }
        off += len;
    }

    kfree(buf);
    return added;
}

static int hid_probe_device_desc(int dev_idx, const usb_device_desc_t *desc) {
    int slot = xhci_device_slot(dev_idx);
    if (slot < 0 || !desc || desc->bDeviceClass != 0x03) return 0;

    int type = 0;
    if (desc->bDeviceProtocol == 1) type = HID_KEYBOARD;
    else if (desc->bDeviceProtocol == 2) type = HID_MOUSE;
    else type = HID_KEYBOARD;

    int added = hid_add_device(type, slot, 0, 0x81, 8, 10);
    if (added) hid_set_boot_protocol(slot, 0);
    return added;
}

int hid_init(void) {
    hid_count = 0;
    usb_kbd_index = -1;
    memset(hid_devices, 0, sizeof(hid_devices));

    int dev_count = xhci_device_count();
    for (int i = 0; i < dev_count && hid_count < HID_MAX_DEVICES; i++) {
        usb_device_desc_t desc;
        if (xhci_get_device_desc(i, &desc) < 0) continue;

        int before = hid_count;
        (void)hid_probe_config(i);
        if (hid_count == before) (void)hid_probe_device_desc(i, &desc);
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

/** 初始化 USB HID 键盘（由 shell 调用） */
int usb_kbd_init(void) {
    if (usb_kbd_initialized) return 0;
    if (xhci_init() < 0) return -1;
    if (hid_init() <= 0) return -1;
    for (int i = 0; i < hid_count; i++) {
        if (hid_devices[i].active && hid_devices[i].type == HID_KEYBOARD) {
            usb_kbd_index = i;
            break;
        }
    }
    if (usb_kbd_index < 0) return -1;
    memset(&last_report, 0, sizeof(last_report));
    usb_kbd_initialized = 1;
    return 0;
}

/** 从 USB 键盘读取一个字符，无输入返回 0 */
int usb_kbd_getc(void) {
    if (!usb_kbd_initialized) return 0;

    hid_kbd_report_t report;
    if (hid_get_keyboard_report(usb_kbd_index, &report) < 0) return 0;

    /* 检查是否有新的按键（与上次报告比较） */
    int shift = (report.modifiers & 0x22) != 0; /* Left/Right Shift */
    int ctrl = (report.modifiers & 0x11) != 0;  /* Left/Right Ctrl */

    for (int i = 0; i < 6; i++) {
        uint8_t kc = report.keys[i];
        if (kc == 0) continue;
        /* 检查此键是否在上次报告中已按下 */
        int was_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (last_report.keys[j] == kc) { was_pressed = 1; break; }
        }
        if (was_pressed) continue; /* 重复按键跳过 */

        if (kc < 128) {
            last_report = report;
            return usb_keycode_to_key(kc, shift, ctrl);
        }
    }

    last_report = report;
    return 0;
}

int usb_kbd_ready(void) {
    return usb_kbd_initialized;
}
