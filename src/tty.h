/**
 * @file    tty.h
 * @brief   导出 81 个接口（6 类型 / 58 宏 / 17 函数）
 *
 * @layer   input
 * @owner   src/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  termios_t      结构体 / 类型别名
 *         winsize_t      结构体 / 类型别名
 *         tty_ringbuf_t  结构体 / 类型别名
 *         ldisc_t        结构体 / 类型别名
 *         tty_t          结构体 / 类型别名
 *         pty_t          结构体 / 类型别名
 *   宏    NCCS           常量 / 特性宏
 *         ISIG           常量 / 特性宏
 *         ICANON         常量 / 特性宏
 *         ECHO           常量 / 特性宏
 *         ECHOE          常量 / 特性宏
 *         ECHOK          常量 / 特性宏
 *         ECHONL         常量 / 特性宏
 *         IEXTEN         常量 / 特性宏
 *         ICRNL          常量 / 特性宏
 *         IXON           常量 / 特性宏
 *         BRKINT         常量 / 特性宏
 *         IGNBRK         常量 / 特性宏
 *         OPOST          常量 / 特性宏
 *         ONLCR          常量 / 特性宏
 *         CS8            常量 / 特性宏
 *         CREAD          常量 / 特性宏
 *         HUPCL          常量 / 特性宏
 *         B38400         常量 / 特性宏
 *         VINTR          常量 / 特性宏
 *         VQUIT          常量 / 特性宏
 *         VERASE         常量 / 特性宏
 *         VKILL          常量 / 特性宏
 *         VEOF           常量 / 特性宏
 *         VTIME          常量 / 特性宏
 *         VMIN           常量 / 特性宏
 *         VSWTC          常量 / 特性宏
 *         VSTART         常量 / 特性宏
 *         VSTOP          常量 / 特性宏
 *         VSUSP          常量 / 特性宏
 *         VEOL           常量 / 特性宏
 *         TCGETS         常量 / 特性宏
 *         TCSETS         常量 / 特性宏
 *         TCSETSW        常量 / 特性宏
 *         TCSETSF        常量 / 特性宏
 *         TCSBRK         常量 / 特性宏
 *         TCXONC         常量 / 特性宏
 *         TCFLSH         常量 / 特性宏
 *         TIOCEXCL       常量 / 特性宏
 *         TIOCSCTTY      常量 / 特性宏
 *         TIOCGPGRP      常量 / 特性宏
 *         TIOCSPGRP      常量 / 特性宏
 *         TIOCGWINSZ     常量 / 特性宏
 *         TIOCSWINSZ     常量 / 特性宏
 *         TIOCNOTTY      常量 / 特性宏
 *         TIOCGSID       常量 / 特性宏
 *         TIOCGPTN       常量 / 特性宏
 *         TIOCSPTLCK     常量 / 特性宏
 *         FIONREAD       常量 / 特性宏
 *         FIONBIO        常量 / 特性宏
 *         TTY_MAX        常量 / 特性宏
 *         TTY_BUF_SIZE   TTY缓冲区
 *         TTY_LDISC_MAX  常量 / 特性宏
 *         PTY_MAX        常量 / 特性宏
 *         ISTRIP         常量 / 特性宏
 *         INLCR          常量 / 特性宏
 *         IGNCR          常量 / 特性宏
 *         TOSTOP         常量 / 特性宏
 *         ECHOCTL        常量 / 特性宏
 *   函数  tty_init            （待补）
 *         tty_switch          （待补）
 *         tty_current         （待补）
 *         tty_get             （待补）
 *         tty_write           （待补）
 *         tty_read            （待补）
 *         tty_input_char      （待补）
 *         tty_output_ready    （待补）
 *         tty_input_ready     （待补）
 *         tty_register_vfs    （待补）
 *         tty_console_read    （待补）
 *         tty_set_foreground  （待补）
 *         tty_ioctl           （待补）
 *         tty_console_ops     devfs/open 钩子
 *         tty_node_is_tty     （待补）
 *         pty_open_master     （待补）
 *         pty_master_index    （待补）
 */
#ifndef HBOS_TTY_H
#define HBOS_TTY_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* termios (Linux x86-64 layout, struct termios)                       */
/* ------------------------------------------------------------------ */

#define NCCS 19

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
} termios_t;

/* c_lflag bits */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define IEXTEN  0100000

/* c_iflag bits */
#define ICRNL   0000400
#define IXON    0002000
#define BRKINT  0000002
#define IGNBRK  0000001

/* c_oflag bits */
#define OPOST   0000001
#define ONLCR   0000004

/* c_cflag bits */
#define CS8     0000060
#define CREAD   0000200
#define HUPCL   0002000
#define B38400  0000017

/* c_cc indices */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP   10
#define VEOL    11

/* ------------------------------------------------------------------ */
/* winsize                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} winsize_t;

/* ------------------------------------------------------------------ */
/* ioctl request codes (Linux x86-64)                                  */
/* ------------------------------------------------------------------ */

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCEXCL    0x540C
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCNOTTY   0x5422
#define TIOCGSID    0x5429
#define TIOCGPTN    0x80045430u   /* get PTY slave number */
#define TIOCSPTLCK  0x40045431u   /* unlock PTY slave     */
#define FIONREAD    0x541B
#define FIONBIO     0x5421

/* ------------------------------------------------------------------ */
/* Line discipline / TTY / PTY                                          */
/* ------------------------------------------------------------------ */

#define TTY_MAX      4
#define TTY_BUF_SIZE 4096
#define TTY_LDISC_MAX 4096
#define PTY_MAX 16

#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define TOSTOP  0000400
#define ECHOCTL 0001000

typedef struct {
    uint8_t buf[TTY_BUF_SIZE];
    uint32_t head, tail, count;
} tty_ringbuf_t;

/* 行规程状态：tty 与 pty slave 共用 */
typedef struct {
    termios_t termios;
    winsize_t winsize;
    uint32_t fg_pgid;      /* 前台进程组（作业控制） */
    uint32_t sid;          /* 控制终端所属会话 */
    tty_ringbuf_t input;   /* 可被 read() 取走的数据 */
    uint8_t  edit[TTY_LDISC_MAX]; /* 规范模式行编辑缓冲 */
    uint32_t edit_len;
    uint32_t lines;        /* input 中已提交的行数 */
    uint32_t reader;       /* 最近的控制台读者（无前台组时 ^C 的目标） */
    int      eof;          /* 空行 ^D：下一次 read 返回 0 */
} ldisc_t;

typedef struct {
    int id;
    int active;
    void *vfs_node;
    ldisc_t ld;
    tty_ringbuf_t output;
} tty_t;

typedef struct {
    bool used, locked;
    int  index;
    ldisc_t ld;            /* slave 侧行规程（input = master→slave 经处理后） */
    tty_ringbuf_t s2m;     /* slave→master */
} pty_t;

void   tty_init(void);
void   tty_switch(int n);
int    tty_current(void);
tty_t *tty_get(int n);
int    tty_write(int n, const char *buf, uint32_t len);
int    tty_read(int n, char *buf, uint32_t len);
void   tty_input_char(char c);
int    tty_output_ready(int n);
int    tty_input_ready(int n);
int    tty_register_vfs(int n, void *node);
int    tty_console_read(char *buf, uint32_t len); /* fd0 读：键盘→行规程 */
void   tty_set_foreground(uint32_t pgid); /* 设 tty0 前台进程组（0=无） */
int    tty_ioctl(int n, uint32_t cmd, void *arg);

/* devfs/open 钩子 */
const void *tty_console_ops(void);        /* /dev/tty、/dev/console 的 ioctl 操作表 */
int    tty_node_is_tty(const void *node);
void  *pty_open_master(void);             /* open("/dev/ptmx")：返回新 master vfs 节点 */
int    pty_master_index(const void *node);

#endif /* HBOS_TTY_H */
