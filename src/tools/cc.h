/**
 * @file    cc.h
 * @brief   导出 8 个接口（1 类型 / 0 宏 / 7 函数）
 *
 * @layer   tools
 * @owner   src/tools/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  cc_gfx_t    结构体 / 类型别名
 *   函数  cc_set_gfx                 Graphics hook table f…
 *         hbos_gcc_run_file          （待补）
 *         hbos_gcc_run_file_capture  （待补）
 *         hbos_gcc_last_error        （待补）
 *         hbos_gcc_last_error_line   （待补）
 *         hbos_gcc_last_return       （待补）
 *         tool_cc_init               （待补）
 */
#ifndef HBOS_CC_H
#define HBOS_CC_H

#include <stdint.h>

/**
 * Graphics hook table for scripted GUI apps.
 * Set via cc_set_gfx() before running a script; clear with cc_set_gfx(NULL).
 */
typedef struct {
    void (*rect)(int x, int y, int w, int h, uint32_t color);
    void (*text)(int x, int y, const char *s, uint32_t color, int scale);
    void (*present)(void);
    int  (*screen_w)(void);
    int  (*screen_h)(void);
    int  (*get_key)(void);   /* non-blocking; 0 = no key */
    int  (*wait_key)(void);  /* blocking */
} cc_gfx_t;

void cc_set_gfx(const cc_gfx_t *hooks);

int         hbos_gcc_run_file(const char *path, int verbose);
int         hbos_gcc_run_file_capture(const char *path, char *out, uint32_t out_cap);
const char *hbos_gcc_last_error(void);
int         hbos_gcc_last_error_line(void);
int         hbos_gcc_last_return(void);
void        tool_cc_init(void);

#endif
