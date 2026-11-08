/**
 * @file    tool.h
 * @brief   导出 16 个接口（0 类型 / 0 宏 / 16 函数）
 *
 * @layer   tools
 * @owner   src/tools/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  tool_outb          （待补）
 *         tool_outw          （待补）
 *         tool_help_init     （待补）
 *         tool_system_init   （待补）
 *         tool_debug_init    （待补）
 *         tool_history_init  （待补）
 *         tool_file_init     （待补）
 *         tool_app_init      （待补）
 *         tool_ata_init      （待补）
 *         tool_disk_init     （待补）
 *         tool_gui_init      （待补）
 *         tool_net_init      （待补）
 *         tool_cc_init       （待补）
 *         tool_cppe_init     （待补）
 *         tool_audio_init    （待补）
 *         tool_init_all      （待补）
 */
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
#if HBOS_ENABLE_GUI
void tool_gui_init(void);
#endif
void tool_net_init(void);
void tool_cc_init(void);
void tool_cppe_init(void);
void tool_audio_init(void);

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
#if HBOS_ENABLE_GUI
    tool_gui_init();
#endif
    tool_net_init();
    tool_cc_init();
    tool_cppe_init();
    tool_audio_init();
}

#endif /* HBOS_TOOL_H */
