/**
 * @file    hax.h
 * @brief   HBOS Application eXtension SDK —— 让用户更容易上手写 HBOS 应用
 *
 * HAX（HBOS Application eXecutable，扩展名 .hax）是 HBOS 的应用程序格式。
 * 它本质是一个标准 ELF64 用户态程序，但带有一段自描述元数据（.haxmeta），
 * 因此系统只要在 ./app 目录里发现编译产物，就能自动把它加入系统——无需手动
 * 注册。应用既可在终端（TUI）运行，也可从图形桌面（GUI）启动。
 *
 * 用法（最小示例）：
 * @code
 *   #include <hax.h>
 *
 *   HAX_APP("hello", "我的第一个 HBOS 应用", HAX_KIND_TUI);
 *
 *   int main(int argc, char **argv) {
 *       hax_println("你好，HBOS！");
 *       return 0;
 *   }
 * @endcode
 *
 * 编译（由 `make` 自动完成；也可手动）：
 *   xhcc app/hello.c -o app/hello.hax        # 等价于链接 HAX 运行时
 * 系统启动后即出现在 `apps` 列表，可用 `run hello` 运行。
 */
#ifndef HBOS_HAX_H
#define HBOS_HAX_H

#include <stdint.h>
#include <stddef.h>

#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/syscall.h"

/* ── 专有类型（参考 XJ380 风格，统一定宽整型与颜色类型） ───────────── */
typedef int8_t   HI8;    /**< 8 位有符号整型 */
typedef uint8_t  HU8;    /**< 8 位无符号整型 */
typedef int16_t  HI16;   /**< 16 位有符号整型 */
typedef uint16_t HU16;   /**< 16 位无符号整型 */
typedef int32_t  HI32;   /**< 32 位有符号整型 */
typedef uint32_t HU32;   /**< 32 位无符号整型 */
typedef int64_t  HI64;   /**< 64 位有符号整型 */
typedef uint64_t HU64;   /**< 64 位无符号整型 */
typedef uint32_t HCOLOR; /**< 0xRRGGBB 颜色 */

/* ── 应用类型 ─────────────────────────────────────────────────────── */
#define HAX_KIND_TUI   1u   /**< 终端应用：在命令行 / GUI 终端窗口运行 */
#define HAX_KIND_GUI   2u   /**< 图形应用：出现在桌面启动器，启动到窗口 */
#define HAX_KIND_BOTH  3u   /**< 两种入口都注册 */

/** HAX 元数据魔数 'HAXM'（小端） */
#define HAX_META_MAGIC 0x4D584148u

/**
 * HAX 自描述元数据结构（放入 ELF 的 .haxmeta 段）。
 * 构建期由 tools/genhax.py 读取，运行期内核据此自动注册应用。
 */
typedef struct {
    HU32 magic;      /**< 必须为 HAX_META_MAGIC */
    HU32 kind;       /**< HAX_KIND_* 之一 */
    char name[32];   /**< 应用名（run 命令使用，须唯一） */
    char desc[64];   /**< 一句话描述 */
} hax_meta_t;

/**
 * @brief 声明一个 HAX 应用的元数据
 * @param nm   应用名（字符串，<=31 字节）
 * @param ds   描述（字符串，<=63 字节）
 * @param knd  HAX_KIND_TUI / HAX_KIND_GUI / HAX_KIND_BOTH
 *
 * 在源文件顶层写一次即可。该结构被放入独立的 .haxmeta 段，
 * 不参与运行时加载，仅供构建工具识别。
 */
#define HAX_APP(nm, ds, knd)                                              \
    __attribute__((section(".haxmeta"), used))                           \
    const hax_meta_t __hax_app_meta = { HAX_META_MAGIC, (knd), (nm), (ds) }

/* ── 文本输入输出 ─────────────────────────────────────────────────── */

/** 输出字符串（不换行） */
static inline void hax_print(const char *s) { fputs(s, stdout); }

/** 输出字符串并换行 */
static inline void hax_println(const char *s) { fputs(s, stdout); fputc('\n', stdout); }

/** 格式化输出（同 printf） */
#define hax_printf(...) printf(__VA_ARGS__)

/** 读取一行输入到 buf（含去除换行），返回读到的长度，失败返回 -1 */
static inline int hax_input(char *buf, int cap) {
    if (!fgets(buf, cap, stdin)) return -1;
    int n = (int)strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return n;
}

/** 读取一个字符（无字符返回 -1） */
static inline int hax_getch(void) { return fgetc(stdin); }

/* ── 系统服务 ─────────────────────────────────────────────────────── */

/** 休眠指定秒数 */
static inline void hax_sleep(unsigned sec) { __syscall1(HBOS_SYS_SLEEP, (long)sec); }

/** 退出应用，返回状态码 */
static inline void hax_exit(int code) { __syscall1(HBOS_SYS_EXIT, code); }

/** 获取当前进程 ID */
static inline int hax_pid(void) { return (int)__syscall1(HBOS_SYS_GETPID, 0); }

/* ── 文件便捷接口 ─────────────────────────────────────────────────── */

/** 读取整个文件到 buf，返回字节数（截断到 cap），失败返回 -1 */
static inline long hax_read_file(const char *path, void *buf, long cap) {
    int fd = (int)__syscall3(HBOS_SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    while (total < cap) {
        long n = __syscall3(HBOS_SYS_READ, fd, (long)((char *)buf + total), cap - total);
        if (n <= 0) break;
        total += n;
    }
    __syscall1(HBOS_SYS_CLOSE, fd);
    return total;
}

/** 把 buf 写入文件（覆盖创建），返回写入字节数，失败返回 -1 */
static inline long hax_write_file(const char *path, const void *buf, long len) {
    int fd = (int)__syscall3(HBOS_SYS_OPEN, (long)path,
                             O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long total = 0;
    while (total < len) {
        long n = __syscall3(HBOS_SYS_WRITE, fd, (long)((const char *)buf + total), len - total);
        if (n <= 0) break;
        total += n;
    }
    __syscall1(HBOS_SYS_CLOSE, fd);
    return total;
}

#endif /* HBOS_HAX_H */
