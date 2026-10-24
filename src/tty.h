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
/* TTY                                                                  */
/* ------------------------------------------------------------------ */

#define TTY_MAX      4
#define TTY_BUF_SIZE 4096
#define TTY_LDISC_MAX 4096

typedef struct {
    uint8_t buf[TTY_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} tty_ringbuf_t;

typedef struct {
    int id;
    tty_ringbuf_t input;   /* cooked/raw bytes available to read()  */
    tty_ringbuf_t output;  /* bytes pending echo/write to screen    */
    int active;
    void *vfs_node;

    termios_t termios;
    winsize_t winsize;

    /* foreground process-group (job control) */
    uint32_t fg_pgid;
    /* session that owns this as ctty */
    uint32_t sid;

    /* canonical line discipline buffer */
    uint8_t  ldisc_buf[TTY_LDISC_MAX];
    uint32_t ldisc_len;
    uint32_t ldisc_lines; /* complete lines (incl. ^D) waiting in input */
} tty_t;

/* ------------------------------------------------------------------ */
/* PTY                                                                  */
/* ------------------------------------------------------------------ */

#define PTY_MAX 16

typedef struct {
    bool      used;
    bool      locked;           /* TIOCSPTLCK: 1 until unlocked  */
    int       index;

    tty_ringbuf_t m2s;          /* master→slave (slave reads)    */
    tty_ringbuf_t s2m;          /* slave→master (master reads)   */

    termios_t termios;
    winsize_t winsize;
    uint32_t  fg_pgid;
    uint32_t  sid;

    uint8_t   ldisc_buf[TTY_LDISC_MAX];
    uint32_t  ldisc_len;
    uint32_t  ldisc_lines;

    void *master_vfs;
    void *slave_vfs;
} pty_t;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

void   tty_init(void);
void   tty_switch(int n);
int    tty_current(void);
tty_t *tty_get(int n);
int    tty_write(int n, const char *buf, uint32_t len);
int    tty_read(int n, char *buf, uint32_t len);
void   tty_input_char(char c);       /* raw scancode → line discipline  */
int    tty_output_ready(int n);
int    tty_input_ready(int n);
int    tty_register_vfs(int n, void *node);
int    tty_ioctl(int n, uint32_t cmd, void *arg);

/* PTY */
int    pty_alloc(void);              /* returns slave index or -1        */
pty_t *pty_get(int idx);
int    pty_master_read(int idx, char *buf, uint32_t len);
int    pty_master_write(int idx, const char *buf, uint32_t len);
int    pty_slave_read(int idx, char *buf, uint32_t len);
int    pty_slave_write(int idx, const char *buf, uint32_t len);
int    pty_master_ioctl(int idx, uint32_t cmd, void *arg);
int    pty_slave_ioctl(int idx, uint32_t cmd, void *arg);
void   pty_set_slave_vfs(int idx, void *node);
void   pty_set_master_vfs(int idx, void *node);

#endif /* HBOS_TTY_H */
