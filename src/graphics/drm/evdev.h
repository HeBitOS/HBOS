/**
 * @file    evdev.h
 * @brief   Linux evdev 输入设备：/dev/input/event* 的内核侧接口
 *
 * @layer   gui
 * @owner   src/graphics/drm/
 * @stable  experimental
 *
 * 依赖    evdev_uapi.h（纯 ABI）。不 include vfs.h：节点类型只用
 *         `struct vfs_node *` 前向声明，宿主单测可只带 ABI 头。
 * 被依赖  src/devfs.c（注册 event0/event1），KWin/libinput 经 syscall 进入。
 *
 * 索引头文件
 *   类型  evdev_info_t     设备静态信息（名字/能力），诊断用
 *   宏    EVDEV_MAX_DEVICES  设备槽位数
 *   函数  evdev_init       初始化两个设备槽（键盘/鼠标）
 *         evdev_register_nodes  把 event0/event1 的 fops 挂到 devfs 节点
 *         evdev_is_node    判断节点是否属于 evdev
 *         evdev_event_pending  是否有待读事件（poll 钩子）
 *         evdev_inject_key 注入按键事件（供输入源 hook 调用）
 *         evdev_inject_rel 注入相对位移
 *         evdev_inject_btn 注入按键边缘
 *         evdev_selftest   无 QEMU 自检：ioctl/read 全链路
 */
#ifndef HBOS_EVDEV_H
#define HBOS_EVDEV_H

#include <stdint.h>

struct vfs_node;

#define EVDEV_MAX_DEVICES 2U   /**< 0 = 键盘，1 = 指针 */

/** 设备静态信息快照。 */
typedef struct {
    uint32_t used;
    uint32_t is_mouse;
    uint32_t grabbed;           /**< 是否被 EVIOCGRAB 独占 */
    uint32_t events_pending;
    const char *name;
} evdev_info_t;

int evdev_init(void);

/**
 * @brief 把两个 evdev 设备的 fops 挂到 devfs 已分配的节点上。
 *
 * @param kbd   /dev/input/event0（键盘）
 * @param mouse /dev/input/event1（指针）
 * @return 0 成功；-1 任一参数为空。
 */
int evdev_register_nodes(struct vfs_node *kbd, struct vfs_node *mouse);

/** @return 1 = 属于 evdev；0 = 不是。 */
int evdev_is_node(const struct vfs_node *node);

/** @return 1 = 有未读事件（供 poll 钩子报 POLLIN）。 */
int evdev_event_pending(const struct vfs_node *node);

/**
 * @brief 注入一个 EV_KEY 事件（value: 1=按下 0=抬起 2=重复）。
 *
 * 供 PS/2 / USB HID 原始扫描码路径调用；没有注入方时 evdev 会在设备被
 * EVIOCGRAB 后用 kb_poll_key()/mouse_poll() 轮询降级取值。
 * @return 0 成功；-1 参数非法（设备槽不存在）。
 */
int evdev_inject_key(uint32_t dev, uint16_t code, int32_t value);

/** @brief 注入一个 EV_REL 事件（code 用 REL_X/REL_Y/REL_WHEEL）。 */
int evdev_inject_rel(uint32_t dev, uint16_t code, int32_t value);

/** @brief 注入一个按键边缘（btn 用 BTN_LEFT/BTN_RIGHT/BTN_MIDDLE）。 */
int evdev_inject_btn(uint32_t dev, uint16_t btn, int32_t value);

/** @brief 取设备信息快照；@param out 非 NULL 时写入。 */
void evdev_info(uint32_t dev, evdev_info_t *out);

/** @brief 自检：EVIOCGNAME/EVIOCGBIT/EVIOCGID + 注入 + read。 */
int evdev_selftest(void);

#endif /* HBOS_EVDEV_H */
