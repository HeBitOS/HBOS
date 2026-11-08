/* Copyright (C) 2022-2025 mintsuki and contributors.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file    stdbool.h
 * @brief   导出 4 个接口（0 类型 / 4 宏 / 0 函数）
 *
 * @layer   types
 * @owner   src/types/
 * @stable  frozen（上游代码，勿改正文）
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   宏    bool                          常量 / 特性宏
 *         true                          常量 / 特性宏
 *         false                         常量 / 特性宏
 *         __bool_true_false_are_defined  常量 / 特性宏
 */
#ifndef __FREESTND_C_HDRS_STDBOOL_H
#    define __FREESTND_C_HDRS_STDBOOL_H 1

#    ifndef __cplusplus

#        if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L
/* These do not need to be defined for C23+ */
#        else
#            undef bool
#            define bool _Bool

#            undef true
#            define true 1
#            undef false
#            define false 0
#        endif

#    endif

#    undef __bool_true_false_are_defined
#    define __bool_true_false_are_defined 1

#endif
