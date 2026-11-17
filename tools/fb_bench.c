/**
 * @file    fb_bench.c
 * @brief   客机侧帧缓冲写带宽探针：判断"整帧拷贝上屏"到底是不是瓶颈
 *
 * 为什么需要它
 * ────────────
 * HBOS 的 KDE 出图链路是
 *   KWin(QPainter) → wl_shm buffer → hbos_wl_fb memcpy → /dev/fb0 → QEMU 扫描输出
 * 争论"要不要 virtio-gpu / 零拷贝"之前，必须先知道这一段到底花多少时间。
 * 宿主上的 memcpy 是 0.43 ms/帧（1920×1080，18 GB/s），但客机里的
 * /dev/fb0 是 QEMU 的 VRAM（PCI BAR），KVM 可能对这块内存开了脏页写保护，
 * 那样每次写 8 MB 会触发上千次 EPT fault，成本会高出一个数量级。**只有
 * 在客机里跑这个探针才能分辨**，本文件不依赖 QEMU 之外的任何东西。
 *
 * 输出读法
 * ────────
 *   ram-memcpy  与 fb-memcpy 的 MB/s 比值：
 *     ≈1x   → 拷帧不是瓶颈，别再往"零拷贝"上投入。
 *     >3x   → fb 写有额外代价（脏页/EPT fault/未写合并），值得做双缓冲、
 *             写合并映射或非临时存储。
 *   nt-store 明显快于 fb-memcpy → 用非临时存储（movntdq）绕开缓存。
 *   coherence: OK 是硬要求。若 READBACK MISMATCH，说明用户态映射与内核
 *   的写合并映射缓存属性不一致（同一物理页两种内存类型），必须修。
 *
 * 构建（宿主 gcc，与 scripts/build_wl_tests.sh 同参数）：
 *   gcc -O2 -fPIE -pie -Wl,-z,max-page-size=0x1000 \
 *       -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
 *       -o build/tests/fb_bench tools/fb_bench.c
 * 装进镜像：
 *   python3 scripts/install_app.py --img-put build/tests/fb_bench \
 *       --img-dest /bin/fb_bench
 * 客机里跑：
 *   /bin/fb_bench            # 默认 200 次/项
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <emmintrin.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *xalloc(size_t n) {
    void *p = aligned_alloc(4096, (n + 4095u) & ~(size_t)4095u);
    if (!p) { fprintf(stderr, "alloc %zu failed\n", n); exit(2); }
    memset(p, 0, n);
    return p;
}

static double mb_s(size_t bytes, double sec) {
    if (sec <= 0) return 0.0;
    return (double)bytes / sec / (1024.0 * 1024.0);
}

/* 行式拷贝：和 hbos_wl_fb.c 的 present 完全一样的循环形状 */
static void copy_rows(uint8_t *dst, uint32_t dst_pitch, const uint8_t *src,
                      uint32_t src_pitch, uint32_t rows, uint32_t bytes) {
    for (uint32_t y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * src_pitch, bytes);
}

/* 非临时存储（SSE2 基线即可）：流式写，不污染 L2/L3 */
static void copy_nt(uint8_t *dst, const uint8_t *src, size_t n) {
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        __m128i a = _mm_loadu_si128((const __m128i *)(src + i));
        __m128i b = _mm_loadu_si128((const __m128i *)(src + i + 16));
        __m128i c = _mm_loadu_si128((const __m128i *)(src + i + 32));
        __m128i d = _mm_loadu_si128((const __m128i *)(src + i + 48));
        _mm_stream_si128((__m128i *)(dst + i), a);
        _mm_stream_si128((__m128i *)(dst + i + 16), b);
        _mm_stream_si128((__m128i *)(dst + i + 32), c);
        _mm_stream_si128((__m128i *)(dst + i + 48), d);
    }
    _mm_sfence();
    for (; i < n; i++) dst[i] = src[i];
}

int main(int argc, char **argv) {
    int iters = (argc > 1) ? atoi(argv[1]) : 200;
    if (iters <= 0) iters = 200;

    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open /dev/fb0"); return 1; }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
        perror("FBIOGET_*SCREENINFO");
        return 1;
    }
    uint32_t w = var.xres, h = var.yres, pitch = fix.line_length;
    size_t size = (size_t)pitch * h;
    uint32_t bytes = (w * 4u < pitch) ? w * 4u : pitch;

    printf("FB %ux%u pitch=%u bpp=%u smem=%u size=%.2f MB\n", w, h, pitch,
           var.bits_per_pixel, fix.smem_len, (double)size / 1048576.0);

    uint8_t *fb = mmap(NULL, fix.smem_len ? fix.smem_len : size,
                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap /dev/fb0"); return 1; }

    uint8_t *ram_src = xalloc(size);
    uint8_t *ram_dst = xalloc(size);
    for (size_t i = 0; i < size; i++) ram_src[i] = (uint8_t)(i * 7u);

    double t, ram_sec;
    t = now_s(); for (int i = 0; i < iters; i++) memcpy(ram_dst, ram_src, size);
    ram_sec = now_s() - t;
    printf("ram-memcpy  : %7.3f ms/frame  %8.1f MB/s\n",
           ram_sec * 1000.0 / iters, mb_s(size * (size_t)iters, ram_sec));

    t = now_s(); for (int i = 0; i < iters; i++) memcpy(fb, ram_src, size);
    double fb_sec = now_s() - t;
    printf("fb-memcpy   : %7.3f ms/frame  %8.1f MB/s\n",
           fb_sec * 1000.0 / iters, mb_s(size * (size_t)iters, fb_sec));

    t = now_s(); for (int i = 0; i < iters; i++)
        copy_rows(fb, pitch, ram_src, pitch, h, bytes);
    double rows_sec = now_s() - t;
    printf("fb-rows     : %7.3f ms/frame  %8.1f MB/s   (hbos_wl_fb 当前形状)\n",
           rows_sec * 1000.0 / iters, mb_s(size * (size_t)iters, rows_sec));

    t = now_s(); for (int i = 0; i < iters; i++) copy_nt(fb, ram_src, size);
    double nt_sec = now_s() - t;
    printf("fb-nt-store : %7.3f ms/frame  %8.1f MB/s\n",
           nt_sec * 1000.0 / iters, mb_s(size * (size_t)iters, nt_sec));

    /* 相干性：写完立刻读回，必须一致。不一致 = 同一物理页有 WB/WC 双重映射。 */
    size_t probes[] = { 0, 4096, 65536, size / 2, size - 4096 };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        size_t off = probes[i] < size ? probes[i] : 0;
        fb[off] = (uint8_t)(0xA5u ^ (unsigned)off);
        if (fb[off] != (uint8_t)(0xA5u ^ (unsigned)off)) bad++;
    }
    printf("coherence   : %s\n", bad ? "READBACK MISMATCH" : "OK");

    /* 单页随机写：看每次写是否被脏页跟踪放大（小写入延迟探针） */
    volatile uint8_t *v = fb;
    t = now_s(); for (uint32_t y = 0; y < h; y++) v[(size_t)y * pitch] ^= 1;
    printf("row-touch   : %7.3f ms  (%u 次单字节写)\n",
           (now_s() - t) * 1000.0, h);

    munmap(fb, fix.smem_len ? fix.smem_len : size);
    close(fd);
    free(ram_src); free(ram_dst);
    return 0;
}
