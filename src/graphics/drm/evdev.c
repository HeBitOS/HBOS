/**
 * @file evdev.c
 * @brief Linux evdev 输入设备：/dev/input/event0（键盘）与 event1（指针）
 *
 * 设计要点（改之前先读）
 * ─────────────────────
 * 1. 真实输入源（PS/2 IRQ 扫描码、USB HID 报文）都在别的模块里，本文件
 *    不改它们。两条取值路径：
 *      · 首选：外部在原始事件处调用 evdev_inject_*()（shell.c/usb_hid.c
 *        各加两行即可，属于别的 scope，已在回报里提出）；
 *      · 降级：设备被 EVIOCGRAB 之后，read() 里用 kb_poll_key()/mouse_poll()
 *        轮询。键盘只能拿到"熟码"（无抬起、无原始键位），所以实现成
 *        "按下后立刻抬起"的事件对，并把修饰键状态按采样合成。够 KWin 识别
 *        设备与收到点击/滚轮，不足以支持真正的同时按键与按键保持。
 * 2. 设备能力位（EVIOCGBIT）必须覆盖 libinput 判型所需的最小集：EV_SYN +
 *    EV_KEY（键盘）/ EV_REL（指针）；缺 EV_SYN 会被当成"不完整设备"。
 * 3. 事件环满时丢最旧的一条。输入设备宁丢不堵：阻塞 read 会把合成器卡死。
 */

#include "evdev.h"
#include "evdev_uapi.h"

#include "vfs.h"
#include "string.h"
#include "errno.h"
#include "graphics/graphics.h"
#include "shell/shell.h"
#include "input/mouse.h"

#define EVDEV_RING 64

typedef struct {
    uint32_t used;
    uint32_t is_mouse;
    uint32_t grabbed;
    const char *name;
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
    evdev_input_event_t ring[EVDEV_RING];
    uint32_t head;              /**< 下一个待读 */
    uint32_t count;
    uint32_t last_buttons;      /**< 指针键边缘用 */
    int last_ctrl;
    int last_alt;
    int last_super;
} evdev_dev_t;

static evdev_dev_t g_devs[EVDEV_MAX_DEVICES];
static int g_evdev_ready;

/* ASCII 键映射表：kb_poll_key() 返回的是"熟码"，大写字母表示用户按了
 * Shift+字母，所以这里用 shift 标志还原成真实的 evdev 事件序列。 */
typedef struct {
    int ascii;
    uint16_t code;
    uint8_t shift;
} evdev_ascii_key_t;

static const evdev_ascii_key_t g_ascii_keys[] = {
    {'a', KEY_A, 0}, {'b', KEY_B, 0}, {'c', KEY_C, 0}, {'d', KEY_D, 0},
    {'e', KEY_E, 0}, {'f', KEY_F, 0}, {'g', KEY_G, 0}, {'h', KEY_H, 0},
    {'i', KEY_I, 0}, {'j', KEY_J, 0}, {'k', KEY_K, 0}, {'l', KEY_L, 0},
    {'m', KEY_M, 0}, {'n', KEY_N, 0}, {'o', KEY_O, 0}, {'p', KEY_P, 0},
    {'q', KEY_Q, 0}, {'r', KEY_R, 0}, {'s', KEY_S, 0}, {'t', KEY_T, 0},
    {'u', KEY_U, 0}, {'v', KEY_V, 0}, {'w', KEY_W, 0}, {'x', KEY_X, 0},
    {'y', KEY_Y, 0}, {'z', KEY_Z, 0},
    {'A', KEY_A, 1}, {'B', KEY_B, 1}, {'C', KEY_C, 1}, {'D', KEY_D, 1},
    {'E', KEY_E, 1}, {'F', KEY_F, 1}, {'G', KEY_G, 1}, {'H', KEY_H, 1},
    {'I', KEY_I, 1}, {'J', KEY_J, 1}, {'K', KEY_K, 1}, {'L', KEY_L, 1},
    {'M', KEY_M, 1}, {'N', KEY_N, 1}, {'O', KEY_O, 1}, {'P', KEY_P, 1},
    {'Q', KEY_Q, 1}, {'R', KEY_R, 1}, {'S', KEY_S, 1}, {'T', KEY_T, 1},
    {'U', KEY_U, 1}, {'V', KEY_V, 1}, {'W', KEY_W, 1}, {'X', KEY_X, 1},
    {'Y', KEY_Y, 1}, {'Z', KEY_Z, 1},
    {'1', KEY_1, 0}, {'2', KEY_2, 0}, {'3', KEY_3, 0}, {'4', KEY_4, 0},
    {'5', KEY_5, 0}, {'6', KEY_6, 0}, {'7', KEY_7, 0}, {'8', KEY_8, 0},
    {'9', KEY_9, 0}, {'0', KEY_0, 0},
    {'!', KEY_1, 1}, {'@', KEY_2, 1}, {'#', KEY_3, 1}, {'$', KEY_4, 1},
    {'%', KEY_5, 1}, {'^', KEY_6, 1}, {'&', KEY_7, 1}, {'*', KEY_8, 1},
    {'(', KEY_9, 1}, {')', KEY_0, 1},
    {' ', KEY_SPACE, 0}, {'-', KEY_MINUS, 0}, {'_', KEY_MINUS, 1},
    {'=', KEY_EQUAL, 0}, {'+', KEY_EQUAL, 1},
    {'[', KEY_LEFTBRACE, 0}, {'{', KEY_LEFTBRACE, 1},
    {']', KEY_RIGHTBRACE, 0}, {'}', KEY_RIGHTBRACE, 1},
    {'\\', KEY_BACKSLASH, 0}, {'|', KEY_BACKSLASH, 1},
    {';', KEY_SEMICOLON, 0}, {':', KEY_SEMICOLON, 1},
    {'\'', KEY_APOSTROPHE, 0}, {'"', KEY_APOSTROPHE, 1},
    {'`', KEY_GRAVE, 0}, {'~', KEY_GRAVE, 1},
    {',', KEY_COMMA, 0}, {'<', KEY_COMMA, 1},
    {'.', KEY_DOT, 0}, {'>', KEY_DOT, 1},
    {'/', KEY_SLASH, 0}, {'?', KEY_SLASH, 1},
    {'\n', KEY_ENTER, 0}, {'\r', KEY_ENTER, 0},
    {'\t', KEY_TAB, 0}, {0x7F, KEY_BACKSPACE, 0}, {0x08, KEY_BACKSPACE, 0},
    {27, KEY_ESC, 0},
};

/* shell.h 的特殊键码 → evdev 键值。 */
typedef struct {
    int key;
    uint16_t code;
} evdev_special_key_t;

static const evdev_special_key_t g_special_keys[] = {
    {KB_KEY_UP, KEY_UP}, {KB_KEY_DOWN, KEY_DOWN},
    {KB_KEY_LEFT, KEY_LEFT}, {KB_KEY_RIGHT, KEY_RIGHT},
    {KB_KEY_PGUP, KEY_PAGEUP}, {KB_KEY_PGDWN, KEY_PAGEDOWN},
    {KB_KEY_HOME, KEY_HOME}, {KB_KEY_END, KEY_END},
    {KB_KEY_INSERT, KEY_INSERT}, {KB_KEY_DELETE, KEY_DELETE},
    {KB_KEY_F1, KEY_F1}, {KB_KEY_F2, KEY_F2}, {KB_KEY_F3, KEY_F3},
    {KB_KEY_F4, KEY_F4}, {KB_KEY_F5, KEY_F5}, {KB_KEY_F6, KEY_F6},
    {KB_KEY_F7, KEY_F7}, {KB_KEY_F8, KEY_F8}, {KB_KEY_F9, KEY_F9},
    {KB_KEY_SHIFT_UP, KEY_LEFTSHIFT}, {KB_KEY_SHIFT_DOWN, KEY_LEFTSHIFT},
    {KB_KEY_SHIFT_LEFT, KEY_LEFTSHIFT}, {KB_KEY_SHIFT_RIGHT, KEY_RIGHTSHIFT},
    {KB_KEY_SUPER, KEY_LEFTMETA},
};

/* ═══ 事件入队 ══════════════════════════════════════════════════════ */

static void evdev_push(uint32_t dev, uint16_t type, uint16_t code,
                       int32_t value) {
    if (dev >= EVDEV_MAX_DEVICES) return;
    evdev_dev_t *d = &g_devs[dev];
    if (!d->used) return;

    if (d->count == EVDEV_RING) {
        d->head = (d->head + 1U) % EVDEV_RING;
        d->count--;
    }
    uint32_t slot = (d->head + d->count) % EVDEV_RING;
    evdev_input_event_t *e = &d->ring[slot];
    memset(e, 0, sizeof(*e));
    e->time.tv_sec = 0;
    e->time.tv_usec = 0;
    e->type = type;
    e->code = code;
    e->value = value;
    d->count++;
}

static void evdev_sync(uint32_t dev) {
    evdev_push(dev, EV_SYN, SYN_REPORT, 0);
}

int evdev_inject_key(uint32_t dev, uint16_t code, int32_t value) {
    if (dev >= EVDEV_MAX_DEVICES || !g_devs[dev].used) return -1;
    evdev_push(dev, EV_KEY, code, value);
    evdev_sync(dev);
    return 0;
}

int evdev_inject_rel(uint32_t dev, uint16_t code, int32_t value) {
    if (dev >= EVDEV_MAX_DEVICES || !g_devs[dev].used) return -1;
    evdev_push(dev, EV_REL, code, value);
    evdev_sync(dev);
    return 0;
}

int evdev_inject_btn(uint32_t dev, uint16_t btn, int32_t value) {
    return evdev_inject_key(dev, btn, value);
}

/* ═══ 降级轮询：从现有输入源合成事件 ════════════════════════════════ */

static void evdev_emit_key_pair(uint32_t dev, uint16_t code) {
    evdev_push(dev, EV_KEY, code, 1);
    evdev_sync(dev);
    evdev_push(dev, EV_KEY, code, 0);
    evdev_sync(dev);
}

static void evdev_modifier_edge(uint32_t dev, uint16_t code, int now, int *last) {
    if (now == *last) return;
    *last = now;
    evdev_push(dev, EV_KEY, code, now ? 1 : 0);
    evdev_sync(dev);
}

static void evdev_emit_cooked_key(uint32_t dev, int key) {
    if (key >= 0x100) {
        for (uint32_t i = 0;
             i < sizeof(g_special_keys) / sizeof(g_special_keys[0]); i++) {
            if (g_special_keys[i].key == key) {
                evdev_emit_key_pair(dev, g_special_keys[i].code);
                return;
            }
        }
        return;   /* 未知特殊键：丢弃而不是瞎猜 */
    }

    for (uint32_t i = 0;
         i < sizeof(g_ascii_keys) / sizeof(g_ascii_keys[0]); i++) {
        if (g_ascii_keys[i].ascii != key) continue;
        if (g_ascii_keys[i].shift) {
            evdev_push(dev, EV_KEY, KEY_LEFTSHIFT, 1);
            evdev_sync(dev);
        }
        evdev_emit_key_pair(dev, g_ascii_keys[i].code);
        if (g_ascii_keys[i].shift) {
            evdev_push(dev, EV_KEY, KEY_LEFTSHIFT, 0);
            evdev_sync(dev);
        }
        return;
    }
}

/** 只在该设备被 EVIOCGRAB 后轮询：否则会把 shell/GUI 的按键抢走。 */
static void evdev_pump_keyboard(void) {
    evdev_dev_t *d = &g_devs[0];
    if (!d->used || !d->grabbed) return;

    evdev_modifier_edge(0, KEY_LEFTCTRL, kb_ctrl_pressed(), &d->last_ctrl);
    evdev_modifier_edge(0, KEY_LEFTALT, kb_alt_pressed(), &d->last_alt);
    evdev_modifier_edge(0, KEY_LEFTMETA, kb_super_pressed(), &d->last_super);

    for (int i = 0; i < 16; i++) {
        int key = kb_poll_key();
        if (!key) break;
        evdev_emit_cooked_key(0, key);
    }
}

static void evdev_pump_mouse(void) {
    evdev_dev_t *d = &g_devs[1];
    if (!d->used || !d->grabbed) return;

    for (int i = 0; i < 16; i++) {
        mouse_event_t ev;
        if (!mouse_poll(&ev)) break;
        if (ev.dx) evdev_push(1, EV_REL, REL_X, ev.dx);
        if (ev.dy) evdev_push(1, EV_REL, REL_Y, ev.dy);
        if (ev.dz) evdev_push(1, EV_REL, REL_WHEEL, ev.dz);

        uint32_t buttons = ev.buttons;
        uint32_t changed = buttons ^ d->last_buttons;
        if (changed & 1U)
            evdev_push(1, EV_KEY, BTN_LEFT, (buttons & 1U) ? 1 : 0);
        if (changed & 2U)
            evdev_push(1, EV_KEY, BTN_RIGHT, (buttons & 2U) ? 1 : 0);
        if (changed & 4U)
            evdev_push(1, EV_KEY, BTN_MIDDLE, (buttons & 4U) ? 1 : 0);
        d->last_buttons = buttons;
        evdev_sync(1);
    }
}

static void evdev_pump_all(void) {
    evdev_pump_keyboard();
    evdev_pump_mouse();
}

/* ═══ 设备初始化与节点挂接 ══════════════════════════════════════════ */

int evdev_init(void) {
    if (g_evdev_ready) return 0;
    memset(g_devs, 0, sizeof(g_devs));

    g_devs[0].used = 1;
    g_devs[0].is_mouse = 0;
    g_devs[0].name = "HBOS virtual keyboard";
    g_devs[0].bustype = BUS_VIRTUAL;
    g_devs[0].vendor = 0x1U;
    g_devs[0].product = 0x1U;
    g_devs[0].version = 0x1U;

    g_devs[1].used = 1;
    g_devs[1].is_mouse = 1;
    g_devs[1].name = "HBOS virtual pointer";
    g_devs[1].bustype = BUS_VIRTUAL;
    g_devs[1].vendor = 0x1U;
    g_devs[1].product = 0x2U;
    g_devs[1].version = 0x1U;

    g_evdev_ready = 1;
    return 0;
}

static int evdev_read(vfs_node_t *node, uint32_t offset, void *buf,
                      uint32_t count);
static int evdev_ioctl(vfs_node_t *node, uint32_t cmd, void *arg);

static const vfs_ops_t g_evdev_ops = {
    .read = evdev_read,
    .ioctl = evdev_ioctl,
};

static int evdev_dev_from_node(const struct vfs_node *node) {
    if (!node || node->ops != &g_evdev_ops) return -1;
    uintptr_t tag = (uintptr_t)node->private_data;
    if (tag < 1 || tag > EVDEV_MAX_DEVICES) return -1;
    return (int)(tag - 1U);
}

int evdev_is_node(const struct vfs_node *node) {
    return node && node->ops == &g_evdev_ops;
}

static int evdev_register_one(struct vfs_node *node, uint32_t dev) {
    if (!node) return -1;
    node->ops = &g_evdev_ops;
    node->private_data = (void *)(uintptr_t)(dev + 1U);
    node->size = 0;
    return 0;
}

int evdev_register_nodes(struct vfs_node *kbd, struct vfs_node *mouse) {
    if (evdev_init() < 0) return -1;
    if (evdev_register_one(kbd, 0) < 0) return -1;
    if (evdev_register_one(mouse, 1) < 0) return -1;
    return 0;
}

int evdev_event_pending(const struct vfs_node *node) {
    int dev = evdev_dev_from_node(node);
    if (dev < 0) return 0;
    evdev_pump_all();
    return g_devs[dev].count ? 1 : 0;
}

void evdev_info(uint32_t dev, evdev_info_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (dev >= EVDEV_MAX_DEVICES) return;
    out->used = g_devs[dev].used;
    out->is_mouse = g_devs[dev].is_mouse;
    out->grabbed = g_devs[dev].grabbed;
    out->events_pending = g_devs[dev].count;
    out->name = g_devs[dev].name;
}

/* ═══ fops ══════════════════════════════════════════════════════════ */

static int evdev_read(vfs_node_t *node, uint32_t offset, void *buf,
                      uint32_t count) {
    (void)offset;
    int dev = evdev_dev_from_node(node);
    if (dev < 0) return -ENODEV;
    if (!buf) return -EINVAL;

    evdev_pump_all();

    evdev_dev_t *d = &g_devs[dev];
    if (d->count == 0) return 0;

    uint32_t max = count / (uint32_t)sizeof(evdev_input_event_t);
    if (max > d->count) max = d->count;
    uint8_t *out = (uint8_t *)buf;
    for (uint32_t i = 0; i < max; i++) {
        evdev_input_event_t *e = &d->ring[d->head];
        memcpy(out + (uint64_t)i * sizeof(*e), e, sizeof(*e));
        memset(e, 0, sizeof(*e));
        d->head = (d->head + 1U) % EVDEV_RING;
        d->count--;
    }
    return (int)(max * sizeof(evdev_input_event_t));
}

/** 置位一个 bit（越界静默忽略：libinput 传的长度就是它关心的范围）。 */
static void evdev_bitmap_set(uint8_t *out, uint32_t len, uint16_t bit) {
    uint32_t byte = bit >> 3;
    if (!out || byte >= len) return;
    out[byte] = (uint8_t)(out[byte] | (uint8_t)(1U << (bit & 7U)));
}

static void evdev_bitmap_zero(uint8_t *out, uint32_t len) {
    if (!out) return;
    memset(out, 0, len);
}

static int evdev_gbit(const evdev_dev_t *d, uint32_t ev, uint8_t *out,
                      uint32_t len) {
    if (!out || len == 0) return -EINVAL;
    evdev_bitmap_zero(out, len);

    if (ev == 0) {
        evdev_bitmap_set(out, len, EV_SYN);
        evdev_bitmap_set(out, len, EV_KEY);
        if (d->is_mouse) {
            evdev_bitmap_set(out, len, EV_REL);
        } else {
            evdev_bitmap_set(out, len, EV_MSC);
            evdev_bitmap_set(out, len, EV_LED);
            evdev_bitmap_set(out, len, EV_REP);
        }
        return 0;
    }
    if (ev == EV_KEY) {
        for (uint32_t i = 0;
             i < sizeof(g_ascii_keys) / sizeof(g_ascii_keys[0]); i++)
            evdev_bitmap_set(out, len, g_ascii_keys[i].code);
        for (uint32_t i = 0;
             i < sizeof(g_special_keys) / sizeof(g_special_keys[0]); i++)
            evdev_bitmap_set(out, len, g_special_keys[i].code);
        evdev_bitmap_set(out, len, KEY_LEFTCTRL);
        evdev_bitmap_set(out, len, KEY_LEFTALT);
        evdev_bitmap_set(out, len, KEY_LEFTSHIFT);
        evdev_bitmap_set(out, len, KEY_LEFTMETA);
        if (d->is_mouse) {
            evdev_bitmap_set(out, len, BTN_LEFT);
            evdev_bitmap_set(out, len, BTN_RIGHT);
            evdev_bitmap_set(out, len, BTN_MIDDLE);
        }
        return 0;
    }
    if (ev == EV_REL && d->is_mouse) {
        evdev_bitmap_set(out, len, REL_X);
        evdev_bitmap_set(out, len, REL_Y);
        evdev_bitmap_set(out, len, REL_WHEEL);
        return 0;
    }
    /* EV_ABS / EV_SW / EV_LED 等：能力位保持全零。 */
    return 0;
}

static int evdev_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    int dev = evdev_dev_from_node(node);
    if (dev < 0) return -ENODEV;
    if (EVDEV_CMD_TYPE(cmd) != (uint32_t)EVDEV_IOCTL_BASE) return -ENOTTY;
    if (!arg) return -EINVAL;

    uint32_t nr = EVDEV_CMD_NR(cmd);
    uint32_t len = EVDEV_CMD_SIZE(cmd);
    evdev_dev_t *d = &g_devs[dev];

    if (nr >= 0x20 && nr < 0x40)
        return evdev_gbit(d, nr - 0x20, (uint8_t *)arg, len);

    if (nr >= 0x40 && nr < 0x80) {   /* EVIOCGABS：本实现没有绝对轴 */
        evdev_bitmap_zero((uint8_t *)arg, sizeof(evdev_absinfo_t));
        return 0;
    }

    switch (nr) {
        case 0x01:   /* EVIOCGVERSION */
            *(int32_t *)arg = 0x00010001;
            return 0;
        case 0x02: { /* EVIOCGID */
            evdev_input_id_t *id = (evdev_input_id_t *)arg;
            id->bustype = d->bustype;
            id->vendor = d->vendor;
            id->product = d->product;
            id->version = d->version;
            return 0;
        }
        case 0x06: { /* EVIOCGNAME */
            if (!d->name || len == 0) return -EINVAL;
            uint32_t n = (uint32_t)strlen(d->name);
            if (n >= len) n = len - 1U;
            memcpy(arg, d->name, n);
            ((char *)arg)[n] = '\0';
            return 0;
        }
        case 0x07:   /* EVIOCGPHYS */
        case 0x08: { /* EVIOCGUNIQ */
            if (len) ((char *)arg)[0] = '\0';
            return 0;
        }
        case 0x09:   /* EVIOCGPROP */
        case 0x18:   /* EVIOCGKEY */
        case 0x19:   /* EVIOCGLED */
        case 0x1a:   /* EVIOCGSND */
        case 0x1b:   /* EVIOCGSW */
            evdev_bitmap_zero((uint8_t *)arg, len);
            return 0;
        case 0x90: { /* EVIOCGRAB */
            int32_t grab = *(int32_t *)arg;
            d->grabbed = grab ? 1U : 0U;
            return 0;
        }
        case 0x91:   /* EVIOCREVOKE */
            d->grabbed = 0;
            return 0;
        case 0xa0:   /* EVIOCSCLOCKID */
            return 0;
        default:
            return -ENOTTY;
    }
}

/* ═══ 自检 ══════════════════════════════════════════════════════════ */

static int g_evdev_failures;

#define EVDEV_SELFTEST_CHECK(expr)                                        \
    do {                                                                  \
        if (expr) {                                                       \
            console_puts("[EVDEV] PASS " #expr "\n");                     \
        } else {                                                          \
            console_puts("[EVDEV] FAIL " #expr "\n");                     \
            g_evdev_failures++;                                           \
        }                                                                 \
    } while (0)

int evdev_selftest(void) {
    g_evdev_failures = 0;
    evdev_init();

    vfs_node_t kbd;
    vfs_node_t mouse;
    memset(&kbd, 0, sizeof(kbd));
    memset(&mouse, 0, sizeof(mouse));
    EVDEV_SELFTEST_CHECK(evdev_register_nodes(&kbd, &mouse) == 0);
    EVDEV_SELFTEST_CHECK(evdev_is_node(&kbd) == 1);
    EVDEV_SELFTEST_CHECK(evdev_is_node(&mouse) == 1);

    char name[64];
    memset(name, 0, sizeof(name));
    EVDEV_SELFTEST_CHECK(evdev_ioctl(&kbd, EVIOCGNAME(sizeof(name)), name) == 0);
    EVDEV_SELFTEST_CHECK(strcmp(name, "HBOS virtual keyboard") == 0);

    evdev_input_id_t id;
    memset(&id, 0, sizeof(id));
    EVDEV_SELFTEST_CHECK(evdev_ioctl(&mouse, EVIOCGID, &id) == 0);
    EVDEV_SELFTEST_CHECK(id.bustype == BUS_VIRTUAL);

    uint8_t evbits[4];
    memset(evbits, 0, sizeof(evbits));
    EVDEV_SELFTEST_CHECK(evdev_ioctl(&kbd, EVIOCGBIT(0, sizeof(evbits)),
                                     evbits) == 0);
    EVDEV_SELFTEST_CHECK((evbits[EV_KEY >> 3] & (1U << (EV_KEY & 7))) != 0);
    EVDEV_SELFTEST_CHECK((evbits[EV_REL >> 3] & (1U << (EV_REL & 7))) == 0);

    uint8_t keybits[KEY_MAX / 8 + 1];
    memset(keybits, 0, sizeof(keybits));
    EVDEV_SELFTEST_CHECK(evdev_ioctl(&kbd, EVIOCGBIT(EV_KEY, sizeof(keybits)),
                                     keybits) == 0);
    EVDEV_SELFTEST_CHECK((keybits[KEY_A >> 3] & (1U << (KEY_A & 7))) != 0);

    /* 注入一条按下 + 一条抬起，read 必须按序取回 4 个事件（含 SYN）。 */
    EVDEV_SELFTEST_CHECK(evdev_inject_key(0, KEY_A, 1) == 0);
    EVDEV_SELFTEST_CHECK(evdev_inject_key(0, KEY_A, 0) == 0);
    EVDEV_SELFTEST_CHECK(evdev_event_pending(&kbd) == 1);

    evdev_input_event_t events[8];
    memset(events, 0, sizeof(events));
    int got = evdev_read(&kbd, 0, events, sizeof(events));
    EVDEV_SELFTEST_CHECK(got == (int)(4 * sizeof(evdev_input_event_t)));
    EVDEV_SELFTEST_CHECK(events[0].type == EV_KEY && events[0].code == KEY_A &&
                         events[0].value == 1);
    EVDEV_SELFTEST_CHECK(events[1].type == EV_SYN);
    EVDEV_SELFTEST_CHECK(events[2].type == EV_KEY && events[2].value == 0);
    EVDEV_SELFTEST_CHECK(evdev_event_pending(&kbd) == 0);

    /* 指针：能力位必须含 EV_REL，注入位移后能读回。 */
    memset(evbits, 0, sizeof(evbits));
    EVDEV_SELFTEST_CHECK(evdev_ioctl(&mouse, EVIOCGBIT(0, sizeof(evbits)),
                                     evbits) == 0);
    EVDEV_SELFTEST_CHECK((evbits[EV_REL >> 3] & (1U << (EV_REL & 7))) != 0);

    EVDEV_SELFTEST_CHECK(evdev_inject_rel(1, REL_X, 7) == 0);
    got = evdev_read(&mouse, 0, events, sizeof(events));
    EVDEV_SELFTEST_CHECK(got == (int)(2 * sizeof(evdev_input_event_t)));
    EVDEV_SELFTEST_CHECK(events[0].type == EV_REL && events[0].code == REL_X &&
                         events[0].value == 7);
    EVDEV_SELFTEST_CHECK(events[1].type == EV_SYN);

    /* 未知 ioctl 必须报 ENOTTY，不能被当成成功。 */
    EVDEV_SELFTEST_CHECK(evdev_ioctl(&kbd, EVDEV_IOR(0xc0, int32_t), &got) ==
                         -ENOTTY);

    if (g_evdev_failures == 0) {
        console_puts("[EVDEV] selftest: all checks passed\n");
        return 0;
    }
    console_puts("[EVDEV] selftest: failed\n");
    return -1;
}
