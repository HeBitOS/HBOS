#include "tty.h"
#include "string.h"
#include "vfs.h"
#include "graphics/graphics.h"

static tty_t ttys[TTY_MAX];
static int current_tty = 0;

#define TTY_COLORS \
    "\x1b[40m\x1b[37m"

static void ringbuf_init(tty_ringbuf_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    rb->raw_mode = 0;
}

static int ringbuf_write(tty_ringbuf_t *rb, const uint8_t *buf, uint32_t len)
{
    uint32_t written = 0;
    while (written < len && rb->count < TTY_BUF_SIZE) {
        rb->buf[rb->head] = buf[written];
        rb->head = (rb->head + 1) % TTY_BUF_SIZE;
        rb->count++;
        written++;
    }
    return (int)written;
}

static int ringbuf_read(tty_ringbuf_t *rb, uint8_t *buf, uint32_t len)
{
    uint32_t read_count = 0;
    while (read_count < len && rb->count > 0) {
        buf[read_count] = rb->buf[rb->tail];
        rb->tail = (rb->tail + 1) % TTY_BUF_SIZE;
        rb->count--;
        read_count++;
    }
    return (int)read_count;
}

static int ringbuf_available(tty_ringbuf_t *rb)
{
    return (int)rb->count;
}

static int ringbuf_free(tty_ringbuf_t *rb)
{
    return TTY_BUF_SIZE - (int)rb->count;
}

static int tty_vfs_read(vfs_node_t *node, uint32_t offset, void *buf, uint32_t count)
{
    (void)offset;
    if (!node || !buf) return -1;
    int tty_idx = (int)(uintptr_t)node->private_data;
    return tty_read(tty_idx, (char *)buf, count);
}

static int tty_vfs_write(vfs_node_t *node, uint32_t offset, const void *buf, uint32_t count)
{
    (void)offset;
    if (!node || !buf) return -1;
    int tty_idx = (int)(uintptr_t)node->private_data;
    return tty_write(tty_idx, (const char *)buf, count);
}

static const vfs_ops_t tty_ops = {
    .read = tty_vfs_read,
    .write = tty_vfs_write,
};

void tty_init(void)
{
    memset(ttys, 0, sizeof(ttys));
    for (int i = 0; i < TTY_MAX; i++) {
        ttys[i].id = i;
        ringbuf_init(&ttys[i].input);
        ringbuf_init(&ttys[i].output);
        ttys[i].active = (i == 0) ? 1 : 0;
        ttys[i].vfs_node = NULL;
    }
    current_tty = 0;
    console_puts(TTY_COLORS);
    console_puts("\n\x1b[32m[TTY] ");
    char buf[8];
    int n = 0, cnt = TTY_MAX;
    do { buf[n++] = '0' + (cnt % 10); cnt /= 10; } while (cnt);
    for (int j = n - 1; j >= 0; j--) console_putchar(buf[j]);
    console_puts(" virtual terminals ready (Alt+F1-F");
    n = 0; cnt = TTY_MAX;
    do { buf[n++] = '0' + (cnt % 10); cnt /= 10; } while (cnt);
    for (int j = n - 1; j >= 0; j--) console_putchar(buf[j]);
    console_puts(")\x1b[0m\n");
}

void tty_switch(int n)
{
    if (n < 0 || n >= TTY_MAX) return;
    if (n == current_tty) return;

    ttys[current_tty].active = 0;
    current_tty = n;
    ttys[current_tty].active = 1;

    console_clear();

    console_puts("\n\x1b[36m========================================\x1b[0m\n");
    console_puts("\x1b[33m  Virtual Terminal ");
    char buf[8];
    int bn = 0, tn = n + 1;
    do { buf[bn++] = '0' + (tn % 10); tn /= 10; } while (tn);
    for (int j = bn - 1; j >= 0; j--) console_putchar(buf[j]);
    console_puts("\x1b[0m\n");
    console_puts("\x1b[36m========================================\x1b[0m\n\n");
}

int tty_current(void)
{
    return current_tty;
}

tty_t *tty_get(int n)
{
    if (n < 0 || n >= TTY_MAX) return NULL;
    return &ttys[n];
}

int tty_write(int n, const char *buf, uint32_t len)
{
    if (n < 0 || n >= TTY_MAX || !buf) return -1;

    int written = ringbuf_write(&ttys[n].output, (const uint8_t *)buf, len);

    if (n == current_tty) {
        for (uint32_t i = 0; i < (uint32_t)written; i++) {
            console_putchar(buf[i]);
        }
    }

    return written;
}

int tty_read(int n, char *buf, uint32_t len)
{
    if (n < 0 || n >= TTY_MAX || !buf) return -1;

    while (ringbuf_available(&ttys[n].input) == 0) {
        __asm__ volatile("pause");
    }

    return ringbuf_read(&ttys[n].input, (uint8_t *)buf, len);
}

void tty_input_char(char c)
{
    int r = ringbuf_write(&ttys[current_tty].input, (const uint8_t *)&c, 1);
    (void)r;
}

int tty_output_ready(int n)
{
    if (n < 0 || n >= TTY_MAX) return 0;
    return ringbuf_available(&ttys[n].output);
}

int tty_input_ready(int n)
{
    if (n < 0 || n >= TTY_MAX) return 0;
    return ringbuf_available(&ttys[n].input);
}

int tty_register_vfs(int n, vfs_node_t *node)
{
    if (n < 0 || n >= TTY_MAX || !node) return -1;
    ttys[n].vfs_node = node;
    node->type = VFS_NODE_CHARDEV;
    node->ops = &tty_ops;
    node->private_data = (void *)(uintptr_t)n;
    return 0;
}