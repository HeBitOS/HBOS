/**
 * @file    utf8.c
 * @brief   兼容翻译单元：实现已整体迁到 src/i18n/utf8.c
 *
 * 为什么保留一个空文件：Makefile 的 src/lib 源文件清单和宿主测试的链接
 * 行都点过这个路径；删掉它会让 build 目录里留下孤儿 .o，也会让
 * `tests/host/test_utf8.c` 的 -Isrc 之外多一次改链接。实现只此一份，
 * 放在 src/i18n/utf8.c（严格解码 + 容错解码 + 字素簇），这里不再定义
 * 任何符号——定义两份会在内核链接时 duplicate symbol。
 */
#include "utf8.h"
