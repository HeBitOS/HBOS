/**
 * @file    hal.h
 * @brief   导出 20 个接口（0 类型 / 5 宏 / 15 函数）
 *
 * @layer   app
 * @owner   src/app/api/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    KB_UP       常量 / 特性宏
 *         KB_DOWN     常量 / 特性宏
 *         KB_LEFT     常量 / 特性宏
 *         KB_RIGHT    常量 / 特性宏
 *         KB_ESC      常量 / 特性宏
 *   函数  console_puts            输出字符串到控制台
 *         console_write           输出定长字节到控制台
 *         console_putchar         输出一个字符
 *         console_flush           刷新控制台缓冲
 *         console_clear           清屏
 *         console_set_fg          设置前景色
 *         console_set_bg          设置背景色
 *         console_get_size        取控制台行列数
 *         console_is_initialized  控制台是否已就绪
 *         kb_get_key              （待补）
 *         sys_reboot              （待补）
 *         sys_poweroff            （待补）
 *         sys_udelay              （待补）
 *         sys_mdelay              （待补）
 *         app_main                （待补）
 */
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
