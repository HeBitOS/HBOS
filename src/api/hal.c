#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* HAL implementation: wraps existing kernel services */
#include "../graphics/graphics.h"

/* Reboot via PS/2 keyboard controller */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ─── Console wrappers ──────────────────────────────── */

void console_puts(const char *str) {
    extern void console_puts(const char *);
    /* use graphics.h function directly */
    console_write(str, 0); /* will be called by name resolution */
}

void console_flush(void) {
    /* For flanterm: trigger a double-buffer swap */
    /* This is a hint; actual flush happens via flanterm_flush */
    __asm__ volatile ("" ::: "memory");
}

/* ─── System calls ──────────────────────────────────── */

void sys_reboot(void) {
    console_puts("\n\x1b[33mRebooting...\x1b[0m\n");
    outb(0x64, 0xFE);
    while(1) __asm__ volatile("cli; hlt");
}

void sys_poweroff(void) {
    console_puts("\n\x1b[33mShutting down...\x1b[0m\n");
    outw(0x604, 0x2000);
    outw(0x4004, 0x3400);
    while(1) __asm__ volatile("cli; hlt");
}

void sys_udelay(uint64_t us) {
    /* Simple busy-wait loop. Approximate: ~1us per iteration at 100MHz */
    for (volatile uint64_t i = 0; i < us * 100; i++) {
        __asm__ volatile ("pause");
    }
}

void sys_mdelay(uint64_t ms) {
    sys_udelay(ms * 1000);
}

/* ─── Application support ───────────────────────────── */

void app_register_command(const char *name, const char *desc,
                          void (*handler)(int argc, char **argv)) {
    /* Shell command registration - used via shell.h */
    /* This function is defined in shell.c */
    extern void cmd_register_external(const char *, const char *,
                                      void (*)(int, char **));
    cmd_register_external(name, desc, handler);
}

/* Weak default: if no app_main defined, this is used */
__attribute__((weak)) void app_main(void) {
    /* No user application loaded */
}
