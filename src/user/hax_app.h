/**
 * @file    hax_app.h
 * @brief   HAX 应用运行时（内核侧）—— 自动发现并运行 ./app 中的 .hax 应用
 *
 * 构建期 tools/genhax.py 扫描 build/app 下的 .hax 文件，读取各自的 .haxmeta 段，
 * 生成一张应用表（hax_app_table）并把所有 .hax 二进制拼接成一个 blob 嵌入内核。
 * 运行期本模块据此把每个应用注册进系统：终端可 `run <名>`，
 * GUI 桌面启动器列出 GUI 类应用。
 */
#ifndef HBOS_USER_HAX_APP_H
#define HBOS_USER_HAX_APP_H

#include <stdint.h>

#define HAX_KIND_TUI   1u
#define HAX_KIND_GUI   2u
#define HAX_KIND_BOTH  3u

/** 应用表项（由 genhax.py 生成的 hax_manifest.c 填充） */
typedef struct {
    const char *name;     /**< 应用名 */
    const char *desc;     /**< 描述 */
    uint32_t    kind;     /**< HAX_KIND_* */
    uint32_t    offset;   /**< 在 blob 中的偏移 */
    uint32_t    size;     /**< ELF 字节数 */
} hax_app_entry_t;

/* 由生成的 hax_manifest.c 提供 */
extern const hax_app_entry_t hax_app_table[];
extern const uint32_t        hax_app_table_count;

/** 已发现的 .hax 应用数量 */
uint32_t hax_app_count(void);

/** 按索引取应用表项，越界返回 NULL */
const hax_app_entry_t *hax_app_at(uint32_t index);

/** 按名称查找应用表项，找不到返回 NULL */
const hax_app_entry_t *hax_app_find(const char *name);

/**
 * @brief 运行一个 .hax 应用（在新任务中加载其 ELF 并等待结束）
 * @return 应用退出码；-1 表示未找到或加载失败
 */
int hax_app_run(const char *name, int argc, char **argv);

/**
 * @brief 非阻塞启动一个 .hax 应用：加载 ELF 并生成任务后立即返回，不等待结束。
 * @return 新任务的 pid；-1 表示未找到或加载失败
 *
 * 供 GUI 合成器并发启动窗口应用——调用方（GUI 主循环）不被阻塞，可继续合成。
 */
int hax_app_spawn(const char *name, int argc, char **argv);

#endif /* HBOS_USER_HAX_APP_H */
