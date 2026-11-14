/* tests/host/test_fd_accmode.c — fd 访问模式记账规则的宿主单测。
 *
 * 被测对象是 src/fd.h 里的**纯逻辑**（fd_accmode_of / fd_getfl_flags /
 * fd_setfl_apply / fd_accmode_normalize）。用 -DHBOS_FD_FLAGS_ONLY 包含
 * 头文件可以跳过 vfs.h 依赖，所以这份测试和内核用的是**同一份实现**，
 * 不是"照抄一遍再断言照抄"。
 *
 * 为什么值得单测：Chromium 的 base::File 在 open / 接管 fd 之后做
 *   fcntl(fd, F_GETFL) & O_ACCMODE  ==  它请求的访问模式
 * 不相等就 IMMEDIATE_CRASH（int3;ud2）。历史事故都出在"记账"这一层：
 *   1) 覆盖层一律记 O_RDWR  → O_RDONLY 重开 shm 时判失败（commit b0c412a）
 *   2) F_SETFL 整个替换 flags → 访问模式被抹成 0，后续校验对不上
 *   3) F_GETFL 透出 O_CREAT/O_EXCL/O_TRUNC → 调用方看到 Linux 上看不到的位
 * 三条都可以用本文件里的断言在宿主上钉死，不需要 QEMU。
 *
 * 编译 / 运行（Makefile 的 `make test` 还没接这一条，需 Lead 审过后加
 * 目标；在那之前直接跑这两行）：
 *   gcc -O2 -Wall -Wextra -Isrc -DHBOS_FD_FLAGS_ONLY \
 *       tests/host/test_fd_accmode.c -o build/test_fd_accmode
 *   ./build/test_fd_accmode
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define HBOS_FD_FLAGS_ONLY 1
#include "fd.h"

static int failures;

static void check(int condition, const char *what, int got, int want) {
    if (condition) return;
    printf("FAIL %-52s got=0x%x want=0x%x\n", what, (unsigned)got,
           (unsigned)want);
    failures++;
}

/* Linux x86-64 open(2) flags，数值取自 src/fcntl.h。 */
#define T_O_RDONLY   0x0000
#define T_O_WRONLY   0x0001
#define T_O_RDWR     0x0002
#define T_O_CREAT    0x0040
#define T_O_EXCL     0x0080
#define T_O_TRUNC    0x0200
#define T_O_APPEND   0x0400
#define T_O_NONBLOCK 0x0800
#define T_O_CLOEXEC  0x80000

int main(void) {
    /* ── 1. F_GETFL 只吐状态位 ─────────────────────────────────── */
    const int create_flags = T_O_CREAT | T_O_EXCL | T_O_TRUNC | T_O_CLOEXEC;
    check((fd_getfl_flags(T_O_RDWR | create_flags) & create_flags) == 0,
          "F_GETFL 不得返回 O_CREAT/O_EXCL/O_TRUNC/O_CLOEXEC",
          fd_getfl_flags(T_O_RDWR | create_flags), T_O_RDWR);
    check(fd_getfl_flags(T_O_RDONLY | T_O_APPEND | T_O_NONBLOCK) ==
              (T_O_APPEND | T_O_NONBLOCK),
          "F_GETFL 必须保留 O_APPEND/O_NONBLOCK",
          fd_getfl_flags(T_O_RDONLY | T_O_APPEND | T_O_NONBLOCK),
          T_O_APPEND | T_O_NONBLOCK);

    /* ── 2. 核心不变式：accmode 与 open 请求的一致 ─────────────── */
    /* 这些 flags 全部取自真实日志（build/tmp/r85.log / base-r1.log）：
     *   dlopen 探测    0x80000          (O_RDONLY|O_CLOEXEC)
     *   shm 创建       0xc2             (O_RDWR|O_CREAT|O_EXCL)
     *   shm 只读重开   0x0
     *   crashpad 写    0x801c2          (O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC)
     *   metrics 追加   0x842            (O_RDWR|O_CREAT|O_APPEND)   */
    static const struct { int flags; int accmode; const char *what; } cases[] = {
        { T_O_RDONLY | T_O_CLOEXEC, T_O_RDONLY, "dlopen 探测" },
        { T_O_RDWR | T_O_CREAT | T_O_EXCL, T_O_RDWR, "shm 创建" },
        { T_O_RDONLY, T_O_RDONLY, "shm 只读重开" },
        { T_O_RDWR | T_O_CREAT | T_O_EXCL | T_O_CLOEXEC, T_O_RDWR,
          "crashpad settings.dat" },
        { T_O_RDWR | T_O_CREAT | T_O_APPEND, T_O_RDWR, "BrowserMetrics 追加" },
        { T_O_WRONLY | T_O_CREAT | T_O_TRUNC, T_O_WRONLY, "creat()" },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int reported = fd_getfl_flags(cases[i].flags) & FD_ACC_MASK;
        check(reported == cases[i].accmode, cases[i].what, reported,
              cases[i].accmode);
    }

    /* ── 3. F_SETFL 不许动访问模式 ─────────────────────────────── */
    int after_setfl = fd_setfl_apply(T_O_RDWR | T_O_CLOEXEC, T_O_NONBLOCK);
    check(fd_accmode_of(after_setfl) == T_O_RDWR,
          "F_SETFL(O_NONBLOCK) 必须保留 O_RDWR", fd_accmode_of(after_setfl),
          T_O_RDWR);
    check((after_setfl & T_O_NONBLOCK) != 0, "F_SETFL 必须设置 O_NONBLOCK",
          after_setfl, T_O_NONBLOCK);
    check((after_setfl & T_O_CLOEXEC) != 0,
          "F_SETFL 不得清掉 O_CLOEXEC 记账", after_setfl, T_O_CLOEXEC);
    /* 清 O_APPEND 也走同一条规则，且仍不动访问模式。 */
    int cleared = fd_setfl_apply(T_O_RDWR | T_O_APPEND, 0);
    check(cleared == T_O_RDWR, "F_SETFL(0) 只清 APPEND/NONBLOCK", cleared,
          T_O_RDWR);

    /* ── 4. O_ACCMODE==3 的兜底 ───────────────────────────────── */
    check(fd_accmode_normalize(3) == T_O_RDWR, "accmode 3 兜底为 O_RDWR",
          fd_accmode_normalize(3), T_O_RDWR);
    check(fd_accmode_normalize(T_O_RDONLY | T_O_APPEND | 3) == T_O_RDWR,
          "accmode 3 + 其它位仍兜底为 O_RDWR",
          fd_accmode_normalize(T_O_RDONLY | T_O_APPEND | 3), T_O_RDWR);
    for (int mode = 0; mode < 3; mode++)
        check(fd_accmode_normalize(mode) == mode, "accmode 0/1/2 原样保留",
              fd_accmode_normalize(mode), mode);

    /* ── 5. dup 语义：复制条目必须原样带走访问模式 ─────────────── */
    /* 渲染进程崩溃时 fd 7 是被继承/dup 出来的 /dev/null（见 r85 的
     * [FD-TABLE]）。dup 只复制条目，不重算模式 —— 这条断言把"dup 不许
     * 重新解释 flags"钉住。 */
    for (int mode = 0; mode < 3; mode++) {
        int source = mode | T_O_CLOEXEC;
        int duplicated = source;
        check(fd_accmode_of(duplicated) == mode,
              "dup 后 accmode 必须与源 fd 一致", fd_accmode_of(duplicated),
              mode);
    }

    if (failures) {
        printf("FAILED: %d assertion(s)\n", failures);
        return 1;
    }
    printf("PASS: fd access-mode accounting (%d groups)\n", 5);
    return 0;
}
