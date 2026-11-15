/**
 * @file    drm_uapi.h
 * @brief   Linux DRM/KMS 的 ioctl 号、结构体与常量（内核侧 ABI 镜像）
 *
 * @layer   gui
 * @owner   src/graphics/drm/
 * @stable  experimental
 *
 * 依赖    只依赖 <stdint.h>：本文件是**对外 ABI**，一个字段的偏移错开
 *         就会让 libdrm 传进来的结构体被读错位，因此刻意不引入内核类型。
 * 被依赖  drm.h / drm_card.c，以及宿主单测（tests/host 通过 -Isrc 直接引用）。
 *
 * 索引头文件
 *   类型  drm_version_t             DRM_IOCTL_VERSION 参数
 *         drm_mode_card_res_t       GETRESOURCES 资源枚举
 *         drm_mode_crtc_t           CRTC/模式/扫描输出设置
 *         drm_mode_get_connector_t  连接器与模式列表
 *         drm_mode_create_dumb_t    dumb buffer 申请
 *         drm_mode_fb_cmd2_t        ADDFB2/GETFB2 参数
 *         drm_event_t               用户态 read() 事件头
 *         drm_mode_atomic_t         原子提交参数（仅探测，不消费）
 *   宏    DRM_IOCTL_VERSION         ioctl 号族（定义见正文）
 *         DRM_CAP_DUMB_BUFFER       能力位族（DRM_CAP_*）
 *         DRM_FORMAT_XRGB8888       像素格式族（DRM_FORMAT_*）
 *         DRM_MODE_TYPE_PREFERRED   模式标志族（DRM_MODE_*）
 *   函数  （本文件只有类型与宏）
 *
 * 何时转正 stable：KWin 的 DRM 后端在 HIVE 里完成一次 modeset 并通过
 * 连续 100 次 page flip 回归后。在那之前字段可加不可改序。
 */
#ifndef HBOS_DRM_UAPI_H
#define HBOS_DRM_UAPI_H

#include <stdint.h>

/* ═══ ioctl 编码 ════════════════════════════════════════════════════
 * 与 Linux / libdrm 的 _IOC 完全一致：dir/size/type/nr 四段。
 * 内核处理时只认 (type, nr)，忽略 size —— 让不同 libdrm 版本的头文件
 * 即使结构体对齐有差异也仍能命中，避免"差 4 字节就 ENOTTY"。
 */
#define DRM_IOC_NRBITS      8
#define DRM_IOC_TYPEBITS    8
#define DRM_IOC_SIZEBITS    14
#define DRM_IOC_DIRBITS     2

#define DRM_IOC_NRSHIFT     0
#define DRM_IOC_TYPESHIFT   (DRM_IOC_NRSHIFT + DRM_IOC_NRBITS)
#define DRM_IOC_SIZESHIFT   (DRM_IOC_TYPESHIFT + DRM_IOC_TYPEBITS)
#define DRM_IOC_DIRSHIFT    (DRM_IOC_SIZESHIFT + DRM_IOC_SIZEBITS)

#define DRM_IOC_NONE        0U
#define DRM_IOC_WRITE       1U
#define DRM_IOC_READ        2U

#define DRM_IOC(dir, type, nr, size) \
    (((dir)  << DRM_IOC_DIRSHIFT)  | \
     ((type) << DRM_IOC_TYPESHIFT) | \
     ((nr)   << DRM_IOC_NRSHIFT)   | \
     ((size) << DRM_IOC_SIZESHIFT))

#define DRM_IOCTL_BASE      'd'
#define DRM_IO(nr)              DRM_IOC(DRM_IOC_NONE, DRM_IOCTL_BASE, nr, 0)
#define DRM_IOW(nr, type)       DRM_IOC(DRM_IOC_WRITE, DRM_IOCTL_BASE, nr, sizeof(type))
#define DRM_IOR(nr, type)       DRM_IOC(DRM_IOC_READ, DRM_IOCTL_BASE, nr, sizeof(type))
#define DRM_IOWR(nr, type)      DRM_IOC(DRM_IOC_READ | DRM_IOC_WRITE, \
                                        DRM_IOCTL_BASE, nr, sizeof(type))

/* 内核侧派发只取这两段。 */
#define DRM_CMD_DIR(cmd)    (((cmd) >> DRM_IOC_DIRSHIFT) & 0x3U)
#define DRM_CMD_TYPE(cmd)   (((cmd) >> DRM_IOC_TYPESHIFT) & 0xFFU)
#define DRM_CMD_NR(cmd)     (((cmd) >> DRM_IOC_NRSHIFT) & 0xFFU)

/* ═══ 数据模型 ══════════════════════════════════════════════════════ */

/** 模式名长度，与 DRM_DISPLAY_MODE_LEN 一致 */
#define DRM_MODE_NAME_LEN   32

/** 显示模式。对齐与 Linux drm_mode_modeinfo 逐字段一致（104 字节）。 */
typedef struct {
    uint32_t clock;         /**< 像素时钟 kHz */
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;      /**< 刷新率 Hz */
    uint32_t flags;         /**< DRM_MODE_FLAG_* */
    uint32_t type;          /**< DRM_MODE_TYPE_* */
    char     name[DRM_MODE_NAME_LEN];
} drm_mode_modeinfo_t;

/** GETRESOURCES：两段式（先给 count，再给数组）。 */
typedef struct {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
} drm_mode_card_res_t;

/** GETCRTC / SETCRTC。 */
typedef struct {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    drm_mode_modeinfo_t mode;
} drm_mode_crtc_t;

/** GETENCODER。 */
typedef struct {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} drm_mode_get_encoder_t;

/** GETCONNECTOR：四个数组各自两段式。 */
typedef struct {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;    /**< DRM_MODE_CONNECTED / DISCONNECTED */
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
} drm_mode_get_connector_t;

/** CREATE_DUMB。 */
typedef struct {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;        /**< 出参 */
    uint32_t pitch;         /**< 出参：字节/行 */
    uint64_t size;          /**< 出参：字节总数 */
} drm_mode_create_dumb_t;

/** MAP_DUMB：offset 作为 mmap() 的偏移量交给用户态。 */
typedef struct {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;        /**< 出参 */
} drm_mode_map_dumb_t;

/** DESTROY_DUMB。 */
typedef struct {
    uint32_t handle;
} drm_mode_destroy_dumb_t;

/** ADDFB2 / GETFB2。 */
typedef struct {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
} drm_mode_fb_cmd2_t;

/** PAGE_FLIP。 */
typedef struct {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
} drm_mode_crtc_page_flip_t;

/** GETPLANERESOURCES。 */
typedef struct {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
} drm_mode_get_plane_res_t;

/** GETPLANE。 */
typedef struct {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
} drm_mode_get_plane_t;

/** SETPLANE（legacy plane 更新，仅作占位）。 */
typedef struct {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    int32_t  crtc_x;
    int32_t  crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_h;
    uint32_t src_w;
} drm_mode_set_plane_t;

/** MODE_ATOMIC：本实现不消费，但保持 ABI 完整以便客户端探测。 */
typedef struct {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
} drm_mode_atomic_t;

/** GET_CAP / SET_CLIENT_CAP 共用（capability, value）。 */
typedef struct {
    uint64_t capability;
    uint64_t value;
} drm_get_cap_t;

/** VERSION。长度字段是 size_t，在 x86_64 上即 8 字节。 */
typedef struct {
    int32_t  version_major;
    int32_t  version_minor;
    int32_t  version_patchlevel;
    uint64_t name_len;
    char    *name;
    uint64_t date_len;
    char    *date;
    uint64_t desc_len;
    char    *desc;
} drm_version_t;

/** 用户态 read(drm_fd) 返回的事件头。 */
typedef struct {
    uint32_t type;
    uint32_t length;
} drm_event_t;

/** FLIP_COMPLETE / VBLANK 事件负载。 */
typedef struct {
    drm_event_t base;
    uint64_t    user_data;
    uint32_t    tv_sec;
    uint32_t    tv_usec;
    uint32_t    sequence;
    uint32_t    crtc_id;
} drm_event_vblank_t;

/** WAIT_VBLANK 请求。 */
typedef struct {
    uint32_t type;
    uint32_t sequence;
    uint64_t signal;        /**< unsigned long */
} drm_wait_vblank_request_t;

/** WAIT_VBLANK 回复。 */
typedef struct {
    uint32_t type;
    uint32_t sequence;
    int64_t  tval_sec;      /**< long */
    int64_t  tval_usec;     /**< long */
} drm_wait_vblank_reply_t;

/** WAIT_VBLANK 联合体（入参与出参同址）。 */
typedef union {
    drm_wait_vblank_request_t request;
    drm_wait_vblank_reply_t   reply;
} drm_wait_vblank_t;

/** GEM_CLOSE。 */
typedef struct {
    uint32_t handle;
    uint32_t pad;
} drm_gem_close_t;

/* ═══ ioctl 号 ══════════════════════════════════════════════════════
 * 与 /usr/include/drm/drm.h 的取值逐条对齐（含 ATOMIC=0xBC、GETFB2=0xCE
 * 这两个"曾被误记成 0xBD/0xBE"的新编号）。内核侧只按 nr 派发。
 */
#define DRM_IOCTL_VERSION               DRM_IOWR(0x00, drm_version_t)
#define DRM_IOCTL_GEM_CLOSE             DRM_IOW (0x09, drm_gem_close_t)
#define DRM_IOCTL_GET_CAP               DRM_IOWR(0x0c, drm_get_cap_t)
#define DRM_IOCTL_SET_CLIENT_CAP        DRM_IOW (0x0d, drm_get_cap_t)
#define DRM_IOCTL_SET_MASTER            DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER           DRM_IO(0x1f)
#define DRM_IOCTL_WAIT_VBLANK           DRM_IOWR(0x3a, drm_wait_vblank_t)
#define DRM_IOCTL_MODE_GETRESOURCES     DRM_IOWR(0xA0, drm_mode_card_res_t)
#define DRM_IOCTL_MODE_GETCRTC          DRM_IOWR(0xA1, drm_mode_crtc_t)
#define DRM_IOCTL_MODE_SETCRTC          DRM_IOWR(0xA2, drm_mode_crtc_t)
#define DRM_IOCTL_MODE_GETENCODER       DRM_IOWR(0xA6, drm_mode_get_encoder_t)
#define DRM_IOCTL_MODE_GETCONNECTOR     DRM_IOWR(0xA7, drm_mode_get_connector_t)
#define DRM_IOCTL_MODE_GETFB            DRM_IOWR(0xAD, drm_mode_fb_cmd2_t)
#define DRM_IOCTL_MODE_RMFB             DRM_IOWR(0xAF, uint32_t)
#define DRM_IOCTL_MODE_PAGE_FLIP        DRM_IOWR(0xB0, drm_mode_crtc_page_flip_t)
#define DRM_IOCTL_MODE_CREATE_DUMB      DRM_IOWR(0xB2, drm_mode_create_dumb_t)
#define DRM_IOCTL_MODE_MAP_DUMB         DRM_IOWR(0xB3, drm_mode_map_dumb_t)
#define DRM_IOCTL_MODE_DESTROY_DUMB     DRM_IOWR(0xB4, drm_mode_destroy_dumb_t)
#define DRM_IOCTL_MODE_GETPLANERESOURCES DRM_IOWR(0xB5, drm_mode_get_plane_res_t)
#define DRM_IOCTL_MODE_GETPLANE         DRM_IOWR(0xB6, drm_mode_get_plane_t)
#define DRM_IOCTL_MODE_SETPLANE         DRM_IOWR(0xB7, drm_mode_set_plane_t)
#define DRM_IOCTL_MODE_ADDFB2           DRM_IOWR(0xB8, drm_mode_fb_cmd2_t)
#define DRM_IOCTL_MODE_ATOMIC           DRM_IOWR(0xBC, drm_mode_atomic_t)
#define DRM_IOCTL_MODE_GETFB2           DRM_IOWR(0xCE, drm_mode_fb_cmd2_t)

/* ═══ 常量与标志 ════════════════════════════════════════════════════ */

/* GET_CAP 能力号 */
#define DRM_CAP_DUMB_BUFFER             0x1
#define DRM_CAP_VBLANK_HIGH_CRTC        0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH    0x3
#define DRM_CAP_DUMB_PREFER_SHADOW      0x4
#define DRM_CAP_PRIME                   0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC     0x6
#define DRM_CAP_ASYNC_PAGE_FLIP         0x7
#define DRM_CAP_CURSOR_WIDTH            0x8
#define DRM_CAP_CURSOR_HEIGHT           0x9
#define DRM_CAP_ADDFB2_MODIFIERS        0x10
#define DRM_CAP_PAGE_FLIP_TARGET        0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT    0x12
#define DRM_CAP_SYNCOBJ                 0x13
#define DRM_CAP_SYNCOBJ_TIMELINE        0x14
/* 注意：Linux 没有 "DRM_CAP_ATOMIC" 能力位 —— 用户态是靠
 * SET_CLIENT_CAP(DRM_CLIENT_CAP_ATOMIC,1) 的返回值来判定的（见 drm_card.c）。 */

/* SET_CLIENT_CAP 能力号 */
#define DRM_CLIENT_CAP_STEREO_3D        1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC           3
#define DRM_CLIENT_CAP_ASPECT_RATIO     4

/* 像素格式（FourCC） */
#define DRM_FORMAT_C8        0x20203843U  /* 'C8  ' 未支持，仅占位 */
#define DRM_FORMAT_RGB565    0x36314752U  /* 'RG16' */
#define DRM_FORMAT_XRGB8888  0x34325258U  /* 'XR24' */
#define DRM_FORMAT_ARGB8888  0x34325241U  /* 'AR24' */
#define DRM_FORMAT_ABGR8888  0x34324241U  /* 'AB24' */
#define DRM_FORMAT_MOD_LINEAR 0ULL

/* 模式标志 */
#define DRM_MODE_TYPE_BUILTIN    (1U << 0)
#define DRM_MODE_TYPE_CLOCK_C    ((1U << 1) | DRM_MODE_TYPE_BUILTIN)
#define DRM_MODE_TYPE_PREFERRED  (1U << 3)
#define DRM_MODE_TYPE_DEFAULT    (1U << 4)
#define DRM_MODE_TYPE_USERDEF    (1U << 5)
#define DRM_MODE_TYPE_DRIVER     (1U << 6)

#define DRM_MODE_FLAG_PHSYNC     (1U << 0)
#define DRM_MODE_FLAG_NHSYNC     (1U << 1)
#define DRM_MODE_FLAG_PVSYNC     (1U << 2)
#define DRM_MODE_FLAG_NVSYNC     (1U << 3)

/* 连接器 */
#define DRM_MODE_CONNECTOR_Unknown  0
#define DRM_MODE_CONNECTOR_HDMIA    11
#define DRM_MODE_CONNECTOR_eDP      14
#define DRM_MODE_CONNECTOR_VIRTUAL  15
#define DRM_MODE_CONNECTED          1
#define DRM_MODE_DISCONNECTED       2
#define DRM_MODE_ENCODER_NONE       0
#define DRM_MODE_ENCODER_TMDS       2
#define DRM_MODE_ENCODER_VIRTUAL    5

/* PAGE_FLIP / ATOMIC 标志 */
#define DRM_MODE_PAGE_FLIP_EVENT        0x01
#define DRM_MODE_PAGE_FLIP_ASYNC        0x02
#define DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE 0x04
#define DRM_MODE_PAGE_FLIP_TARGET_RELATIVE 0x08
#define DRM_MODE_PAGE_FLIP_FLAGS \
    (DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC | \
     DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE | DRM_MODE_PAGE_FLIP_TARGET_RELATIVE)
#define DRM_MODE_ATOMIC_TEST_ONLY       0x0100
#define DRM_MODE_ATOMIC_NONBLOCK        0x0200
#define DRM_MODE_ATOMIC_ALLOW_MODESET   0x0400

/* 事件类型 */
#define DRM_EVENT_VBLANK        0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

/* WAIT_VBLANK 请求类型 */
#define DRM_VBLANK_ABSOLUTE     0x00
#define DRM_VBLANK_RELATIVE     0x01
#define DRM_VBLANK_HIGH_CRTC_MASK 0x0000003e
#define DRM_VBLANK_EVENT        0x4000000
#define DRM_VBLANK_FLIP         0x8000000

#endif /* HBOS_DRM_UAPI_H */
