/* app/lib/leaputil/leaputil.h
 * 闰年判定独立库 —— 多结构 HAX 构建的“独立库”示例。
 *
 * 用法：应用在 deps 文件（app/<name>/deps 或 app/<name>.deps）中写一行
 *   leaputil
 * 然后在源码里 #include <leaputil/leaputil.h> 即可调用。
 * 库头文件按 <库名/文件名.h> 引入（构建期自动加了 -I app/lib）。
 */
#ifndef HBOS_LIB_LEAPUTIL_H
#define HBOS_LIB_LEAPUTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 判定 year（1~10000 之间）是否为闰年：是返回 1，否返回 0 */
int leaputil_is_leap(int year);

#ifdef __cplusplus
}
#endif

#endif /* HBOS_LIB_LEAPUTIL_H */
