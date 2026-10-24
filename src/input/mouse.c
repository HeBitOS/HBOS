#include "mouse.h"
#include "../usb_hid.h"
#include "../core/cpu.h"
#include "../core/io.h"
#include "../core/wait.h"

#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

/* 1 = 严格 IntelliMouse 探测（含失败回退 + 运行时错位自检） */
#define MOUSE_ENABLE_WHEEL_PROBE 1
#define PS2_IO_TIMEOUT_MS 100U
#define PS2_RESET_TIMEOUT_MS 500U
#define PS2_MAX_STALL_SPINS 500000U

static int mouse_ready;
static int mouse_backend;
static int mouse_packet_size = 3;
/**
 * @brief PS/2 初始化成功后，还没收到任何数据包的累计轮询次数
 *
 * 真机问题：部分芯片组（以及某些虚拟化）的 i8042 aux 端口会对 0xF4 回 ACK，
 * 让 `ps2_mouse_init()` 判定"PS/2 鼠标就绪"，但端口上其实没有鼠标 —— 此后
 * `mouse_poll()` 永远什么都收不到，而 USB 后端**已经不会再被尝试**（init 只
 * 跑一次）。纯 USB 鼠标的整机（很多迷你主机/一体机）就会表现成"鼠标没有
 * 驱动"。
 *
 * 对策：PS/2 就绪后给它一段宽限期（见 MOUSE_PS2_PROBE_POLLS），期间若始终
 * 没有任何字节到达，就认定误判，切到 USB 后端。宽限期按"轮询次数"而不是
 * 时间计算，因为 GUI 主循环的轮询频率随负载变化，按次数更稳定。
 */
static uint32_t ps2_silent_polls;
/** @brief PS/2 宽限期：约 1~2 秒（GUI 主循环通常每帧轮询一次） */
#define MOUSE_PS2_PROBE_POLLS 120U

/** @brief 尝试从 PS/2 切到 USB 后端（PS/2 被误判时用）；成功返回 1 */
static int mouse_fallback_to_usb(void);
/** @brief PS/2 原始队列里是否还有待解析字节 */
static int ps2_mouse_has_data(void);
static uint8_t packet[4];
static uint8_t packet_i;
/* 128 字节只有 ~32 个包：GUI 在 1080p 下画一整帧要上百毫秒（TCG），
 * 期间 IRQ 持续塞字节，队列一满就丢字节——丢字节 = 包流错位 = 乱码包。
 * 扩到 256 少丢一些，剩下的靠下面的自愈逻辑兜底。 */
static volatile uint8_t mouse_raw_queue[256];
static volatile uint8_t mouse_raw_head = 0;
static volatile uint8_t mouse_raw_tail = 0;

enum {
    MOUSE_BACKEND_NONE = 0,
    MOUSE_BACKEND_PS2,
    MOUSE_BACKEND_USB,
};

static int wait_write(void) {
    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        if (!(io_in8(PS2_STATUS) & 0x02)) return 0;
        if (hw_deadline_expired_ms(&deadline, PS2_IO_TIMEOUT_MS,
                                   PS2_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

static int wait_read(void) {
    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        if (io_in8(PS2_STATUS) & 0x01) return 0;
        if (hw_deadline_expired_ms(&deadline, PS2_IO_TIMEOUT_MS,
                                   PS2_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

static void flush_output(void) {
    for (uint32_t i = 0; i < 32 && (io_in8(PS2_STATUS) & 0x01); i++)
        (void)io_in8(PS2_DATA);
}

static int write_cmd(uint8_t cmd) {
    if (wait_write() < 0) return -1;
    io_out8(PS2_CMD, cmd);
    return 0;
}

static int write_data(uint8_t data) {
    if (wait_write() < 0) return -1;
    io_out8(PS2_DATA, data);
    return 0;
}

static int write_mouse(uint8_t data) {
    if (write_cmd(0xD4) < 0) return -1;
    return write_data(data);
}

static int read_data(uint8_t *out) {
    if (!out || wait_read() < 0) return -1;
    *out = io_in8(PS2_DATA);
    return 0;
}

static int read_ack(void) {
    uint8_t ack = 0;
    if (read_data(&ack) < 0) return -1;
    return ack == 0xFA ? 0 : -1;
}

static void restore_interrupt_state(bool was_enabled) {
    if (was_enabled) int_enable();
}

static int wait_for_byte(uint8_t expected, uint32_t timeout_ms) {
    hw_deadline_t deadline = hw_deadline_start();
    for (;;) {
        if (io_in8(PS2_STATUS) & 0x01) {
            if (io_in8(PS2_DATA) == expected) return 0;
        }
        if (hw_deadline_expired_ms(&deadline, timeout_ms,
                                   PS2_MAX_STALL_SPINS)) return -1;
        cpu_relax();
    }
}

static int ps2_mouse_init(void) {
    mouse_packet_size = 3;
    packet_i = 0;
    mouse_raw_head = 0;
    mouse_raw_tail = 0;

    /* Disable interrupts — keyboard ISR would steal our command
     * responses from port 0x60, causing timeouts. */
    bool interrupts_were_enabled = int_get_state();
    int_disable();

    /* Drain any stale bytes in the output buffer first. Otherwise the very
     * next read_data() (for the 0x20 config response) can return a leftover
     * keyboard/aux byte and we write back a corrupt controller config, leaving
     * the mouse silent. This mattered once we boot straight into the GUI with
     * no prior get_key() draining the buffer. */
    flush_output();

    /*
     * CRITICAL: clear bit 5 (aux clock) BEFORE 0xA8.
     * VirtualBox checks bit 5 when processing 0xA8 and ignores
     * the command if the clock is disabled.
     * Also set bit 1 (enable IRQ12) so mouse data stays on its
     * own interrupt line — never routed to IRQ1 / keyboard ISR.
     */
    if (write_cmd(0x20) < 0) goto fail;
    {
        uint8_t cfg = 0;
        if (read_data(&cfg) < 0) goto fail;
        uint8_t new_cfg = (cfg & (uint8_t)~0x30) | 0x03 | 0x40;
        if (new_cfg != cfg) {
            if (write_cmd(0x60) < 0) goto fail;
            if (write_data(new_cfg) < 0) goto fail;
        }
    }

    /* Now enable auxiliary port — clock is guaranteed on */
    if (write_cmd(0xA8) < 0) goto fail;
    flush_output();

    /* IntelliMouse（滚轮）探测：标准魔法序列——连续把采样率设成
     * 200/100/80，再发 Get Device ID (0xF2)。支持滚轮的鼠标看到这个序列
     * 会把设备 ID 从 0x00 改成 0x03，之后每个数据包多带一个第 4 字节
     * （滚轮增量，见下面解析循环里 packet[3] 那段——已经写好了，只是
     * mouse_packet_size 一直停在 3，从没触发过）。任何一步失败就放弃，
     * 退回原来的 3 字节协议，不影响现有的移动/按键。 */
    /* 滚轮探测暂时关闭：那串 200/100/80 魔法序列一旦发出去，设备就进入
     * 4 字节包模式；只要 ID 读取失败/时序抖动（TCG 下常见），就会出现
     * "设备 4 字节 / 解析 3 字节"（X 锁死）或反向错位（Y 读到 flags 字节
     * 0x08，取反后恒为负 → 指针被顶在屏幕上边缘）。
     * 关掉它 → 设备固定在 3 字节默认模式，与解析一致，X/Y 都正常。
     * 需要滚轮时把这里改成 1，并确保 ID 读取成功率为 100%。 */
#if MOUSE_ENABLE_WHEEL_PROBE
    /* 严格的 IntelliMouse 探测（顺序照 Linux psmouse）：
     *   0xF5 关上报 → 0xF3 200/100/80 → 0xF2 取 ID → 只认 0x03/0x04
     * 任何一步失败/ID 不认，就用 0xF6(set defaults) 把设备拉回 3 字节。
     * 切模式后必须清空队列与解析状态，否则残留字节会和新包长混在一起
     * —— 这正是"X 锁死 / 指针贴顶边"的来源。 */
    mouse_packet_size = 3;
    (void)write_mouse(0xF5);
    (void)read_ack();
    flush_output();
    {
        uint8_t rates[3] = {200, 100, 80};
        int ok = 1;
        int wheel = 0;
        for (int i = 0; i < 3 && ok; i++) {
            if (write_mouse(0xF3) < 0 || read_ack() < 0) ok = 0;
            else if (write_mouse(rates[i]) < 0 || read_ack() < 0) ok = 0;
        }
        if (ok && write_mouse(0xF2) == 0 && read_ack() == 0) {
            uint8_t id = 0xFF;
            if (read_data(&id) == 0 && (id == 0x03 || id == 0x04)) wheel = 1;
        }
        if (!wheel) {
            (void)write_mouse(0xF6);
            (void)read_ack();
        }
        mouse_packet_size = wheel ? 4 : 3;
    }
    flush_output();
    packet_i = 0;
    mouse_raw_head = 0;
    mouse_raw_tail = 0;
#else
    mouse_packet_size = 3;
#endif

    /* Enable data reporting */
    for (int attempt = 0; attempt < 4; attempt++) {
        flush_output();
        if (write_mouse(0xF4) == 0 && read_ack() == 0) {
            packet_i = 0;
            mouse_raw_head = 0;
            mouse_raw_tail = 0;
            /* Unmask the cascade (IRQ2 on the master) and IRQ12 (mouse) on the
             * slave PIC so mouse bytes are captured by the IRQ handler into the
             * raw queue. Only IRQ1 (keyboard) was unmasked before, so the mouse
             * ran on polling alone — fine for slow motion but the GUI loop can't
             * poll the 1-byte i8042 buffer fast enough for a real mouse's byte
             * stream, dropping bytes and desyncing packets (cursor freezes after
             * the first move). */
            io_out8(0x21, (uint8_t)(io_in8(0x21) & ~0x04)); /* IRQ2 */
            io_out8(0xA1, (uint8_t)(io_in8(0xA1) & ~0x10)); /* IRQ12 */
            restore_interrupt_state(interrupts_were_enabled);
            return 0;
        }
        /* 0xF4 failed — reset mouse and retry */
        flush_output();
        if (write_mouse(0xFF) < 0) continue;
        /* ACK 后等待设备自检通过；忽略途中其它响应字节。 */
        (void)wait_for_byte(0xAA, PS2_RESET_TIMEOUT_MS);
        /* Drain device ID byte */
        if (wait_read() == 0) (void)io_in8(PS2_DATA);
    }

fail:
    restore_interrupt_state(interrupts_were_enabled);
    return -1;
}

int mouse_init(void) {
    mouse_ready = 0;
    mouse_backend = MOUSE_BACKEND_NONE;

    ps2_silent_polls = 0;
    if (ps2_mouse_init() == 0) {
        mouse_ready = 1;
        mouse_backend = MOUSE_BACKEND_PS2;
        return 0;
    }

    if (usb_mouse_init() == 0) {
        mouse_ready = 1;
        mouse_backend = MOUSE_BACKEND_USB;
        return 0;
    }

    return -1;
}

void mouse_shutdown(void) {
    if (!mouse_ready) return;
    if (mouse_backend != MOUSE_BACKEND_PS2) {
        mouse_ready = 0;
        mouse_backend = MOUSE_BACKEND_NONE;
        return;
    }
    (void)write_mouse(0xF5);
    (void)read_ack();
    flush_output();
    mouse_ready = 0;
    mouse_backend = MOUSE_BACKEND_NONE;
    packet_i = 0;
}

extern void kb_clear_modifiers(void);



void ps2_mouse_enqueue_byte(uint8_t b) {
    uint8_t next = (uint8_t)((mouse_raw_head + 1) & 255);
    if (next == mouse_raw_tail) return; /* drop if full */
    mouse_raw_queue[mouse_raw_head] = b;
    mouse_raw_head = next;
}

static int ps2_mouse_dequeue_byte(void) {
    if (mouse_raw_tail == mouse_raw_head) return -1;
    uint8_t b = mouse_raw_queue[mouse_raw_tail];
    mouse_raw_tail = (uint8_t)((mouse_raw_tail + 1) & 255);
    return b;
}

/** @brief PS/2 原始队列里是否还有待解析字节（宽限期判定用） */
static int ps2_mouse_has_data(void) {
    return mouse_raw_tail != mouse_raw_head;
}

/* 每个鼠标包对应一个事件。之前是把一轮里所有包累加成"一个"事件，只保留
 * 最后的按键状态——QEMU 的 sendkey/mouse_button 是瞬间按下+松开，两次边沿
 * 会被合并成 buttons=0 的单个事件，GUI 于是完全看不到这次点击。真机上人手
 * 点击跨越多帧所以掩盖了这个 bug。 */
#define PS2_Q 16
static mouse_event_t ps2_q[PS2_Q];
static uint8_t ps2_q_head, ps2_q_tail;

static void ps2_q_push(int dx, int dy, int dz, int buttons) {
    uint8_t next = (uint8_t)((ps2_q_head + 1) & (PS2_Q - 1));
    if (next == ps2_q_tail) return;   /* 满：丢最旧的移动，按键不丢 */
    ps2_q[ps2_q_head].dx = dx;
    ps2_q[ps2_q_head].dy = dy;
    ps2_q[ps2_q_head].dz = dz;
    ps2_q[ps2_q_head].buttons = buttons;
    ps2_q_head = next;
}

static void ps2_button_edge(int buttons) {
    static int last_buttons = 0;
    if (buttons != last_buttons) {
        kb_clear_modifiers();
        last_buttons = buttons;
    }
}

/* ── 包流自愈 ─────────────────────────────────────────────────
 * 正常对齐时，每个包的首字节一定是 flags（bit3 恒为 1），被同步检查
 * 跳过的字节应该非常少。一旦"包长和设备实际包长不一致"（滚轮探测
 * 半途失败：设备进了 wheel 模式发 4 字节，我们却按 3 字节收，或相反），
 * 流会每包漂移 1 字节：X 的位置读到的变成近似常数，真正的 X 位移跑到
 * Y 的位置上——表现就是"只能沿 Y 移动，X 锁死"。
 * 跳过数超过完整包数就把包长翻过来重试，几帧内自动恢复。 */
static uint32_t sync_skips, sync_oks;

static void ps2_resync_check(void) {
    uint32_t total = sync_skips + sync_oks;
    if (total < 96) return;
    if (sync_skips > sync_oks) {
        mouse_packet_size = (mouse_packet_size == 3) ? 4 : 3;
        packet_i = 0;
    }
    sync_skips = 0;
    sync_oks = 0;
}

static int ps2_mouse_poll(mouse_event_t *ev) {
    if (ps2_q_tail != ps2_q_head) {
        *ev = ps2_q[ps2_q_tail];
        ps2_q_tail = (uint8_t)((ps2_q_tail + 1) & (PS2_Q - 1));
        ps2_button_edge(ev->buttons);
        return 1;
    }

    while (1) {
        int queued = ps2_mouse_dequeue_byte();
        uint8_t b = 0;
        if (queued >= 0) {
            b = (uint8_t)queued;
        } else {
            /* Fallback if no queued bytes (e.g. IRQ12 unavailable): poll the
             * physical port. With IRQ12 unmasked the ISR fills the queue and
             * this rarely runs. */
            bool interrupts_were_enabled = int_get_state();
            int_disable();
            if (io_in8(PS2_STATUS) & 0x01) {
                uint8_t status = io_in8(PS2_STATUS);
                if (status & 0x20) {
                    b = io_in8(PS2_DATA);
                }
            }
            restore_interrupt_state(interrupts_were_enabled);
        }
        if (b == 0 && queued < 0) break;

        if (packet_i == 0 && !(b & 0x08)) {
            sync_skips++;
            ps2_resync_check();
            continue;
        }
        packet[packet_i++] = b;
        if (packet_i < mouse_packet_size) continue;

        packet_i = 0;
        sync_oks++;
        ps2_resync_check();
        int x = (int)packet[1];
        int y = (int)packet[2];
        if (packet[0] & 0x10) x -= 256;
        if (packet[0] & 0x20) y -= 256;
        if (packet[0] & 0xC0) {
            /* X/Y 溢出位：这份样本不可信（多半是流丢过字节后凑出来的
             * 假包）。旧行为只把溢出的那一轴清 0——0x40 一旦被错误置位，
             * X 就恒为 0，正好是"只能沿 Y 移动"的症状。整包丢弃：
             * 高速甩鼠标丢一个样本肉眼不可见。 */
            continue;
        }
        /* X 锁死自适应：设备包长与 mouse_packet_size 不一致时（滚轮探测
         * 半途失败 / 设备被魔法序列留在 4 字节模式），X 位移会一直为 0 而
         * Y 正常。连续 24 个包都这样就把包长翻过来，几帧内自愈，不依赖
         * 96 字节的 sync 统计（那条在"错位后仍能对齐 flags 字节"时不触发）。 */
        static uint32_t x_stuck_packets;
        if (x == 0 && y != 0) {
            if (++x_stuck_packets >= 24) {
                mouse_packet_size = (mouse_packet_size == 3) ? 4 : 3;
                packet_i = 0;
                x_stuck_packets = 0;
            }
        } else if (x != 0) {
            x_stuck_packets = 0;
        }
        int z = 0;
        if (mouse_packet_size == 4) {
            z = (int)(packet[3] & 0x0F);
            if (z & 0x08) z -= 16;
        }
        /* 运行时错位自检：设备其实是 3 字节包时，被当成第 4 字节的往往是
         * 下一个包的 flags(0x08 起) —— z 会连续恒为同一个非零值。
         * 连续 24 包如此就认定错位，退回 3 字节并重置解析。 */
        {
            static int last_z, z_stuck;
            if (mouse_packet_size == 4 && z != 0 && z == last_z) {
                if (++z_stuck >= 24) {
                    mouse_packet_size = 3;
                    packet_i = 0;
                    z_stuck = 0;
                }
            } else {
                z_stuck = 0;
            }
            last_z = z;
        }
        ps2_q_push(x, -y, z, (int)(packet[0] & 0x07));
    }

    if (ps2_q_tail != ps2_q_head) {
        *ev = ps2_q[ps2_q_tail];
        ps2_q_tail = (uint8_t)((ps2_q_tail + 1) & (PS2_Q - 1));
        ps2_button_edge(ev->buttons);
        return 1;
    }
    return 0;
}


/**
 * @brief USB 后端取一次鼠标事件
 *
 * 注意 `hid_mouse_report_t` 里已经是**解好的状态**（usb_hid.c 按报告描述符
 * 的字段布局解位），这里的 dx/dy/dz 可能是 8/12/16 位，不再假定 int8_t；
 * 位移依旧按 PS/2 后端同样的约定累加到 mouse_event_t。
 */
static int usb_mouse_poll(mouse_event_t *ev) {
    hid_mouse_report_t report;
    if (usb_mouse_get_report(&report) < 0) return 0;
    if (report.dx == 0 && report.dy == 0 && report.dz == 0 &&
        report.buttons == 0) return 0;

    static uint32_t last_usb_buttons = 0;
    if (report.buttons != last_usb_buttons) {
        kb_clear_modifiers();
        last_usb_buttons = report.buttons;
    }

    ev->dx = report.dx;
    ev->dy = report.dy;
    ev->dz = report.dz;
    ev->buttons = (int)(report.buttons & (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE));
    return 1;
}

int mouse_poll(mouse_event_t *ev) {
    if (!mouse_ready || !ev) return 0;
    if (mouse_backend == MOUSE_BACKEND_USB) return usb_mouse_poll(ev);
    if (mouse_backend == MOUSE_BACKEND_PS2) {
        int got = ps2_mouse_poll(ev);
        if (got) {
            ps2_silent_polls = 0;
            return got;
        }
        /* PS/2 一直没数据：过了宽限期就尝试切 USB（真机纯 USB 鼠标场景）。
         * 判定用"原始队列是否有字节"而不是"poll 是否有事件"：移动量为 0 的
         * 数据包解析成功但不产生事件，那说明 PS/2 是活的，不该被判为误判。 */
        if (ps2_mouse_has_data()) {
            ps2_silent_polls = 0;
            return 0;
        }
        if (ps2_silent_polls < MOUSE_PS2_PROBE_POLLS) {
            ps2_silent_polls++;
            return 0;
        }
        ps2_silent_polls = 0;
        if (mouse_fallback_to_usb()) return usb_mouse_poll(ev);
        return 0;
    }
    return 0;
}

/**
 * @brief PS/2 无数据时尝试改用 USB 后端
 *
 * 只在"PS/2 队列空 + USB 上确实有鼠标设备"时才切换，避免把一台真的接了
 * PS/2 鼠标、只是暂时没动的机器误切走。切换后原 PS/2 端口保持使能状态，
 * 不影响键盘（键盘走 IRQ1，与本端口无关）。
 *
 * @return 1 已切到 USB 且就绪，0 未切换
 */
static int mouse_fallback_to_usb(void) {
    if (usb_mouse_init() != 0) return 0;
    mouse_backend = MOUSE_BACKEND_USB;
    ps2_silent_polls = 0;
    return 1;
}

int mouse_is_ready(void) {
    return mouse_ready;
}

const char *mouse_backend_name(void) {
    if (mouse_backend == MOUSE_BACKEND_PS2) return "ps2";
    if (mouse_backend == MOUSE_BACKEND_USB) return "usb-hid";
    return "none";
}
