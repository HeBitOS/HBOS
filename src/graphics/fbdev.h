/**
 * @file    fbdev.h
 * @brief   /dev/fb0 Linux fbdev 兼容设备接口
 *
 * 为 KWin QPainter 后端及用户态图形程序提供标准 Linux fbdev ABI：
 *   - open("/dev/fb0", O_RDWR)
 *   - ioctl: FBIOGET_VSCREENINFO / FBIOPUT_VSCREENINFO / FBIOGET_FSCREENINFO
 *            / FBIOPAN_DISPLAY / FBIOBLANK / FBIO_WAITFORVSYNC
 *   - mmap: 将真实硬件帧缓冲物理页直接映射进用户态（VM_BACKING_DRM）
 *
 * @layer   gui
 * @owner   src/graphics/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  src/devfs.c、src/syscall.c
 *
 * 索引头文件
 *   函数  fbdev_init       注册 /dev/fb0 设备节点
 *         fbdev_is_node    判断 VFS 节点是否为 /dev/fb0
 *         fbdev_page_phys  返回 /dev/fb0 第 page_index 页的物理地址
 */
#ifndef HBOS_FBDEV_H
#define HBOS_FBDEV_H

#include <stdint.h>
#include "../vfs.h"

/** 向 devfs 注册 /dev/fb0 设备节点（在 devfs_init 末尾调用） */
void fbdev_init(void);

/** 判断 VFS 节点是否为 /dev/fb0 */
int fbdev_is_node(vfs_node_t *node);

/** 返回 /dev/fb0 指定页的物理地址，越界或未就绪返回 0 */
uint64_t fbdev_page_phys(vfs_node_t *node, uint64_t page_index);

#endif
