/**
 * @file    flanterm.h
 * @brief   导出 26 个接口（0 类型 / 17 宏 / 9 函数）
 *
 * @layer   gui
 * @owner   src/
 * @stable  frozen（上游代码，勿改正文）
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    FLANTERM_H                  常量 / 特性宏
 *         FLANTERM_CB_DEC             常量 / 特性宏
 *         FLANTERM_CB_BELL            常量 / 特性宏
 *         FLANTERM_CB_PRIVATE_ID      FLANTERMCBPRIVATE编号
 *         FLANTERM_CB_STATUS_REPORT   FLANTERMCB状态REPORT
 *         FLANTERM_CB_POS_REPORT      常量 / 特性宏
 *         FLANTERM_CB_KBD_LEDS        常量 / 特性宏
 *         FLANTERM_CB_MODE            常量 / 特性宏
 *         FLANTERM_CB_LINUX           常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_OCRNL   常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_OFDEL   常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_OFILL   常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_OLCUC   常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_ONLCR   常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_ONLRET  常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_ONOCR   常量 / 特性宏
 *         FLANTERM_OOB_OUTPUT_OPOST   常量 / 特性宏
 *   函数  flanterm_putchar         （待补）
 *         flanterm_write           （待补）
 *         flanterm_flush           （待补）
 *         flanterm_full_refresh    （待补）
 *         flanterm_get_dimensions  （待补）
 *         flanterm_set_autoflush   （待补）
 *         flanterm_get_oob_output  （待补）
 *         flanterm_set_oob_output  （待补）
 */
#ifndef _FLANTERM_H
#define _FLANTERM_H
#include "types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef FLANTERM_H
#    define FLANTERM_H 1



#    ifdef __cplusplus
extern "C" {
#    endif

#    define FLANTERM_CB_DEC           10
#    define FLANTERM_CB_BELL          20
#    define FLANTERM_CB_PRIVATE_ID    30
#    define FLANTERM_CB_STATUS_REPORT 40
#    define FLANTERM_CB_POS_REPORT    50
#    define FLANTERM_CB_KBD_LEDS      60
#    define FLANTERM_CB_MODE          70
#    define FLANTERM_CB_LINUX         80

#    define FLANTERM_OOB_OUTPUT_OCRNL  (1 << 0)
#    define FLANTERM_OOB_OUTPUT_OFDEL  (1 << 1)
#    define FLANTERM_OOB_OUTPUT_OFILL  (1 << 2)
#    define FLANTERM_OOB_OUTPUT_OLCUC  (1 << 3)
#    define FLANTERM_OOB_OUTPUT_ONLCR  (1 << 4)
#    define FLANTERM_OOB_OUTPUT_ONLRET (1 << 5)
#    define FLANTERM_OOB_OUTPUT_ONOCR  (1 << 6)
#    define FLANTERM_OOB_OUTPUT_OPOST  (1 << 7)

#    ifdef FLANTERM_IN_FLANTERM

#        include "flanterm_private.h"

#    else

struct flanterm_context;

#    endif

void flanterm_putchar(struct flanterm_context *ctx, uint8_t c);
void flanterm_write(struct flanterm_context *ctx, const char *buf, uint64_t count);
void flanterm_flush(struct flanterm_context *ctx);
void flanterm_full_refresh(struct flanterm_context *ctx);
void flanterm_deinit(struct flanterm_context *ctx, void (*_free)(void *ptr, uint64_t size));

void flanterm_get_dimensions(struct flanterm_context *ctx, uint64_t *cols, uint64_t *rows);
void flanterm_set_autoflush(struct flanterm_context *ctx, bool state);
void flanterm_set_callback(
    struct flanterm_context *ctx,
    void (*callback)(struct flanterm_context *, uint64_t, uint64_t, uint64_t, uint64_t)
);
uint64_t flanterm_get_oob_output(struct flanterm_context *ctx);
void flanterm_set_oob_output(struct flanterm_context *ctx, uint64_t oob_output);

#    ifdef __cplusplus
}
#    endif

#endif
#endif
