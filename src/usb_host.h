/**
 * @file usb_host.h
 * @brief USB 主机控制器抽象层：把 xHCI / EHCI 等控制器统一成同一套设备访问接口
 *
 * 为什么需要这一层：HBOS 原来的输入路径只认 xHCI 控制器。真机上（尤其
 * 2012 年前后的主板与笔记本）USB 2.0 端口往往挂在 **EHCI** 控制器上，
 * xHCI 只负责 USB 3.0 端口——于是 `xhci_init()` 成功、端口上却没有设备，
 * HID 层拿到 0 个设备，鼠标就"没有驱动"。把控制器抽象成可注册的 ops
 * 之后，HID 层遍历的是"系统里所有 USB 主机控制器 × 各自的设备"，某一个
 * 控制器没设备不再等于整个 USB 输入没有设备。
 *
 * 索引约定：ops 里的 dev_idx 是**该控制器内部**的设备下标；调用方自己
 * 记录 (host_idx, dev_idx) 二元组，控制器之间互不干扰。
 */

#ifndef HBOS_USB_HOST_H
#define HBOS_USB_HOST_H

#include <stdint.h>

#include "xhci.h" /**< usb_device_desc_t 等通用 USB 描述符定义 */

/** @brief 控制器种类上限（xHCI / EHCI / …） */
#define USB_HOST_MAX_TYPES 4

/**
 * @brief 一类 USB 主机控制器的驱动接口
 *
 * 每个函数都不得在失败时改变调用方缓冲区（控制/中断传输失败返回 -1）。
 */
typedef struct {
    /** @brief 诊断名，如 "xHCI" */
    const char *name;
    /** @brief 初始化（幂等）；0 = 至少一个该类型控制器就绪，-1 = 系统里没有 */
    int (*init)(void);
    /** @brief 已枚举到的设备数量 */
    int (*device_count)(void);
    /** @brief 读取设备描述符；0 成功，-1 失败 */
    int (*get_device_desc)(int dev_idx, usb_device_desc_t *desc);
    /** @brief 控制传输；0/-1 */
    int (*control_transfer)(int dev_idx, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex,
                            void *data, uint16_t wLength);
    /** @brief 中断 IN 传输（轮询模型下就是"取一次报告"）；返回实际长度或 -1 */
    int (*interrupt_transfer)(int dev_idx, int ep_addr, void *data,
                              uint32_t len, uint32_t interval);
    /** @brief 推进控制器事件处理；可为 NULL */
    void (*poll)(void);
} usb_host_ops_t;

/**
 * @brief 注册一类主机控制器驱动
 *
 * 同一 ops 指针重复注册是幂等的（返回 0），便于各控制器模块在自己的
 * 初始化路径里自注册而不必关心调用顺序。
 *
 * @return 0 成功，-1 参数非法或超出 USB_HOST_MAX_TYPES
 */
int usb_host_register(const usb_host_ops_t *ops);

/**
 * @brief 初始化所有已注册的控制器
 *
 * 幂等：第一次调用做实际初始化，之后直接返回上次的结果。某个控制器初始化
 * 失败（系统里没有这类硬件）不影响其它控制器。
 *
 * @return 初始化成功的控制器数量
 */
int usb_host_init_all(void);

/** @brief 已注册的控制器数量 */
int usb_host_count(void);

/**
 * @brief 取第 host_idx 个已注册控制器的 ops
 * @return ops 指针；下标越界返回 NULL
 */
const usb_host_ops_t *usb_host_get(int host_idx);

/** @brief 遍历所有控制器推进事件处理（hid_poll 用） */
void usb_host_poll_all(void);

#endif /* HBOS_USB_HOST_H */
