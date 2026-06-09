#ifndef HBOS_MOUSE_H
#define HBOS_MOUSE_H

#include <stdint.h>

typedef struct {
    int dx;
    int dy;
    int dz;
    uint8_t buttons;
} mouse_event_t;

int mouse_init(void);
void mouse_shutdown(void);
int mouse_poll(mouse_event_t *ev);
int mouse_is_ready(void);
const char *mouse_backend_name(void);

#endif
