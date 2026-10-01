#ifndef HBOS_USB_HID_H
#define HBOS_USB_HID_H

#include <stdint.h>

#include "usb_hid_parser.h"

#define HID_KEYBOARD 1
#define HID_MOUSE     2

#define HID_MAX_DEVICES 8

typedef struct {
    uint8_t modifiers;
    uint8_t _reserved;
    uint8_t keys[6];
} __attribute__((packed)) hid_kbd_report_t;

/**
 * @brief 解好的鼠标状态（与传输格式无关）
 *
 * 由 usb_hid.c 按**报告描述符解析出的布局**填充（report protocol），因此
 * dx/dy 可以是 8/12/16 位，不再是固定的 int8_t。旧结构体把 X/Y 写死成
 * int8_t，正是"真机鼠标乱跳"的根因之一：非 boot 布局的位偏移与这里对不上。
 */
typedef struct {
    uint32_t buttons; /**< 按键位图，低位在前（左/右/中 = bit0/1/2） */
    int dx;           /**< X 位移（已按字段符号性扩展） */
    int dy;           /**< Y 位移 */
    int dz;           /**< 滚轮（垂直滚轮；无该字段时为 0） */
} hid_mouse_report_t;

typedef struct {
    int type;
    int host_idx;   /**< 所属 USB 主机控制器下标（见 usb_host.h；xHCI/EHCI 各有自己的设备空间） */
    int dev_idx;    /**< 该控制器内部的设备下标 */
    int interface_num;
    int ep_addr;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t report[64];
    int report_len;
    int active;
    /** 报告描述符解析结果（仅鼠标）：report protocol 的字段位布局 */
    hid_mouse_layout_t mouse_layouts[HID_MAX_MOUSE_LAYOUTS];
    int mouse_layout_count;
    int mouse_boot_mode;   /**< 1 = 未解析出布局，按 boot 固定布局收发 */
} hid_device_t;

int hid_init(void);
int hid_device_count(void);
int hid_keyboard_count(void);
int hid_mouse_count(void);
int hid_get_keyboard_report(int idx, hid_kbd_report_t *report);
int hid_get_mouse_report(int idx, hid_mouse_report_t *report);
void hid_poll(void);
int usb_hid_init(void);
int usb_kbd_init(void);
int usb_kbd_getc(void);
int usb_kbd_ready(void);
int usb_mouse_init(void);
int usb_mouse_get_report(hid_mouse_report_t *report);
int usb_mouse_ready(void);

#endif
