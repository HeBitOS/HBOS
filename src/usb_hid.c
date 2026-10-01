/**
 * @file usb_hid.c
 * @brief USB HID 类驱动：枚举接口、读报告描述符、按字段布局解鼠标/键盘报告
 *
 * 性能要点 / 不变式
 *  - 报告描述符只在枚举时读一次，解析结果（位偏移/位宽）缓存在 hid_device_t 里，
 *    热路径（每次中断 IN）只做位提取，不再碰 USB 控制传输；
 *  - 报告缓冲按"该设备报告描述符声明的最大 Input 报告长度"取，而不是写死 8/4
 *    字节：真机上带 Report ID 或扩展字段的鼠标报告比 boot 布局长，固定长度会
 *    截断并错位；
 *  - 纯轮询：没有中断上下文，所有传输失败都以 -1 返回给上层。
 */

#include "usb_hid.h"
#include "usb_host.h"
#include "core/heap.h"
#include "core/wait.h"
#include "string.h"

extern void console_puts(const char *s);
extern void console_putchar(char c);
static void hid_dbg_hex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    console_putchar(hex[(v >> 4) & 0xF]);
    console_putchar(hex[v & 0xF]);
}

static hid_device_t hid_devices[HID_MAX_DEVICES];
static int hid_count;
static hid_kbd_report_t last_report;
static int usb_kbd_initialized;
static int usb_kbd_index = -1;
static int usb_hid_initialized;
static int usb_mouse_initialized;
static int usb_mouse_index = -1;

#define USB_KEY_UP      0x100
#define USB_KEY_DOWN    0x101
#define USB_KEY_LEFT    0x102
#define USB_KEY_RIGHT   0x103
#define USB_KEY_PGUP    0x104
#define USB_KEY_PGDWN   0x105
#define USB_KEY_HOME    0x106
#define USB_KEY_END     0x107
#define USB_KEY_INSERT  0x108
#define USB_KEY_DELETE  0x109
#define USB_KEY_SHIFT_UP    0x110
#define USB_KEY_SHIFT_DOWN  0x111
#define USB_KEY_SHIFT_LEFT  0x112
#define USB_KEY_SHIFT_RIGHT 0x113

/* USB HID keycode → ASCII (0x04..0x57 → a-z, 1-9, 0, Enter, etc.) */
static int usb_keycode_to_key(uint8_t kc, int shift, int ctrl) {
    if (kc >= 0x04 && kc <= 0x1D && ctrl) {
        char ctrl_c = (kc - 0x04) + 1;
        if (ctrl_c == 0x13 || ctrl_c == 0x11 || ctrl_c == 0x03) return ctrl_c;
        return shift ? 'A' + (kc - 0x04) : 'a' + (kc - 0x04);
    }
    if (kc >= 0x04 && kc <= 0x1D) return shift ? 'A' + (kc - 0x04) : 'a' + (kc - 0x04);
    if (kc >= 0x1E && kc <= 0x26) return shift ? "!@#$%^&*("[kc - 0x1E] : '1' + (kc - 0x1E);
    if (kc == 0x27) return shift ? ')' : '0';
    if (kc == 0x28) return '\n';
    if (kc == 0x29) return 27;
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
    if (kc == 0x49) return USB_KEY_INSERT;
    if (kc == 0x4A) return USB_KEY_HOME;
    if (kc == 0x4B) return USB_KEY_PGUP;
    if (kc == 0x4C) return USB_KEY_DELETE;
    if (kc == 0x4D) return USB_KEY_END;
    if (kc == 0x4E) return USB_KEY_PGDWN;
    if (kc == 0x4F) return shift ? USB_KEY_SHIFT_RIGHT : USB_KEY_RIGHT;
    if (kc == 0x50) return shift ? USB_KEY_SHIFT_LEFT  : USB_KEY_LEFT;
    if (kc == 0x51) return shift ? USB_KEY_SHIFT_DOWN  : USB_KEY_DOWN;
    if (kc == 0x52) return shift ? USB_KEY_SHIFT_UP    : USB_KEY_UP;
    return 0;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int hid_add_device(int type, int host_idx, int dev_idx, int interface_num,
                          int ep_addr, uint16_t max_packet, uint8_t interval) {
    if (hid_count >= HID_MAX_DEVICES) return 0;
    hid_device_t *dev = &hid_devices[hid_count++];
    memset(dev, 0, sizeof(*dev));
    dev->type = type;
    dev->host_idx = host_idx;
    dev->dev_idx = dev_idx;
    dev->interface_num = interface_num;
    dev->ep_addr = ep_addr;
    dev->max_packet_size = max_packet ? max_packet : 8;
    dev->interval = interval;
    dev->report_len = type == HID_MOUSE ? (int)sizeof(hid_mouse_report_t) : (int)sizeof(hid_kbd_report_t);
    dev->active = 1;
    dev->mouse_boot_mode = 1;   /* 未解析出报告描述符前，先按 boot 布局 */
    return 1;
}

/**
 * @brief 请求 HID 接口使用 **report** protocol（SET_PROTOCOL, 1 = report）
 *
 * 旧实现发的是 SET_PROTOCOL(0)（boot protocol），这是真机鼠标失效的直接原因：
 * Linux 的 usbhid **从不**发这条请求（全树只有定义与设备侧 gadget 用到），它
 * 依赖 HID 1.11 §7.2.6 的"上电默认即 report protocol"，总是读报告描述符
 * (`bDescriptorType=0x22`) 并按其字段偏移解析。对 `bInterfaceProtocol=0`
 * （不声明 boot 支持的现代鼠标）来说，boot protocol 根本不存在，强制切过去
 * 会让设备改用 3 字节固定格式，而我们按描述符算出的偏移就全错位了。
 *
 * 这里显式发 1 是为了纠正可能被固件切到 boot 的设备；**失败必须容忍**：
 * 很多设备（含 QEMU 的 usb-mouse）对这条请求直接 STALL，而它们本来就在
 * report protocol 下工作，STALL 不代表后续收发会失败。
 */
static void hid_request_report_protocol(int host_idx, int dev_idx, int interface_num) {
    const usb_host_ops_t *host = usb_host_get(host_idx);
    if (!host) return;
    (void)host->control_transfer(dev_idx, 0x21, 11, 1,
                                 (uint16_t)interface_num, 0, 0);
}

/** @brief HID 类描述符类型码：HID descriptor / Report descriptor */
#define HID_DT_HID    0x21
#define HID_DT_REPORT 0x22

/**
 * @brief SET_IDLE(0, 0)：让设备"只在状态变化时上报"，而不是每个中断周期都发
 *
 * Linux usbhid 在 `usbhid_start()` 之前就发这一条（`usbhid/hid-core.c` 的
 * `hid_set_idle(dev, ifnum, 0, 0)`）。对轮询模型尤其重要：真机鼠标按 1kHz
 * 上报时，每 10ms 轮询一次却要清掉上百个报告，而我们要的是"最新状态"。
 * 失败不影响功能（部分设备不支持 IDLE）。
 */
static void hid_set_idle(int host_idx, int dev_idx, int interface_num) {
    const usb_host_ops_t *host = usb_host_get(host_idx);
    if (!host) return;
    (void)host->control_transfer(dev_idx, 0x21, 0x0A, 0,
                                 (uint16_t)interface_num, 0, 0);
}

/**
 * @brief 读取并解析某个 HID 接口的报告描述符，缓存鼠标字段布局
 *
 * 顺序照抄 Linux usbhid（`usbhid/hid-core.c: hid_start_parse` 之前的取值）：
 * 先在配置描述符里找到 HID 描述符里的 wDescriptorLength，再
 * `GET_DESCRIPTOR(0x22)` 把描述符整块取回，最后解析。任何一步失败都只是
 * "退化成 boot 布局"，不影响设备可用性。
 *
 * @return 解析出的鼠标布局数量（>0 表示鼠标并已缓存布局），否则 0
 */
static int hid_load_mouse_layout(hid_device_t *dev) {
    const usb_host_ops_t *host = usb_host_get(dev->host_idx);
    if (!host) return 0;

    /* HID 描述符在接口描述符之后：wDescriptorLength 位于偏移 7（LE16）。
     * 直接向接口请求 Report 描述符也可以，但拿不到长度；这里用固定上限读，
     * 由设备返回实际长度。HID_MAX_DESCRIPTOR_SIZE 取 Linux 的上限 4096。 */
    uint8_t *desc = (uint8_t *)kmalloc(4096);
    if (!desc) return 0;

    int ret = host->control_transfer(dev->dev_idx, 0x81, 6, 0x2200,
                                     (uint16_t)dev->interface_num, desc, 4096);
    if (ret <= 0) {
        kfree(desc);
        return 0;
    }

    int n = hid_parse_mouse_layouts(desc, (uint32_t)ret, dev->mouse_layouts,
                                   HID_MAX_MOUSE_LAYOUTS);
    kfree(desc);
    if (n <= 0) return 0;

    dev->mouse_layout_count = n;
    dev->mouse_boot_mode = 0;
    console_puts("[HID] mouse report descriptor parsed, layouts=");
    hid_dbg_hex8((uint8_t)n);
    console_puts(" (report protocol)\n");
    return n;
}

static int hid_probe_config(int host_idx, int dev_idx) {
    const usb_host_ops_t *host = usb_host_get(host_idx);
    if (!host) return 0;

    uint8_t hdr[9];
    if (host->control_transfer(dev_idx, 0x80, 6, 0x0200, 0, hdr, sizeof(hdr)) < 0)
        return 0;

    uint16_t total = rd16(hdr + 2);
    if (total < sizeof(hdr)) return 0;
    if (total > 1024) total = 1024;

    uint8_t *buf = (uint8_t *)kmalloc(total);
    if (!buf) return 0;
    int ret = host->control_transfer(dev_idx, 0x80, 6, 0x0200, 0, buf, total);
    if (ret < 0) {
        kfree(buf);
        return 0;
    }

    int added = 0;
    uint16_t got = ret > 0 && ret < total ? (uint16_t)ret : total;
    uint8_t cfg_value = got >= 6 ? buf[5] : 0;
    if (cfg_value) (void)host->control_transfer(dev_idx, 0x00, 9, cfg_value, 0, 0, 0);

    int current_type = 0;
    int current_if = -1;
    for (uint16_t off = 0; off + 2 <= got; ) {
        uint8_t len = buf[off];
        uint8_t dtype = buf[off + 1];
        if (len < 2 || off + len > got) break;

        if (dtype == 4 && len >= 9) {
            uint8_t if_class = buf[off + 5];
            uint8_t if_subclass = buf[off + 6];
            uint8_t if_protocol = buf[off + 7];
            current_if = buf[off + 2];
            current_type = 0;
            if (if_class == 0x03) {
                console_puts("[HID] interface class=03 subclass=");
                hid_dbg_hex8(if_subclass);
                console_puts(" protocol=");
                hid_dbg_hex8(if_protocol);
                console_puts("\n");
            }
            if (if_class == 0x03 && if_protocol == 1) current_type = HID_KEYBOARD;
            else if (if_class == 0x03 && if_protocol == 2) current_type = HID_MOUSE;
            /* Real mice frequently leave bInterfaceProtocol at 0 ("none" —
             * no boot-protocol support declared, since they use a custom
             * report format for extra buttons/DPI switches etc.) even
             * though they still honor a SET_PROTOCOL(boot) request and send
             * boot-compatible reports as a practical legacy/BIOS-compat
             * fallback — this is how real BIOS/UEFI USB mouse support gets
             * away without ever parsing a HID Report Descriptor either.
             * QEMU's emulated usb-mouse always cleanly declares protocol 2,
             * which is why this gap never showed up before real hardware.
             * Keyboards essentially always declare protocol 1 (near-
             * universal BIOS legacy-keyboard requirement), so biasing an
             * undeclared HID interface toward "mouse" is the safer guess. */
            else if (if_class == 0x03 && if_protocol == 0) current_type = HID_MOUSE;
        } else if (dtype == 5 && len >= 7 && current_type) {
            uint8_t ep_addr = buf[off + 2];
            uint8_t attrs = buf[off + 3] & 0x03;
            if ((ep_addr & 0x80) && attrs == 0x03) {
                uint16_t max_packet = rd16(buf + off + 4) & 0x07FF;
                uint8_t interval = buf[off + 6];
                int ok = hid_add_device(current_type, host_idx, dev_idx,
                                        current_if, ep_addr, max_packet, interval);
                added += ok;
                if (ok) {
                    /* Linux 先 SET_IDLE 再读描述符；两者之间没有依赖，顺序上
                     * 只要"解析完成后再请求 report protocol"即可——先请求再
                     * 解析会让部分设备提前切到 boot 格式，拿到的描述符仍是对的，
                     * 但为了与 Linux 行为一致，这里统一放在解析之后。 */
                    hid_device_t *hdev = &hid_devices[hid_count - 1];
                    hid_set_idle(host_idx, dev_idx, current_if);
                    if (hdev->type == HID_MOUSE) (void)hid_load_mouse_layout(hdev);
                    hid_request_report_protocol(host_idx, dev_idx, current_if);
                }
                console_puts(ok ? "[HID] registered as " : "[HID] add failed, wanted ");
                console_puts(current_type == HID_MOUSE ? "mouse\n" : "keyboard\n");
                current_type = 0;
            }
        }
        off += len;
    }

    kfree(buf);
    return added;
}

/**
 * @brief 退路：设备级（而非接口级）声明自己是 HID 时的注册
 *
 * 少数设备把 HID 类信息放在 device descriptor 上（bDeviceClass=0x03），
 * 接口描述符里不再重复；此时按设备协议号猜键鼠，端点固定用 0x81 / 8 字节
 * boot 布局。这是最后手段，正常情况下 hid_probe_config() 已经能注册成功。
 */
static int hid_probe_device_desc(int host_idx, int dev_idx,
                                 const usb_device_desc_t *desc) {
    if (!desc || desc->bDeviceClass != 0x03) return 0;

    int type = 0;
    if (desc->bDeviceProtocol == 1) type = HID_KEYBOARD;
    else if (desc->bDeviceProtocol == 2) type = HID_MOUSE;
    else type = HID_KEYBOARD;

    int added = hid_add_device(type, host_idx, dev_idx, 0, 0x81, 8, 10);
    if (added) {
        hid_device_t *hdev = &hid_devices[hid_count - 1];
        hid_set_idle(host_idx, dev_idx, 0);
        if (hdev->type == HID_MOUSE) (void)hid_load_mouse_layout(hdev);
        hid_request_report_protocol(host_idx, dev_idx, 0);
    }
    return added;
}

int hid_init(void) {
    hid_count = 0;
    usb_kbd_index = -1;
    memset(hid_devices, 0, sizeof(hid_devices));

    /* 遍历"所有 USB 主机控制器 × 各自的设备"：真机上键鼠可能挂在 EHCI
     * （USB 2.0）而 xHCI 上一个设备都没有，只遍历一个控制器就会漏掉。 */
    for (int h = 0; h < usb_host_count() && hid_count < HID_MAX_DEVICES; h++) {
        const usb_host_ops_t *host = usb_host_get(h);
        if (!host) continue;
        int dev_count = host->device_count();
        for (int i = 0; i < dev_count && hid_count < HID_MAX_DEVICES; i++) {
            usb_device_desc_t desc;
            if (host->get_device_desc(i, &desc) < 0) continue;

            /* Skip VMware virtual USB devices (0x0E0F and 0x15AD) to force PS/2 fallback,
             * as VMware's USB emulation drops keystrokes with our XHCI driver.
             * PS/2 works perfectly in VMware as proven in 0.1_beta2. */
            if (desc.idVendor == 0x0E0F || desc.idVendor == 0x15AD) continue;

            int before = hid_count;
            (void)hid_probe_config(h, i);
            if (hid_count == before) (void)hid_probe_device_desc(h, i, &desc);
        }
    }

    return hid_count;
}

int hid_device_count(void) {
    return hid_count;
}

int hid_keyboard_count(void) {
    int count = 0;
    for (int i = 0; i < hid_count; i++) {
        if (hid_devices[i].active && hid_devices[i].type == HID_KEYBOARD) count++;
    }
    return count;
}

int hid_mouse_count(void) {
    int count = 0;
    for (int i = 0; i < hid_count; i++) {
        if (hid_devices[i].active && hid_devices[i].type == HID_MOUSE) count++;
    }
    return count;
}

int hid_get_keyboard_report(int idx, hid_kbd_report_t *report) {
    if (idx < 0 || idx >= hid_count) return -1;
    hid_device_t *dev = &hid_devices[idx];
    if (!dev->active || dev->type != HID_KEYBOARD) return -1;
    if (!report) return -1;

    const usb_host_ops_t *host = usb_host_get(dev->host_idx);
    if (!host) return -1;
    return host->interrupt_transfer(dev->dev_idx, dev->ep_addr,
                                    report, sizeof(hid_kbd_report_t), dev->interval);
}

/** @brief boot 协议鼠标报告的固定布局：byte0 按键，byte1/2 为 X/Y，byte3 滚轮 */
static void hid_mouse_decode_boot(const uint8_t *raw, int len,
                                  hid_mouse_report_t *report) {
    report->buttons = raw[0];
    report->dx = (int)(int8_t)raw[1];
    report->dy = (int)(int8_t)raw[2];
    report->dz = len >= 4 ? (int)(int8_t)raw[3] : 0;
}

/**
 * @brief 取一次鼠标报告（report protocol 优先，boot 布局回退）
 *
 * 返回的是"解好的状态"而不是原始字节：调用方（mouse.c）不关心设备用的是
 * 3 字节 boot 报告还是带 Report ID 的 8 字节自定义报告。
 */
int hid_get_mouse_report(int idx, hid_mouse_report_t *report) {
    if (idx < 0 || idx >= hid_count) return -1;
    hid_device_t *dev = &hid_devices[idx];
    if (!dev->active || dev->type != HID_MOUSE) return -1;
    if (!report) return -1;

    const usb_host_ops_t *host = usb_host_get(dev->host_idx);
    if (!host) return -1;

    /* 报告长度按描述符声明的最大 Input 报告取，并给 Report ID 前缀留 1 字节；
     * 上限 64 字节（HID 规范允许更长，但输入设备不会用到）。 */
    uint8_t raw[64];
    uint32_t want = sizeof(raw);
    if (!dev->mouse_boot_mode && dev->mouse_layout_count > 0) {
        uint32_t bits = dev->mouse_layouts[0].report_len_bits;
        uint32_t bytes = (bits + 7U) / 8U + 1U; /* +1 给 Report ID 前缀 */
        if (bytes < 4U) bytes = 4U;
        if (bytes > sizeof(raw)) bytes = sizeof(raw);
        want = bytes;
    }
    memset(raw, 0, sizeof(raw));

    int got = host->interrupt_transfer(dev->dev_idx, dev->ep_addr, raw, want,
                                       dev->interval);
    if (got < 0) return -1;

    if (!dev->mouse_boot_mode && dev->mouse_layout_count > 0) {
        /* 多个布局（多个 Report ID）时按报文首字节挑：带 ID 的报告首字节就是
         * Report ID；无 ID 的报告（report_id==0）与之不会同时存在。 */
        const hid_mouse_layout_t *layout = &dev->mouse_layouts[0];
        for (int i = 0; i < dev->mouse_layout_count; i++) {
            const hid_mouse_layout_t *cand = &dev->mouse_layouts[i];
            if (cand->report_id == 0) { layout = cand; break; }
            if (got >= 1 && raw[0] == cand->report_id) { layout = cand; break; }
        }
        int dx = 0, dy = 0, wheel = 0;
        uint32_t buttons = 0;
        if (hid_mouse_decode(layout, raw, (uint32_t)got, &dx, &dy, &wheel,
                             &buttons) == 0) {
            report->buttons = buttons;
            report->dx = dx;
            report->dy = dy;
            report->dz = wheel;
            return 0;
        }
        /* 布局解不动（例如设备其实还在 boot 格式）：落到 boot 布局试一次，
         * 总比丢帧让鼠标完全不动好。 */
    }

    if (got < 3) return -1;
    hid_mouse_decode_boot(raw, got, report);
    return 0;
}

void hid_poll(void) {
    usb_host_poll_all();
}

int usb_hid_init(void) {
    if (usb_hid_initialized) return 0;
    if (usb_host_init_all() <= 0) return -1;
    if (hid_init() <= 0) return -1;
    usb_hid_initialized = 1;
    return 0;
}

/** 初始化 USB HID 键盘（由 shell 调用） */
int usb_kbd_init(void) {
    if (usb_kbd_initialized) return 0;
    if (usb_hid_init() < 0) return -1;
    for (int i = 0; i < hid_count; i++) {
        if (hid_devices[i].active && hid_devices[i].type == HID_KEYBOARD) {
            usb_kbd_index = i;
            break;
        }
    }
    if (usb_kbd_index < 0) return -1;
    memset(&last_report, 0, sizeof(last_report));
    usb_kbd_initialized = 1;
    return 0;
}

/** 从 USB 键盘读取一个字符，无输入返回 0 */
int usb_kbd_getc(void) {
    if (!usb_kbd_initialized) {
        static uint64_t retry_deadline;
        if (pit_deadline_reached(retry_deadline)) {
            retry_deadline = pit_deadline_after_ms(2000);
            (void)usb_kbd_init();
        }
        if (!usb_kbd_initialized) return 0;
    }

    static uint64_t poll_deadline;
    /* Throttle to 15ms interval to prevent pegging the CPU during polling */
    if (!pit_deadline_reached(poll_deadline)) return 0;
    poll_deadline = pit_deadline_after_ms(15);

    hid_kbd_report_t report;
    if (hid_get_keyboard_report(usb_kbd_index, &report) < 0) return 0;

    /* 检查是否有新的按键（与上次报告比较） */
    int shift = (report.modifiers & 0x22) != 0; /* Left/Right Shift */
    int ctrl = (report.modifiers & 0x11) != 0;  /* Left/Right Ctrl */

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
            return usb_keycode_to_key(kc, shift, ctrl);
        }
    }

    last_report = report;
    return 0;
}

int usb_kbd_ready(void) {
    return usb_kbd_initialized;
}

int usb_mouse_init(void) {
    if (usb_mouse_initialized) return 0;
    if (usb_hid_init() < 0) return -1;
    for (int i = 0; i < hid_count; i++) {
        if (hid_devices[i].active && hid_devices[i].type == HID_MOUSE) {
            usb_mouse_index = i;
            break;
        }
    }
    if (usb_mouse_index < 0) return -1;
    usb_mouse_initialized = 1;
    return 0;
}

int usb_mouse_get_report(hid_mouse_report_t *report) {
    if (!usb_mouse_initialized || !report) {
        if (report) {
            static uint64_t retry_deadline;
            if (pit_deadline_reached(retry_deadline)) {
                retry_deadline = pit_deadline_after_ms(2000);
                (void)usb_mouse_init();
            }
        }
        if (!usb_mouse_initialized) return -1;
    }

    static uint64_t poll_deadline;
    /* Throttle to 10ms interval to prevent pegging the CPU during mouse polling */
    if (!pit_deadline_reached(poll_deadline)) return -1;
    poll_deadline = pit_deadline_after_ms(10);

    if (hid_get_mouse_report(usb_mouse_index, report) < 0) return -1;
    return 0;
}

int usb_mouse_ready(void) {
    return usb_mouse_initialized;
}
