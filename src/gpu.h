/**
 * @file gpu.h
 * @brief GPU 驱动框架头文件
 *
 * 提供统一的 GPU 抽象层，支持 Bochs/QEMU BGA、VirtIO GPU 等硬件加速，
 * 并自动回退到软件渲染。上层组件（如窗口合成器）通过此接口操作 GPU，
 * 无需了解底层硬件细节。
 */

#ifndef HBOS_GPU_H
#define HBOS_GPU_H

#include <stdint.h>
#include "pci.h"

/* ================================================================
 * GPU 驱动类型枚举
 * ================================================================ */

typedef enum {
    GPU_NONE = 0,     /**< 无 GPU / 未初始化 */
    GPU_BOCHS,        /**< Bochs/QEMU BGA (VBE 扩展) */
    GPU_VMWARE,       /**< VMware SVGA-II */
    GPU_VIRTIO,       /**< VirtIO GPU */
    GPU_SOFTWARE,     /**< 软件渲染回退 */
} gpu_driver_t;

/* ================================================================
 * GPU 能力标志
 * ================================================================ */

#define GPU_CAP_2D_ACCEL   (1 << 0)  /**< 2D 硬件加速 */
#define GPU_CAP_3D_ACCEL   (1 << 1)  /**< 3D 硬件加速 */
#define GPU_CAP_CURSOR     (1 << 2)  /**< 硬件光标 */
#define GPU_CAP_DOUBLE_BUF (1 << 3)  /**< 双缓冲 */

/* ================================================================
 * GPU 设备描述结构体
 * ================================================================ */

typedef struct {
    gpu_driver_t type;           /**< 驱动类型 */
    uint32_t    *fb_addr;        /**< 帧缓冲物理地址 */
    uint64_t     fb_width;       /**< 帧缓冲宽度（像素） */
    uint64_t     fb_height;      /**< 帧缓冲高度（像素） */
    uint64_t     fb_pitch;       /**< 帧缓冲每行字节数 */
    uint8_t      fb_bpp;         /**< 每像素位数 */

    uint32_t     capabilities;   /**< 能力标志位掩码 */

    /** PCI 设备信息（硬件加速时有效） */
    pci_device_t pci_dev;
    int          pci_valid;      /**< 是否通过 PCI 发现 */
} gpu_device_t;

/* ================================================================
 * GPU 操作函数指针结构体
 * ================================================================ */

typedef struct {
    /** 初始化 GPU 设备 */
    int  (*init)(gpu_device_t *dev);

    /** 填充矩形区域（硬件加速或软件回退） */
    void (*fill_rect)(gpu_device_t *dev, uint64_t x, uint64_t y,
                      uint64_t w, uint64_t h, uint32_t color);

    /** 拷贝矩形区域（blit，硬件加速或软件回退） */
    void (*copy_rect)(gpu_device_t *dev,
                      uint64_t src_x, uint64_t src_y,
                      uint64_t dst_x, uint64_t dst_y,
                      uint64_t w, uint64_t h);

    /** 拷贝矩形区域（从外部缓冲区到帧缓冲） */
    void (*bitblt)(gpu_device_t *dev,
                   const uint32_t *src, uint64_t src_pitch,
                   uint64_t dst_x, uint64_t dst_y,
                   uint64_t w, uint64_t h);

    /** 刷新 / 同步 GPU 命令 */
    void (*flush)(gpu_device_t *dev);
} gpu_ops_t;

/* ================================================================
 * 公共 API
 * ================================================================ */

/**
 * @brief 初始化 GPU 子系统
 *
 * 扫描 PCI 总线查找显示设备，尝试初始化硬件加速 GPU 驱动，
 * 失败时回退到软件渲染（使用已存在的帧缓冲）。
 *
 * @return 成功返回 0，失败返回 -1
 */
int gpu_init(void);

/**
 * @brief 获取主 GPU 设备指针
 *
 * @return 指向主 GPU 设备结构的指针，未初始化时返回 NULL
 */
gpu_device_t *gpu_primary(void);

/**
 * @brief 填充矩形区域
 *
 * @param x     左上角 X 坐标
 * @param y     左上角 Y 坐标
 * @param w     宽度（像素）
 * @param h     高度（像素）
 * @param color 32 位颜色值（0x00RRGGBB）
 */
void gpu_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                   uint32_t color);

/**
 * @brief 拷贝矩形区域（帧缓冲内 blit）
 *
 * @param src_x 源矩形左上角 X
 * @param src_y 源矩形左上角 Y
 * @param dst_x 目标矩形左上角 X
 * @param dst_y 目标矩形左上角 Y
 * @param w     宽度（像素）
 * @param h     高度（像素）
 */
void gpu_copy_rect(uint64_t src_x, uint64_t src_y,
                   uint64_t dst_x, uint64_t dst_y,
                   uint64_t w, uint64_t h);

/**
 * @brief 从外部缓冲区拷贝矩形区域到帧缓冲
 *
 * @param src       源缓冲区指针
 * @param src_pitch 源缓冲区每行像素数
 * @param dst_x     目标矩形左上角 X
 * @param dst_y     目标矩形左上角 Y
 * @param w         宽度（像素）
 * @param h         高度（像素）
 */
void gpu_bitblt(const uint32_t *src, uint64_t src_pitch,
                uint64_t dst_x, uint64_t dst_y,
                uint64_t w, uint64_t h);

/**
 * @brief 刷新 GPU 命令队列
 */
void gpu_flush(void);

#endif /* HBOS_GPU_H */