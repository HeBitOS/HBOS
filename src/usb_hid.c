#include "usb_hid.h"
#include "xhci.h"
#include "core/heap.h"
#include "string.h"

static hid_device_t hid_devices[HID_MAX_DEVICES];
static int hid_count;
static hid_kbd_report_t last_report;
static int usb_kbd_initialized;

/* USB HID keycode → ASCII (0x04..0x57 → a-z, 1-9, 0, Enter, etc.) */
static char usb_keycode_to_ascii(uint8_t kc, int shift) {
    if (kc >= 0x04 && kc <= 0x1D) return shift ? 'A' + (kc - 0x04) : 'a' + (kc - 0x04);
    if (kc >= 0x1E && kc <= 0x26) return shift ? "!@#$%^&*("[kc - 0x1E] : '1' + (kc - 0x1E);
    if (kc == 0x27) return shift ? ')' : '0';
    if (kc == 0x28) return '\n';
    if (kc == 0x29) return 0; /* Escape */
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
    return 0;
}

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

/** 初始化 USB HID 键盘（由 shell 调用） */
int usb_kbd_init(void) {
    if (usb_kbd_initialized) return 0;
    if (xhci_init() < 0) return -1;
    if (hid_init() <= 0) return -1;
    memset(&last_report, 0, sizeof(last_report));
    usb_kbd_initialized = 1;
    return 0;
}

/** 从 USB 键盘读取一个字符，无输入返回 0 */
int usb_kbd_getc(void) {
    if (!usb_kbd_initialized) return 0;

    hid_kbd_report_t report;
    if (hid_get_keyboard_report(0, &report) < 0) return 0;

    /* 检查是否有新的按键（与上次报告比较） */
    int shift = (report.modifiers & 0x22) != 0; /* Left/Right Shift */

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
            char c = usb_keycode_to_ascii(kc, shift);
            return c ? c : 0;
        }
    }

    last_report = report;
    return 0;
}