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

/* ── GUI 画布接口（仅 HAX_KIND_GUI / BOTH 应用，从图形桌面启动时可用） ──
 *
 * 用法：先 hax_gui_begin() 探测 GUI 是否可用；可用则进入“绘制-提交-取输入”
 * 循环；不可用（返回 0）时回退到文本模式。应用运行期间独占整屏画布。
 *
 * @code
 *   int w, h;
 *   if (!hax_gui_begin(&w, &h)) { hax_println("需要从桌面启动"); return 1; }
 *   for (;;) {
 *       hax_gui_clear(0x202830);
 *       hax_gui_rect(20, 20, 100, 40, 0x14A6E0);
 *       hax_gui_text(28, 30, "Hello", 0xFFFFFF, 2);
 *       hax_gui_present();
 *       int k = hax_gui_pollkey();
 *       if (k == 'q' || k == 27) break;   // q 或 ESC 退出
 *       hax_sleep(0);                      // 让出 CPU
 *   }
 * @endcode
 */

/** 探测并初始化 GUI 画布；可用返回 1 并写入宽高，否则返回 0 */
static inline int hax_gui_begin(int *w, int *h) {
    return (int)__syscall3(HBOS_SYS_GUI_INFO, (long)w, (long)h, 0);
}

/** 用颜色填满整个画布（0xRRGGBB） */
static inline void hax_gui_clear(HCOLOR color) {
    __syscall1(HBOS_SYS_GUI_CLEAR, (long)color);
}

/** 填充矩形 */
static inline void hax_gui_rect(int x, int y, int w, int h, HCOLOR color) {
    __syscall6(HBOS_SYS_GUI_RECT, x, y, w, h, (long)color, 0);
}

/** 绘制文本（UTF-8，scale 为整数放大倍数，>=1） */
static inline void hax_gui_text(int x, int y, const char *s, HCOLOR color, int scale) {
    __syscall6(HBOS_SYS_GUI_TEXT, x, y, (long)s, (long)color, scale, 0);
}

/** 把画布提交到屏幕 */
static inline void hax_gui_present(void) {
    __syscall1(HBOS_SYS_GUI_PRESENT, 0);
}

/** 轮询一个按键，返回键值；无按键返回 -1 */
static inline int hax_gui_pollkey(void) {
    return (int)__syscall1(HBOS_SYS_GUI_POLLKEY, 0);
}

/** 轮询鼠标，写入绝对坐标 x、y，返回按键位掩码（bit0=左键） */
static inline int hax_gui_pollmouse(int *x, int *y) {
    return (int)__syscall3(HBOS_SYS_GUI_POLLMOUSE, (long)x, (long)y, 0);
}

/* ── 画布扩展原语（纯 SDK 实现，基于 hax_gui_rect，无需额外系统调用） ── */

/** 画一个像素 */
static inline void hax_gui_pixel(int x, int y, HCOLOR c) {
    hax_gui_rect(x, y, 1, 1, c);
}

/** 矩形描边（线宽 t，默认建议 1~2） */
static inline void hax_gui_frame(int x, int y, int w, int h, int t, HCOLOR c) {
    if (t < 1) t = 1;
    hax_gui_rect(x, y, w, t, c);              /* top */
    hax_gui_rect(x, y + h - t, w, t, c);      /* bottom */
    hax_gui_rect(x, y, t, h, c);              /* left */
    hax_gui_rect(x + w - t, y, t, h, c);      /* right */
}

/** 直线（Bresenham；线宽 1）。端点任意方向。 */
static inline void hax_gui_line(int x0, int y0, int x1, int y1, HCOLOR c) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        hax_gui_rect(x0, y0, 1, 1, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/** 实心圆（按水平 span 填充，约 2r 次绘制，高效） */
static inline void hax_gui_fill_circle(int cx, int cy, int r, HCOLOR c) {
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        /* 半弦长 = floor(sqrt(r*r - dy*dy))，用整数迭代避免浮点 */
        int rr = r * r - dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= rr) dx++;
        hax_gui_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

/** 圆环描边（中点画圆法，8 对称） */
static inline void hax_gui_circle(int cx, int cy, int r, HCOLOR c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        hax_gui_pixel(cx + x, cy + y, c); hax_gui_pixel(cx - x, cy + y, c);
        hax_gui_pixel(cx + x, cy - y, c); hax_gui_pixel(cx - x, cy - y, c);
        hax_gui_pixel(cx + y, cy + x, c); hax_gui_pixel(cx - y, cy + x, c);
        hax_gui_pixel(cx + y, cy - x, c); hax_gui_pixel(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* ── 并发窗口 API（HAX_KIND_GUI 应用：拥有独立窗口，与桌面/其他窗口并存） ──
 *
 * 与全屏画布 hax_gui_*（独占整屏、运行时桌面冻结）不同，窗口 API 让应用拥有
 * 一个可与桌面、其他应用窗口同时显示、可拖动、可关闭的独立窗口。
 *
 * @code
 *   int w, h;
 *   if (hax_win_open("我的应用", 360, 240) < 0) return 1;
 *   while (hax_win_active(&w, &h)) {       // 窗口被关闭时返回 0
 *       hax_win_clear(0x202830);
 *       hax_win_fill(20, 20, 100, 40, 0x14A6E0);
 *       hax_win_text(28, 30, "Hello", 0xFFFFFF);
 *       hax_win_present();
 *       int ev[4];
 *       int t = hax_win_poll(ev);          // 1=键 2=鼠标 3=关闭
 *       if (t == HAX_EV_KEY && ev[1] == 'q') break;
 *       if (t == HAX_EV_CLOSE) break;
 *       hax_sleep(0);
 *   }
 *   hax_win_close();
 *   return 0;
 * @endcode
 */
#define HAX_EV_NONE  0
#define HAX_EV_KEY   1   /**< ev[1]=键值 */
#define HAX_EV_MOUSE 2   /**< ev[1]=x ev[2]=y ev[3]=按键位掩码 */
#define HAX_EV_CLOSE 3   /**< 窗口被请求关闭 */

/** 打开一个窗口（内容区 w×h），成功返回窗口 id（≥0），失败 -1 */
static inline int hax_win_open(const char *title, int w, int h) {
    return (int)__syscall3(HBOS_SYS_WIN_OPEN, (long)title, w, h);
}

/** 查询窗口是否仍活动：是返回 1 并写入当前 w、h；被关闭返回 0 */
static inline int hax_win_active(int *w, int *h) {
    return (int)__syscall3(HBOS_SYS_WIN_INFO, (long)w, (long)h, 0);
}

/** 用颜色填满窗口 */
static inline void hax_win_clear(HCOLOR c) {
    __syscall1(HBOS_SYS_WIN_CLEAR, (long)c);
}

/** 在窗口内填充矩形（坐标相对窗口内容区） */
static inline void hax_win_fill(int x, int y, int w, int h, HCOLOR c) {
    __syscall6(HBOS_SYS_WIN_FILL, x, y, w, h, (long)c, 0);
}

/** 在窗口内绘制文本 */
static inline void hax_win_text(int x, int y, const char *s, HCOLOR c) {
    __syscall6(HBOS_SYS_WIN_TEXT, x, y, (long)s, (long)c, 0, 0);
}

/** 提交一帧（让出 CPU，使合成器尽快显示） */
static inline void hax_win_present(void) {
    __syscall1(HBOS_SYS_WIN_PRESENT, 0);
}

/** 取一个事件，写入 ev[4]={type,a,b,c}，返回事件类型（HAX_EV_*，0=无） */
static inline int hax_win_poll(int *ev4) {
    return (int)__syscall1(HBOS_SYS_WIN_POLL, (long)ev4);
}

/** 关闭并销毁窗口 */
static inline void hax_win_close(void) {
    __syscall1(HBOS_SYS_WIN_CLOSE, 0);
}

#endif /* HBOS_HAX_H */
