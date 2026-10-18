/* UTF-8 严格性单测：合法序列要解对，非法序列（超长/代理区/越界/截断）
 * 必须拒绝。这些负例正是各家自己写的解码器最常放过去的。 */
#include <stdio.h>
#include <string.h>
#include "lib/utf8.h"

static int failures;
#define CHECK(e) do { if (!(e)) { printf("  [FAIL] %s:%d %s\n", __FILE__, __LINE__, #e); failures++; } } while (0)

static uint32_t dec(const char *s) {
    uint32_t cp = 0;
    int n = utf8_decode(s, (uint32_t)strlen(s), &cp);
    return n ? cp : UTF8_INVALID;
}

int main(void) {
    printf("[UTF8] 合法序列\n");
    CHECK(dec("A") == 'A');
    CHECK(dec("\xC2\xA2") == 0xA2);              /* ¢ */
    CHECK(dec("\xE4\xB8\xAD") == 0x4E2D);        /* 中 */
    CHECK(dec("\xF0\x9F\x98\x80") == 0x1F600);   /* 😀 */
    CHECK(utf8_decode("A", 1, 0) == 1);
    CHECK(utf8_decode("\xE4\xB8\xAD", 3, 0) == 3);

    printf("[UTF8] 非法：超长编码\n");
    CHECK(dec("\xC0\x80") == UTF8_INVALID);          /* overlong NUL */
    CHECK(dec("\xC1\xBF") == UTF8_INVALID);          /* overlong 0x7F */
    CHECK(dec("\xE0\x80\x80") == UTF8_INVALID);      /* overlong 3B */
    CHECK(dec("\xF0\x80\x80\x80") == UTF8_INVALID);  /* overlong 4B */

    printf("[UTF8] 非法：代理区 / 越界\n");
    CHECK(dec("\xED\xA0\x80") == UTF8_INVALID);      /* U+D800 */
    CHECK(dec("\xED\xBF\xBF") == UTF8_INVALID);      /* U+DFFF */
    CHECK(dec("\xF4\x90\x80\x80") == UTF8_INVALID);  /* U+110000 */
    CHECK(dec("\xF5\x80\x80\x80") == UTF8_INVALID);  /* 首字节越界 */

    printf("[UTF8] 非法：截断 / 孤立续字节\n");
    CHECK(dec("\xE4\xB8") == UTF8_INVALID);
    CHECK(dec("\x80") == UTF8_INVALID);
    CHECK(dec("\xC2") == UTF8_INVALID);
    CHECK(!utf8_valid("ab\xE4\xB8", 4));

    printf("[UTF8] 编码往返\n");
    const uint32_t samples[] = {0x24, 0xA2, 0x4E2D, 0x20AC, 0x1F600, 0x10FFFF};
    for (unsigned i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        char buf[8] = {0};
        int n = utf8_encode(samples[i], buf);
        CHECK(n >= 1 && n <= 4);
        uint32_t back = 0;
        CHECK(utf8_decode(buf, (uint32_t)n, &back) == n);
        CHECK(back == samples[i]);
    }
    CHECK(utf8_encode(0xD800, (char[4]){0}) == 0);
    CHECK(utf8_encode(0x110000, (char[4]){0}) == 0);

    printf("[UTF8] 长度与整串校验\n");
    const char *mix = "中文abc";
    CHECK(utf8_valid(mix, (uint32_t)strlen(mix)));
    CHECK(utf8_length(mix, (uint32_t)strlen(mix)) == 5);
    CHECK(utf8_length("\x80\x80", 2) == 2);   /* 非法按替换字符计数，不崩 */
    CHECK(utf8_valid("", 0));

    if (failures) {
        printf("[UTF8] FAILED: %d\n", failures);
        return 1;
    }
    printf("[UTF8] all tests passed\n");
    return 0;
}
