/**
 * @file    utf8.h
 * @brief   严格 UTF-8 编解码（兼容入口，实现已迁到 src/i18n/utf8.h）
 *
 * 保留这个头是为了不打断已有的 `#include "lib/utf8.h"`（exfat.c、宿主
 * 单测）。**新代码请直接 include "i18n/utf8.h"**，那里才有容错解码和
 * 字素簇迭代。这里只做符号转发，避免两套实现再次分叉。
 *
 * @layer   lib
 * @owner   src/i18n/
 * @stable  internal
 *
 * 依赖    src/i18n/utf8.h
 * 被依赖  src/exfat.c、tests/host/test_utf8.c
 *
 * 索引头文件
 *   宏    UTF8_INVALID      解码失败哨兵（见 i18n/utf8.h）
 *   函数  utf8_decode / utf8_encode / utf8_valid / utf8_length
 *         实现均在 src/i18n/utf8.c
 */
#ifndef HBOS_UTF8_H
#define HBOS_UTF8_H

#include "i18n/utf8.h"

#endif /* HBOS_UTF8_H */
