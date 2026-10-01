/**
 * @file usb_host.c
 * @brief USB 主机控制器注册表：内置 xHCI 适配器，并为 EHCI 等控制器提供挂载点
 *
 * 设计要点：
 *  - 注册表只保存 `const usb_host_ops_t *`，ops 本身是各控制器文件里的
 *    静态常量，注册表不复制函数指针表，也不做动态分配；
 *  - `usb_host_init_all()` 幂等，因为 `usb_hid_init()`、`usb_kbd_getc()` 的
 *    重试路径、`mouse_init()` 都会调用它，重复初始化会让控制器被复位两次；
 *  - 某个控制器 init 失败只记录"没就绪"，不阻止其它控制器——真机上常见
 *    "有 xHCI 没有 EHCI"或反过来，任何一类失败都不应该让另一类不可用。
 */

#include "usb_host.h"

#include "xhci.h"
#include "usb_ehci.h"
#include "string.h"

/** @brief 已注册的控制器 ops 表（按注册顺序初始化） */
static const usb_host_ops_t *hosts[USB_HOST_MAX_TYPES];
/** @brief 已注册的控制器数量 */
static int host_count;
/** @brief usb_host_init_all() 是否已经跑过 */
static int hosts_initialized;
/** @brief 上次 usb_host_init_all() 时初始化成功的控制器数量 */
static int hosts_ready;

int usb_host_register(const usb_host_ops_t *ops) {
    if (!ops || !ops->init || !ops->device_count || !ops->get_device_desc ||
        !ops->control_transfer || !ops->interrupt_transfer) {
        return -1;
    }
    for (int i = 0; i < host_count; i++) {
        if (hosts[i] == ops) return 0; /* 幂等：同一份 ops 只登记一次 */
    }
    if (host_count >= USB_HOST_MAX_TYPES) return -1;
    hosts[host_count++] = ops;
    return 0;
}

int usb_host_count(void) {
    return host_count;
}

const usb_host_ops_t *usb_host_get(int host_idx) {
    if (host_idx < 0 || host_idx >= host_count) return 0;
    return hosts[host_idx];
}

/* ── 内置 xHCI 适配器 ─────────────────────────────────────────────
 * xhci.c 的对外 API 混用了两种下标：设备级接口（device_count/slot/desc）
 * 用设备下标，传输接口用 slot id。控制器内部保持一致，适配器负责在
 * 边界上换算，避免把 slot 语义泄漏到 HID 层（EHCI 根本没有 slot 概念）。
 */

/** @brief xHCI 设备下标 → slot id；失败返回 -1 */
static int xhci_host_slot(int dev_idx) {
    return xhci_device_slot(dev_idx);
}

static int xhci_host_init(void) {
    return xhci_init();
}

static int xhci_host_device_count(void) {
    return xhci_device_count();
}

static int xhci_host_get_device_desc(int dev_idx, usb_device_desc_t *desc) {
    return xhci_get_device_desc(dev_idx, desc);
}

static int xhci_host_control_transfer(int dev_idx, uint8_t bmRequestType,
                                      uint8_t bRequest, uint16_t wValue,
                                      uint16_t wIndex, void *data,
                                      uint16_t wLength) {
    int slot = xhci_host_slot(dev_idx);
    if (slot < 0) return -1;
    return xhci_control_transfer(slot, bmRequestType, bRequest, wValue,
                                 wIndex, data, wLength);
}

static int xhci_host_interrupt_transfer(int dev_idx, int ep_addr, void *data,
                                        uint32_t len, uint32_t interval) {
    int slot = xhci_host_slot(dev_idx);
    if (slot < 0) return -1;
    return xhci_interrupt_transfer(slot, ep_addr, data, len, interval);
}

static void xhci_host_poll(void) {
    xhci_poll();
}

/** @brief 内置 xHCI 控制器 ops；始终注册（xHCI 是 2012 年后主板的主力控制器） */
static const usb_host_ops_t xhci_host_ops = {
    "xHCI",
    xhci_host_init,
    xhci_host_device_count,
    xhci_host_get_device_desc,
    xhci_host_control_transfer,
    xhci_host_interrupt_transfer,
    xhci_host_poll,
};

int usb_host_init_all(void) {
    if (hosts_initialized) return hosts_ready;
    hosts_initialized = 1;

    (void)usb_host_register(&xhci_host_ops);
    /* EHCI（USB 2.0）是 USB 输入的真机主力控制器：注册失败/系统里没有都
     * 不影响 xHCI，反之亦然。 */
    (void)usb_ehci_register();

    hosts_ready = 0;
    for (int i = 0; i < host_count; i++) {
        if (hosts[i]->init() == 0) hosts_ready++;
    }
    return hosts_ready;
}

void usb_host_poll_all(void) {
    for (int i = 0; i < host_count; i++) {
        if (hosts[i]->poll) hosts[i]->poll();
    }
}
