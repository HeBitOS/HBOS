/* ── ~/.hiverc：HIVE 桌面配置持久化 ───────────────────────────
 * 简单 key=value 文本，按用户家目录隔离（简单多用户）：
 *   当前用户名来自 /etc/cur-user（登录管理写入）；缺省 linpinf。
 *   配置路径 = /home/<user>/.hiverc
 * 保存时机：设置页任何项 apply 之后（见 app_settings.c）。
 * 加载时机：GUI 启动、默认值填好之后。 */
/**
 * @file    hiverc.h
 * @brief   导出 6 个接口（0 类型 / 0 宏 / 6 函数）
 *
 * @layer   gui
 * @owner   src/gui/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   函数  hiverc_load            （待补）
 *         hiverc_save            （待补）
 *         hiverc_current_user    当前用户与其家目录（简单多用户入口）
 *         hiverc_user_home       （待补）
 *         hive_display_name      显示名（用户卡上的名字）：/etc/dis…
 *         hive_set_display_name  （待补）
 */
#ifndef HIVERC_H
#define HIVERC_H

struct gui_state;   /* gui_state.h 里的 typedef 结构，避免互相 include */

void hiverc_load(struct gui_state *st);
void hiverc_save(struct gui_state *st);

/* 当前用户与其家目录（简单多用户入口） */
const char *hiverc_current_user(void);
void hiverc_user_home(char *out, int cap);

/* 显示名（用户卡上的名字）：/etc/display-user，默认 "user"，
 * 与登录用户名（决定家目录）相互独立，可随时自定义。 */
void hive_display_name(char *out, int cap);
int  hive_set_display_name(const char *name);

#endif
