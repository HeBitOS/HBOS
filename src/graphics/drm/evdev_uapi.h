/**
 * @file    evdev_uapi.h
 * @brief   Linux evdev 的 ioctl 号、事件结构与键值（内核侧 ABI 镜像）
 *
 * @layer   gui
 * @owner   src/graphics/drm/
 * @stable  experimental
 *
 * 依赖    只依赖 <stdint.h>：字段偏移必须与 libinput/libevdev 一致。
 * 被依赖  evdev.h / evdev.c 与宿主单测。
 *
 * 索引头文件
 *   类型  evdev_input_event_t  read() 返回的事件
 *         evdev_input_id_t     设备身份（bus/vendor/product/version）
 *         evdev_absinfo_t      绝对轴范围（libinput 会查）
 *   宏    EVIOCGNAME           ioctl 号族（EVIOCG/EVIOCS*）
 *         EV_KEY               事件类型族（EV_*）
 *         KEY_A                键值族（KEY_ 与 BTN_ 前缀）
 *         REL_X                相对轴族（REL_ 前缀）
 *   函数  （本文件只有类型与宏）
 */
#ifndef HBOS_EVDEV_UAPI_H
#define HBOS_EVDEV_UAPI_H

#include <stdint.h>

/* ═══ ioctl 编码（与 Linux _IOC 一致，type = 'E'） ═════════════════ */
#define EVDEV_IOC_NRSHIFT   0
#define EVDEV_IOC_TYPESHIFT (EVDEV_IOC_NRSHIFT + 8)
#define EVDEV_IOC_SIZESHIFT (EVDEV_IOC_TYPESHIFT + 8)
#define EVDEV_IOC_DIRSHIFT  (EVDEV_IOC_SIZESHIFT + 14)

#define EVDEV_IOC_NONE  0U
#define EVDEV_IOC_WRITE 1U
#define EVDEV_IOC_READ  2U

#define EVDEV_IOC(dir, type, nr, size) \
    (((dir)  << EVDEV_IOC_DIRSHIFT)  | \
     ((type) << EVDEV_IOC_TYPESHIFT) | \
     ((nr)   << EVDEV_IOC_NRSHIFT)   | \
     ((size) << EVDEV_IOC_SIZESHIFT))

#define EVDEV_IOCTL_BASE 'E'
#define EVDEV_IO(nr)          EVDEV_IOC(EVDEV_IOC_NONE, EVDEV_IOCTL_BASE, nr, 0)
#define EVDEV_IOR(nr, type)   EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, nr, \
                                        sizeof(type))
#define EVDEV_IOW(nr, type)   EVDEV_IOC(EVDEV_IOC_WRITE, EVDEV_IOCTL_BASE, nr, \
                                        sizeof(type))

/* 内核派发只取这三段；size 段在 EVIOCGNAME 这类变长 ioctl 里就是缓冲长度。 */
#define EVDEV_CMD_DIR(cmd)  (((cmd) >> EVDEV_IOC_DIRSHIFT) & 0x3U)
#define EVDEV_CMD_TYPE(cmd) (((cmd) >> EVDEV_IOC_TYPESHIFT) & 0xFFU)
#define EVDEV_CMD_NR(cmd)   (((cmd) >> EVDEV_IOC_NRSHIFT) & 0xFFU)
#define EVDEV_CMD_SIZE(cmd) (((cmd) >> EVDEV_IOC_SIZESHIFT) & 0x3FFFU)

/* ═══ 数据模型 ══════════════════════════════════════════════════════ */

/** struct timeval（x86_64）。 */
typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} evdev_timeval_t;

/** read() 返回的事件，24 字节（与 Linux input_event 完全一致）。 */
typedef struct {
    evdev_timeval_t time;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} evdev_input_event_t;

/** EVIOCGID 返回。 */
typedef struct {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
} evdev_input_id_t;

/** EVIOCGABS 返回。 */
typedef struct {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
} evdev_absinfo_t;

/* ═══ ioctl 号 ══════════════════════════════════════════════════════ */
#define EVIOCGVERSION   EVDEV_IOR(0x01, int32_t)
#define EVIOCGID        EVDEV_IOR(0x02, evdev_input_id_t)
#define EVIOCGNAME(len) EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x06, (len))
#define EVIOCGPHYS(len) EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x07, (len))
#define EVIOCGUNIQ(len) EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x08, (len))
#define EVIOCGPROP(len) EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x09, (len))
#define EVIOCGKEY(len)  EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x18, (len))
#define EVIOCGLED(len)  EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x19, (len))
#define EVIOCGSND(len)  EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x1a, (len))
#define EVIOCGSW(len)   EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x1b, (len))
#define EVIOCGBIT(ev, len) \
    EVDEV_IOC(EVDEV_IOC_READ, EVDEV_IOCTL_BASE, 0x20 + (ev), (len))
#define EVIOCGABS(abs)  EVDEV_IOR(0x40 + (abs), evdev_absinfo_t)
#define EVIOCGRAB       EVDEV_IOW(0x90, int32_t)
#define EVIOCREVOKE     EVDEV_IOW(0x91, int32_t)
#define EVIOCSCLOCKID   EVDEV_IOW(0xa0, int32_t)

/* ═══ 常量 ══════════════════════════════════════════════════════════ */

/* 事件类型 */
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_REL   0x02
#define EV_ABS   0x03
#define EV_MSC   0x04
#define EV_SW    0x05
#define EV_LED   0x11
#define EV_SND   0x12
#define EV_REP   0x14

/* 同步事件 */
#define SYN_REPORT   0
#define SYN_CONFIG   1
#define SYN_DROPPED  3

/* 相对轴 */
#define REL_X       0x00
#define REL_Y       0x01
#define REL_Z       0x02
#define REL_HWHEEL  0x06
#define REL_WHEEL   0x08

/* 绝对轴（本实现不产生，只回应 libinput 的探测） */
#define ABS_X       0x00
#define ABS_Y       0x01

/* 总线类型 */
#define BUS_PCI     0x01
#define BUS_USB     0x03
#define BUS_VIRTUAL 0x06

/* 键值：字母数字与常用标点 */
#define KEY_ESC        1
#define KEY_1          2
#define KEY_2          3
#define KEY_3          4
#define KEY_4          5
#define KEY_5          6
#define KEY_6          7
#define KEY_7          8
#define KEY_8          9
#define KEY_9          10
#define KEY_0          11
#define KEY_MINUS      12
#define KEY_EQUAL      13
#define KEY_BACKSPACE  14
#define KEY_TAB        15
#define KEY_Q          16
#define KEY_W          17
#define KEY_E          18
#define KEY_R          19
#define KEY_T          20
#define KEY_Y          21
#define KEY_U          22
#define KEY_I          23
#define KEY_O          24
#define KEY_P          25
#define KEY_LEFTBRACE  26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER      28
#define KEY_LEFTCTRL   29
#define KEY_A          30
#define KEY_S          31
#define KEY_D          32
#define KEY_F          33
#define KEY_G          34
#define KEY_H          35
#define KEY_J          36
#define KEY_K          37
#define KEY_L          38
#define KEY_SEMICOLON  39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE      41
#define KEY_LEFTSHIFT  42
#define KEY_BACKSLASH  43
#define KEY_Z          44
#define KEY_X          45
#define KEY_C          46
#define KEY_V          47
#define KEY_B          48
#define KEY_N          49
#define KEY_M          50
#define KEY_COMMA      51
#define KEY_DOT        52
#define KEY_SLASH      53
#define KEY_RIGHTSHIFT 54
#define KEY_KPASTERISK 55
#define KEY_LEFTALT    56
#define KEY_SPACE      57
#define KEY_CAPSLOCK   58
#define KEY_F1         59
#define KEY_F2         60
#define KEY_F3         61
#define KEY_F4         62
#define KEY_F5         63
#define KEY_F6         64
#define KEY_F7         65
#define KEY_F8         66
#define KEY_F9         67
#define KEY_F10        68
#define KEY_NUMLOCK    69
#define KEY_SCROLLLOCK 70
#define KEY_F11        87
#define KEY_F12        88
#define KEY_RIGHTCTRL  97
#define KEY_RIGHTALT   100
#define KEY_HOME       102
#define KEY_UP         103
#define KEY_PAGEUP     104
#define KEY_LEFT       105
#define KEY_RIGHT      106
#define KEY_END        107
#define KEY_DOWN       108
#define KEY_PAGEDOWN   109
#define KEY_INSERT     110
#define KEY_DELETE     111
#define KEY_LEFTMETA   125
#define KEY_RIGHTMETA  126
#define KEY_MAX        0x2ff

/* 鼠标键 */
#define BTN_MISC       0x100
#define BTN_LEFT       0x110
#define BTN_RIGHT      0x111
#define BTN_MIDDLE     0x112

#endif /* HBOS_EVDEV_UAPI_H */
