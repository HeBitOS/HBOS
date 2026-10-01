/* tests/host/test_hid_parser.c — USB HID 报告描述符解析器宿主机单测。
 *
 * 为什么这么测：解析器的全部价值在于"位偏移算对"。真机上的错误表现（鼠标不
 * 动 / 乱跳）在宿主机上无法复现，但描述符 → 偏移 → 取位这条纯逻辑链条可以逐
 * 字节钉死：用写死的 uint8_t 描述符数组（不读文件、不依赖硬件/内核头文件），
 * 断言抽出的布局字段，再用手工构造的报告字节验证 decode 的位序与符号扩展。
 *
 * 覆盖：boot protocol 鼠标、带 Report ID 的复合设备、padding/16 位绝对坐标
 * 造成的非字节对齐偏移、Output/Feature 不干扰 Input 位流、Push/Pop、long
 * item 跳过、半截描述符、越界解码、报告数上限。
 *
 * 运行（仓库根目录）：
 *   gcc -O2 -Wall -Wextra -Isrc tests/host/test_hid_parser.c src/usb_hid_parser.c \
 *       -o /tmp/test_hid_parser && /tmp/test_hid_parser
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../src/usb_hid_parser.h"

static int g_failures;
static const char *g_case = "?";

#define CASE_BEGIN(name) do { g_case = (name); printf("-- case: %s\n", (name)); } while (0)

#define CHECK(cond, ...) do {                                            \
    if (!(cond)) {                                                       \
        printf("FAIL [%s] %s:%d: ", g_case, __FILE__, __LINE__);         \
        printf(__VA_ARGS__);                                             \
        printf("\n");                                                    \
        g_failures++;                                                    \
    }                                                                    \
} while (0)

#define CHECK_INT(label, got, want) \
    CHECK((int)(got) == (int)(want), "%s = %d, 期望 %d", (label), (int)(got), (int)(want))

/* ── 描述符：常见 boot protocol 鼠标（无 Report ID，按键走 Usage Min/Max）──
 * 这是 QEMU usb-mouse / 大量真机 boot 描述符的形状：3 个按键位 + 5 位填充 +
 * X/Y/Wheel 各 8 位有符号相对量。 */
static const uint8_t desc_boot_mouse[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x03,             /*     Usage Maximum (3) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x95, 0x03,             /*     Report Count (3) */
    0x75, 0x01,             /*     Report Size (1) */
    0x81, 0x02,             /*     Input (Data,Var,Abs) → bit 0..2 按键 */
    0x95, 0x01,             /*     Report Count (1) */
    0x75, 0x05,             /*     Report Size (5) */
    0x81, 0x03,             /*     Input (Cnst,Var,Abs) → bit 3..7 填充 */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x09, 0x38,             /*     Usage (Wheel) */
    0x15, 0x81,             /*     Logical Minimum (-127) */
    0x25, 0x7F,             /*     Logical Maximum (127) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x03,             /*     Report Count (3) */
    0x81, 0x06,             /*     Input (Data,Var,Rel) → X@8, Y@16, Wheel@24 */
    0xC0, 0xC0              /*   End Collection ×2 */
};

/* ── 描述符：复合设备，Report ID 1 = 键盘、Report ID 2 = 鼠标 ──
 * 鼠标报告里额外塞了 Consumer 页 AC Pan(0x0238) 横向滚轮与 4 字节扩展
 * usage，用来验证它们不会被误当成 Generic Desktop 的 Wheel(0x38)。 */
static const uint8_t desc_composite[] = {
    /* ── 键盘（Report ID 1）── */
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, 0x01,                         /* Report ID (1) */
    0x05, 0x07,                         /* Usage Page (Keyboard) */
    0x19, 0xE0, 0x29, 0xE7,             /* Usage Min/Max 0xE0..0xE7（修饰键）*/
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, /* 8 位修饰键 */
    0x75, 0x08, 0x95, 0x01, 0x81, 0x03, /* 保留字节 */
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65,
    0x15, 0x00, 0x25, 0x65,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, /* 6 个按键（Array）*/
    0xC0,
    /* ── 鼠标（Report ID 2）── */
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, 0x02,                         /* Report ID (2) */
    0x09, 0x01, 0xA1, 0x00,             /* Usage (Pointer) / Collection (Physical) */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x05, 0x81, 0x02, /* 5 个按键 → bit 0..4 */
    0x75, 0x03, 0x95, 0x01, 0x81, 0x03, /* 3 位填充 → bit 5..7 */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x03, 0x81, 0x06, /* X@8, Y@16, Wheel@24 */
    0x05, 0x0C,                         /* Usage Page (Consumer) */
    0x09, 0x38, 0x02,                   /* Usage (0x0238 = AC Pan，2 字节 usage）*/
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, /* 横向滚轮 @32，不得抢占 Wheel */
    0x0B, 0x38, 0x02, 0x01, 0x00,       /* Usage（4 字节扩展：page 0x0001, id 0x0238）*/
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, /* @40，同样不得抢占 Wheel */
    0xC0, 0xC0
};

/* ── 描述符：先 2 位填充，再 3 个按键，X/Y 落在非字节对齐的 bit 5/13 ── */
static const uint8_t desc_pad_first[] = {
    0x75, 0x02, 0x95, 0x01, 0x81, 0x03, /* 2 位填充 → bit 0..1 */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02, /* 按键 → bit 2..4 */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06  /* X@5, Y@13（都跨字节）*/
};

/* ── 描述符：16 位绝对坐标 X/Y，逻辑最小值 0 ⇒ 无符号 ── */
static const uint8_t desc_abs16[] = {
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x00,                         /* Logical Minimum (0) */
    0x26, 0xFF, 0x7F,                   /* Logical Maximum (0x7FFF，2 字节 item) */
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, /* X@0 16 位, Y@16 16 位 */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02  /* 按键 → bit 32..34 */
};

/* ── 描述符：含 Output/Feature，Input 偏移必须只按 Input 累加 ── */
static const uint8_t desc_mixed_reports[] = {
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x03, 0x81, 0x06, /* X@0, Y@8, Wheel@16（Input 共 24 位）*/
    0x75, 0x08, 0x95, 0x08, 0x91, 0x02, /* Output 64 位：不进 Input 位流 */
    0xB1, 0x02,                         /* Feature 64 位：不进 Input 位流 */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x05, 0x81, 0x02  /* 按键 → bit 24..28 */
};

/* ── 描述符：只有 Feature 里有 X/Y ⇒ 不是鼠标（必须不进 Input 位流）── */
static const uint8_t desc_feature_only[] = {
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0xB1, 0x02
};

/* ── 描述符：Push/Pop 恢复 Report Size/Count，尾部 16 位填充证明 Pop 生效 ── */
static const uint8_t desc_push_pop[] = {
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, /* X@0, Y@8 → 游标 16 */
    0xA4,                               /* Push */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x03, 0x81, 0x02, /* 按键 → bit 16..18，游标 19 */
    0xB4,                               /* Pop：恢复 size 8 / count 2 */
    0x81, 0x03                          /* 无 usage 的 16 位填充 → 游标 35 */
};

/* ── 描述符：两个鼠标报告（ID 2、ID 3），用于测 max_layouts 截断 ── */
static const uint8_t desc_two_mice[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,
    0x85, 0x02,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, /* ID 2: X@0, Y@8 */
    0x85, 0x03,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, /* ID 3: 位流独立，X@0, Y@8 */
    0xC0
};

static void dump_layout(const hid_mouse_layout_t *L)
{
    printf("  布局: id=%u len_bits=%u x@%u/%u%s y@%u/%u%s wheel@%u/%u%s buttons@%u+%u\n",
           (unsigned)L->report_id, (unsigned)L->report_len_bits,
           (unsigned)L->x_bit, (unsigned)L->x_bits, L->x_signed ? "(s)" : "(u)",
           (unsigned)L->y_bit, (unsigned)L->y_bits, L->y_signed ? "(s)" : "(u)",
           (unsigned)L->wheel_bit, (unsigned)L->wheel_bits, L->wheel_signed ? "(s)" : "(u)",
           (unsigned)L->buttons_bit, (unsigned)L->buttons_count);
}

/* ── 用例 1：boot protocol 鼠标（无 Report ID）── */
static void case_boot_mouse(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n, dx, dy, wheel;
    uint32_t buttons;
    const uint8_t report1[4] = { 0x01, 0xFE, 0x02, 0x00 };
    const uint8_t report2[4] = { 0x00, 0x7F, 0x80, 0xFF };

    CASE_BEGIN("boot 鼠标（Usage Min/Max 按键 + 8 位相对 X/Y/Wheel）");
    memset(L, 0, sizeof(L));
    n = hid_parse_mouse_layouts(desc_boot_mouse, (uint32_t)sizeof(desc_boot_mouse),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数", n, 1);
    if (n != 1) return;
    dump_layout(&L[0]);

    CHECK_INT("report_id", L[0].report_id, 0);
    CHECK_INT("report_len_bits", L[0].report_len_bits, 32);
    CHECK(L[0].has_x && L[0].has_y && L[0].has_wheel, "has_x/y/wheel 应全为真");
    CHECK_INT("x_bit", L[0].x_bit, 8);
    CHECK_INT("x_bits", L[0].x_bits, 8);
    CHECK_INT("y_bit", L[0].y_bit, 16);
    CHECK_INT("wheel_bit", L[0].wheel_bit, 24);
    CHECK_INT("x_signed", L[0].x_signed, 1);
    CHECK_INT("buttons_bit", L[0].buttons_bit, 0);
    CHECK_INT("buttons_count", L[0].buttons_count, 3);

    dx = dy = wheel = 12345;
    buttons = 0;
    CHECK_INT("decode 返回值", hid_mouse_decode(&L[0], report1, sizeof(report1),
                                                &dx, &dy, &wheel, &buttons), 0);
    printf("  {01 FE 02 00} → dx=%d dy=%d wheel=%d buttons=0x%X\n",
           dx, dy, wheel, (unsigned)buttons);
    CHECK_INT("dx", dx, -2);
    CHECK_INT("dy", dy, 2);
    CHECK_INT("wheel", wheel, 0);
    CHECK_INT("buttons", buttons, 1);

    CHECK_INT("decode(边界值) 返回值", hid_mouse_decode(&L[0], report2, sizeof(report2),
                                                         &dx, &dy, &wheel, &buttons), 0);
    printf("  {00 7F 80 FF} → dx=%d dy=%d wheel=%d buttons=0x%X\n",
           dx, dy, wheel, (unsigned)buttons);
    CHECK_INT("dx=+127", dx, 127);
    CHECK_INT("dy=-128", dy, -128);
    CHECK_INT("wheel=-1", wheel, -1);
    CHECK_INT("buttons=0", buttons, 0);
}

/* ── 用例 2：带 Report ID 的复合设备（ID 1 键盘 / ID 2 鼠标）── */
static void case_composite_report_id(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n, dx, dy, wheel;
    uint32_t buttons;
    const uint8_t ok[7]  = { 0x02, 0x1F, 0x0A, 0xFB, 0x01, 0x00, 0x00 };
    const uint8_t bad[7] = { 0x01, 0x1F, 0x0A, 0xFB, 0x01, 0x00, 0x00 };
    const uint8_t cut[5] = { 0x02, 0x1F, 0x0A, 0xFB, 0x01 };

    CASE_BEGIN("复合设备：只抽 Report ID 2 的鼠标，偏移不含 ID 前缀之外的前缀");
    memset(L, 0xFF, sizeof(L));
    n = hid_parse_mouse_layouts(desc_composite, (uint32_t)sizeof(desc_composite),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数（键盘报告不得入选）", n, 1);
    if (n != 1) return;
    dump_layout(&L[0]);

    CHECK_INT("report_id", L[0].report_id, 2);
    CHECK_INT("report_len_bits", L[0].report_len_bits, 48);
    CHECK_INT("x_bit", L[0].x_bit, 8);      /* 前 8 位 = 5 按键 + 3 填充 */
    CHECK_INT("y_bit", L[0].y_bit, 16);
    CHECK_INT("wheel_bit（Consumer 0x0238 不得抢占）", L[0].wheel_bit, 24);
    CHECK_INT("wheel_bits", L[0].wheel_bits, 8);
    CHECK_INT("buttons_bit", L[0].buttons_bit, 0);
    CHECK_INT("buttons_count", L[0].buttons_count, 5);

    dx = dy = wheel = 0;
    buttons = 0;
    CHECK_INT("decode 返回值", hid_mouse_decode(&L[0], ok, sizeof(ok),
                                                &dx, &dy, &wheel, &buttons), 0);
    printf("  {02 1F 0A FB 01 00 00} → dx=%d dy=%d wheel=%d buttons=0x%X\n",
           dx, dy, wheel, (unsigned)buttons);
    CHECK_INT("dx", dx, 10);
    CHECK_INT("dy", dy, -5);
    CHECK_INT("wheel", wheel, 1);
    CHECK_INT("buttons", buttons, 0x1F);

    dx = dy = wheel = 777;
    buttons = 777;
    CHECK_INT("Report ID 不匹配 → -1", hid_mouse_decode(&L[0], bad, sizeof(bad),
                                                         &dx, &dy, &wheel, &buttons), -1);
    CHECK_INT("长度不足 → -1", hid_mouse_decode(&L[0], cut, sizeof(cut),
                                                &dx, &dy, &wheel, &buttons), -1);
}

/* ── 用例 3a：先 2 位填充，X/Y 落在非字节对齐的 bit 5 / bit 13 ── */
static void case_pad_first(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n, dx, dy, wheel;
    uint32_t buttons;
    const uint8_t report[3] = { 0xD4, 0x7F, 0x00 };   /* 手算见下 */
    const uint8_t half[2]   = { 0xD4, 0x7F };

    CASE_BEGIN("额外字段：2 位填充在前，X@5 / Y@13 非字节对齐");
    memset(L, 0, sizeof(L));
    n = hid_parse_mouse_layouts(desc_pad_first, (uint32_t)sizeof(desc_pad_first),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数", n, 1);
    if (n != 1) return;
    dump_layout(&L[0]);

    CHECK_INT("report_len_bits", L[0].report_len_bits, 21);
    CHECK_INT("buttons_bit", L[0].buttons_bit, 2);
    CHECK_INT("buttons_count", L[0].buttons_count, 3);
    CHECK_INT("x_bit", L[0].x_bit, 5);
    CHECK_INT("x_bits", L[0].x_bits, 8);
    CHECK_INT("y_bit", L[0].y_bit, 13);
    CHECK_INT("y_bits", L[0].y_bits, 8);

    /* 期望：padding=00，buttons=101（bit2=1,bit3=0,bit4=1），X=0xFE，Y=0x03 */
    dx = dy = wheel = 0;
    buttons = 0;
    CHECK_INT("decode 返回值", hid_mouse_decode(&L[0], report, sizeof(report),
                                                &dx, &dy, &wheel, &buttons), 0);
    printf("  {D4 7F 00} → dx=%d dy=%d wheel=%d buttons=0x%X\n",
           dx, dy, wheel, (unsigned)buttons);
    CHECK_INT("dx", dx, -2);
    CHECK_INT("dy", dy, 3);
    CHECK_INT("wheel（无字段恒 0）", wheel, 0);
    CHECK_INT("buttons", buttons, 5);

    CHECK_INT("只给 2 字节（21 位放不下）→ -1",
              hid_mouse_decode(&L[0], half, sizeof(half), &dx, &dy, &wheel, &buttons), -1);
}

/* ── 用例 3b：16 位绝对坐标 X/Y（逻辑最小值 0 ⇒ 无符号）── */
static void case_abs16(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n, dx, dy, wheel;
    uint32_t buttons;
    const uint8_t report[5] = { 0x34, 0x12, 0xFE, 0xFF, 0x01 };

    CASE_BEGIN("额外字段：16 位绝对 X/Y + 其后的按键位");
    memset(L, 0, sizeof(L));
    n = hid_parse_mouse_layouts(desc_abs16, (uint32_t)sizeof(desc_abs16),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数", n, 1);
    if (n != 1) return;
    dump_layout(&L[0]);

    CHECK_INT("report_len_bits", L[0].report_len_bits, 35);
    CHECK_INT("x_bit", L[0].x_bit, 0);
    CHECK_INT("x_bits", L[0].x_bits, 16);
    CHECK_INT("x_signed（Logical Min=0 ⇒ 无符号）", L[0].x_signed, 0);
    CHECK_INT("y_bit", L[0].y_bit, 16);
    CHECK_INT("y_bits", L[0].y_bits, 16);
    CHECK_INT("buttons_bit", L[0].buttons_bit, 32);
    CHECK_INT("buttons_count", L[0].buttons_count, 3);

    dx = dy = wheel = -1;
    buttons = 0;
    CHECK_INT("decode 返回值", hid_mouse_decode(&L[0], report, sizeof(report),
                                                &dx, &dy, &wheel, &buttons), 0);
    printf("  {34 12 FE FF 01} → dx=%d dy=%d wheel=%d buttons=0x%X\n",
           dx, dy, wheel, (unsigned)buttons);
    CHECK_INT("dx=0x1234", dx, 0x1234);
    CHECK_INT("dy=0xFFFE（无符号不扩展）", dy, 0xFFFE);
    CHECK_INT("buttons", buttons, 1);
}

/* ── 用例 4：Output/Feature 不计入 Input 位流；Feature 里的 X/Y 不算鼠标 ── */
static void case_output_feature(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n, dx, dy, wheel;
    uint32_t buttons;
    const uint8_t report[4] = { 0x01, 0x02, 0x03, 0x1F };

    CASE_BEGIN("Output/Feature 不打乱 Input 偏移；只有 Feature 的 X/Y 不算鼠标");
    memset(L, 0, sizeof(L));
    n = hid_parse_mouse_layouts(desc_mixed_reports, (uint32_t)sizeof(desc_mixed_reports),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数", n, 1);
    if (n == 1) {
        dump_layout(&L[0]);
        CHECK_INT("x_bit", L[0].x_bit, 0);
        CHECK_INT("y_bit", L[0].y_bit, 8);
        CHECK_INT("wheel_bit", L[0].wheel_bit, 16);
        CHECK_INT("buttons_bit（Output 64 位不得计入）", L[0].buttons_bit, 24);
        CHECK_INT("buttons_count", L[0].buttons_count, 5);
        CHECK_INT("report_len_bits", L[0].report_len_bits, 29);

        dx = dy = wheel = 0;
        buttons = 0;
        CHECK_INT("decode 返回值", hid_mouse_decode(&L[0], report, sizeof(report),
                                                    &dx, &dy, &wheel, &buttons), 0);
        printf("  {01 02 03 1F} → dx=%d dy=%d wheel=%d buttons=0x%X\n",
               dx, dy, wheel, (unsigned)buttons);
        CHECK_INT("dx", dx, 1);
        CHECK_INT("dy", dy, 2);
        CHECK_INT("wheel", wheel, 3);
        CHECK_INT("buttons", buttons, 0x1F);
    }

    n = hid_parse_mouse_layouts(desc_feature_only, (uint32_t)sizeof(desc_feature_only),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("Feature 里的 X/Y 不产生鼠标布局", n, 0);
}

/* ── 用例 5：Push/Pop 恢复全局环境 ── */
static void case_push_pop(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n;

    CASE_BEGIN("Push/Pop：Pop 后 Report Size/Count 必须恢复到 Push 前的值");
    memset(L, 0, sizeof(L));
    n = hid_parse_mouse_layouts(desc_push_pop, (uint32_t)sizeof(desc_push_pop),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数", n, 1);
    if (n != 1) return;
    dump_layout(&L[0]);

    CHECK_INT("x_bit", L[0].x_bit, 0);
    CHECK_INT("y_bit", L[0].y_bit, 8);
    CHECK_INT("buttons_bit", L[0].buttons_bit, 16);
    CHECK_INT("buttons_count", L[0].buttons_count, 3);
    /* 16（X/Y）+ 3（按键）+ 16（Pop 恢复后的填充）= 35；若 Pop 失效则为 22 */
    CHECK_INT("report_len_bits（Pop 生效的证据）", L[0].report_len_bits, 35);
}

/* ── 用例 6：long item 跳过 + 半截/畸形描述符一律返回 0 ── */
static void case_truncated(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n;
    /* 0xFE = long item：bDataSize=4，bLongItemTag=0xAA，后跟 4 字节数据 */
    static const uint8_t long_item_head[] = { 0xFE, 0x04, 0xAA, 0x01, 0x02, 0x03, 0x04 };
    uint8_t with_long[sizeof(long_item_head) + sizeof(desc_boot_mouse)];
    static const uint8_t cut_usage[] = { 0x05 };                        /* 数据缺失 */
    static const uint8_t cut_long[]  = { 0xFE, 0x05, 0x01, 0x02 };      /* long item 截断 */
    static const uint8_t bad_push[]  = { 0xA4, 0x81, 0x00 };            /* 只 Push 不 Pop */
    static const uint8_t bad_pop[]   = { 0xB4, 0x81, 0x00 };            /* 无 Push 就 Pop */
    static const uint8_t too_wide[]  = { 0x75, 0xFF, 0x95, 0x08, 0x81, 0x02 }; /* size 255*8 > 位流上限 */
    static const uint8_t bad_id[]    = { 0x85, 0x00, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02 };

    CASE_BEGIN("long item / 半截描述符 / Push-Pop 失配 / 非法字段宽度 → 返回 0");

    memcpy(with_long, long_item_head, sizeof(long_item_head));
    memcpy(with_long + sizeof(long_item_head), desc_boot_mouse, sizeof(desc_boot_mouse));
    n = hid_parse_mouse_layouts(with_long, (uint32_t)sizeof(with_long),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("long item 被整段跳过，后续鼠标仍能解析", n, 1);
    if (n == 1) {
        CHECK_INT("  report_len_bits", L[0].report_len_bits, 32);
        CHECK_INT("  x_bit", L[0].x_bit, 8);
    }

    /* 在 Usage Minimum（0x19）处截断：item 声明 1 字节数据但流已结束 */
    CHECK_INT("半截 item（0x19 缺数据）", hid_parse_mouse_layouts(desc_boot_mouse, 13,
                                                                  L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("只 1 字节（0x05 缺数据）", hid_parse_mouse_layouts(cut_usage, sizeof(cut_usage),
                                                                 L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("long item 声明 5 字节仅 2 字节",
              hid_parse_mouse_layouts(cut_long, sizeof(cut_long), L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("空描述符", hid_parse_mouse_layouts(desc_boot_mouse, 0,
                                                  L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("desc == NULL", hid_parse_mouse_layouts(NULL, 16, L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("out == NULL", hid_parse_mouse_layouts(desc_boot_mouse,
                                                     sizeof(desc_boot_mouse), NULL, 4), 0);
    CHECK_INT("max_layouts == 0", hid_parse_mouse_layouts(desc_boot_mouse,
                                                          sizeof(desc_boot_mouse), L, 0), 0);
    CHECK_INT("Push 无 Pop（栈泄漏）", hid_parse_mouse_layouts(bad_push, sizeof(bad_push),
                                                               L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("无 Push 就 Pop", hid_parse_mouse_layouts(bad_pop, sizeof(bad_pop),
                                                        L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("Report Size 255（超 256 之前的位流上限）",
              hid_parse_mouse_layouts(too_wide, sizeof(too_wide), L, HID_MAX_MOUSE_LAYOUTS), 0);
    CHECK_INT("Report ID 0 非法", hid_parse_mouse_layouts(bad_id, sizeof(bad_id),
                                                          L, HID_MAX_MOUSE_LAYOUTS), 0);
}

/* ── 用例 7：decode 的越界与空指针防护 ── */
static void case_decode_bounds(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n, dx, dy, wheel;
    uint32_t buttons;
    const uint8_t report[4] = { 0x01, 0xFE, 0x02, 0x00 };
    const uint8_t padded[8] = { 0x01, 0xFE, 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };

    CASE_BEGIN("decode 越界/空指针：一律返回 -1 且不写半个结果");
    memset(L, 0, sizeof(L));
    n = hid_parse_mouse_layouts(desc_boot_mouse, (uint32_t)sizeof(desc_boot_mouse),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数", n, 1);
    if (n != 1) return;

    dx = dy = wheel = 555;
    buttons = 555;
    CHECK_INT("report == NULL", hid_mouse_decode(&L[0], NULL, 4, &dx, &dy, &wheel, &buttons), -1);
    CHECK_INT("layout == NULL", hid_mouse_decode(NULL, report, 4, &dx, &dy, &wheel, &buttons), -1);
    CHECK_INT("只给 3 字节（32 位不够）",
              hid_mouse_decode(&L[0], report, 3, &dx, &dy, &wheel, &buttons), -1);
    CHECK_INT("空缓冲", hid_mouse_decode(&L[0], report, 0, &dx, &dy, &wheel, &buttons), -1);
    CHECK_INT("失败后 dx 未被改写", dx, 555);
    CHECK_INT("失败后 buttons 未被改写", buttons, 555);

    CHECK_INT("NULL 输出指针可容忍",
              hid_mouse_decode(&L[0], report, sizeof(report), NULL, NULL, NULL, NULL), 0);
    CHECK_INT("恰好 4 字节成功",
              hid_mouse_decode(&L[0], report, sizeof(report), &dx, &dy, &wheel, &buttons), 0);
    /* 缓冲比报告长（USB 短包补齐 / 调用方多给空间）不该失败，尾部字节被忽略 */
    CHECK_INT("8 字节缓冲（多出的尾部忽略）",
              hid_mouse_decode(&L[0], padded, sizeof(padded), &dx, &dy, &wheel, &buttons), 0);
    CHECK_INT("  多字节缓冲下 dx 仍正确", dx, -2);
}

/* ── 用例 8：max_layouts 截断（ID 2 / ID 3 两份鼠标报告）── */
static void case_max_layouts(void)
{
    hid_mouse_layout_t L[HID_MAX_MOUSE_LAYOUTS];
    int n;

    CASE_BEGIN("max_layouts 截断：多份鼠标报告只写调用方允许的数量");
    memset(L, 0, sizeof(L));
    n = hid_parse_mouse_layouts(desc_two_mice, (uint32_t)sizeof(desc_two_mice),
                                L, HID_MAX_MOUSE_LAYOUTS);
    CHECK_INT("布局数", n, 2);
    if (n == 2) {
        CHECK_INT("out[0].report_id", L[0].report_id, 2);
        CHECK_INT("out[1].report_id", L[1].report_id, 3);
        CHECK_INT("out[0].report_len_bits（每个 ID 独立计数）", L[0].report_len_bits, 16);
        CHECK_INT("out[1].x_bit（ID 3 的位流从 0 重来）", L[1].x_bit, 0);
    }
    n = hid_parse_mouse_layouts(desc_two_mice, (uint32_t)sizeof(desc_two_mice), L, 1);
    CHECK_INT("max_layouts=1 时只返回 1", n, 1);
}

int main(void)
{
    printf("[HID-PARSER] 宿主机单测：src/usb_hid_parser.c\n");

    case_boot_mouse();
    case_composite_report_id();
    case_pad_first();
    case_abs16();
    case_output_feature();
    case_push_pop();
    case_truncated();
    case_decode_bounds();
    case_max_layouts();

    if (g_failures) {
        printf("[HID-PARSER] %d 项断言失败\n", g_failures);
        exit(1);
    }
    printf("[HID-PARSER] all tests passed\n");
    return 0;
}
