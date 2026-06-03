/**
 * @file gpu.c
 * @brief GPU 驱动框架实现
 *
 * 扫描 PCI 总线查找显示设备，初始化硬件加速 GPU 驱动（Bochs BGA、
 * VirtIO GPU），失败时回退到软件渲染。
 */

#include "gpu.h"
#include "graphics.h"
#include "string.h"
#include "core/heap.h"

/* ================================================================
 * PCI 厂商/设备 ID
 * ================================================================ */

#define PCI_VENDOR_BOCHS      0x1234
#define PCI_DEVICE_BOCHS_VGA  0x1111

#define PCI_VENDOR_VIRTIO     0x1AF4
#define PCI_DEVICE_VIRTIO_GPU 0x1050

/* ================================================================
 * BGA / VBE 扩展 I/O 端口
 * ================================================================ */

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID           0
#define VBE_DISPI_INDEX_XRES         1
#define VBE_DISPI_INDEX_YRES         2
#define VBE_DISPI_INDEX_BPP          3
#define VBE_DISPI_INDEX_ENABLE       4
#define VBE_DISPI_INDEX_BANK         5
#define VBE_DISPI_INDEX_VIRT_WIDTH   6
#define VBE_DISPI_INDEX_VIRT_HEIGHT  7
#define VBE_DISPI_INDEX_X_OFFSET     8
#define VBE_DISPI_INDEX_Y_OFFSET     9

#define VBE_DISPI_ENABLED    0x01
#define VBE_DISPI_LFB_ENABLED 0x40

/* ================================================================
 * 单例：主 GPU 设备
 * ================================================================ */

static gpu_device_t g_primary_gpu;
static gpu_ops_t    g_primary_ops;
static int          g_gpu_ready = 0;

/* ================================================================
 * I/O 端口辅助函数
 * ================================================================ */

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* ================================================================
 * BGA 寄存器读写
 * ================================================================ */

static void __attribute__((unused)) bga_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t bga_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

/* ================================================================
 * 软件渲染操作
 * ================================================================ */

static void sw_fill_rect(gpu_device_t *dev, uint64_t x, uint64_t y,
                         uint64_t w, uint64_t h, uint32_t color) {
    if (!dev || !dev->fb_addr || w == 0 || h == 0) return;
    if (x >= dev->fb_width || y >= dev->fb_height) return;
    if (x + w > dev->fb_width)  w = dev->fb_width - x;
    if (y + h > dev->fb_height) h = dev->fb_height - y;

    uint64_t pitch_px = dev->fb_pitch / 4;
    for (uint64_t yy = 0; yy < h; yy++) {
        volatile uint32_t *row = &dev->fb_addr[(y + yy) * pitch_px + x];
        for (uint64_t xx = 0; xx < w; xx++) row[xx] = color;
    }
}

static void sw_copy_rect(gpu_device_t *dev,
                         uint64_t src_x, uint64_t src_y,
                         uint64_t dst_x, uint64_t dst_y,
                         uint64_t w, uint64_t h) {
    if (!dev || !dev->fb_addr || w == 0 || h == 0) return;

    uint64_t pitch_px = dev->fb_pitch / 4;

    if (src_y < dst_y) {
        /* 从上到下拷贝（源在下、目标在上），防止重叠时自覆盖 */
        for (uint64_t yy = 0; yy < h; yy++) {
            uint64_t sy = src_y + (h - 1 - yy);
            uint64_t dy = dst_y + (h - 1 - yy);
            if (sy >= dev->fb_height || dy >= dev->fb_height) continue;
            volatile uint32_t *src_row = &dev->fb_addr[sy * pitch_px];
            volatile uint32_t *dst_row = &dev->fb_addr[dy * pitch_px];
            for (uint64_t xx = 0; xx < w; xx++) {
                if (src_x + xx < dev->fb_width && dst_x + xx < dev->fb_width)
                    dst_row[dst_x + xx] = src_row[src_x + xx];
            }
        }
    } else {
        for (uint64_t yy = 0; yy < h; yy++) {
            if (src_y + yy >= dev->fb_height || dst_y + yy >= dev->fb_height) continue;
            volatile uint32_t *src_row = &dev->fb_addr[(src_y + yy) * pitch_px];
            volatile uint32_t *dst_row = &dev->fb_addr[(dst_y + yy) * pitch_px];
            for (uint64_t xx = 0; xx < w; xx++) {
                if (src_x + xx < dev->fb_width && dst_x + xx < dev->fb_width)
                    dst_row[dst_x + xx] = src_row[src_x + xx];
            }
        }
    }
}

static void sw_bitblt(gpu_device_t *dev,
                      const uint32_t *src, uint64_t src_pitch,
                      uint64_t dst_x, uint64_t dst_y,
                      uint64_t w, uint64_t h) {
    if (!dev || !dev->fb_addr || !src || w == 0 || h == 0) return;
    if (dst_x >= dev->fb_width || dst_y >= dev->fb_height) return;
    if (dst_x + w > dev->fb_width)  w = dev->fb_width - dst_x;
    if (dst_y + h > dev->fb_height) h = dev->fb_height - dst_y;

    uint64_t dst_pitch_px = dev->fb_pitch / 4;
    for (uint64_t yy = 0; yy < h; yy++) {
        volatile uint32_t *dst_row = &dev->fb_addr[(dst_y + yy) * dst_pitch_px + dst_x];
        const uint32_t    *src_row = &src[yy * src_pitch];
        for (uint64_t xx = 0; xx < w; xx++) dst_row[xx] = src_row[xx];
    }
}

static void sw_flush(gpu_device_t *dev) {
    (void)dev;
    /* 软件渲染直接写入帧缓冲，无需 flush */
}

/* ================================================================
 * Bochs BGA 初始化
 * ================================================================ */

static int bochs_init(gpu_device_t *dev) {
    if (!dev) return -1;

    uint32_t bar0 = pci_bar(dev->pci_dev.bus, dev->pci_dev.slot,
                            dev->pci_dev.func, 0);
    if (!bar0) return -1;

    /* 启用 PCI 内存空间和总线主控 */
    pci_enable_bus_master_mmio(&dev->pci_dev);

    /* 检测 BGA 是否可用 */
    uint16_t id = bga_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0) return -1;

    /* 读取当前分辨率 */
    uint16_t xres = bga_read(VBE_DISPI_INDEX_XRES);
    uint16_t yres = bga_read(VBE_DISPI_INDEX_YRES);
    uint16_t bpp  = bga_read(VBE_DISPI_INDEX_BPP);

    if (xres == 0 || yres == 0 || bpp == 0) return -1;

    dev->type     = GPU_BOCHS;
    dev->fb_addr  = (uint32_t *)(uintptr_t)bar0;
    dev->fb_width  = xres;
    dev->fb_height = yres;
    dev->fb_pitch  = xres * (bpp / 8);
    dev->fb_bpp    = (uint8_t)bpp;
    dev->capabilities = 0;

    return 0;
}

/* ================================================================
 * VirtIO GPU 初始化（占位 / 存根）
 * ================================================================ */

static int virtio_gpu_init(gpu_device_t *dev) {
    if (!dev) return -1;
    /* VirtIO GPU 需要完整的 virtio 传输层实现，当前占位 */
    (void)dev;
    return -1;
}

/* ================================================================
 * 软件渲染初始化（使用 graphics 系统提供的帧缓冲）
 * ================================================================ */

static int sw_init(gpu_device_t *dev) {
    if (!dev) return -1;

    fb_info_t info;
    if (fb_get_info(&info) != 0) return -1;

    dev->type     = GPU_SOFTWARE;
    dev->fb_addr  = info.addr;
    dev->fb_width  = info.width;
    dev->fb_height = info.height;
    dev->fb_pitch  = info.pitch;
    dev->fb_bpp    = info.bpp;
    dev->capabilities = 0;

    return 0;
}

/* ================================================================
 * gpu_init — 扫描 PCI 并初始化 GPU
 * ================================================================ */

int gpu_init(void) {
    memset(&g_primary_gpu, 0, sizeof(g_primary_gpu));
    memset(&g_primary_ops, 0, sizeof(g_primary_ops));

    pci_device_t dev;

    /* 查找 PCI 显示设备 */
    if (pci_find_class(0x03, 0x00, 0xFF, &dev) == 0) {
        g_primary_gpu.pci_dev   = dev;
        g_primary_gpu.pci_valid = 1;

        /* 尝试 Bochs BGA */
        if (dev.vendor_id == PCI_VENDOR_BOCHS &&
            dev.device_id == PCI_DEVICE_BOCHS_VGA) {
            if (bochs_init(&g_primary_gpu) == 0) {
                g_primary_ops.init      = bochs_init;
                g_primary_ops.fill_rect = sw_fill_rect;
                g_primary_ops.copy_rect = sw_copy_rect;
                g_primary_ops.bitblt    = sw_bitblt;
                g_primary_ops.flush     = sw_flush;
                g_gpu_ready = 1;
                return 0;
            }
        }

        /* 尝试 VirtIO GPU */
        if (dev.vendor_id == PCI_VENDOR_VIRTIO &&
            dev.device_id == PCI_DEVICE_VIRTIO_GPU) {
            if (virtio_gpu_init(&g_primary_gpu) == 0) {
                g_primary_ops.init      = virtio_gpu_init;
                g_primary_ops.fill_rect = sw_fill_rect;
                g_primary_ops.copy_rect = sw_copy_rect;
                g_primary_ops.bitblt    = sw_bitblt;
                g_primary_ops.flush     = sw_flush;
                g_gpu_ready = 1;
                return 0;
            }
        }
    }

    /* 回退：使用 graphics 系统提供的帧缓冲进行软件渲染 */
    if (sw_init(&g_primary_gpu) == 0) {
        g_primary_ops.init      = sw_init;
        g_primary_ops.fill_rect = sw_fill_rect;
        g_primary_ops.copy_rect = sw_copy_rect;
        g_primary_ops.bitblt    = sw_bitblt;
        g_primary_ops.flush     = sw_flush;
        g_gpu_ready = 1;
        return 0;
    }

    g_primary_gpu.type = GPU_NONE;
    return -1;
}

/* ================================================================
 * 公共 API
 * ================================================================ */

gpu_device_t *gpu_primary(void) {
    return g_gpu_ready ? &g_primary_gpu : NULL;
}

void gpu_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                   uint32_t color) {
    if (!g_gpu_ready || !g_primary_ops.fill_rect) return;
    g_primary_ops.fill_rect(&g_primary_gpu, x, y, w, h, color);
}

void gpu_copy_rect(uint64_t src_x, uint64_t src_y,
                   uint64_t dst_x, uint64_t dst_y,
                   uint64_t w, uint64_t h) {
    if (!g_gpu_ready || !g_primary_ops.copy_rect) return;
    g_primary_ops.copy_rect(&g_primary_gpu, src_x, src_y, dst_x, dst_y, w, h);
}

void gpu_bitblt(const uint32_t *src, uint64_t src_pitch,
                uint64_t dst_x, uint64_t dst_y,
                uint64_t w, uint64_t h) {
    if (!g_gpu_ready || !g_primary_ops.bitblt) return;
    g_primary_ops.bitblt(&g_primary_gpu, src, src_pitch, dst_x, dst_y, w, h);
}

void gpu_flush(void) {
    if (!g_gpu_ready || !g_primary_ops.flush) return;
    g_primary_ops.flush(&g_primary_gpu);
}