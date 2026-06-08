#ifndef HBOS_USB_HID_H
#define HBOS_USB_HID_H

#include <stdint.h>

#define HID_KEYBOARD 1
#define HID_MOUSE     2

#define HID_MAX_DEVICES 8

typedef struct {
    uint8_t modifiers;
    uint8_t _reserved;
    uint8_t keys[6];
} __attribute__((packed)) hid_kbd_report_t;

typedef struct {
    uint8_t buttons;
    int8_t  x;
    int8_t  y;
    int8_t  wheel;
} __attribute__((packed)) hid_mouse_report_t;

typedef struct {
    int type;
    int slot_id;
    int interface_num;
    int ep_addr;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t report[64];
    int report_len;
    int active;
} hid_device_t;

int hid_init(void);
int hid_device_count(void);
int hid_get_keyboard_report(int idx, hid_kbd_report_t *report);
int hid_get_mouse_report(int idx, hid_mouse_report_t *report);
void hid_poll(void);
int usb_kbd_init(void);
int usb_kbd_getc(void);
int usb_kbd_ready(void);

#endif
