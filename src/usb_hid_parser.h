#ifndef HBOS_USB_HID_PARSER_H
#define HBOS_USB_HID_PARSER_H

/**
 * @file  usb_hid_parser.h
 * @brief USB HID 报告描述符解析器接口：把 Report Descriptor 还原成鼠标
 *        字段的位偏移 / 位宽布局，并按该布局解报告（report protocol）。
 *
 * 与 src/usb_hid.c 的分工：usb_hid.c 负责 USB 总线（取描述符、SET_PROTOCOL、
 * 中断传输），本文件只做纯逻辑，不碰硬件、不用动态内存、不用浮点，因此可以
 * 在宿主机上直接单测（tests/host/test_hid_parser.c）。
 */

#include <stdint.h>

/** 一个 Report ID 对应的鼠标字段布局；report_id == 0 表示无 Report ID 前缀 */
typedef struct {
    uint8_t  report_id;
    uint16_t report_len_bits;   /* 该报告 Input 字段的位数总和，**不含** Report ID 前缀字节 */
    int      has_x, has_y, has_wheel;
    uint16_t x_bit, x_bits;     int x_signed;
    uint16_t y_bit, y_bits;     int y_signed;
    uint16_t wheel_bit, wheel_bits; int wheel_signed;
    uint16_t buttons_bit, buttons_count;
    /**
     * X/Y 是否为相对位移（HID Input item 的 RELATIVE 位，位掩码 0x04）。
     *
     * 相对 = 真鼠标；绝对 = 数位板/触摸屏/`usb-tablet` 这类设备。上层的
     * `mouse_event_t` 是相对位移语义，绝对设备必须由调用方自行做差分，
     * 否则会把"坐标值"当成"一次移动上千像素"。
     */
    int xy_relative;
} hid_mouse_layout_t;

#define HID_MAX_MOUSE_LAYOUTS 4

/**
 * 解析报告描述符，抽出所有"含 X/Y（Generic Desktop 0x30/0x31）"的布局。
 * @param desc        报告描述符字节流（USB HID Report Descriptor）
 * @param desc_len    描述符长度（字节）
 * @param out         布局输出数组，调用方提供
 * @param max_layouts out 可容纳的布局数量
 * @return 抽出的布局数量（0 = 不是鼠标 / 解析失败）
 */
int hid_parse_mouse_layouts(const uint8_t *desc, uint32_t desc_len,
                            hid_mouse_layout_t *out, int max_layouts);

/**
 * 按布局从一份报告里解出鼠标状态。
 *
 * 长度语义：若布局 report_id != 0，report[0] 必须是该 Report ID；报告按 Report
 * ID 定长，因此缓冲不足以容纳 report_len_bits 时返回 -1（例如设备实际还在
 * boot protocol 下、发的是固定 3/4 字节 boot 报告——此时用 report protocol 的
 * 偏移取位必然错位，调用方应丢弃该帧或回退 boot 布局）。任何字段越界同样返回
 * -1，且失败时不写任何输出。
 *
 * @param layout     由 hid_parse_mouse_layouts() 得到的布局
 * @param report     报告缓冲；若布局 report_id != 0，则 report[0] 必须是 Report ID
 * @param report_len 缓冲可用长度
 * @param dx         输出：X（有符号按 x_signed 符号扩展；无 X 字段时为 0）
 * @param dy         输出：Y
 * @param wheel      输出：滚轮
 * @param buttons    输出：按键位图（低位在前，取低 32 位；无按键字段时为 0）
 * @return 0 成功；-1 长度不足 / Report ID 不匹配 / 布局自相矛盾（可为 NULL）
 */
int hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *report,
                     uint32_t report_len, int *dx, int *dy, int *wheel,
                     uint32_t *buttons);

#endif
