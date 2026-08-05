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

#define PS2_IO_TIMEOUT_MS 100U
#define PS2_RESET_TIMEOUT_MS 500U
#define PS2_MAX_STALL_SPINS 500000U

static int mouse_ready;
static int mouse_backend;
static int mouse_packet_size = 3;
static uint8_t packet[4];
static uint8_t packet_i;
static volatile uint8_t mouse_raw_queue[128];
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
    {
        uint8_t rates[3] = {200, 100, 80};
        int ok = 1;
        for (int i = 0; i < 3 && ok; i++) {
            if (write_mouse(0xF3) < 0 || read_ack() < 0) ok = 0;
            else if (write_mouse(rates[i]) < 0 || read_ack() < 0) ok = 0;
        }
        if (ok && write_mouse(0xF2) == 0 && read_ack() == 0) {
            uint8_t dev_id = 0xFF;
            if (read_data(&dev_id) == 0 && dev_id == 0x03) {
                mouse_packet_size = 4;
            }
        }
        flush_output();
    }

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
    uint8_t next = (uint8_t)((mouse_raw_head + 1) & 127);
    if (next == mouse_raw_tail) return; /* drop if full */
    mouse_raw_queue[mouse_raw_head] = b;
    mouse_raw_head = next;
}

static int ps2_mouse_dequeue_byte(void) {
    if (mouse_raw_tail == mouse_raw_head) return -1;
    uint8_t b = mouse_raw_queue[mouse_raw_tail];
    mouse_raw_tail = (uint8_t)((mouse_raw_tail + 1) & 127);
    return b;
}

static int ps2_mouse_poll(mouse_event_t *ev) {
    int sum_x = 0, sum_y = 0, sum_z = 0, buttons = 0;
    int got_any = 0;

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

        if (packet_i == 0 && !(b & 0x08)) continue;
        packet[packet_i++] = b;
        if (packet_i < mouse_packet_size) continue;

        packet_i = 0;
        int x = (int)packet[1];
        int y = (int)packet[2];
        if (packet[0] & 0x10) x -= 256;
        if (packet[0] & 0x20) y -= 256;
        if (packet[0] & 0x40) x = 0;
        if (packet[0] & 0x80) y = 0;
        int z = 0;
        if (mouse_packet_size == 4) {
            z = (int)(packet[3] & 0x0F);
            if (z & 0x08) z -= 16;
        }
        sum_x += x; sum_y += y; sum_z += z;
        buttons = (int)(packet[0] & 0x07);
        got_any = 1;
    }

    if (got_any) {
        static int last_buttons = 0;
        if (buttons != last_buttons) {
            kb_clear_modifiers();
            last_buttons = buttons;
        }
        ev->dx = sum_x; ev->dy = -sum_y;
        ev->dz = sum_z; ev->buttons = buttons;
        return 1;
    }
    return 0;
}


static int usb_mouse_poll(mouse_event_t *ev) {
    hid_mouse_report_t report;
    if (usb_mouse_get_report(&report) < 0) return 0;
    if (report.x == 0 && report.y == 0 && report.wheel == 0 &&
        report.buttons == 0) return 0;

    static int last_usb_buttons = 0;
    if (report.buttons != last_usb_buttons) {
        kb_clear_modifiers();
        last_usb_buttons = report.buttons;
    }

    ev->dx = report.x;
    ev->dy = report.y;
    ev->dz = report.wheel;
    ev->buttons = report.buttons & (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
    return 1;
}

int mouse_poll(mouse_event_t *ev) {
    if (!mouse_ready || !ev) return 0;
    if (mouse_backend == MOUSE_BACKEND_USB) return usb_mouse_poll(ev);
    if (mouse_backend == MOUSE_BACKEND_PS2) return ps2_mouse_poll(ev);
    return 0;
}

int mouse_is_ready(void) {
    return mouse_ready;
}

const char *mouse_backend_name(void) {
    if (mouse_backend == MOUSE_BACKEND_PS2) return "ps2";
    if (mouse_backend == MOUSE_BACKEND_USB) return "usb-hid";
    return "none";
}
