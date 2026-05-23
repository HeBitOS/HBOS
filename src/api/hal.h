#ifndef HBOS_HAL_H
#define HBOS_HAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void console_puts(const char *str);
void console_write(const char *str, uint64_t len);
void console_putchar(char c);
void console_flush(void);
void console_clear(void);
void console_set_fg(uint32_t color);
void console_set_bg(uint32_t color);
void console_get_size(uint64_t *cols, uint64_t *rows);
bool console_is_initialized(void);

int kb_get_key(void);
#define KB_UP    0x100
#define KB_DOWN  0x101
#define KB_LEFT  0x102
#define KB_RIGHT 0x103
#define KB_ESC   0x104

void sys_reboot(void);
void sys_poweroff(void);
void sys_udelay(uint64_t us);
void sys_mdelay(uint64_t ms);

void app_register_command(const char *name, const char *desc,
                          void (*handler)(int argc, char **argv));
void app_main(void);

#endif
