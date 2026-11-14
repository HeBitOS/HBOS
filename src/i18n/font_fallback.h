/**
 * @file    font_fallback.h
 * @brief   按脚本的字体回退链：主字体缺字时找下一个能画的字体
 *
 * 数据模型（三张表，全部静态、无分配）：
 *   1. script_ranges[]：码点 → 脚本，按 first 升序的闭区间表，二分查找。
 *   2. g_faces[]：注册的字体面，每个面只声明自己覆盖哪个脚本（不逐字
 *      列码点——CJK 字体上千字，逐字表会吃掉启动内存）。
 *   3. g_cache[]：按码点直接映射的解析缓存（默认 64 项），避免同一行
 *      文字里重复走"二分 + 回退链 + 逐面 lookup"。
 *
 * 查表成本（写进契约，改实现要同步 docs/wiki/I18N-PLAN.md §5）：
 *   · i18n_script_of：~7 次区间比较（70 个区间）。
 *   · i18n_font_chain：脚本命中后按注册顺序收集，O(面数) ≤ 8。
 *   · i18n_font_face_for：最坏 = script_of + chain + 每个候选面的
 *     lookup（CJK 面是对码点数组二分，O(log N)）。命中缓存后 O(1)。
 *   · 回退顺序：同脚本面按注册顺序 → 主拉丁面兜底。
 *
 * 双向文本（阿拉伯/希伯来）本轮只做**字体选择**，不做 bidi 重排与
 * Arabic shaping：逻辑顺序存字节，渲染顺序仍是 LTR。这是已知缺口，
 * 里程碑写在 I18N-PLAN.md，别把 SCRIPT_ARABIC 当成"RTL 已支持"。
 *
 * @layer   lib
 * @owner   src/i18n/
 * @stable  internal
 *
 * 依赖    <stdint.h>、i18n/wcwidth.h（复用 i18n_cp_range_t）
 * 被依赖  src/i18n/font_fallback.c 的使用方、tests/host/test_i18n.c
 *
 * 索引头文件
 *   类型  i18n_script_t      脚本分类（回退链的键）
 *         i18n_glyph_lookup_t 字体面的字形查询回调
 *         i18n_font_face_t   一个字体面的描述
 *   宏    I18N_FONT_MAX_FACES 注册面上限
 *         I18N_FONT_CHAIN_MAX 单次回退链最多返回几个候选
 *         I18N_FONT_CACHE_SIZE 码点→面 缓存槽数（2 的幂）
 *         I18N_SCRIPT_COUNT  脚本数量（含 DEFAULT）
 *   函数  i18n_script_of       码点属于哪个脚本
 *         i18n_script_name     脚本名（日志/诊断）
 *         i18n_font_reset      清空注册表与缓存
 *         i18n_font_add_face   追加一个字体面
 *         i18n_font_face_count 已注册面数
 *         i18n_font_chain      取某码点的回退链（不探测字形）
 *         i18n_font_face_for   解析实际能画出该码点的面（走缓存）
 *         i18n_font_lookup_probes 累计字形探测次数（成本观测/单测）
 */
#ifndef HBOS_I18N_FONT_FALLBACK_H
#define HBOS_I18N_FONT_FALLBACK_H

#include <stdint.h>

#define I18N_FONT_MAX_FACES  8
#define I18N_FONT_CHAIN_MAX  4
#define I18N_FONT_CACHE_SIZE 64

typedef enum {
    I18N_SCRIPT_LATIN = 0,   /* 拉丁/希腊字母以外的默认兜底面 */
    I18N_SCRIPT_GREEK,
    I18N_SCRIPT_CYRILLIC,
    I18N_SCRIPT_HEBREW,
    I18N_SCRIPT_ARABIC,
    I18N_SCRIPT_DEVANAGARI,
    I18N_SCRIPT_CJK,
    I18N_SCRIPT_EMOJI,
    I18N_SCRIPT_SYMBOL,
    I18N_SCRIPT_COUNT
} i18n_script_t;

/** 字体面查字形：返回位图指针或 NULL。ctx 由注册方解释。 */
typedef const uint8_t *(*i18n_glyph_lookup_t)(uint32_t cp, void *ctx);

typedef struct {
    const char *name;          /* 诊断用，不复制（必须是静态字符串） */
    i18n_script_t script;
    uint32_t glyph_px;         /* 正方形点阵边长；0 表示矢量/未知 */
    i18n_glyph_lookup_t lookup;
    void *ctx;
} i18n_font_face_t;

int i18n_script_of(uint32_t cp);
const char *i18n_script_name(int script);

/** 清空注册表、缓存与探测计数。 */
void i18n_font_reset(void);

/**
 * @brief 追加字体面（复制结构体，不复制 name 字符串）。
 * @return 新面的下标；表满/参数非法返回 -1
 */
int i18n_font_add_face(const i18n_font_face_t *face);

/** 已注册字体面数。 */
uint32_t i18n_font_face_count(void);

/**
 * @brief 取 cp 的回退链（只按脚本选面，不调用 lookup）。
 * @param out  输出面下标数组
 * @param max  容量
 * @return 链长（0..max）；cp 大于 U+10FFFF 返回 0
 */
int i18n_font_chain(uint32_t cp, int *out, uint32_t max);

/**
 * @brief 解析能画出 cp 的面。
 * @return 面下标；没有面能画返回 -1
 *
 * 不变量：同一次注册表状态下，同一码点重复调用结果稳定；命中缓存时
 * 不增加 i18n_font_lookup_probes()。
 */
int i18n_font_face_for(uint32_t cp);

/** 累计字形探测次数（可观测成本；i18n_font_reset 清零）。 */
uint32_t i18n_font_lookup_probes(void);

#endif /* HBOS_I18N_FONT_FALLBACK_H */
