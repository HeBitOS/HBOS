/**
 * @file    fcntl.h
 * @brief   POSIX 文件控制头文件 — HBOS 实现
 *
 * 定义文件打开标志、文件控制命令等常量。
 * 参考 Linux fcntl.h 补充了 O_DIRECTORY, O_NOFOLLOW, O_NONBLOCK 等标志。
 */
#ifndef HBOS_FCNTL_H
#define HBOS_FCNTL_H

#define O_RDONLY   0x0000   /**< 只读打开 */
#define O_WRONLY   0x0001   /**< 只写打开 */
#define O_RDWR     0x0002   /**< 读写打开 */
#define O_ACCMODE  0x0003   /**< 访问模式掩码 */
#define O_CREAT    0x0040   /**< 若不存在则创建 */
#define O_EXCL     0x0080   /**< 与 O_CREAT 一起使用，文件必须不存在 */
#define O_NOCTTY   0x0100   /**< 不将终端设为控制终端 */
#define O_TRUNC    0x0200   /**< 截断为 0 长度 */
#define O_APPEND   0x0400   /**< 追加写入 */
#define O_NONBLOCK 0x0800   /**< 非阻塞模式 */
#define O_DIRECTORY 0x10000 /**< 必须是目录 */
#define O_NOFOLLOW 0x20000  /**< 不跟随符号链接 */
#define O_CLOEXEC  0x80000  /**< 执行时关闭 */

int open(const char *path, int flags, ...);

#endif
