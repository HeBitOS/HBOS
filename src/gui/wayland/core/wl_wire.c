/**
 * @file    wl_wire.c
 * @brief   Wayland 线协议编解码实现
 *
 * 纯函数 + 一个输出缓冲，不依赖内核、不依赖连接状态，可以在宿主机上单测。
 */

#include "wl_wire.h"
#include "../../../string.h"

/* ── 参数读取 ───────────────────────────────────────────────── */

uint32_t wl_arg_u32(wl_args_t *args) {
    if (!args || args->error || args->offset + 4 > args->length) {
        if (args) args->error = 1;
        return 0;
    }
    uint32_t value;
    memcpy(&value, args->data + args->offset, 4);
    args->offset += 4;
    return value;
}

int32_t wl_arg_i32(wl_args_t *args) {
    return (int32_t)wl_arg_u32(args);
}

int wl_arg_string(wl_args_t *args, char *buf, uint32_t capacity) {
    uint32_t length = wl_arg_u32(args);
    if (args->error) return 0;
    uint32_t padded = (length + 3) & ~3U;
    if (args->offset + padded > args->length) {
        args->error = 1;
        return 0;
    }
    uint32_t copy = length;
    if (copy && buf && capacity) {
        if (copy >= capacity) copy = capacity - 1;
        memcpy(buf, args->data + args->offset, copy);
        buf[copy] = '\0';
    }
    args->offset += padded;
    return 1;
}

void wl_arg_skip_string(wl_args_t *args) {
    uint32_t length = wl_arg_u32(args);
    if (args->error) return;
    uint32_t padded = (length + 3) & ~3U;
    if (args->offset + padded > args->length) {
        args->error = 1;
        return;
    }
    args->offset += padded;
}

void wl_arg_skip_u32(wl_args_t *args, int count) {
    for (int i = 0; i < count; i++) (void)wl_arg_u32(args);
}

int wl_args_failed(const wl_args_t *args) {
    return args && args->error;
}

/* ── 消息头 ─────────────────────────────────────────────────── */

int wl_message_parse(const uint8_t *data, uint32_t available,
                     wl_message_header_t *header) {
    if (!data || !header || available < WL_MESSAGE_HEADER_SIZE) return 0;
    memcpy(&header->object_id, data, 4);
    uint32_t word;
    memcpy(&word, data + 4, 4);
    header->opcode = (uint16_t)(word & 0xffffU);
    header->size = word >> 16;
    if (header->size < WL_MESSAGE_HEADER_SIZE) return 0;
    if (header->size > available) return 0;
    return 1;
}

/* ── 事件写入 ───────────────────────────────────────────────── */

void wl_outbuf_init(wl_outbuf_t *out, uint8_t *buffer, uint32_t capacity) {
    if (!out) return;
    out->buffer = buffer;
    out->capacity = capacity;
    out->length = 0;
    out->event_offset = 0;
    out->in_error = 0;
}

int wl_out_begin(wl_outbuf_t *out, uint32_t object_id, uint16_t opcode) {
    if (!out || !out->buffer) return 0;
    if (out->in_error) return 0;
    if (out->length + WL_MESSAGE_HEADER_SIZE > out->capacity) {
        /* 必须置 in_error：调用方普遍不检查返回值，若这里只是 return 0，
         * 后续 wl_out_u32/string 会继续往缓冲里写，把上一条事件的尾巴
         * 覆盖成半条新消息 —— 而 wl_out_end 仍会用 event_offset 回填
         * size，产出一条 header 与 body 不符的畸形消息。宁可整条缓冲
         * 报废（由 flush 侧丢弃并记录），也不要发出损坏的字节流。 */
        out->in_error = 1;
        return 0;
    }
    out->event_offset = out->length;
    uint32_t *slot = (uint32_t *)(void *)&out->buffer[out->length];
    slot[0] = object_id;
    slot[1] = (uint32_t)opcode;
    out->length += WL_MESSAGE_HEADER_SIZE;
    return 1;
}

void wl_out_u32(wl_outbuf_t *out, uint32_t value) {
    if (!out || out->in_error) return;
    if (out->length + 4 > out->capacity) {
        out->in_error = 1;
        return;
    }
    memcpy(&out->buffer[out->length], &value, 4);
    out->length += 4;
}

void wl_out_i32(wl_outbuf_t *out, int32_t value) {
    wl_out_u32(out, (uint32_t)value);
}

void wl_out_string(wl_outbuf_t *out, const char *text) {
    if (!out || out->in_error) return;
    if (!text) {
        wl_out_u32(out, 0);
        return;
    }
    /* 长度字段包含结尾 NUL（这是 Wayland 的规定），然后补齐到 4 字节。 */
    uint32_t length = (uint32_t)strlen(text);
    wl_out_u32(out, length + 1);
    for (uint32_t i = 0; i < length; i++) {
        if (out->in_error) return;
        if (out->length + 1 > out->capacity) {
            out->in_error = 1;
            return;
        }
        out->buffer[out->length++] = (uint8_t)text[i];
    }
    if (out->length + 1 > out->capacity) {
        out->in_error = 1;
        return;
    }
    out->buffer[out->length++] = 0;
    while (out->length & 3) {
        if (out->length + 1 > out->capacity) {
            out->in_error = 1;
            return;
        }
        out->buffer[out->length++] = 0;
    }
}

void wl_out_end(wl_outbuf_t *out) {
    if (!out || out->in_error) return;
    uint32_t start = out->event_offset;
    if (start >= out->length) return;
    uint32_t size = out->length - start;
    if (size > WL_MESSAGE_MAX_SIZE) {
        out->length = start;
        out->in_error = 1;
        return;
    }
    uint32_t *header = (uint32_t *)(void *)&out->buffer[start];
    header[1] = (header[1] & 0xffffU) | ((size & 0xffffU) << 16);
}

void wl_out_abort(wl_outbuf_t *out) {
    if (!out) return;
    if (out->event_offset < out->length) out->length = out->event_offset;
    out->in_error = 0;
}
