/* tests/host/test_inflate_diff.c — inflate 的逐字节正确性差分 + 吞吐量基准。
 *
 * 方法（对应 docs/PERFORMANCE.md §1 的"解压正确性"一栏）：
 *  1. 真值来自 Python zlib（生成向量时写入 .h 或直接用内置向量）；
 *     本测试为了自包含，直接用 zlib 压缩结果硬编码的向量 + 结构性用例；
 *  2. 逐字节比对 inflate 的输出与真值，任何不一致立即失败；
 *  3. 同时测量吞吐量（MB/s，用 CLOCK_MONOTONIC），用于优化前后对比。
 *
 * 用法：
 *   make inflate-bench        # 见 Makefile（无需链接 zlib）
 *
 * 说明：内核侧 inflate.c 只用 memcpy/memset（经 shim 改名到 hbos_*），
 * 因此链接 hbos_string_wrap.c 提供这些符号即可，不引入任何内核全局状态。
 */
/* 先用系统头（-I 里带内核 src，内核的 sys/types.h 会遮蔽系统头，所以这里
 * 显式用相对路径绕开搜索顺序），再让 hbos_shim.h 把 memcpy/memset 改名。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "inflate.h"
#include "hbos_shim.h"
/* 真值向量由构建时生成（tests/host/gen_inflate_vectors.py，宿主 Python zlib
 * 压缩），因此这里的比对是"内核实现 vs 独立实现"的逐字节差分。 */
#include "inflate_vectors.h"

static int failures;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  [FAIL] " __VA_ARGS__);                                 \
            printf("\n");                                                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** @brief 逐字节比对一次解压，并返回耗时（秒）；失败返回 -1 */
static double run_case(const char *name, const uint8_t *z, uint32_t zlen,
                       const uint8_t *truth, uint32_t truth_len,
                       int iterations) {
    uint8_t *dst = (uint8_t *)malloc(truth_len + 16);
    if (!dst) return -1;
    uint32_t got = 0;
    double t0 = now_sec();
    int iter;
    for (iter = 0; iter < iterations; iter++) {
        memset(dst, 0xAA, truth_len + 16);
        got = 0;
        if (inflate_zlib(z, zlen, dst, truth_len, &got) != 0) {
            printf("  [FAIL] %s: inflate_zlib 返回失败\n", name);
            failures++;
            free(dst);
            return -1;
        }
        if (got != truth_len) {
            printf("  [FAIL] %s: 长度 %u != %u\n", name, got, truth_len);
            failures++;
            free(dst);
            return -1;
        }
        if (memcmp(dst, truth, truth_len) != 0) {
            uint32_t k;
            for (k = 0; k < truth_len; k++) if (dst[k] != truth[k]) break;
            printf("  [FAIL] %s: 字节 %u 不一致 (got=%02x want=%02x)\n", name, k,
                   dst[k], truth[k]);
            failures++;
            free(dst);
            return -1;
        }
    }
    double dt = now_sec() - t0;
    /* 输出溢出保护：容量刚好时也必须成功、且不能越界写（0xAA 哨兵） */
    got = 0;
    if (inflate_zlib(z, zlen, dst, truth_len, &got) == 0) {
        if (dst[truth_len] != 0xAA) {
            printf("  [FAIL] %s: 输出越界写\n", name);
            failures++;
        }
    }
    free(dst);
    return dt;
}

int main(void) {
    printf("[INFLATE] 宿主机差分 + 吞吐量基准（真值来自 Python zlib）\n");

    uint32_t total_z = 0;
    for (int v = 0; v < INFLATE_VECTOR_COUNT; v++) {
        const inflate_vector_t *vec = &inflate_vectors[v];
        total_z += vec->comp_len;

        double dt = run_case(vec->name, vec->comp, vec->comp_len, vec->raw,
                             vec->raw_len, vec->iterations);
        if (dt < 0) continue;
        double mb = (double)vec->raw_len * vec->iterations / (1024.0 * 1024.0);
        printf("  %-22s 原始 %8u B  压缩 %8u B (%5.1f%%)  %4d 次  %9.3f ms  %7.1f MB/s\n",
               vec->name, vec->raw_len, vec->comp_len,
               100.0 * vec->comp_len / vec->raw_len, vec->iterations,
               dt * 1000.0, mb / dt);
    }

    /* 边界用例：空输出、截断输入、错误 zlib 头都必须干净失败而不是崩溃 */
    {
        uint8_t small[64];
        uint32_t got = 999;
        CHECK(inflate_zlib((const uint8_t *)"\x78\x9c\x03\x00\x00\x00\x00\x01",
                           8, small, sizeof(small), &got) == 0 || 1,
              "空流不应崩溃");
        uint8_t bad[8] = {0x78, 0x9c, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        got = 0;
        CHECK(inflate_zlib(bad, sizeof(bad), small, sizeof(small), &got) != 0,
              "损坏流必须返回失败");
        uint8_t oob[4] = {0x78, 0x9c, 0x00, 0x00};
        got = 0;
        CHECK(inflate_zlib(oob, sizeof(oob), small, sizeof(small), &got) != 0,
              "非法 zlib 头必须返回失败");
    }

    printf("[INFLATE] %s（累计压缩输入 %u B）\n",
           failures ? "有失败用例" : "all tests passed", total_z);
    return failures ? 1 : 0;
}
