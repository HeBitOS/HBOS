#ifndef _FLANTERM_H
#define _FLANTERM_H
#include "types.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef FLANTERM_H
#    define FLANTERM_H 1



#    ifdef __cplusplus
extern "C" {
#    endif

#    define FLANTERM_CB_DEC           10
#    define FLANTERM_CB_BELL          20
#    define FLANTERM_CB_PRIVATE_ID    30
#    define FLANTERM_CB_STATUS_REPORT 40
#    define FLANTERM_CB_POS_REPORT    50
#    define FLANTERM_CB_KBD_LEDS      60
#    define FLANTERM_CB_MODE          70
#    define FLANTERM_CB_LINUX         80

#    define FLANTERM_OOB_OUTPUT_OCRNL  (1 << 0)
#    define FLANTERM_OOB_OUTPUT_OFDEL  (1 << 1)
#    define FLANTERM_OOB_OUTPUT_OFILL  (1 << 2)
#    define FLANTERM_OOB_OUTPUT_OLCUC  (1 << 3)
#    define FLANTERM_OOB_OUTPUT_ONLCR  (1 << 4)
#    define FLANTERM_OOB_OUTPUT_ONLRET (1 << 5)
#    define FLANTERM_OOB_OUTPUT_ONOCR  (1 << 6)
#    define FLANTERM_OOB_OUTPUT_OPOST  (1 << 7)

#    ifdef FLANTERM_IN_FLANTERM

#        include "flanterm_private.h"

#    else

struct flanterm_context;

#    endif

void flanterm_putchar(struct flanterm_context *ctx, uint8_t c);
void flanterm_write(struct flanterm_context *ctx, const char *buf, uint64_t count);
void flanterm_flush(struct flanterm_context *ctx);
void flanterm_full_refresh(struct flanterm_context *ctx);
void flanterm_deinit(struct flanterm_context *ctx, void (*_free)(void *ptr, uint64_t size));

void flanterm_get_dimensions(struct flanterm_context *ctx, uint64_t *cols, uint64_t *rows);
void flanterm_set_autoflush(struct flanterm_context *ctx, bool state);
void flanterm_set_callback(
    struct flanterm_context *ctx,
    void (*callback)(struct flanterm_context *, uint64_t, uint64_t, uint64_t, uint64_t)
);
uint64_t flanterm_get_oob_output(struct flanterm_context *ctx);
void flanterm_set_oob_output(struct flanterm_context *ctx, uint64_t oob_output);

#    ifdef __cplusplus
}
#    endif

#endif
#endif
