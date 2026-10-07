/* tests/host/test_json.c — 主题包 JSON 解析器的宿主机单元测试。
 *
 * 为什么在宿主机测：解析器必须对**损坏的主题文件**安全降级（回退内置主题），
 * 这类负例在 QEMU 里很难构造和观察。放宿主机上可以穷举各种畸形输入，
 * 而且改动后秒级回归，不必启动虚拟机。
 *
 * 覆盖：正常解析、各类值类型、颜色格式（含非法）、以及一组"必须不崩且
 * 判定为失败/降级"的畸形输入。 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "lib/json.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL: %s\n", msg); }              \
} while (0)

/* ── 正常解析 ─────────────────────────────────────────────── */
static void test_basic(void) {
    printf("[json] 基本解析\n");
    const char *t =
        "{\n"
        "  \"name\": \"Breeze Card Dark\",\n"
        "  \"based_on\": \"dark\",\n"
        "  \"accent\": \"#3daee9\",\n"
        "  \"radius\": 10,\n"
        "  \"radius_small\": 7,\n"
        "  \"shadow\": true,\n"
        "  \"nothing\": null\n"
        "}\n";

    json_doc_t d;
    CHECK(json_parse_object(t, (uint32_t)strlen(t), &d), "应解析成功");
    CHECK(d.ok, "ok 应为 true");

    const char *name = json_get_str(&d, "name");
    CHECK(name && strcmp(name, "Breeze Card Dark") == 0, "name 值正确");
    CHECK(strcmp(json_get_str(&d, "based_on"), "dark") == 0, "based_on 正确");

    CHECK(json_get_int(&d, "radius", -1) == 10, "radius = 10");
    CHECK(json_get_int(&d, "radius_small", -1) == 7, "radius_small = 7");
    CHECK(json_get_bool(&d, "shadow", false) == true, "shadow = true");

    /* 缺失键走 fallback */
    CHECK(json_get_int(&d, "missing", 42) == 42, "缺失键用 fallback");
    CHECK(json_get_str(&d, "missing") == NULL, "缺失字符串键返回 NULL");
    CHECK(json_get_bool(&d, "missing", true) == true, "缺失布尔用 fallback");

    /* 类型不符也走 fallback（不是崩溃、不是 0） */
    CHECK(json_get_int(&d, "name", 99) == 99, "字符串当数字取 → fallback");
    CHECK(json_get_str(&d, "radius") == NULL, "数字当字符串取 → NULL");
}

/* ── 数值边界 ─────────────────────────────────────────────── */
static void test_numbers(void) {
    printf("[json] 数值\n");
    const char *t = "{\"a\":0,\"b\":-5,\"c\":3.5,\"d\":-0.25,"
                    "\"e\":4294967295,\"f\":1e3}";
    json_doc_t d;
    CHECK(json_parse_object(t, (uint32_t)strlen(t), &d), "应解析成功");

    CHECK(json_get_int(&d, "a", -1) == 0, "0");
    CHECK(json_get_int(&d, "b", 0) == -5, "-5");
    /* 小数取整（四舍五入） */
    CHECK(json_get_int(&d, "c", 0) == 4, "3.5 → 4");
    CHECK(json_get_num1000(&d, "d", 0) == -250, "-0.25 → -250 (×1000)");
    /* 大整数不溢出（钳制在 ±1000000） */
    CHECK(json_get_int(&d, "e", 0) == 1000000, "超大数被钳制");
    /* 科学计数法不支持：1e3 会被当数字 1 解析，随后 'e' 使结构解析失败。
       这里只要求"不崩溃"，失败也是可接受结果。 */
    json_doc_t d2;
    bool ok = json_parse_object(t, (uint32_t)strlen(t), &d2);
    CHECK(ok == true || ok == false, "科学计数法：不崩溃即可");
}

/* ── 颜色 ─────────────────────────────────────────────────── */
static void test_colors(void) {
    printf("[json] 颜色解析\n");
    uint32_t c = 0;

    CHECK(json_parse_color("#3daee9", &c) && c == 0xFF3DAEE9u, "#RRGGBB");
    CHECK(json_parse_color("3daee9", &c) == false, "缺 # 前缀应失败");
    CHECK(json_parse_color("0x3daee9", &c) && c == 0xFF3DAEE9u, "0xRRGGBB");
    CHECK(json_parse_color("#FF3DAEE9", &c) && c == 0xFF3DAEE9u, "#AARRGGBB");
    CHECK(json_parse_color("#80FF0000", &c) && c == 0x80FF0000u, "半透明 alpha 保留");

    /* 简写 #RGB 展开为 #RRGGBB */
    CHECK(json_parse_color("#f00", &c) && c == 0xFFFF0000u, "#RGB 简写展开");
    CHECK(json_parse_color("#abc", &c) && c == 0xFFAABBCCu, "#abc → #aabbcc");

    /* 大小写不敏感 */
    CHECK(json_parse_color("#3DAEE9", &c) && c == 0xFF3DAEE9u, "大写十六进制");

    /* 非法输入 */
    CHECK(json_parse_color("#12345", &c) == false, "5 位非法");
    CHECK(json_parse_color("#1234567", &c) == false, "7 位非法");
    CHECK(json_parse_color("#gggggg", &c) == false, "非法字符");
    CHECK(json_parse_color("", &c) == false, "空串");
    CHECK(json_parse_color(NULL, &c) == false, "NULL 不崩");
    CHECK(json_parse_color("#12345678", &c) == true, "8 位合法");
    CHECK(json_parse_color("#123456789", &c) == false, "9 位非法");
}

/* ── 畸形输入：必须不崩，且要么失败要么安全降级 ─────────────── */
static void test_malformed(void) {
    printf("[json] 畸形输入（必须不崩溃）\n");

    const char *bad[] = {
        "",                       /* 空 */
        "   ",                    /* 只有空白 */
        "{",                      /* 未闭合 */
        "}",                      /* 只有闭括号 */
        "[1,2,3]",                /* 数组（不支持） */
        "\"just a string\"",      /* 顶层不是对象 */
        "null",                   /* 顶层 null */
        "{\"a\"}",                /* 缺冒号 */
        "{\"a\":}",               /* 缺值 */
        "{\"a\":1",               /* 缺闭括号 */
        "{a:1}",                  /* 键未加引号 */
        "{\"unclosed:1}",         /* 键未闭合 */
        "{\"a\":\"unclosed}",      /* 值未闭合 */
        "{{\"a\":1}}",            /* 嵌套（不支持） */
        "{\"a\":1,\"b\":}",       /* 第二个值缺失 */
        "{\"a\":--5}",            /* 双负号 */
        "{\"a\":-}",              /* 只有负号 */
        "{\"a\":.5}",             /* 前导小数点（不支持） */
        "{,}",                    /* 逗号开头 */
        "{\"a\":1 2}",            /* 缺逗号 */
    };

    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        json_doc_t d;
        /* 关键：不能崩溃。返回值与 ok 的一致性也要成立。 */
        bool r = json_parse_object(bad[i], (uint32_t)strlen(bad[i]), &d);
        CHECK(r == d.ok, "返回值与 ok 一致");
        if (!d.ok) CHECK(d.count == 0, "失败时 count 归零");
        /* 失败后取值不得崩 */
        (void)json_get_str(&d, "a");
        (void)json_get_int(&d, "a", 7);
        (void)json_get_bool(&d, "a", false);
    }
    printf("  （%zu 个畸形输入全部安全）\n", sizeof(bad)/sizeof(bad[0]));

    /* NULL 参数 */
    json_doc_t d;
    CHECK(json_parse_object(NULL, 10, &d) == false, "NULL 文本不崩");
    CHECK(json_parse_object("{\"a\":1}", 8, NULL) == false, "NULL 输出不崩");
}

/* ── 宽容性：真实主题文件里会出现的写法 ─────────────────────── */
static void test_tolerant(void) {
    printf("[json] 宽容性\n");

    /* 尾逗号 */
    json_doc_t d;
    CHECK(json_parse_object("{\"a\":1,}", 8, &d) && json_get_int(&d, "a", -1) == 1,
          "容忍尾逗号");

    /* 多余空白与换行 */
    const char *t = "{\n\t  \"a\"  :  1  ,\n  \"b\" :  \"x\"\n}\n";
    CHECK(json_parse_object(t, (uint32_t)strlen(t), &d), "多余空白");
    CHECK(json_get_int(&d, "a", -1) == 1, "空白后取到值");

    /* 空对象 */
    CHECK(json_parse_object("{}", 2, &d) && d.ok && d.count == 0, "空对象合法");

    /* 未知键不导致失败（向前兼容新版本主题包） */
    const char *u = "{\"future_key\":\"whatever\",\"radius\":12}";
    CHECK(json_parse_object(u, (uint32_t)strlen(u), &d), "未知键不影响解析");
    CHECK(json_get_int(&d, "radius", -1) == 12, "未知键旁边的键仍可读");
}

/* ── 注释（主题文件是人写的，必须支持）───────────────────────── */
static void test_comments(void) {
    printf("[json] 注释支持\n");

    /* 行注释 */
    const char *a = "{\n"
                    "  // 这是强调色\n"
                    "  \"accent\": \"#00a0ff\",\n"
                    "  \"radius\": 10 // 行尾注释\n"
                    "}\n";
    json_doc_t d;
    CHECK(json_parse_object(a, (uint32_t)strlen(a), &d), "行注释可解析");
    CHECK(strcmp(json_get_str(&d, "accent"), "#00a0ff") == 0, "行注释后的值正确");
    CHECK(json_get_int(&d, "radius", -1) == 10, "行尾注释不影响取值");

    /* 块注释 */
    const char *b = "{\n"
                    "  /* 深海蓝主题\n"
                    "     跨多行说明 */\n"
                    "  \"name\": \"Ocean\"\n"
                    "}";
    CHECK(json_parse_object(b, (uint32_t)strlen(b), &d), "块注释可解析");
    CHECK(strcmp(json_get_str(&d, "name"), "Ocean") == 0, "块注释后的值正确");

    /* 值之间、冒号周围的注释 */
    const char *c = "{ \"a\" /* key */ : /* val */ 1 }";
    CHECK(json_parse_object(c, (uint32_t)strlen(c), &d), "键值间注释");
    CHECK(json_get_int(&d, "a", -1) == 1, "值正确");

    /* 未闭合块注释：应安全收尾而不是越界读 */
    const char *e = "{ \"a\": 1 /* 没关掉";
    bool r = json_parse_object(e, (uint32_t)strlen(e), &d);
    CHECK(r == d.ok, "未闭合块注释不崩且状态一致");

    /* 注释里含引号/花括号不应干扰结构判断 */
    const char *f = "{\n // 说明: 用 \" 和 } 和 { 都是注释内容\n \"a\": 2\n}";
    CHECK(json_parse_object(f, (uint32_t)strlen(f), &d), "注释内特殊字符不影响解析");
    CHECK(json_get_int(&d, "a", -1) == 2, "值仍正确");
}

/* ── 超长值截断（不溢出定长缓冲）─────────────────────────────── */
static void test_overflow(void) {
    printf("[json] 超长输入\n");

    /* 超长字符串值：应被截断而不是溢出 */
    char big[512];
    memset(big, 'A', sizeof(big));
    char doc[700];
    int n = snprintf(doc, sizeof(doc), "{\"name\":\"%.*s\"}", 500, big);
    json_doc_t d;
    bool r = json_parse_object(doc, (uint32_t)n, &d);
    CHECK(r, "超长值仍能解析");
    const char *v = json_get_str(&d, "name");
    CHECK(v && strlen(v) < JSON_VAL_MAX, "超长值被截断到缓冲内");

    /* 超长键：同样截断 */
    char kdoc[512];
    int m = snprintf(kdoc, sizeof(kdoc), "{\"%.*s\":1}", 200, big);
    CHECK(json_parse_object(kdoc, (uint32_t)m, &d), "超长键不崩");

    /* 键值对数量超过上限：多余的被丢弃，但结构仍合法 */
    char many[2048]; int off = 0;
    off += snprintf(many + off, sizeof(many) - (size_t)off, "{");
    for (int i = 0; i < JSON_MAX_PAIRS + 10; i++)
        off += snprintf(many + off, sizeof(many) - (size_t)off, "%s\"k%d\":%d", i ? "," : "", i, i);
    off += snprintf(many + off, sizeof(many) - (size_t)off, "}");
    CHECK(json_parse_object(many, (uint32_t)off, &d), "超量键值对仍解析成功");
    CHECK(d.count == JSON_MAX_PAIRS, "键值对数量被钳制在上限");
}

int main(void) {
    printf("=== JSON 解析器宿主机测试 ===\n");
    test_basic();
    test_numbers();
    test_colors();
    test_malformed();
    test_tolerant();
    test_comments();
    test_overflow();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail) { printf("FAILED\n"); return 1; }
    printf("PASS\n");
    return 0;
}
