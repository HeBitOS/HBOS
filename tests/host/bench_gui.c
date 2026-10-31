/* tests/host/bench_gui.c — GUI 侧宿主基准：blit 吞吐 + PNG 解码吞吐。
 *
 * 为什么需要它
 * ============
 * 原来的性能基线只有"启动时间"和"跑一个 Wayland 用例的墙钟"，两者都在
 * QEMU/TCG 里测，抖动大、且都被串口输出污染。GUI 侧"某帧花多久"这种
 * 问题根本没法回答。这里把两个纯计算的热路径搬到宿主机上直接测：
 *
 *   1. `gui_blit_argb8888`：窗口重绘主循环（src/tools/gui.c）。
 *      测它贴一整帧要多久，以及——这是重点——**每行扫 alpha 的那一遍
 *      占了多少**。做法是拿同一份字节量做一次纯 memcpy 当参照，
 *      两者之差就是 alpha 扫描的净成本。不改被测函数任何语义。
 *   2. `png_decode`：网页图片解码（src/png.c + src/inflate.c）。
 *
 * 两者都是确定性输入、无 I/O、无随机数，重复跑结果稳定；被测函数取自
 * 真实内核源码（编译进本程序），不是复制粘贴的替身。
 *
 * 编译方式见 Makefile 的 `gui-bench` 目标：gui.c 用 -ffunction-sections
 * 编译后靠 --gc-sections 只保留被测函数，其余 291 个未定义符号不进最终
 * 产物。这样既能测到真实实现，又不用给整个 GUI 写桩。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 被测函数（真实实现，来自 src/tools/gui.c） */
void gui_blit_argb8888(int x, int y, const uint8_t *argb, int img_w, int img_h,
                       int src_stride, int clip_x, int clip_y, int clip_w,
                       int clip_h);

/* gui_blit_argb8888 依赖的表面访问器。这里给一个固定尺寸的假表面：
 * 被测函数只通过这四个访问器碰表面，因此替身不改变它自己的算法。 */
#define SURF_W 1280
#define SURF_H 720
static uint32_t g_surf[SURF_W * SURF_H];

uint32_t *gd_surface_ptr(void) { return g_surf; }
uint32_t gd_surface_pitch(void) { return SURF_W; }
int gd_surface_w(void) { return SURF_W; }
int gd_surface_h(void) { return SURF_H; }
void gd_target_origin(int *ox, int *oy) { *ox = 0; *oy = 0; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int failures;
#define CHECK(cond, ...) do {                                   \
        if (!(cond)) {                                          \
            printf("  [FAIL] ");                                \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
            failures++;                                         \
        }                                                       \
    } while (0)

/* 中位数：单次计时在宿主机上会被换页/频率调节扰动（实测 memcpy 参照
 * 单次 0.21~0.36ms，差 70%）。取多次试验的中位数才能让"改动前后的差"
 * 大于噪声。奇数个样本，中位数就是正中间那个。 */
#define TRIALS 5
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median_of(double *samples, int n) {
    qsort(samples, (size_t)n, sizeof(double), cmp_double);
    return samples[n / 2];
}

/* 跑 trials 次 run_one，返回中位耗时（ms）。run_one 做一遍完整的工作量。 */
static double timed_median(int trials, void (*run_one)(int), int arg) {
    double samples[TRIALS];
    for (int i = 0; i < trials; i++) {
        double t0 = now_ms();
        run_one(arg);
        samples[i] = now_ms() - t0;
    }
    return median_of(samples, trials);
}

/* 同上，但同时给出最小值与最大值。宿主机上单帧成本受换页/频率调节影响，
 * 只报一个中位数会让人误以为精度到 0.001ms。把区间一起打出来，
 * "改动前后的差"才不会被当成噪声。 */
static double timed_range(int trials, void (*run_one)(int), int arg,
                          double *lo, double *hi) {
    double samples[TRIALS];
    for (int i = 0; i < trials; i++) {
        double t0 = now_ms();
        run_one(arg);
        samples[i] = now_ms() - t0;
    }
    *lo = samples[0];
    *hi = samples[0];
    for (int i = 1; i < trials; i++) {
        if (samples[i] < *lo) *lo = samples[i];
        if (samples[i] > *hi) *hi = samples[i];
    }
    return median_of(samples, trials);
}

/* ── 1. blit：不透明整帧 vs 含 alpha 整帧 ────────────────────────────
 *
 * 不透明帧是浏览器/终端窗口的常态：每行扫一遍 alpha 全部命中 0xFF，
 * 扫完才 memcpy —— 那一遍扫描在常态下是纯开销。
 * 含 alpha 帧走逐像素混合，代表视频/圆角窗口。
 */
static uint8_t g_opaque[SURF_W * SURF_H * 4];
static uint8_t g_alpha[SURF_W * SURF_H * 4];
static uint32_t g_ref[SURF_W * SURF_H];

/* 每种工作量做成"做 frames 帧"的函数，交给 timed_median 重复计时。 */
static void run_opaque(int frames) {
    for (int i = 0; i < frames; i++)
        gui_blit_argb8888(0, 0, g_opaque, SURF_W, SURF_H, SURF_W * 4,
                          0, 0, SURF_W, SURF_H);
}
static void run_alpha(int frames) {
    for (int i = 0; i < frames; i++)
        gui_blit_argb8888(0, 0, g_alpha, SURF_W, SURF_H, SURF_W * 4,
                          0, 0, SURF_W, SURF_H);
}
static void run_memcpy(int frames) {
    for (int i = 0; i < frames; i++) {
        memcpy(g_ref, g_opaque, sizeof(g_ref));
        __asm__ volatile("" :: "r"(g_ref) : "memory");  /* 防止被优化掉 */
    }
}

/* 「去掉每行 alpha 扫描」的对照实现：逐行 memcpy，不做 alpha 检查。
 *
 * 这是被评估的那项优化本身（见 docs/jindu.md P3 第 11 条），写成对照
 * 才能测出"省掉扫描能拿回多少"，而不是靠 blit 减 memcpy 反推——后者把
 * 宿主 memcpy 带宽的波动全算进去了（实测同一台机器上会算出负值）。
 *
 * 刻意与被测函数保持同样的循环结构和相同的 memcpy 调用，唯一差别就是
 * 少了那遍扫描，所以差值只反映扫描成本。 */
static void blit_no_scan_ref(int x, int y, const uint8_t *argbsrc,
                             int img_w, int img_h, int src_stride,
                             int clip_w, int clip_h) {
    int x0 = x, y0 = y, x1 = x + img_w, y1 = y + img_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > clip_w) x1 = clip_w;
    if (y1 > clip_h) y1 = clip_h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int py = y0; py < y1; py++) {
        int src_y = py - y;
        uint32_t *drow = g_surf + (uint32_t)py * SURF_W;
        const uint32_t *srow = (const uint32_t *)(const void *)
            (argbsrc + (size_t)src_y * (size_t)src_stride);
        memcpy(drow + x0, srow + (x0 - x), (size_t)(x1 - x0) * sizeof(uint32_t));
    }
}

static void run_no_scan(int frames) {
    for (int i = 0; i < frames; i++)
        blit_no_scan_ref(0, 0, g_opaque, SURF_W, SURF_H, SURF_W * 4,
                         SURF_W, SURF_H);
}

/* 交错 A/B：同一轮里先测 A 再测 B，逐轮取差值再求中位数。
 *
 * 为什么不分成两个时间块各测 5 次：宿主负载会漂移（实测 load average 3+），
 * 两个块落在不同的负载窗口里，差值就变成噪声。交错之后漂移对 A 和 B 的
 * 影响近似相等，差值才是"实现差异"而不是"机器状态差异"。 */
static double interleaved_delta_ms(void (*a)(int), void (*b)(int), int frames) {
    double deltas[TRIALS];
    for (int i = 0; i < TRIALS; i++) {
        double t0 = now_ms();
        a(frames);
        double ta = now_ms() - t0;
        t0 = now_ms();
        b(frames);
        double tb = now_ms() - t0;
        deltas[i] = (ta - tb) / frames;   /* 正 = A 比 B 慢 */
    }
    return median_of(deltas, TRIALS);
}

static void bench_blit(int frames) {
    for (int i = 0; i < SURF_W * SURF_H; i++) {
        g_opaque[i * 4 + 0] = (uint8_t)(i & 0xFF);
        g_opaque[i * 4 + 1] = (uint8_t)((i >> 8) & 0xFF);
        g_opaque[i * 4 + 2] = (uint8_t)((i >> 16) & 0xFF);
        g_opaque[i * 4 + 3] = 0xFF;               /* 全不透明 */
        g_alpha[i * 4 + 0] = (uint8_t)(i & 0xFF);
        g_alpha[i * 4 + 1] = (uint8_t)((i >> 8) & 0xFF);
        g_alpha[i * 4 + 2] = (uint8_t)((i >> 16) & 0xFF);
        g_alpha[i * 4 + 3] = (uint8_t)(i & 0x7F); /* 半透明 */
    }

    const double bytes = (double)SURF_W * SURF_H * 4.0;
    /* timed_range 返回的是 frames 帧的总耗时，除回去得到单帧成本。 */
    double o_lo, o_hi, a_lo, a_hi, m_lo, m_hi;
    double opaque_ms = timed_range(TRIALS, run_opaque, frames, &o_lo, &o_hi) / frames;
    double alpha_ms = timed_range(TRIALS, run_alpha, frames, &a_lo, &a_hi) / frames;
    double memcpy_ms = timed_range(TRIALS, run_memcpy, frames, &m_lo, &m_hi) / frames;

    printf("[BLIT] %dx%d，%d 帧 x %d 次取中位数（括号内为 min..max）\n",
           SURF_W, SURF_H, frames, TRIALS);
    printf("  不透明帧（每行扫 alpha 后 memcpy）  %8.3f ms/帧  %7.1f MB/s"
           "  (%.3f..%.3f)\n",
           opaque_ms, bytes / (opaque_ms * 1e-3) / 1e6,
           o_lo / frames, o_hi / frames);
    printf("  含 alpha 帧（逐像素混合）           %8.3f ms/帧  %7.1f MB/s"
           "  (%.3f..%.3f)\n",
           alpha_ms, bytes / (alpha_ms * 1e-3) / 1e6,
           a_lo / frames, a_hi / frames);
    printf("  纯 memcpy 参照                      %8.3f ms/帧  %7.1f MB/s"
           "  (%.3f..%.3f)\n",
           memcpy_ms, bytes / (memcpy_ms * 1e-3) / 1e6,
           m_lo / frames, m_hi / frames);
    printf("  → 相对纯 memcpy 的差额              %8.3f ms/帧  "
           "占不透明帧 %.1f%%\n",
           opaque_ms - memcpy_ms,
           100.0 * (opaque_ms - memcpy_ms) / opaque_ms);
    printf("  → 混合比整块拷贝慢                  %8.2fx\n",
           alpha_ms / (memcpy_ms > 0 ? memcpy_ms : 1e-9));

    /* 交错 A/B：真实 blit（含每行 alpha 扫描） vs 去掉扫描的对照实现。
     * 这一项才是"P3 第 11 条那个优化值不值得做"的直接答案。 */
    double no_scan_ms = timed_median(TRIALS, run_no_scan, frames) / frames;
    double scan_delta = interleaved_delta_ms(run_opaque, run_no_scan, frames);
    printf("  去掉扫描的对照实现（逐行 memcpy）   %8.3f ms/帧\n", no_scan_ms);
    printf("  → 每行 alpha 扫描的净成本（交错A/B）%8.3f ms/帧  "
           "占不透明帧 %.1f%%\n",
           scan_delta, 100.0 * scan_delta / opaque_ms);
    printf("  ※ 上一条用交错 A/B 求差（每轮先 A 后 B，逐轮取差再取中位数），\n"
           "    宿主负载漂移对两边影响近似相等；"
           "直接拿 blit 减 memcpy 会算出负值。\n");

    /* 正确性：贴完之后表面像素必须等于源（不透明帧）。 */
    CHECK(g_surf[0] == 0xFF000000u, "首像素 %08x 期望 ff000000", g_surf[0]);
    CHECK((g_surf[100] & 0xFF000000u) == 0xFF000000u, "像素不透明");
}

/* ── 2. PNG 解码吞吐 ────────────────────────────────────────────────
 *
 * 输入由 Python zlib 生成（见 bench_gui.sh）：zlib 是独立实现，
 * 和内核自己的 inflate 逐字节无关，所以这里同时是"能解开"和"解得多快"。
 * png_decode 输出 RGB888，吞吐按输出字节算。
 */
extern int png_decode(const uint8_t *data, uint32_t size, uint8_t *out_rgb,
                      uint32_t out_cap, int max_w, int max_h,
                      int *out_w, int *out_h);

static void bench_png(const char *path, int iters) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("[PNG] 打不开 %s（先跑 bench_gui.sh 生成输入）\n", path);
        failures++;
        return;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, fp) != (size_t)size) {
        printf("[PNG] 读取失败\n");
        failures++;
        fclose(fp);
        free(data);
        return;
    }
    fclose(fp);

    static uint8_t rgb[1024 * 1024];
    int w = 0, h = 0;
    int rc = png_decode(data, (uint32_t)size, rgb, sizeof(rgb),
                        560, 384, &w, &h);
    CHECK(rc == 0, "png_decode 返回 %d", rc);
    if (rc != 0) {
        free(data);
        return;
    }

    double t0 = now_ms();
    for (int i = 0; i < iters; i++) {
        int dw = 0, dh = 0;
        if (png_decode(data, (uint32_t)size, rgb, sizeof(rgb),
                       560, 384, &dw, &dh) != 0) {
            printf("[PNG] 第 %d 次解码失败\n", i);
            failures++;
            break;
        }
    }
    (void)t0;
    /* 同样取中位数：分 TRIALS 轮、每轮 iters 次，再折算单次成本。 */
    double samples[TRIALS];
    for (int trial = 0; trial < TRIALS; trial++) {
        double start = now_ms();
        for (int i = 0; i < iters; i++) {
            int dw = 0, dh = 0;
            if (png_decode(data, (uint32_t)size, rgb, sizeof(rgb),
                           560, 384, &dw, &dh) != 0) {
                printf("[PNG] 第 %d 次解码失败\n", i);
                failures++;
                break;
            }
        }
        samples[trial] = (now_ms() - start) / iters;
    }
    double ms = median_of(samples, TRIALS);
    double out_bytes = (double)w * h * 3.0;
    printf("[PNG] %s  %dx%d  压缩 %ld B  →  输出 %.0f B\n",
           path, w, h, size, out_bytes);
    printf("  %d 次解码 x %d 轮取中位数  %8.3f ms/次  "
           "输出 %7.2f MB/s  输入 %7.2f MB/s\n",
           iters, TRIALS, ms,
           out_bytes / (ms * 1e-3) / 1e6,
           (double)size / (ms * 1e-3) / 1e6);
    free(data);
}

int main(int argc, char **argv) {
    int frames = 200;
    int iters = 200;
    const char *png = argc > 1 ? argv[1] : "build/tmp/bench-gui/in.png";
    if (argc > 2) frames = atoi(argv[2]);

    printf("[GUI-BENCH] 宿主基准：blit 与 PNG 解码（确定性输入，可重复）\n");
    bench_blit(frames);
    printf("\n");
    bench_png(png, iters);

    if (failures) {
        printf("\n[GUI-BENCH] FAILED: %d\n", failures);
        return 1;
    }
    printf("\n[GUI-BENCH] all checks passed\n");
    return 0;
}
