/* tests/host/test_xj380_compat.c
 *
 * 宿主侧单元测试：验证 app/include/xj380_compat.h 的类型/颜色/句柄层与
 * 「明示缺口」桩的行为。只测**不触碰 HBOS syscall** 的部分，因此可在宿主
 * Linux 上直接编译运行；syscall 路径由 QEMU 侧用例覆盖。
 *
 * 手动运行（不进入 make test，避免与主代理的构建线冲突）：
 *   gcc -O2 -Wall -Wextra -Iapp/include -Isrc/user \
 *       tests/host/test_xj380_compat.c -o build/tests/test_xj380_compat
 *   ./build/tests/test_xj380_compat
 *
 * 对标文档：docs/XJ380-API-DIFF.md §4 / §5
 */
/* 只包含 HBOS SDK 头：宿主 glibc 的 stdio/string 会与本仓库
 * src/user/libc 的同名宏冲突（BUFSIZ/FILENAME_MAX），且被测代码
 * 只应依赖 HBOS 侧声明的原型。链接期由宿主 libc 提供实现。 */
#include <xj380_compat.h>

/* 宿主桩：HBOS 的 syscall 入口在 libc 编译单元里，宿主链接不到。
 * 全部返回 -1（失败），使所有「窗口/绘制/时间」路径变成可测的空操作，
 * 而不是链接失败。真实的 syscall 行为由 QEMU 侧用例覆盖。 */
long __syscall1(long nr, long a0) { (void)nr; (void)a0; return -1; }
long __syscall3(long nr, long a0, long a1, long a2) {
    (void)nr; (void)a0; (void)a1; (void)a2;
    return -1;
}
long __syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    (void)nr; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -1;
}

static int g_fail;
static int g_pass;

#define CHECK(cond, what)                                                  \
    do {                                                                   \
        if (cond) { g_pass++; }                                            \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, what); } \
    } while (0)

/* L0：XJ380 1.3 的类型宽度（p15–p17） */
static void test_type_widths(void) {
    CHECK(sizeof(INT8) == 1, "INT8 = 1B");
    CHECK(sizeof(UINT8) == 1, "UINT8 = 1B");
    CHECK(sizeof(INT16) == 2, "INT16 = 2B");
    CHECK(sizeof(UINT16) == 2, "UINT16 = 2B");
    CHECK(sizeof(INT32) == 4, "INT32 = 4B");
    CHECK(sizeof(UINT32) == 4, "UINT32 = 4B");
    CHECK(sizeof(INT64) == 8, "INT64 = 8B");
    CHECK(sizeof(UINT64) == 8, "UINT64 = 8B");
    CHECK(sizeof(WSTR) == sizeof(void *), "WSTR = pointer");
    CHECK(sizeof(HDLE) == 8, "HDLE = 8B (opaque handle)");
}

/* 句柄装载：HBOS 的负值窗口必须映射成 XAPI_HDLE_INVALID，不能变成大正数 */
static void test_hdle_pack(void) {
    CHECK(xj380_hdle_from_win((hax_window_t)7) == (HDLE)7u, "valid win -> hdle");
    CHECK(xj380_hdle_from_win((hax_window_t)-1) == XAPI_HDLE_INVALID, "invalid win -> 0");
    CHECK(xj380_win_from_hdle((HDLE)7u) == (hax_window_t)7, "hdle -> win");
    CHECK(xj380_win_from_hdle(XAPI_HDLE_INVALID) < 0, "0 -> invalid win");
}

/* 3.3 类型转换（p28–p29） */
static void test_conversions(void) {
    CHECK(xcr_char2int("1849") == 1849, "xcr_char2int");
    CHECK(xcr_char2int(NULL) == 0, "xcr_char2int(NULL)");

    char buf[24];
    int n = xcr_int2char(-42, buf, (int)sizeof(buf));
    CHECK(n == 3 && strcmp(buf, "-42") == 0, "xcr_int2char");

    n = xcr_hex2char(0xBEEFu, buf, (int)sizeof(buf));
    CHECK(n == 4 && strcmp(buf, "beef") == 0, "xcr_hex2char");

    /* 截断：不得溢出，返回值不超过 cap-1 */
    n = xcr_int2char(123456789L, buf, 4);
    CHECK(n <= 3 && (int)strlen(buf) <= 3, "xcr_int2char truncates safely");
}

/* 4.3.1 / 4.5.1：双轨颜色 → HCOLOR（HBOS 单一表示） */
static void test_colors(void) {
    XCOLOR c = toRGB(0x14A6E0u);
    CHECK(c.Red == 0x14 && c.Green == 0xA6 && c.Blue == 0xE0, "toRGB");
    CHECK(xj380_hcolor(c) == 0x14A6E0u, "XCOLOR -> HCOLOR");

    XCOLORA a = toRGBA(0x14A6E0u, 0x80);
    CHECK(a.Alpha == 0x80, "toRGBA alpha");
    CHECK(xj380_hcolor_a(a) == 0x8014A6E0u, "XCOLORA -> 0xAARRGGBB");
}

/* XJ380 公开结构的字段与手册一致（p65 / p23 / p24） */
static void test_struct_layout(void) {
    XWINDOW w;
    memset(&w, 0, sizeof(w));
    w.width = 640; w.height = 480; w.title = (WSTR)"t"; w.sets = XWIN_SUPPORT_RESIZEABLE;
    CHECK(w.width == 640 && w.height == 480, "XWINDOW fields");
    CHECK((w.sets & XWIN_TYPE_MASK) == XWIN_NORMAL, "XWIN_TYPE_MASK extracts type");
    CHECK((w.sets & 0x80) != 0, "resizable bit");

    CHECK(sizeof(((XFILE *)0)->length) == 8, "XFILE.length is UINT64");
    CHECK(sizeof(((DirNode *)0)->filename) == 256, "DirNode.filename[256]");
}

/* §5：明示缺口必须返回 -ENOSYS（38），而不是静默成功 */
static void test_explicit_gaps(void) {
    char path[64];
    CHECK(xapi_FileDialog(0, (WSTR)"t", (WSTR)"/", path, sizeof(path)) == XAPI_ENOSYS,
          "FileDialog -> ENOSYS");
    CHECK(path[0] == '\0', "FileDialog clears out_path");

    CHECK(xapi_ClipboardSetText("x", 1) == -(INT64)XAPI_ENOSYS, "ClipboardSetText -> ENOSYS");
    CHECK(xapi_ClipboardQuery(NULL) == -(INT64)XAPI_ENOSYS, "ClipboardQuery -> ENOSYS");
    CHECK(xapi_FinishPaste(0) == -(INT64)XAPI_ENOSYS, "FinishPaste -> ENOSYS");
    CHECK(xapi_SetSecret() == -(INT64)XAPI_ENOSYS, "SetSecret -> ENOSYS");
    CHECK(xapi_DrawSvg(0, 0, 0, 0, (WSTR)"<svg/>", false) == -(INT32)XAPI_ENOSYS,
          "DrawSvg -> ENOSYS");
    CHECK(xapi_DrawFA(0, 0, 0, 0, (char *)"star", false) == -(INT32)XAPI_ENOSYS,
          "DrawFA -> ENOSYS");

    UINT32 w = 9, h = 9;
    CHECK(xapi_CaptureScreen(NULL, 0, &w, &h) == XAPI_ENOSYS, "CaptureScreen -> ENOSYS");
    CHECK(w == 0 && h == 0, "CaptureScreen zeroes dimensions");

    CHECK(xapi_GetTaskList(NULL, 0) == 0, "GetTaskList -> 0 (cannot enumerate)");
    CHECK(xapi_GetMemorySize() == 0, "GetMemorySize -> 0 (unknown)");

    XapiSystemStats st;
    CHECK(xapi_GetSystemStats(&st) == 0 && st.cpu_count == 0, "GetSystemStats -> zeroed");

    UINT32 width = 77;
    xapi_DrawTextl((HDLE)0, 0, 0, (WSTR)"text", 0, 0, &width);
    CHECK(width == 0, "DrawTextl reports width 0 (no metrics)");
}

/* 已映射项的最小语义（不触发 syscall 的分支） */
static void test_mapped_pure(void) {
    UserInfo u;
    memset(&u, 0xFF, sizeof(u));
    xapi_GetCurrentUser(&u);
    CHECK(strcmp(u.name, "user") == 0, "GetCurrentUser placeholder");
    CHECK(u.user_type == 0, "GetCurrentUser zeroes user_type");

    TimeType tm;
    memset(&tm, 0xFF, sizeof(tm));
    xapi_GetTimeX(&tm);
    CHECK(tm.tm_mon >= 0 && tm.tm_mon <= 11, "GetTimeX month in range");

    char v[128];
    v[0] = '\0';
    xapi_GetCpuModel(v);
    CHECK(strstr(v, "x86_64") != NULL, "GetCpuModel returns x86_64");
}

int main(void) {
    test_type_widths();
    test_hdle_pack();
    test_conversions();
    test_colors();
    test_struct_layout();
    test_explicit_gaps();
    test_mapped_pure();

    printf("xj380_compat: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
