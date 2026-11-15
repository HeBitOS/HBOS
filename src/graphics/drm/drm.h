/**
 * @file    drm.h
 * @brief   最小 KMS 卡片驱动：/dev/dri/card0 的内核侧接口
 *
 * @layer   gui
 * @owner   src/graphics/drm/
 * @stable  experimental
 *
 * 依赖    drm_uapi.h（纯 ABI 定义）。刻意不 include vfs.h：节点类型只用
 *         `struct vfs_node *` 前向引用，这样宿主单测可以只带 ABI 头。
 * 被依赖  src/devfs.c（注册 /dev/dri/card0 时调用 drm_card_register_node），
 *         src/syscall.c（mmap 字符设备的脏页/物理页查询）。
 *
 * 索引头文件
 *   类型  drm_card_status_t    对外可观测的 KMS 状态快照
 *   宏    DRM_CARD_CRTC_ID     伪造对象 id 族（CRTC/CONNECTOR/ENCODER/PLANE）
 *         DRM_CARD_MAX_DUMB    dumb buffer 槽位数
 *   函数  drm_card_init        从真实 framebuffer 建立单 CRTC KMS 状态
 *         drm_card_ready       是否已可用
 *         drm_card_register_node  把 card0 的 fops 挂到 devfs 节点
 *         drm_card_is_card_node   判断某个 vfs 节点是否就是 card0
 *         drm_card_page_phys   dumb buffer 的页物理地址（供 mmap 钩子）
 *         drm_card_event_pending 是否有未读事件（供 poll 钩子）
 *         drm_card_status      取状态快照
 *         drm_card_selftest    无 QEMU 的自检：完整 modeset 状态机
 */
#ifndef HBOS_DRM_H
#define HBOS_DRM_H

#include <stdint.h>

#include "drm_uapi.h"

/* ═══ 伪造的 KMS 对象 id ════════════════════════════════════════════
 * 单卡单 CRTC：id 固定，好处是任何一次 trace 都能直接读懂，不依赖
 * 对象分配顺序。KWin 只用这些 id 做句柄，不要求连续性。
 */
#define DRM_CARD_CRTC_ID       1U
#define DRM_CARD_CONNECTOR_ID  2U
#define DRM_CARD_ENCODER_ID    3U
#define DRM_CARD_PLANE_ID      4U

#define DRM_CARD_MAX_DUMB      8U   /**< 同时存在的 dumb buffer 上限 */
#define DRM_CARD_MAX_FB        8U   /**< 同时存在的 framebuffer 上限 */
#define DRM_CARD_MAX_EVENTS    8U   /**< 未读 DRM 事件上限 */
#define DRM_CARD_MAX_MODES     4U   /**< connector 暴露的模式数上限 */

/* dumb buffer 的 mmap 偏移编码：offset = handle << 32。
 * 这样 "不是真实文件偏移" 一目了然，同时保持页对齐（mmap 要求）。 */
#define DRM_CARD_DUMB_OFFSET_SHIFT 32

/* VFS 节点只做前向声明：本模块不 include vfs.h（宿主单测可只带 ABI 头）。 */
struct vfs_node;

/* ═══ 数据模型 ══════════════════════════════════════════════════════ */

/** 状态快照：给 selftest / 诊断命令读，不参与 ioctl。 */
typedef struct {
    uint32_t ready;
    uint32_t width;             /**< 面板宽度（像素） */
    uint32_t height;
    uint32_t pitch;             /**< 真实 framebuffer 的字节/行 */
    uint32_t crtc_active;
    uint32_t crtc_fb_id;        /**< 0 = 无扫描输出 */
    uint32_t mode_count;
    uint32_t dumb_used;
    uint32_t fb_used;
    uint32_t events_pending;
    uint32_t flips;             /**< 累计成功 page flip 次数 */
    uint32_t client_caps;
} drm_card_status_t;

/* ═══ 接口 ══════════════════════════════════════════════════════════ */

/**
 * @brief 用 graphics 已初始化的真实 framebuffer 建立单 CRTC KMS 状态。
 *
 * 前置条件：graphics_init() 已成功（fb_get_info 能取到地址）。
 * 可重入性：非重入，只在启动期调用一次；重复调用返回 0 且不重置状态。
 * @return 0 成功；-1 无 framebuffer（VGA 文本模式或未初始化）。
 */
int drm_card_init(void);

/** @return 1 = 已就绪；0 = 未初始化。 */
int drm_card_ready(void);

/**
 * @brief 把 card0 的 fops 与私有数据挂到已分配的 devfs 节点上。
 *
 * 由 src/devfs.c 在 devfs_init() 里调用（节点创建归 devfs，ops 归本模块）。
 * @param node 已 set_name("/dev/dri/card0")、type=CHDEV 的节点
 * @return 0 成功；-1 参数为空或 drm 未初始化。
 */
int drm_card_register_node(struct vfs_node *node);

/** @return 1 = 该节点是 card0；0 = 不是。 */
int drm_card_is_card_node(const struct vfs_node *node);

/**
 * @brief 查 dumb buffer 某个字节偏移对应的物理页。
 *
 * 供 syscall.c 的 mmap 钩子逐页调用（每页一次，offset 为页对齐）。
 * @return 物理地址；0 表示该 offset 不属于任何在用 dumb buffer。
 */
uint64_t drm_card_page_phys(const struct vfs_node *node, uint64_t offset);

/** @return 1 = 有未读 DRM 事件（供 poll 钩子报 POLLIN）；0 = 无。 */
int drm_card_event_pending(const struct vfs_node *node);

/** @brief 取状态快照。@param out 非 NULL 时写入。 */
void drm_card_status(drm_card_status_t *out);

/**
 * @brief 自检：VERSION → GET_CAP → GET_RESOURCES → CREATE_DUMB →
 *        ADDFB2 → SETCRTC → PAGE_FLIP → 取事件，逐项打印 PASS/FAIL。
 *
 * 不需要 QEMU：整条链路是纯内核状态机 + 一次真实 framebuffer 拷贝（若已
 * 初始化）。用来在宿主或启动自检里验证"KMS 骨架能完成一次 modeset"。
 * @return 0 全部通过；-1 有失败项（失败明细已打印）。
 */
int drm_card_selftest(void);

#endif /* HBOS_DRM_H */
