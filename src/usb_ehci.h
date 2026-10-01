/**
 * @file usb_ehci.h
 * @brief EHCI（USB 2.0）主机控制器驱动对外接口
 *
 * 存在意义：真机上大量键盘/鼠标挂在 USB 2.0 端口上，而 USB 2.0 端口在
 * 2012 年前后的机器上属于 EHCI 控制器，与 xHCI 是两条独立总线。只做 xHCI
 * 时这类机器"有 USB 控制器却看不到鼠标"。本驱动把 EHCI 接进 `usb_host.h`
 * 的控制器抽象，让 HID 层无需区分控制器类型。
 *
 * 模型：纯轮询。不使用中断（`USBINTR` 全程为 0），所有完成判定都靠轮询
 * qTD 的 Active 位，并带时间 + 自旋双重截止。
 */

#ifndef HBOS_USB_EHCI_H
#define HBOS_USB_EHCI_H

#include <stdint.h>

#include "xhci.h" /**< 复用 usb_device_desc_t 等通用 USB 描述符 */

/**
 * @brief 把 EHCI 控制器注册进 USB 主机控制器注册表
 *
 * 由 `usb_host_init_all()` 调用；系统里没有 EHCI 时注册本身仍然成功
 * （注册只是登记 ops），真正的探测发生在 `usb_ehci_init()` 里。
 *
 * @return 0 成功，-1 注册表已满或参数非法
 */
int usb_ehci_register(void);

/**
 * @brief 探测并初始化所有 EHCI 控制器，复位根端口并给设备分配地址
 *
 * 幂等。初始化完成后，每个根端口上的设备都可以用 dev_idx 通过
 * `usb_ehci_get_device_desc()` / `usb_ehci_control_transfer()` 访问；
 * 设备只需要被"寻址"（SET_ADDRESS）成功，SET_CONFIGURATION 由 HID 层在
 * 解析配置描述符时下发（与 xHCI 路径一致）。
 *
 * @return 0 至少一个控制器就绪且 EP0 可用，-1 系统里没有可用 EHCI
 */
int usb_ehci_init(void);

/** @brief 已枚举（已分配地址）的设备数量 */
int usb_ehci_device_count(void);

/**
 * @brief 读取设备描述符（内部发 GET_DESCRIPTOR(DEVICE)，18 字节）
 * @return 0 成功，-1 失败
 */
int usb_ehci_get_device_desc(int dev_idx, usb_device_desc_t *desc);

/**
 * @brief 控制传输（EP0）
 *
 * 支持标准请求的三阶段流程：SETUP → 可选 DATA（IN/OUT）→ STATUS。
 * 数据方向由 bmRequestType 的 bit7 决定。wLength 为 0 时只发 SETUP 与
 * 无数据 STATUS 阶段。
 *
 * @return 完成后的实际数据长度（DATA 阶段收到的字节数），失败返回 -1
 */
int usb_ehci_control_transfer(int dev_idx, uint8_t bmRequestType,
                              uint8_t bRequest, uint16_t wValue,
                              uint16_t wIndex, void *data, uint16_t wLength);

/**
 * @brief 中断 IN 传输（轮询式"取一次报告"）
 * @param ep_addr  端点地址（含 0x80 方向位）
 * @param data     接收缓冲
 * @param len      期望长度
 * @param interval 端点报告的 bInterval（毫秒）；仅用于超时估算
 * @return 实际收到的字节数，失败/无数据返回 -1
 */
int usb_ehci_interrupt_transfer(int dev_idx, int ep_addr, void *data,
                                uint32_t len, uint32_t interval);

/** @brief 推进控制器事件处理（本驱动为轮询模型，主要用于状态清理） */
void usb_ehci_poll(void);

#endif /* HBOS_USB_EHCI_H */
