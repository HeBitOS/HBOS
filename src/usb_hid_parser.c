/**
 * @file  usb_hid_parser.c
 * @brief USB HID 报告描述符解析器：把 Report Descriptor 还原成鼠标字段的位
 *        偏移 / 位宽布局，并按该布局取位解报告。纯逻辑、无硬件依赖、无动态
 *        内存、无浮点，可在宿主机上用 gcc 单测。
 *
 * 为什么需要它
 * ────────────
 * src/usb_hid.c 原先假设鼠标一定按 HID boot protocol 的固定 3/4 字节布局发
 * 报告（hid_set_boot_protocol() 发 SET_PROTOCOL(boot)）。真机上大量鼠标
 * bInterfaceProtocol = 0（不声明 boot 协议），或按 report protocol 上报带
 * DPI 键 / 横向滚轮 / 复合 Report ID 的报告——按固定布局解析必然错位，用户
 * 可见现象就是"鼠标不动 / 乱跳"。正确做法与 Linux 一致：先解析 Report
 * Descriptor 得到每个字段的 bit 偏移与宽度，再按 report protocol 取位
 * （drivers/hid/hid-core.c: hid_add_field() 里 offset = report->size;
 * report->size += report_size * report_count 的累加就是位偏移的来源，
 * drivers/hid/hid-input.c 再按 usage 把字段翻译成 X/Y/Wheel/Button）。
 *
 * 位偏移累加规则（本文件的不变式，改动务必保持）
 * ──────────────────────────────────────────────
 *  1. 只有 Input(0x81) main item 的字段进入"报告位流"；Output(0x91)/
 *     Feature(0xB1) 各有独立的位流，既不累加也不得打乱 Input 的偏移。Linux
 *     的 report->size 是按 (report_type, report_id) 分开记的，见
 *     drivers/hid/hid-core.c: hid_add_field()。
 *  2. 位流按 Report ID 分组：同一 Report ID 的 Input 字段按描述符中出现顺序首
 *     尾相接（cursor += report_size * report_count）；不同 Report ID 的 cursor
 *     互不影响。layout 里的 *_bit 是该报告 **载荷** 内的 bit 下标，0 号位
 *     是第一字节的最低位（HID 规定 LSB 在前），不含 Report ID 前缀字节。
 *  3. usage 少于 report_count 时，Linux 复制最后一个 usage 补齐剩余值
 *     （drivers/hid/hid-core.c: hid_add_field() 中 "Duplicate the last usage
 *     we parsed if we have excess values"），本文件照此处理，否则 padding
 *     字段之后的偏移会与设备实际字节流错开。
 *  4. 局部环境（usage 列表）在每个 main item 之后清空
 *     （drivers/hid/hid-core.c: hid_parser_main() 末尾 memset(&parser->local)），
 *     全局环境由 Push/Pop 显式存取。
 *
 * 未覆盖项（如需支持请在调用方兜底，勿假定本文件能处理）
 * ─────────────────────────────────────────────────────
 *  1. Report ID 复用：同一 Report ID 下多个 Application Collection 各有一套
 *     布局时，本文件按 Report ID 合并成一份布局。
 *  2. 32 位逻辑变量的完整符号语义、Usage 数组（Usage Minimum/Maximum 已做
 *     区间展开，但 >HID_PARSER_MAX_USAGE_RANGES 个区间的尾部会被截断）。
 *  3. 相对/绝对语义的换算：绝对坐标（Logical Minimum >= 0 的 X/Y）只按
 *     *_signed = 0 原样返回，转成相对位移是调用方（鼠标子系统）的事。
 *  4. 超过 HID_PARSER_MAX_REPORTS 个 Report ID 时，多出的 Report ID 被丢弃。
 *  5. 按键位不连续（中间夹非按键位）时 buttons_count 会把中间位也算进去。
 */

#include "usb_hid_parser.h"

/* ── item 前缀编码（drivers/hid/hid-core.c: fetch_item()）────────────
 * 短 item 首字节：bit[1:0] = size code（0/1/2/4 字节；code 3 → 4 字节），
 * bit[3:2] = type（0 main / 1 global / 2 local / 3 reserved），bit[7:4] = tag。
 * 0xFE 前缀是 long item，本解析器整段跳过（Linux 也支持但"尚未使用"）。 */
#define HID_ITEM_TYPE_MAIN   0
#define HID_ITEM_TYPE_GLOBAL 1
#define HID_ITEM_TYPE_LOCAL  2

#define HID_TAG_LONG 0x0Fu

/* main item tag（drivers/hid/hid-core.c: hid_parser_main()） */
#define HID_MAIN_TAG_INPUT          0x8u
#define HID_MAIN_TAG_OUTPUT         0x9u
#define HID_MAIN_TAG_COLLECTION     0xAu
#define HID_MAIN_TAG_FEATURE        0xBu
#define HID_MAIN_TAG_END_COLLECTION 0xCu

/* global item tag（drivers/hid/hid-core.c: hid_parser_global()） */
#define HID_GLOBAL_TAG_USAGE_PAGE   0x0u
#define HID_GLOBAL_TAG_LOGICAL_MIN  0x1u
#define HID_GLOBAL_TAG_LOGICAL_MAX  0x2u
#define HID_GLOBAL_TAG_PHYSICAL_MIN 0x3u
#define HID_GLOBAL_TAG_PHYSICAL_MAX 0x4u
#define HID_GLOBAL_TAG_REPORT_SIZE  0x7u
#define HID_GLOBAL_TAG_REPORT_ID    0x8u
#define HID_GLOBAL_TAG_REPORT_COUNT 0x9u
#define HID_GLOBAL_TAG_PUSH         0xAu
#define HID_GLOBAL_TAG_POP          0xBu

/* local item tag（drivers/hid/hid-core.c: hid_parser_local()） */
#define HID_LOCAL_TAG_USAGE     0x0u
#define HID_LOCAL_TAG_USAGE_MIN 0x1u
#define HID_LOCAL_TAG_USAGE_MAX 0x2u

/* usage page / usage id（USB HID Usage Tables；Linux 侧头文件在
 * include/linux/hid.h 的 HID_UP_* / HID_GD_*） */
#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01u
#define HID_USAGE_PAGE_BUTTON          0x09u
#define HID_USAGE_GD_X                 0x30u
#define HID_USAGE_GD_Y                 0x31u
#define HID_USAGE_GD_WHEEL             0x38u

/** 同时跟踪的 Report ID 上限：每个约 52 字节，8 个约占 416 字节栈。
 *  复合设备（键鼠 + 多媒体 + 厂商自定义）实测 Report ID 数远小于此值。 */
#define HID_PARSER_MAX_REPORTS 8

/** 单个 main item 内 usage 区间数的上限（超出部分截断，见文件头"未覆盖项"）。
 *  区间而非逐个 usage 存储，是为了让 Usage Min/Max（按键描述的标准写法，
 *  如 19 01 29 05）在常数内存内展开。 */
#define HID_PARSER_MAX_USAGE_RANGES 16

/** 全局环境 Push/Pop 栈深：HID 规范要求至少 4 层。 */
#define HID_PARSER_GLOBAL_STACK 4

/** report_size 上限，取自 Linux 的校验（hid-core.c: hid_parser_global()）。 */
#define HID_PARSER_MAX_REPORT_SIZE 256u

/** 单份报告的位数上限：布局结构体用 uint16_t 记 report_len_bits，故位流
 *  总量不得超过 0xFFFF（即 8 KiB 报告），超出即判定描述符不可信。 */
#define HID_PARSER_MAX_REPORT_BITS 0xFFFFu

/** 按键位图上限：多余的按键位被忽略（uint32_t 位图放不下）。 */
#define HID_PARSER_MAX_BUTTONS 32

/** 解析途中读出的一个 item（数据值按小端拼成 uint32_t）。 */
typedef struct {
    int      type;  /* 0/1/2 = main/global/local；-1 = long item（跳过） */
    int      tag;
    uint32_t size;  /* 数据字节数 */
    uint32_t data;  /* 数据值（size <= 4），long item 恒为 0 */
} hid_parser_item_t;

/** 全局 item 环境（Push/Pop 存取的就是这一整块）。 */
typedef struct {
    uint32_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max;
    int32_t  physical_min;
    int32_t  physical_max;
    uint32_t report_size;
    uint32_t report_count;
    uint32_t report_id;
} hid_parser_global_t;

/** 一段 usage 区间：单个 Usage 是 [id, id]，Usage Min/Max 是 [min, max]。 */
typedef struct {
    uint16_t page;
    uint16_t usage_min;
    uint16_t usage_max;
} hid_parser_usage_range_t;

/** 局部 item 环境：每个 main item 之后清空。 */
typedef struct {
    hid_parser_usage_range_t range[HID_PARSER_MAX_USAGE_RANGES];
    int      range_count;
    uint32_t pending_min;    /* Usage Minimum 待与 Usage Maximum 配对 */
    int      has_pending_min;
    int      truncated;      /* usage 区间超出上限，只影响字段识别 */
} hid_parser_local_t;

/** 一个 Report ID 的位流游标与已识别字段。 */
typedef struct {
    uint8_t  report_id;
    uint32_t bit_cursor;     /* 该 Report ID 下已排布的 Input 位数 */
    int      has_x, has_y, has_wheel;
    uint16_t x_bit, x_bits;     int x_signed;
    uint16_t y_bit, y_bits;     int y_signed;
    uint16_t wheel_bit, wheel_bits; int wheel_signed;
    uint16_t buttons_bit, buttons_count;
    int      xy_relative;    /* Input item 的 RELATIVE 位：1 = 相对位移（真鼠标） */
} hid_parser_report_t;

/**
 * @brief 从描述符流中取一个 item，游标前进到下一个 item。
 * @param desc     描述符起点
 * @param desc_len 描述符长度
 * @param pos      in/out：当前读取位置（字节）
 * @param item     输出 item
 * @return 0 成功；-1 item 被截断（调用方应判定整份描述符不可信）
 */
static int hid_parser_fetch_item(const uint8_t *desc, uint32_t desc_len,
                                 uint32_t *pos, hid_parser_item_t *item)
{
    uint32_t p = *pos;
    uint8_t b;
    uint32_t i, v;

    if (p >= desc_len) return -1;   /* 调用方已保证 p < desc_len，防御性保留 */
    b = desc[p++];

    item->type = -1;
    item->tag = 0;
    item->size = 0;
    item->data = 0;

    if ((uint32_t)((b >> 4) & 0x0Fu) == HID_TAG_LONG) {
        /* long item：0xFE bDataSize bLongItemTag data[bDataSize]，整段跳过。
         * 注意 bDataSize 只描述 data 部分，不含后两个头字节
         * （drivers/hid/hid-core.c: fetch_item() 的 start += item->size）。 */
        uint32_t data_size;
        if (desc_len - p < 2) return -1;    /* 头都不全 */
        data_size = desc[p];
        p += 2;
        if (desc_len - p < data_size) return -1;
        *pos = p + data_size;
        return 0;
    }

    item->type = (int)((b >> 2) & 0x03u);
    item->tag  = (int)((b >> 4) & 0x0Fu);
    item->size = (b & 0x03u) == 0x03u ? 4u : (uint32_t)(b & 0x03u);
    if (desc_len - p < item->size) return -1;

    v = 0;
    for (i = 0; i < item->size; i++) v |= (uint32_t)desc[p + i] << (8u * i);
    item->data = v;
    *pos = p + item->size;
    return 0;
}

/** @brief 按 item 长度做有符号解释（drivers/hid/hid-core.c: item_sdata()）。 */
static int32_t hid_parser_item_sdata(const hid_parser_item_t *item)
{
    if (item->size == 1) return (int32_t)(int8_t)item->data;
    if (item->size == 2) return (int32_t)(int16_t)item->data;
    return (int32_t)item->data;     /* size 0/4：0 或 int32 回绕，与 Linux 一致 */
}

/** @brief 全局环境复位到 HID 规范默认值（Report ID 默认 0 = 无前缀）。 */
static void hid_parser_global_reset(hid_parser_global_t *g)
{
    g->usage_page = 0;
    g->logical_min = 0;
    g->logical_max = 0;
    g->physical_min = 0;
    g->physical_max = 0;
    g->report_size = 0;
    g->report_count = 0;
    g->report_id = 0;
}

/** @brief 局部环境复位（每个 main item 之后必须调用，见文件头规则 4）。 */
static void hid_parser_local_reset(hid_parser_local_t *local)
{
    local->range_count = 0;
    local->pending_min = 0;
    local->has_pending_min = 0;
    local->truncated = 0;
}

/** @brief 追加一段 usage 区间；超出上限则截断并置 truncated（不影响位偏移）。 */
static void hid_parser_local_add_range(hid_parser_local_t *local, uint32_t page,
                                       uint32_t usage_min, uint32_t usage_max)
{
    hid_parser_usage_range_t *r;

    if (usage_max < usage_min) return;              /* 非法区间：忽略 */
    if (local->range_count >= HID_PARSER_MAX_USAGE_RANGES) {
        local->truncated = 1;
        return;
    }
    r = &local->range[local->range_count++];
    r->page = (uint16_t)page;
    r->usage_min = (uint16_t)usage_min;
    r->usage_max = (uint16_t)usage_max;
}

/** @brief 某个 Report ID 的累加器；create 为真且未见过时新建，满则返回 NULL。 */
static hid_parser_report_t *hid_parser_report_get(hid_parser_report_t *acc,
                                                  int *acc_count,
                                                  uint32_t report_id, int create)
{
    int i;
    hid_parser_report_t *a;

    for (i = 0; i < *acc_count; i++) {
        if (acc[i].report_id == (uint8_t)report_id) return &acc[i];
    }
    if (!create || *acc_count >= HID_PARSER_MAX_REPORTS) return 0;

    a = &acc[(*acc_count)++];
    /* 逐字段清零：内核侧不带 libc，避免依赖 memset；字段少，成本可忽略。 */
    a->report_id = (uint8_t)report_id;
    a->bit_cursor = 0;
    a->has_x = 0; a->has_y = 0; a->has_wheel = 0;
    a->x_bit = 0; a->x_bits = 0; a->x_signed = 0;
    a->y_bit = 0; a->y_bits = 0; a->y_signed = 0;
    a->wheel_bit = 0; a->wheel_bits = 0; a->wheel_signed = 0;
    a->buttons_bit = 0; a->buttons_count = 0;
    a->xy_relative = 0;
    return a;
}

/**
 * @brief 识别一个字段值的 usage，并把它记进布局（若是 X/Y/Wheel/Button）。
 * @param a     目标 Report ID 的累加器
 * @param page  usage page
 * @param usage usage id
 * @param bit   该值在报告载荷内的起始 bit
 * @param g     当前全局环境（决定 report_size 与符号性）
 *
 * 符号性判定沿用 Linux 的做法：逻辑最小值为负 ⇒ 有符号
 * （drivers/hid/hid-input.c 用 field->logical_minimum 选 s32/u32 取值路径）。
 * 同一 Report ID 内同名字段重复出现时保留第一次出现的（避免后出现的 padding
 * 覆盖真正的 X/Y）。
 */
static void hid_parser_note_field(hid_parser_report_t *a, uint32_t page,
                                  uint32_t usage, uint32_t bit,
                                  const hid_parser_global_t *g,
                                  uint32_t flags)
{
    int is_signed = g->logical_min < 0;
    /* HID Input item 的 bit2 = RELATIVE：相对鼠标为 1，绝对设备为 0
     * （drivers/hid/hid-input.c 用 HID_MAIN_ITEM_RELATIVE 走 map_rel）。 */
    if (flags & 0x04u) a->xy_relative = 1;

    if (page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
        if (usage == HID_USAGE_GD_X && !a->has_x && g->report_size > 0) {
            a->has_x = 1;
            a->x_bit = (uint16_t)bit;
            a->x_bits = (uint16_t)g->report_size;
            a->x_signed = is_signed;
        } else if (usage == HID_USAGE_GD_Y && !a->has_y && g->report_size > 0) {
            a->has_y = 1;
            a->y_bit = (uint16_t)bit;
            a->y_bits = (uint16_t)g->report_size;
            a->y_signed = is_signed;
        } else if (usage == HID_USAGE_GD_WHEEL && !a->has_wheel && g->report_size > 0) {
            /* 只认 Generic Desktop 页的 Wheel(0x38)。Consumer 页的 AC Pan
             * (0x0238) 是水平滚轮，语义不同，必须忽略——否则真机上横向
             * 滚轮会被当成纵向滚轮（drivers/hid/hid-input.c 里 REL_WHEEL 与
             * REL_HWHEEL 是分开映射的）。 */
            a->has_wheel = 1;
            a->wheel_bit = (uint16_t)bit;
            a->wheel_bits = (uint16_t)g->report_size;
            a->wheel_signed = is_signed;
        }
    } else if (page == HID_USAGE_PAGE_BUTTON && usage >= 1) {
        /* 按键是 Button 页的 1 位变量；低位在前即按键 1 在最前。 */
        if (g->report_size == 1 && a->buttons_count < HID_PARSER_MAX_BUTTONS) {
            if (a->buttons_count == 0) a->buttons_bit = (uint16_t)bit;
            a->buttons_count++;
        }
    }
}

/**
 * @brief 处理一个 Input item：按 usage × report_count 识别字段。
 *
 * 值的排布：第 i 个值占 [cursor + i*report_size, +report_size)。usage 少于
 * report_count 时用最后一个 usage 补齐（见文件头规则 3）。padding 字段
 * （无 Usage）不识别任何字段，但位数照常由调用方累加。
 */
static void hid_parser_add_input(hid_parser_report_t *a,
                                 const hid_parser_global_t *g,
                                 const hid_parser_local_t *local,
                                 uint32_t flags)
{
    uint32_t slot = 0;
    int r;

    if (g->report_size == 0 || g->report_count == 0) return;

    for (r = 0; r < local->range_count && slot < g->report_count; r++) {
        const hid_parser_usage_range_t *rg = &local->range[r];
        uint32_t id;
        for (id = rg->usage_min; id <= (uint32_t)rg->usage_max && slot < g->report_count;
             id++, slot++) {
            hid_parser_note_field(a, rg->page, id,
                                  a->bit_cursor + slot * g->report_size, g,
                                  flags);
        }
    }

    if (slot < g->report_count && local->range_count > 0) {
        const hid_parser_usage_range_t *rg = &local->range[local->range_count - 1];
        for (; slot < g->report_count; slot++) {
            hid_parser_note_field(a, rg->page, rg->usage_max,
                                  a->bit_cursor + slot * g->report_size, g,
                                  flags);
        }
    }
}

/**
 * @brief 解析报告描述符，抽出所有含 X/Y 的报告布局。
 * @return 写入 out 的布局数量；0 = 不是鼠标 / 描述符被截断或不可信
 *
 * 失败策略与 Linux 的 hid_open_report() 一致：item 截断、Report ID 非法、
 * Push/Pop 失配、字段宽度越界都判定整份描述符不可信并返回 0，避免用"半个
 * 描述符"算出的偏移去解析报告（那正是错位跳动的来源）。
 */
int hid_parse_mouse_layouts(const uint8_t *desc, uint32_t desc_len,
                            hid_mouse_layout_t *out, int max_layouts)
{
    hid_parser_report_t acc[HID_PARSER_MAX_REPORTS];
    hid_parser_global_t g;
    hid_parser_global_t stack[HID_PARSER_GLOBAL_STACK];
    hid_parser_local_t local;
    int acc_count = 0;
    int stack_top = 0;
    int n = 0;
    int i;
    uint32_t pos = 0;

    if (!desc || !out || max_layouts <= 0) return 0;

    hid_parser_global_reset(&g);
    hid_parser_local_reset(&local);

    while (pos < desc_len) {
        hid_parser_item_t item;

        if (hid_parser_fetch_item(desc, desc_len, &pos, &item) < 0) return 0;
        if (item.type < 0) continue;            /* long item：跳过 */

        if (item.type == HID_ITEM_TYPE_GLOBAL) {
            switch ((uint32_t)item.tag) {
            case HID_GLOBAL_TAG_USAGE_PAGE:
                g.usage_page = item.data;
                break;
            case HID_GLOBAL_TAG_LOGICAL_MIN:
                g.logical_min = hid_parser_item_sdata(&item);
                break;
            case HID_GLOBAL_TAG_LOGICAL_MAX:
                /* 逻辑最小值为负 ⇒ 逻辑最大值也按有符号读，否则 0x7F 会被
                 * 当成 127 而 0xFF 被当成 255（hid-core.c: hid_parser_global()）。 */
                g.logical_max = (g.logical_min < 0) ? hid_parser_item_sdata(&item)
                                                    : (int32_t)item.data;
                break;
            case HID_GLOBAL_TAG_PHYSICAL_MIN:
                g.physical_min = hid_parser_item_sdata(&item);
                break;
            case HID_GLOBAL_TAG_PHYSICAL_MAX:
                g.physical_max = (g.physical_min < 0) ? hid_parser_item_sdata(&item)
                                                     : (int32_t)item.data;
                break;
            case HID_GLOBAL_TAG_REPORT_SIZE:
                if (item.data > HID_PARSER_MAX_REPORT_SIZE) return 0;
                g.report_size = item.data;
                break;
            case HID_GLOBAL_TAG_REPORT_COUNT:
                if (item.data > HID_PARSER_MAX_REPORT_BITS) return 0;
                g.report_count = item.data;
                break;
            case HID_GLOBAL_TAG_REPORT_ID:
                if (item.data == 0 || item.data > 0xFFu) return 0;
                g.report_id = item.data;
                break;
            case HID_GLOBAL_TAG_PUSH:
                if (stack_top >= HID_PARSER_GLOBAL_STACK) return 0;
                stack[stack_top++] = g;
                break;
            case HID_GLOBAL_TAG_POP:
                if (stack_top <= 0) return 0;
                g = stack[--stack_top];         /* 结构体整体赋值即完整恢复 */
                break;
            default:
                break;                          /* Unit / Unit Exponent 等：解析层不关心 */
            }
            continue;
        }

        if (item.type == HID_ITEM_TYPE_LOCAL) {
            switch ((uint32_t)item.tag) {
            case HID_LOCAL_TAG_USAGE:
                if (item.size >= 3) {
                    /* 4 字节 Usage 是"扩展 usage"：高 16 位自带 page，不与当前
                     * Usage Page 拼接（Linux: usage_size > 2 时跳过
                     * complete_usage()）。 */
                    hid_parser_local_add_range(&local, item.data >> 16,
                                               item.data & 0xFFFFu, item.data & 0xFFFFu);
                } else {
                    hid_parser_local_add_range(&local, g.usage_page,
                                               item.data & 0xFFFFu, item.data & 0xFFFFu);
                }
                break;
            case HID_LOCAL_TAG_USAGE_MIN:
                local.pending_min = item.data & 0xFFFFu;
                local.has_pending_min = 1;
                break;
            case HID_LOCAL_TAG_USAGE_MAX:
                if (local.has_pending_min) {
                    hid_parser_local_add_range(&local, g.usage_page,
                                               local.pending_min, item.data & 0xFFFFu);
                    local.has_pending_min = 0;
                }
                break;
            default:
                break;                          /* Designator / String 等：忽略 */
            }
            continue;
        }

        if (item.type == HID_ITEM_TYPE_MAIN) {
            if ((uint32_t)item.tag == HID_MAIN_TAG_INPUT) {
                uint32_t bits = g.report_size * g.report_count;
                hid_parser_report_t *a = hid_parser_report_get(acc, &acc_count,
                                                               g.report_id, 1);
                if (a) {
                    if (bits > HID_PARSER_MAX_REPORT_BITS ||
                        a->bit_cursor + bits > HID_PARSER_MAX_REPORT_BITS) {
                        return 0;               /* 位偏移溢出 uint16 布局字段 */
                    }
                    hid_parser_add_input(a, &g, &local, (uint32_t)item.data);
                    a->bit_cursor += bits;
                }
                /* 累加器已满（Report ID 超上限）：该报告的字段被丢弃，但它的
                 * 位流与其它 Report ID 无关，不会污染已有布局。 */
            }
            /* Output(0x91)/Feature(0xB1) 不进入 Input 位流，不做累加；
             * Collection/End Collection 只影响 collection 栈（本解析器不需要
             * application 归属即可判定鼠标，故不跟踪）。 */
            hid_parser_local_reset(&local);     /* 文件头规则 4 */
        }
        /* type == 3（reserved）忽略 */
    }

    for (i = 0; i < acc_count && n < max_layouts; i++) {
        hid_mouse_layout_t *L;
        if (!(acc[i].has_x && acc[i].has_y)) continue;  /* 判定鼠标：X + Y */
        L = &out[n++];
        L->report_id = acc[i].report_id;
        L->report_len_bits = (uint16_t)acc[i].bit_cursor;
        L->has_x = acc[i].has_x;
        L->has_y = acc[i].has_y;
        L->has_wheel = acc[i].has_wheel;
        L->x_bit = acc[i].x_bit;       L->x_bits = acc[i].x_bits;
        L->x_signed = acc[i].x_signed;
        L->y_bit = acc[i].y_bit;       L->y_bits = acc[i].y_bits;
        L->y_signed = acc[i].y_signed;
        L->wheel_bit = acc[i].wheel_bit; L->wheel_bits = acc[i].wheel_bits;
        L->wheel_signed = acc[i].wheel_signed;
        L->buttons_bit = acc[i].buttons_bit;
        L->buttons_count = acc[i].buttons_count;
        L->xy_relative = acc[i].xy_relative;
    }
    return n;
}

/**
 * @brief 校验一个字段是否完整落在可用位流内。
 * @param bit          字段起始 bit（相对报告载荷）
 * @param bits         字段宽度（bit）
 * @param declared_bits 布局声明的报告位数；0 表示调用方未声明，跳过该检查
 * @param avail_bits   缓冲实际可提供的位数（含 Report ID 前缀扣除）
 * @return 1 可安全取位；0 越界或宽度非法
 */
static int hid_parser_field_fits(uint32_t bit, uint32_t bits,
                                 uint32_t declared_bits, uint64_t avail_bits)
{
    if (bits == 0 || bits > 32) return 0;
    if ((uint64_t)bit + bits > avail_bits) return 0;
    if (declared_bits != 0 && bit + bits > declared_bits) return 0;
    return 1;
}

/** @brief 取 bit_cnt 位（LSB 在前：第 0 位是首字节 bit0）。越界前提由调用方保证。 */
static uint32_t hid_parser_extract_bits(const uint8_t *buf, uint32_t bit_off,
                                        uint32_t bit_cnt)
{
    uint32_t v = 0;
    uint32_t i;
    for (i = 0; i < bit_cnt; i++) {
        uint32_t b = bit_off + i;
        if (buf[b >> 3] & (uint8_t)(1u << (b & 7u))) v |= (uint32_t)1u << i;
    }
    return v;
}

/** @brief 按位宽做二补数符号扩展（bits >= 32 时原样返回）。 */
static int hid_parser_sign_extend(uint32_t raw, uint32_t bits)
{
    uint32_t sign;
    if (bits == 0 || bits >= 32) return (int)raw;
    sign = (uint32_t)1u << (bits - 1);
    if (raw & sign) return (int)(raw | ~((sign << 1) - 1u));
    return (int)raw;
}

int hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *report,
                     uint32_t report_len, int *dx, int *dy, int *wheel,
                     uint32_t *buttons)
{
    uint64_t avail_bits;
    uint32_t bit_base = 0;
    uint32_t declared_bits;
    uint32_t raw;
    int out_dx = 0, out_dy = 0, out_wheel = 0;
    uint32_t out_buttons = 0;

    if (!layout || !report) return -1;

    if (layout->report_id != 0) {
        /* 有 Report ID 的设备每份报告首字节是 ID，字段位偏移从 ID 之后算。 */
        if (report_len < 1 || report[0] != layout->report_id) return -1;
        bit_base = 8;
    }

    avail_bits = (uint64_t)report_len * 8u;
    if (avail_bits < bit_base) return -1;
    avail_bits -= bit_base;

    declared_bits = layout->report_len_bits;
    /* 报告长度是设备按 Report ID 固定死的：缓冲比声明短说明这份报告不完整
     * （典型场景：设备其实还在 boot protocol 下，发的是固定 3/4 字节的 boot
     * 报告，用 report protocol 的偏移去解必然错位）。此时宁可让调用方丢掉这
     * 一帧，也不能拿"半个报告"当真。 */
    if (declared_bits != 0 && declared_bits > avail_bits) return -1;

    if (layout->has_x) {
        if (!hid_parser_field_fits(layout->x_bit, layout->x_bits, declared_bits, avail_bits))
            return -1;
        raw = hid_parser_extract_bits(report, bit_base + layout->x_bit, layout->x_bits);
        out_dx = layout->x_signed ? hid_parser_sign_extend(raw, layout->x_bits) : (int)raw;
    }
    if (layout->has_y) {
        if (!hid_parser_field_fits(layout->y_bit, layout->y_bits, declared_bits, avail_bits))
            return -1;
        raw = hid_parser_extract_bits(report, bit_base + layout->y_bit, layout->y_bits);
        out_dy = layout->y_signed ? hid_parser_sign_extend(raw, layout->y_bits) : (int)raw;
    }
    if (layout->has_wheel) {
        if (!hid_parser_field_fits(layout->wheel_bit, layout->wheel_bits, declared_bits,
                                   avail_bits))
            return -1;
        raw = hid_parser_extract_bits(report, bit_base + layout->wheel_bit,
                                      layout->wheel_bits);
        out_wheel = layout->wheel_signed ? hid_parser_sign_extend(raw, layout->wheel_bits)
                                         : (int)raw;
    }
    if (layout->buttons_count > 0) {
        uint32_t n = layout->buttons_count > HID_PARSER_MAX_BUTTONS
                   ? HID_PARSER_MAX_BUTTONS : layout->buttons_count;
        if (!hid_parser_field_fits(layout->buttons_bit, n, declared_bits, avail_bits))
            return -1;
        out_buttons = hid_parser_extract_bits(report, bit_base + layout->buttons_bit, n);
    }

    /* 全部校验通过后才写输出，失败路径不留半个结果给调用方。 */
    if (dx) *dx = out_dx;
    if (dy) *dy = out_dy;
    if (wheel) *wheel = out_wheel;
    if (buttons) *buttons = out_buttons;
    return 0;
}
