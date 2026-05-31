#include "mouse.h"

#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

static int mouse_ready;
static int mouse_packet_size = 3;
static uint8_t packet[4];
static uint8_t packet_i;

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

static int mouse_set_sample_rate(uint8_t rate) {
    if (write_mouse(0xF3) < 0 || read_ack() < 0) return -1;
    if (write_mouse(rate) < 0 || read_ack() < 0) return -1;
    return 0;
}

static int mouse_read_id(uint8_t *id) {
    if (write_mouse(0xF2) < 0 || read_ack() < 0) return -1;
    return read_data(id);
}

int mouse_init(void) {
    uint8_t status = 0;
    mouse_ready = 0;
    mouse_packet_size = 3;
    packet_i = 0;

    flush_output();
    if (write_cmd(0xA8) < 0) return -1;
    if (write_cmd(0x20) < 0) return -1;
    if (read_data(&status) < 0) return -1;
    status &= (uint8_t)~0x02;
    status &= (uint8_t)~0x20;
    if (write_cmd(0x60) < 0) return -1;
    if (write_data(status) < 0) return -1;

    if (write_mouse(0xF6) < 0 || read_ack() < 0) return -1;
    if (mouse_set_sample_rate(200) == 0 &&
        mouse_set_sample_rate(100) == 0 &&
        mouse_set_sample_rate(80) == 0) {
        uint8_t id = 0;
        if (mouse_read_id(&id) == 0 && id == 3) mouse_packet_size = 4;
    }
    if (mouse_set_sample_rate(60) < 0) return -1;
    if (write_mouse(0xF4) < 0 || read_ack() < 0) return -1;
    mouse_ready = 1;
    return 0;
}

void mouse_shutdown(void) {
    if (!mouse_ready) return;
    (void)write_mouse(0xF5);
    (void)read_ack();
    flush_output();
    mouse_ready = 0;
    packet_i = 0;
}

int mouse_poll(mouse_event_t *ev) {
    if (!mouse_ready || !ev) return 0;

    while (inb(PS2_STATUS) & 0x01) {
        uint8_t status = inb(PS2_STATUS);
        if (!(status & 0x20)) return 0;
        uint8_t b = inb(PS2_DATA);

        if (packet_i == 0 && !(b & 0x08)) continue;
        packet[packet_i++] = b;
        if (packet_i < mouse_packet_size) continue;

        packet_i = 0;
        int x = (int)packet[1];
        int y = (int)packet[2];
        int z = 0;
        if (packet[0] & 0x10) x -= 256;
        if (packet[0] & 0x20) y -= 256;
        if (packet[0] & 0x40) x = 0;
        if (packet[0] & 0x80) y = 0;
        if (mouse_packet_size == 4) {
            z = packet[3] & 0x0F;
            if (z & 0x08) z -= 16;
        }

        ev->dx = x;
        ev->dy = -y;
        ev->dz = z;
        ev->buttons = packet[0] & (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
        return 1;
    }

    return 0;
}
