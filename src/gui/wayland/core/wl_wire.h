/**
 * @file    wl_wire.h
 * @brief   Wayland 线协议编解码：消息头、参数游标、事件写入
 *
 * 这一层只认"字节长什么样"，不认任何接口语义：
 *   header = { uint32 object_id, uint32 (size<<16 | opcode) }
 *   参数   = 按 4 字节对齐依次排列；string 是 uint32 长度 + 字节 + NUL + 补齐
 *
 * 之所以把 size 放在高 16 位：Wayland 的 opcode 只有 16 位，剩下 16 位给
 * 消息长度（含 8 字节头，上限 64KB）。这两半弄反是整条连接报废级错误。
 *
 * @layer   gui
 * @owner   src/gui/wayland/core/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  wl_args_t            结构体 / 类型别名
 *         wl_message_header_t  结构体 / 类型别名
 *         wl_outbuf_t          结构体 / 类型别名
 *   宏    WL_MESSAGE_HEADER_SIZE  WLMESSAGE首部
 *         WL_MESSAGE_MAX_SIZE     常量 / 特性宏
 *   函数  wl_arg_u32          （待补）
 *         wl_arg_i32          （待补）
 *         wl_arg_string       读一个字符串到 buf（超长截断到 cap…
 *         wl_arg_skip_string  跳过字符串/任意 4 字节参数
 *         wl_arg_skip_u32     （待补）
 *         wl_args_failed      （待补）
 *         wl_outbuf_init      （待补）
 *         wl_out_begin        （待补）
 *         wl_out_u32          （待补）
 *         wl_out_i32          （待补）
 *         wl_out_string       （待补）
 *         wl_out_end          （待补）
 *         wl_out_abort        （待补）
 */
#ifndef HBOS_WL_WIRE_H
#define HBOS_WL_WIRE_H

#include <stdint.h>

#define WL_MESSAGE_HEADER_SIZE 8
#define WL_MESSAGE_MAX_SIZE    0xffffU

/** 参数游标：handler 按需顺序消费，读失败后后续读取全部失效。 */
typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint32_t offset;
    int error;
} wl_args_t;

uint32_t wl_arg_u32(wl_args_t *args);
int32_t wl_arg_i32(wl_args_t *args);
/** 读一个字符串到 buf（超长截断到 capacity-1）。返回是否成功。 */
int wl_arg_string(wl_args_t *args, char *buf, uint32_t capacity);
/** 跳过字符串/任意 4 字节参数。 */
void wl_arg_skip_string(wl_args_t *args);
void wl_arg_skip_u32(wl_args_t *args, int count);
int wl_args_failed(const wl_args_t *args);

/** 消息头解析结果。 */
typedef struct {
    uint32_t object_id;
    uint16_t opcode;
    uint32_t size;          /**< 已含 8 字节头 */
} wl_message_header_t;

/** 解析一条消息头；size 越界或不足 8 返回 0。 */
int wl_message_parse(const uint8_t *data, uint32_t available,
                     wl_message_header_t *header);

/* ── 事件写入 ────────────────────────────────────────────────
 *
 * 调用方按 begin → 参数若干 → end 的顺序写。长度在 end 时回填。
 * 缓冲区满时 begin 会把长度清零并置 in_error，后续写入全部丢弃，
 * 由 flush 统一处理 —— 不要在半写状态下 end。 */

typedef struct {
    uint8_t *buffer;
    uint32_t capacity;
    uint32_t length;
    uint32_t event_offset;   /**< 当前事件的起点 */
    int in_error;
} wl_outbuf_t;

void wl_outbuf_init(wl_outbuf_t *out, uint8_t *buffer, uint32_t capacity);
int wl_out_begin(wl_outbuf_t *out, uint32_t object_id, uint16_t opcode);
void wl_out_u32(wl_outbuf_t *out, uint32_t value);
void wl_out_i32(wl_outbuf_t *out, int32_t value);
void wl_out_string(wl_outbuf_t *out, const char *text);
void wl_out_end(wl_outbuf_t *out);
void wl_out_abort(wl_outbuf_t *out);

#endif /* HBOS_WL_WIRE_H */
