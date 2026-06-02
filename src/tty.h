#ifndef HBOS_TTY_H
#define HBOS_TTY_H

#include <stdint.h>

#define TTY_MAX      4
#define TTY_BUF_SIZE 4096

typedef struct {
    uint8_t buf[TTY_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int raw_mode;
} tty_ringbuf_t;

typedef struct {
    int id;
    tty_ringbuf_t input;
    tty_ringbuf_t output;
    int active;
    void *vfs_node;
} tty_t;

void tty_init(void);
void tty_switch(int n);
int  tty_current(void);
tty_t *tty_get(int n);
int  tty_write(int n, const char *buf, uint32_t len);
int  tty_read(int n, char *buf, uint32_t len);
void tty_input_char(char c);
int  tty_output_ready(int n);
int  tty_input_ready(int n);

#endif