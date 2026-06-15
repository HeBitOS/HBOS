#include "mouse.h"
#include "../usb_hid.h"
#include "../core/cpu.h"

#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

static int mouse_ready;
static int mouse_backend;
static int mouse_packet_size = 3;
static uint8_t packet[4];
static uint8_t packet_i;

enum {
    MOUSE_BACKEND_NONE = 0,
    MOUSE_BACKEND_PS2,
    MOUSE_BACKEND_USB,
};

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static int wait_write(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) return 0;
    }
    return -1;
}

static int wait_read(void) {
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) return 0;
    }
    return -1;
}

static void flush_output(void) {
    for (uint32_t i = 0; i < 32 && (inb(PS2_STATUS) & 0x01); i++)
        (void)inb(PS2_DATA);
}

static int write_cmd(uint8_t cmd) {
    if (wait_write() < 0) return -1;
    outb(PS2_CMD, cmd);
    return 0;
}

static int write_data(uint8_t data) {
    if (wait_write() < 0) return -1;
    outb(PS2_DATA, data);
    return 0;
}

static int write_mouse(uint8_t data) {
    if (write_cmd(0xD4) < 0) return -1;
    return write_data(data);
}

static int read_data(uint8_t *out) {
    if (!out || wait_read() < 0) return -1;
    *out = inb(PS2_DATA);
    return 0;
}

static int read_ack(void) {
    uint8_t ack = 0;
    if (read_data(&ack) < 0) return -1;
    return ack == 0xFA ? 0 : -1;
}

static int ps2_mouse_init(void) {
    mouse_packet_size = 3;
    packet_i = 0;

    /* Disable interrupts — keyboard ISR would steal our command
     * responses from port 0x60, causing timeouts. */
    int_disable();

    /*
     * CRITICAL: clear bit 5 (aux clock) BEFORE 0xA8.
     * VirtualBox checks bit 5 when processing 0xA8 and ignores
     * the command if the clock is disabled.
     * Also set bit 1 (enable IRQ12) so mouse data stays on its
     * own interrupt line — never routed to IRQ1 / keyboard ISR.
     */
    if (write_cmd(0x20) < 0) { int_enable(); return -1; }
    {
        uint8_t cfg = 0;
        if (read_data(&cfg) < 0) { int_enable(); return -1; }
        uint8_t new_cfg = (cfg & (uint8_t)~0x20) | 0x02;
        if (new_cfg != cfg) {
            if (write_cmd(0x60) < 0) { int_enable(); return -1; }
            if (write_data(new_cfg) < 0) { int_enable(); return -1; }
        }
    }

    /* Now enable auxiliary port — clock is guaranteed on */
    if (write_cmd(0xA8) < 0) { int_enable(); return -1; }
    flush_output();

    /* Enable data reporting */
    for (int attempt = 0; attempt < 4; attempt++) {
        flush_output();
        if (write_mouse(0xF4) == 0 && read_ack() == 0) {
            int_enable();
            return 0;
        }
        /* 0xF4 failed — reset mouse and retry */
        flush_output();
        if (write_mouse(0xFF) < 0) continue;
        /* Wait for self-test pass (0xAA) */
        for (int t = 0; t < 500000; t++) {
            if (inb(PS2_STATUS) & 0x01) {
                if (inb(PS2_DATA) == 0xAA) break;
            }
        }
        /* Drain device ID byte */
        for (int t = 0; t < 50000; t++) {
            if (inb(PS2_STATUS) & 0x01) { (void)inb(PS2_DATA); break; }
        }
    }

    int_enable();
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

static int ps2_mouse_poll(mouse_event_t *ev) {
    int sum_x = 0, sum_y = 0, sum_z = 0, buttons = 0;
    int got_any = 0;

    /* Atomically read all pending mouse bytes — prevents keyboard
     * ISR from stealing bytes mid-packet. */
    int_disable();
    while (inb(PS2_STATUS) & 0x01) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & 0x20)) break;
        uint8_t b = inb(PS2_DATA);

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
    int_enable();

    if (got_any) {
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
