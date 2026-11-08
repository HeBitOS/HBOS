/**
 * @file    desktop_entry.h
 * @brief   导出 15 个接口（1 类型 / 9 宏 / 5 函数）
 *
 * @layer   gui
 * @owner   src/gui/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  de_entry_t  结构体 / 类型别名
 *   宏    DE_MAX_ID        文件名（去 .desktop），唯一键
 *         DE_MAX_NAME      Name=，开始菜单显示名
 *         DE_MAX_COMMENT   Comment=，一句话说明
 *         DE_MAX_EXEC      Exec=，可执行文件绝对路径
 *         DE_MAX_ICON      Icon=，图标名
 *         DE_MAX_CATS      Categories=，分号分隔
 *         DE_MAX_KW        Keywords=，开始菜单搜索用（可放拼…
 *         DE_SCOPE_USER    常量 / 特性宏
 *         DE_SCOPE_SYSTEM  常量 / 特性宏
 *   函数  de_parse          解析 .desktop 文本
 *         de_format         生成 .desktop 文本写入 buf，…
 *         de_icon_id        图标名 → ICON_* id
 *         de_has_category   Categories 里是否含某个分类（忽…
 *         de_exec_basename  从 exec 里取第一个 token（可执…
 */
#ifndef HBOS_DESKTOP_ENTRY_H
#define HBOS_DESKTOP_ENTRY_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════
 * .desktop 条目：安装的 GUI 应用如何"变成"开始菜单/桌面上的图标
 *
 * 以前 HPT 装完包只做两件事：往 /opt 铺文件、在 /bin 建一个同名软链。
 * 开始菜单靠"盲扫 /bin"把它们列出来，于是名字是包名（calc 而不是
 * 「计算器」）、没有图标、分类混在一起，连 libc6 / zlib-tree 这种纯
 * 库包也会冒出来（点了必然失败）。
 *
 * 现在走 XDG 的 .desktop：包内 `usr/share/applications/` 下的 .desktop 文件是
 * 权威来源；安装器把它落到 registry 目录，启动器只读 registry。
 * 这个文件只做**纯字符串解析/生成**，不碰 VFS，方便宿主单测覆盖。
 * ══════════════════════════════════════════════════════════════════ */

#define DE_MAX_ID       32
#define DE_MAX_NAME     48
#define DE_MAX_COMMENT  96
#define DE_MAX_EXEC     96
#define DE_MAX_ICON     24
#define DE_MAX_CATS     48
#define DE_MAX_KW       64

/* 作用域：用户级装到 ~/.local，系统级装到 /opt + /usr */
#define DE_SCOPE_USER   0
#define DE_SCOPE_SYSTEM 1

typedef struct {
    char id[DE_MAX_ID];           /**< 文件名（去 .desktop），唯一键 */
    char name[DE_MAX_NAME];       /**< Name=，开始菜单显示名 */
    char comment[DE_MAX_COMMENT]; /**< Comment=，一句话说明 */
    char exec[DE_MAX_EXEC];       /**< Exec=，可执行文件绝对路径 */
    char icon_name[DE_MAX_ICON];  /**< Icon=，图标名 */
    char categories[DE_MAX_CATS]; /**< Categories=，分号分隔 */
    char keywords[DE_MAX_KW];     /**< Keywords=，开始菜单搜索用（可放拼音/英文） */
    char package[DE_MAX_ID];      /**< X-HBOS-Package=，所属包（卸载用） */
    int  terminal;                /**< Terminal=true：命令行应用 */
    int  no_display;              /**< NoDisplay=true：不进菜单 */
    int  icon;                    /**< 映射后的 ICON_* */
    int  scope;                   /**< DE_SCOPE_* */
} de_entry_t;

/** 解析 .desktop 文本。[Desktop Entry] 段与 Name= 为必需项。
 *  返回 1 表示解析成功，0 表示不是有效的桌面条目。
 *  键名带 [locale] 的变体（Name[zh_CN]=）一律忽略，避免覆盖 Name=。 */
int de_parse(const char *text, de_entry_t *out);

/** 生成 .desktop 文本写入 buf，返回写入字节数（不含结尾 0）。 */
uint32_t de_format(const de_entry_t *e, char *buf, uint32_t cap);

/** 图标名 → ICON_* id。未知名字回退 ICON_APPS。 */
int de_icon_id(const char *icon_name);

/** Categories 里是否含某个分类（忽略大小写，按分号切分）。 */
int de_has_category(const char *categories, const char *want);

/** 从 exec 里取第一个 token（可执行文件名），用于 /bin 兜底解析。 */
void de_exec_basename(const char *exec, char *out, uint32_t cap);

#endif
