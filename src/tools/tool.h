#ifndef HBOS_TOOL_H
#define HBOS_TOOL_H

#include <stdint.h>
#include <stddef.h>
#include "../shell/shell.h"

// ============================================================
// 工具模块接口 — 所有 Shell 命令通过此接口注册
// ============================================================

// 端口 I/O（被 reboot/poweroff 等命令使用）
static inline void tool_outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void tool_outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

// 各工具模块初始化
void tool_help_init(void);
void tool_system_init(void);
void tool_debug_init(void);
void tool_history_init(void);
void tool_file_init(void);
void tool_app_init(void);
void tool_ata_init(void);
void tool_disk_init(void);
void tool_gui_init(void);
void tool_net_init(void);
void tool_cc_init(void);

// 聚合初始化 — 注册所有内建工具
static inline void tool_init_all(void) {
    tool_help_init();
    tool_system_init();
    tool_debug_init();
    tool_history_init();
    tool_file_init();
    tool_app_init();
    tool_ata_init();
    tool_disk_init();
    tool_gui_init();
    tool_net_init();
    tool_cc_init();
}

#endif /* HBOS_TOOL_H */
