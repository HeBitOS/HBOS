#include "tty.h"
#include "string.h"
#include "vfs.h"
#include "errno.h"
#include "signal.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "core/task.h"
#include "core/heap.h"
#include "graphics/graphics.h"

static tty_t ttys[TTY_MAX];
static pty_t ptys[PTY_MAX];
static int current_tty = 0;

static int rb_put(tty_ringbuf_t *rb, const uint8_t *b, uint32_t n)
{
    uint32_t w = 0;
    while (w < n && rb->count < TTY_BUF_SIZE) {
        rb->buf[rb->head] = b[w++];
        rb->head = (rb->head + 1) % TTY_BUF_SIZE;
        rb->count++;
    }
    return (int)w;
}
static int rb_get(tty_ringbuf_t *rb, uint8_t *b, uint32_t n)
{
    uint32_t r = 0;
    while (r < n && rb->count) {
        b[r++] = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % TTY_BUF_SIZE;
        rb->count--;
    }
    return (int)r;
}
static void rb_clear(tty_ringbuf_t *rb) { rb->count = rb->head = rb->tail = 0; }

typedef void (*emit_fn)(void *ctx, uint8_t c);

static void ld_defaults(ldisc_t *ld)
{
    memset(ld, 0, sizeof(*ld));
    termios_t *t = &ld->termios;
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = B38400 | CS8 | CREAD | HUPCL;
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN | ECHOCTL;
    t->c_cc[VINTR] = 3;  t->c_cc[VQUIT] = 28; t->c_cc[VERASE] = 127;
    t->c_cc[VKILL] = 21; t->c_cc[VEOF] = 4;   t->c_cc[VMIN] = 1;
    t->c_cc[VSTART] = 17; t->c_cc[VSTOP] = 19; t->c_cc[VSUSP] = 26;
    ld->winsize.ws_row = 25; ld->winsize.ws_col = 80;
}

static void ld_out(const ldisc_t *ld, emit_fn emit, void *ctx, uint8_t c)
{
    if (c == '\n' && (ld->termios.c_oflag & OPOST) && (ld->termios.c_oflag & ONLCR))
        emit(ctx, '\r');
    emit(ctx, c);
}

static void ld_str(emit_fn emit, void *ctx, const char *s)
{
    while (*s) emit(ctx, (uint8_t)*s++);
}

static void ld_commit(ldisc_t *ld)
{
    rb_put(&ld->input, ld->edit, ld->edit_len);
    ld->edit_len = 0;
    ld->lines++;
}

static void ld_input(ldisc_t *ld, emit_fn emit, void *ctx, uint8_t c)
{
    const termios_t *t = &ld->termios;
    uint32_t lf = t->c_lflag;

    if (t->c_iflag & ISTRIP) c &= 0x7f;
    if (c == '\r') {
        if (t->c_iflag & IGNCR) return;
        if (t->c_iflag & ICRNL) c = '\n';
    } else if (c == '\n' && (t->c_iflag & INLCR)) c = '\r';

    if (lf & ISIG) {
        int sig = 0;
        if (c == t->c_cc[VINTR]) sig = SIGINT;
        else if (c == t->c_cc[VQUIT]) sig = SIGQUIT;
        else if (c == t->c_cc[VSUSP]) sig = SIGTSTP;
        if (sig) {
            if (lf & ECHO) {
                char s[3] = { '^', (char)(c + 64), 0 };
                ld_str(emit, ctx, s);
                ld_out(ld, emit, ctx, '\n');
            }
            ld->edit_len = 0; rb_clear(&ld->input); ld->lines = 0;
            if (ld->fg_pgid) task_kill_pgrp(ld->fg_pgid, sig);
            else if (ld->reader && sig != SIGTSTP) task_kill(ld->reader, sig);
            return;
        }
    }

    if (lf & ICANON) {
        if (c == t->c_cc[VERASE] || c == 8) {
            if (ld->edit_len) {
                ld->edit_len--;
                if (lf & ECHOE) ld_str(emit, ctx, "\b \b");
            }
            return;
        }
        if (c == t->c_cc[VKILL]) {
            while (ld->edit_len) {
                ld->edit_len--;
                if (lf & ECHOE) ld_str(emit, ctx, "\b \b");
            }
            if ((lf & ECHOK) && !(lf & ECHOE)) ld_out(ld, emit, ctx, '\n');
            return;
        }
        if (c == t->c_cc[VEOF]) {
            if (ld->edit_len) ld_commit(ld); else ld->eof = 1;
            return;
        }
        if (ld->edit_len < TTY_LDISC_MAX - 1) ld->edit[ld->edit_len++] = c;
        if (lf & ECHO) ld_out(ld, emit, ctx, c);
        else if (c == '\n' && (lf & ECHONL)) ld_out(ld, emit, ctx, c);
        if (c == '\n' || (t->c_cc[VEOL] && c == t->c_cc[VEOL])) ld_commit(ld);
        return;
    }
    rb_put(&ld->input, &c, 1);
    if (lf & ECHO) ld_out(ld, emit, ctx, c);
}

/* 后台进程访问控制终端：向其进程组发 SIGTTIN/SIGTTOU（默认动作=停止） */
static void ld_bg_check(ldisc_t *ld, int sig)
{
    task_t *cur = task_current();
    if (!cur || !ld->fg_pgid || cur->sid != ld->sid || cur->pgid == ld->fg_pgid) return;
    if (sig == SIGTTOU && !(ld->termios.c_lflag & TOSTOP)) return;
    task_kill_pgrp(cur->pgid, sig);
}

static int ld_read(ldisc_t *ld, uint8_t *buf, uint32_t len)
{
    if (!len) return 0;
    ld_bg_check(ld, SIGTTIN);
    if (ld->termios.c_lflag & ICANON) {
        while (!ld->lines) {
            if (ld->eof) { ld->eof = 0; return 0; }
            task_yield();
        }
        uint32_t n = 0;
        while (n < len && ld->input.count) {
            uint8_t c;
            rb_get(&ld->input, &c, 1);
            buf[n++] = c;
            if (c == '\n') { ld->lines--; return (int)n; }
        }
        if (!ld->input.count) ld->lines = 0;   /* ^D 提交的无换行行已读完 */
        return (int)n;
    }
    uint32_t vmin = ld->termios.c_cc[VMIN];
    if (vmin > len) vmin = len;
    if (!vmin) return rb_get(&ld->input, buf, len);
    while (ld->input.count < vmin) task_yield();
    return rb_get(&ld->input, buf, len);
}

static int ld_ioctl(ldisc_t *ld, tty_ringbuf_t *outq, uint32_t cmd, void *arg)
{
    task_t *cur = task_current();
    switch (cmd) {
    case TCGETS:
        if (!arg) return -EFAULT;
        memcpy(arg, &ld->termios, sizeof(termios_t));
        return 0;
    case TCSETSF:
        rb_clear(&ld->input); ld->lines = 0; ld->edit_len = 0;
        /* fallthrough */
    case TCSETS: case TCSETSW:
        if (!arg) return -EFAULT;
        ld_bg_check(ld, SIGTTOU);
        memcpy(&ld->termios, arg, sizeof(termios_t));
        return 0;
    case TCSBRK: case TCXONC: case TIOCEXCL:
        return 0;
    case TCFLSH: {
        long q = (long)(uintptr_t)arg;     /* 0=in 1=out 2=both */
        if (q == 0 || q == 2) { rb_clear(&ld->input); ld->lines = 0; ld->edit_len = 0; }
        if ((q == 1 || q == 2) && outq) rb_clear(outq);
        return 0;
    }
    case TIOCGWINSZ:
        if (!arg) return -EFAULT;
        memcpy(arg, &ld->winsize, sizeof(winsize_t));
        return 0;
    case TIOCSWINSZ: {
        if (!arg) return -EFAULT;
        winsize_t old = ld->winsize;
        memcpy(&ld->winsize, arg, sizeof(winsize_t));
        if (ld->fg_pgid && memcmp(&old, &ld->winsize, sizeof(old)))
            task_kill_pgrp(ld->fg_pgid, SIGWINCH);
        return 0;
    }
    case TIOCSCTTY:
        if (!cur || cur->sid != cur->id) return -EPERM;   /* 须为会话首进程 */
        ld->sid = cur->sid;
        ld->fg_pgid = cur->pgid;
        return 0;
    case TIOCNOTTY:
        if (cur && ld->sid == cur->sid) {
            if (ld->fg_pgid) {
                task_kill_pgrp(ld->fg_pgid, SIGHUP);
                task_kill_pgrp(ld->fg_pgid, SIGCONT);
            }
            ld->sid = 0; ld->fg_pgid = 0;
        }
        return 0;
    case TIOCGPGRP:
        if (!arg) return -EFAULT;
        if (!ld->sid || !cur || cur->sid != ld->sid) return -ENOTTY;
        *(int *)arg = (int)ld->fg_pgid;
        return 0;
    case TIOCSPGRP: {
        if (!arg) return -EFAULT;
        if (!cur || !ld->sid || cur->sid != ld->sid) return -ENOTTY;
        int pg = *(int *)arg;
        if (pg <= 0) return -EINVAL;
        int ok = 0;
        for (uint32_t i = 1; i < 256 && !ok; i++) {
            const task_t *t = task_get_by_id(i);
            if (t && t->state != TASK_TERMINATED && t->pgid == (uint32_t)pg &&
                t->sid == ld->sid) ok = 1;
        }
        if (!ok) return -EPERM;
        ld->fg_pgid = (uint32_t)pg;
        return 0;
    }
    case TIOCGSID:
        if (!arg) return -EFAULT;
        if (!ld->sid) return -ENOTTY;
        *(int *)arg = (int)ld->sid;
        return 0;
    case FIONREAD:
        if (!arg) return -EFAULT;
        *(int *)arg = (int)ld->input.count;
        return 0;
    default:
        return -ENOTTY;
    }
}

void tty_set_foreground(uint32_t pgid)
{
    task_t *cur = task_current();
    ttys[0].ld.fg_pgid = pgid;
    if (pgid && cur) ttys[0].ld.sid = cur->sid;
}

int kb_get_key(void);
static void emit_tty(void *ctx, uint8_t c);
int tty_console_read(char *buf, uint32_t len)
{
    ldisc_t *ld = &ttys[0].ld;
    task_t *cur = task_current();
    if (!len) return 0;
    if (cur) ld->reader = cur->id;
    uint32_t vmin = ld->termios.c_cc[VMIN];
    for (;;) {
        if (ld->termios.c_lflag & ICANON) { if (ld->lines || ld->eof) break; }
        else if (ld->input.count >= (vmin ? (vmin > len ? len : vmin) : 1)) break;
        int k = kb_get_key();
        if (k >= 0 && k <= 0xff) ld_input(ld, emit_tty, &ttys[0], (uint8_t)k);
        else task_yield();
    }
    return ld_read(ld, (uint8_t *)buf, len);
}

/* ------------------------------ console tty ------------------------------ */
static void emit_tty(void *ctx, uint8_t c)
{
    tty_t *t = (tty_t *)ctx;
    rb_put(&t->output, &c, 1);
    if (t->output.count > TTY_BUF_SIZE - 16) rb_clear(&t->output);
    if (t->id == current_tty) console_putchar((char)c);
}

static int tty_vfs_read(vfs_node_t *node, uint32_t off, void *buf, uint32_t n)
{
    (void)off;
    return node && buf ? tty_read((int)(uintptr_t)node->private_data, buf, n) : -1;
}
static int tty_vfs_write(vfs_node_t *node, uint32_t off, const void *buf, uint32_t n)
{
    (void)off;
    return node && buf ? tty_write((int)(uintptr_t)node->private_data, buf, n) : -1;
}
static int tty_vfs_ioctl(vfs_node_t *node, uint32_t cmd, void *arg)
{
    return tty_ioctl(node ? (int)(uintptr_t)node->private_data : 0, cmd, arg);
}
static const vfs_ops_t tty_ops = {
    .read = tty_vfs_read, .write = tty_vfs_write, .ioctl = tty_vfs_ioctl,
};

void tty_init(void)
{
    memset(ttys, 0, sizeof(ttys));
    memset(ptys, 0, sizeof(ptys));
    for (int i = 0; i < TTY_MAX; i++) {
        ttys[i].id = i;
        ttys[i].active = (i == 0);
        ld_defaults(&ttys[i].ld);
    }
    current_tty = 0;
}

void tty_switch(int n)
{
    if (n < 0 || n >= TTY_MAX || n == current_tty) return;
    ttys[current_tty].active = 0;
    current_tty = n;
    ttys[n].active = 1;
}
int tty_current(void) { return current_tty; }
tty_t *tty_get(int n) { return (n < 0 || n >= TTY_MAX) ? NULL : &ttys[n]; }

int tty_write(int n, const char *buf, uint32_t len)
{
    if (n < 0 || n >= TTY_MAX || !buf) return -1;
    ld_bg_check(&ttys[n].ld, SIGTTOU);
    for (uint32_t i = 0; i < len; i++)
        ld_out(&ttys[n].ld, emit_tty, &ttys[n], (uint8_t)buf[i]);
    return (int)len;
}
int tty_read(int n, char *buf, uint32_t len)
{
    if (n < 0 || n >= TTY_MAX || !buf) return -1;
    return ld_read(&ttys[n].ld, (uint8_t *)buf, len);
}
void tty_input_char(char c)
{
    ld_input(&ttys[current_tty].ld, emit_tty, &ttys[current_tty], (uint8_t)c);
}
int tty_output_ready(int n) { return (n < 0 || n >= TTY_MAX) ? 0 : (int)ttys[n].output.count; }
int tty_input_ready(int n) { return (n < 0 || n >= TTY_MAX) ? 0 : (int)ttys[n].ld.input.count; }
int tty_ioctl(int n, uint32_t cmd, void *arg)
{
    if (n < 0 || n >= TTY_MAX) return -ENOTTY;
    return ld_ioctl(&ttys[n].ld, &ttys[n].output, cmd, arg);
}
int tty_register_vfs(int n, void *nodep)
{
    vfs_node_t *node = nodep;
    if (n < 0 || n >= TTY_MAX || !node) return -1;
    ttys[n].vfs_node = node;
    node->type = VFS_NODE_CHARDEV;
    node->ops = &tty_ops;
    node->private_data = (void *)(uintptr_t)n;
    return 0;
}
const void *tty_console_ops(void) { return &tty_ops; }

/* ------------------------------ PTY ------------------------------ */
static void emit_s2m(void *ctx, uint8_t c)
{
    pty_t *p = ctx;
    while (p->s2m.count >= TTY_BUF_SIZE) task_yield();
    rb_put(&p->s2m, &c, 1);
}
#define PTY_OF(nd) (&ptys[(int)(uintptr_t)(nd)->private_data])

static int pty_slave_read_(vfs_node_t *nd, uint32_t off, void *buf, uint32_t n)
{
    (void)off;
    return ld_read(&PTY_OF(nd)->ld, buf, n);
}
static int pty_slave_write_(vfs_node_t *nd, uint32_t off, const void *buf, uint32_t n)
{
    (void)off;
    pty_t *p = PTY_OF(nd);
    ld_bg_check(&p->ld, SIGTTOU);
    for (uint32_t i = 0; i < n; i++) ld_out(&p->ld, emit_s2m, p, ((const uint8_t *)buf)[i]);
    return (int)n;
}
static int pty_slave_ioctl_(vfs_node_t *nd, uint32_t cmd, void *arg)
{
    pty_t *p = PTY_OF(nd);
    return ld_ioctl(&p->ld, &p->s2m, cmd, arg);
}
static const vfs_ops_t pty_slave_ops = {
    .read = pty_slave_read_, .write = pty_slave_write_, .ioctl = pty_slave_ioctl_,
};

static int pty_master_read_(vfs_node_t *nd, uint32_t off, void *buf, uint32_t n)
{
    (void)off;
    pty_t *p = PTY_OF(nd);
    while (!p->s2m.count) task_yield();
    return rb_get(&p->s2m, buf, n);
}
static int pty_master_write_(vfs_node_t *nd, uint32_t off, const void *buf, uint32_t n)
{
    (void)off;
    pty_t *p = PTY_OF(nd);
    for (uint32_t i = 0; i < n; i++) ld_input(&p->ld, emit_s2m, p, ((const uint8_t *)buf)[i]);
    return (int)n;
}
static int pty_master_ioctl_(vfs_node_t *nd, uint32_t cmd, void *arg)
{
    pty_t *p = PTY_OF(nd);
    switch (cmd) {
    case TIOCGPTN:
        if (!arg) return -EFAULT;
        *(uint32_t *)arg = (uint32_t)p->index;
        return 0;
    case TIOCSPTLCK:
        if (!arg) return -EFAULT;
        p->locked = *(int *)arg != 0;
        return 0;
    case TIOCGPGRP: case TIOCSPGRP: case TIOCSCTTY: case TIOCNOTTY: case TIOCGSID:
        return -ENOTTY;
    case FIONREAD:
        if (!arg) return -EFAULT;
        *(int *)arg = (int)p->s2m.count;
        return 0;
    default:
        return ld_ioctl(&p->ld, &p->s2m, cmd, arg);
    }
}
static const vfs_ops_t pty_master_ops = {
    .read = pty_master_read_, .write = pty_master_write_, .ioctl = pty_master_ioctl_,
};

void *pty_open_master(void)
{
    for (int i = 0; i < PTY_MAX; i++) {
        if (ptys[i].used) continue;
        pty_t *p = &ptys[i];
        memset(p, 0, sizeof(*p));
        p->used = true; p->locked = true; p->index = i;
        ld_defaults(&p->ld);
        vfs_node_t *m = kmalloc(sizeof(*m));
        if (!m) { p->used = false; return NULL; }
        memset(m, 0, sizeof(*m));
        strcpy(m->name, "/dev/ptmx");
        m->type = VFS_NODE_CHARDEV; m->mode = S_IFCHR | 0666;
        m->ops = &pty_master_ops; m->private_data = (void *)(uintptr_t)i;
        char path[32] = "/dev/pts/";
        int l = 9;
        if (i >= 10) path[l++] = (char)('0' + i / 10);
        path[l++] = (char)('0' + i % 10);
        path[l] = 0;
        vfs_node_t *s = vfs_lookup(path);
        if (!s) s = vfs_create(path);
        if (s) {
            s->type = VFS_NODE_CHARDEV; s->mode = S_IFCHR | 0620;
            s->ops = &pty_slave_ops; s->private_data = (void *)(uintptr_t)i;
        }
        return m;
    }
    return NULL;
}

int pty_master_index(const void *node)
{
    const vfs_node_t *n = node;
    return (n && n->ops == &pty_master_ops) ? (int)(uintptr_t)n->private_data : -1;
}

int tty_node_is_tty(const void *node)
{
    const vfs_node_t *n = node;
    return n && (n->ops == &tty_ops || n->ops == &pty_slave_ops || n->ops == &pty_master_ops);
}