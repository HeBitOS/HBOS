/**
 * @file    json.h
 * @brief   极简只读 JSON 解析器（用于主题包等配置文件的读取）。
 *
 * 设计取向与取舍：
 *  - **只读、零分配**：解析时在原文本上就地定位，键值对写进调用方给的槽位
 *    数组，不调用 kmalloc。主题文件很小（<4 KB），且解析失败必须安全降级，
 *    不应因为内存不足让系统起不来。
 *  - **扁平对象**：只支持 `{ "key": value, ... }` 一层，值限
 *    字符串 / 数字 / 布尔 / null。主题包用不到嵌套数组，硬塞完整 JSON
 *    只会把解析器和测试面积放大。
 *  - **宽容**：不因未知键、多余空白、行尾逗号而失败；调用方按需取键。
 *
 * 不支持（也是有意不支持）：嵌套对象/数组、转义序列 \uXXXX、注释。
 * 若将来需要嵌套，应换用完整解析器而不是在这里堆特例。
 */
#ifndef HBOS_JSON_H
#define HBOS_JSON_H

#include <stdbool.h>
#include <stdint.h>

#define JSON_MAX_PAIRS   48   /**< 单个对象最多键值对（主题键约 20 个，留足余量） */
#define JSON_KEY_MAX     32
#define JSON_VAL_MAX     64

/** 值类型 */
typedef enum {
    JSON_NONE = 0,
    JSON_STR,
    JSON_NUM,
    JSON_BOOL,
    JSON_NULL
} json_type_t;

/** 一个键值对 */
typedef struct {
    char        key[JSON_KEY_MAX];
    char        val[JSON_VAL_MAX];   /**< 字符串值或数字的原始文本 */
    json_type_t type;
    /* 数值用整数存放：内核以 -mno-80387 -mno-sse 编译，double 的传参与返回
     * 会落到 SSE 寄存器而编译失败。主题里只有圆角/阴影这类小整数，以及
     * 极少数小数（如 1.5），因此存"放大 1000 倍后的整数"，需要小数时由
     * json_get_num1000() 取。这样内核侧全程整数，不引入浮点。 */
    int32_t     num1000;             /**< 数值 ×1000（3.5 → 3500） */
    bool        boolean;             /**< type==JSON_BOOL 时的值 */
} json_pair_t;

/** 解析结果：定长槽位数组，无动态分配 */
typedef struct {
    json_pair_t pairs[JSON_MAX_PAIRS];
    int         count;
    bool        ok;                  /**< 顶层结构是否可解析（有 { } 且键值合法） */
} json_doc_t;

/**
 * @brief 解析一个扁平 JSON 对象。
 * @param text  以 NUL 结尾的文本
 * @param len   文本长度（不含结尾 NUL）
 * @param out   输出
 * @return true 顶层结构解析成功（即使某些键被跳过）；false 完全无法解析
 *
 * 失败时 out->ok = false 且 count = 0，调用方应整体回退到默认值。
 */
bool json_parse_object(const char *text, uint32_t len, json_doc_t *out);

/** 查找字符串值；找不到或类型不符返回 NULL */
const char *json_get_str(const json_doc_t *doc, const char *key);

/** 查找数值（×1000 定点）；找不到或类型不符返回 fallback */
int32_t json_get_num1000(const json_doc_t *doc, const char *key, int32_t fallback);

/** 查找布尔；找不到或类型不符返回 fallback */
bool json_get_bool(const json_doc_t *doc, const char *key, bool fallback);

/** 取整数（内部走 json_get_num 并做范围钳制） */
int json_get_int(const json_doc_t *doc, const char *key, int fallback);

/**
 * @brief 解析 "#RRGGBB" 或 "#AARRGGBB" / "0xRRGGBB" 颜色。
 * @param out 写出 0xAARRGGBB（缺省 alpha 为 0xFF）
 * @return true 成功
 */
bool json_parse_color(const char *s, uint32_t *out);

#endif /* HBOS_JSON_H */
