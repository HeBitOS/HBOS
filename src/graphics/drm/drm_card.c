/**
 * @file drm_card.c
 * @brief 最小 KMS：/dev/dri/card0 的 ioctl 状态机与 dumb buffer
 *
 * 设计要点（改之前先读）
 * ─────────────────────
 * 1. HBOS 只有一块由引导器交下来的 framebuffer，没有可编程扫描引擎。
 *    因此整个 KMS 是**单 CRTC / 单 connector / 单 encoder / 单 primary
 *    plane** 的伪造模型：对象 id 是常量（见 drm.h），mode 列表只暴露
 *    当前面板分辨率一个模式，SETCRTC/PAGE_FLIP 的"上屏"动作就是一次
 *    memcpy 到真实 framebuffer。不伪造不支持的模式，避免 KWin 选到
 *    我们无法呈现的尺寸。
 * 2. dumb buffer 用 pmm_alloc_blocks() 取**连续物理页**，内核通过恒等
 *    映射（VA == PA，boot.asm 建 0-4GiB，kernel.c 为高位内存补建）直接
 *    读写；用户态 mmap 由 syscall.c 的钩子逐页 vmm_map_page 到同一批
 *    物理页（VMM_P|VMM_U|VMM_W，**不带 VMM_OWNED**，因此 munmap 只拆
 *    映射、不 pmm_free_page，不会把还在扫描输出的页还回去）。
 * 3. 事件队列只有 FLIP_COMPLETE/VBLANK 两类，read() 一次交一个完整事件。
 *    poll 就绪由 drm_card_event_pending() 提供，接线在 syscall/linux_compat
 *    侧（本文件不依赖它们，保持可单测）。
 * 4. atomic 刻意不实现：SET_CLIENT_CAP(ATOMIC) 返回 -EINVAL，KWin 会退回
 *    legacy SETCRTC/PAGE_FLIP 路径。半吊子的 atomic（接受 commit 却忽略
 *    属性）比明确拒绝更危险，它会让 KWin 走进没人验证过的分支。
 */

#include "drm.h"

#include "graphics/graphics.h"
#include "vfs.h"
#include "string.h"
#include "errno.h"
#include "core/heap.h"
#include "core/pmm.h"
#include "core/vmm.h"

/* ═══ 内部数据模型 ══════════════════════════════════════════════════ */

/** dumb buffer：一段连续物理页 + 描述符。 */
typedef struct {
    uint32_t used;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /**< 字节/行（64 字节对齐） */
    uint32_t bpp;
    uint64_t size;          /**< pitch * height */
    uint64_t phys;          /**< 连续物理基址 */
    uint32_t pages;
    uint32_t mmapped;       /**< 是否曾被 mmap（无 munmap 钩子，只置位） */
} drm_dumb_t;

/** framebuffer：dumb buffer 的扫描输出视图。 */
typedef struct {
    uint32_t used;
    uint32_t fb_id;
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t pixel_format;
} drm_fb_t;

/** 一个未读事件的紧凑表示（不直接存用户可见结构，读时再展开）。 */
typedef struct {
    uint32_t used;
    uint32_t type;
    uint64_t user_data;
    uint32_t crtc_id;
    uint32_t sequence;
} drm_event_slot_t;

/** 全局单例。所有字段只在系统调用路径上被访问（同一 CRTC，无并行意义）。 */
static struct {
    int      ready;
    int      have_real_fb;
    uint32_t width;             /**< 面板宽（像素） */
    uint32_t height;
    uint32_t fb_pitch;          /**< 真实 framebuffer 字节/行 */
    uint8_t  fb_bpp;
    uint32_t *fb_addr;          /**< 真实 framebuffer 虚拟地址 */
    uint32_t mode_count;
    drm_mode_modeinfo_t modes[DRM_CARD_MAX_MODES];
    int      crtc_active;
    uint32_t crtc_fb_id;
    drm_mode_modeinfo_t crtc_mode;
    uint32_t sequence;
    uint32_t flips;
    uint32_t client_caps;
    uint32_t master;
    /* 分配表 */
    drm_dumb_t dumb[DRM_CARD_MAX_DUMB];
    drm_fb_t   fbs[DRM_CARD_MAX_FB];
    uint32_t next_handle;
    uint32_t next_fb_id;
    /* 事件环 */
    drm_event_slot_t events[DRM_CARD_MAX_EVENTS];
    uint32_t ev_head;           /**< 下一个待读 */
    uint32_t ev_count;
} g_drm;

static int drm_card_read(vfs_node_t *node, uint32_t offset, void *buf,
                         uint32_t count);
static int drm_card_ioctl(vfs_node_t *node, uint32_t cmd, void *arg);

static const vfs_ops_t g_drm_card_ops = {
    .read = drm_card_read,
    .ioctl = drm_card_ioctl,
};

/* ═══ 小工具 ════════════════════════════════════════════════════════ */

/** 无 snprintf 依赖的 "1920x1080" 生成器。 */
static void drm_mode_set_name(drm_mode_modeinfo_t *mode, uint32_t w, uint32_t h) {
    char digits[12];
    uint32_t n = 0;
    uint32_t v = w;
    do { digits[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 10);
    uint32_t pos = 0;
    while (n && pos < DRM_MODE_NAME_LEN - 1) mode->name[pos++] = digits[--n];
    if (pos < DRM_MODE_NAME_LEN - 1) mode->name[pos++] = 'x';
    v = h;
    n = 0;
    do { digits[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 10);
    while (n && pos < DRM_MODE_NAME_LEN - 1) mode->name[pos++] = digits[--n];
    mode->name[pos] = '\0';
}

/** 由分辨率合成一个 60Hz 的 CVT 近似模式（只保证 hdisplay/vdisplay 正确）。 */
static void drm_mode_build(drm_mode_modeinfo_t *mode, uint32_t w, uint32_t h) {
    memset(mode, 0, sizeof(*mode));
    mode->hdisplay    = (uint16_t)w;
    mode->hsync_start = (uint16_t)(w + w / 8);
    mode->hsync_end   = (uint16_t)(w + w / 8 + 32);
    mode->htotal      = (uint16_t)(w + w / 4);
    mode->vdisplay    = (uint16_t)h;
    mode->vsync_start = (uint16_t)(h + 3);
    mode->vsync_end   = (uint16_t)(h + 8);
    mode->vtotal      = (uint16_t)(h + h / 20);
    mode->vrefresh    = 60;
    mode->flags       = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    mode->type        = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    /* clock(kHz) = htotal * vtotal * 60 / 1000 */
    mode->clock = (uint32_t)(((uint64_t)mode->htotal * mode->vtotal * 60U) / 1000U);
    drm_mode_set_name(mode, w, h);
}

static drm_dumb_t *drm_dumb_find(uint32_t handle) {
    for (uint32_t i = 0; i < DRM_CARD_MAX_DUMB; i++)
        if (g_drm.dumb[i].used && g_drm.dumb[i].handle == handle)
            return &g_drm.dumb[i];
    return NULL;
}

static drm_fb_t *drm_fb_find(uint32_t fb_id) {
    for (uint32_t i = 0; i < DRM_CARD_MAX_FB; i++)
        if (g_drm.fbs[i].used && g_drm.fbs[i].fb_id == fb_id)
            return &g_drm.fbs[i];
    return NULL;
}

static drm_fb_t *drm_fb_find_handle(uint32_t handle) {
    for (uint32_t i = 0; i < DRM_CARD_MAX_FB; i++)
        if (g_drm.fbs[i].used && g_drm.fbs[i].handle == handle)
            return &g_drm.fbs[i];
    return NULL;
}

/** 事件入环；环满丢最旧的一条（KWin 的 flip 事件允许合并/丢失）。 */
static void drm_event_push(uint32_t type, uint64_t user_data, uint32_t crtc_id) {
    uint32_t slot;
    if (g_drm.ev_count == DRM_CARD_MAX_EVENTS) {
        slot = g_drm.ev_head;
        g_drm.ev_head = (g_drm.ev_head + 1U) % DRM_CARD_MAX_EVENTS;
        g_drm.ev_count--;
    }
    slot = (g_drm.ev_head + g_drm.ev_count) % DRM_CARD_MAX_EVENTS;
    drm_event_slot_t *e = &g_drm.events[slot];
    e->used = 1;
    e->type = type;
    e->user_data = user_data;
    e->crtc_id = crtc_id;
    e->sequence = g_drm.sequence;
    g_drm.ev_count++;
}

/**
 * 把某个 framebuffer 的内容拷到真实 framebuffer（唯一"上屏"动作）。
 *
 * 只拷两边都存在的行列；模式与 fb 尺寸已在 SETCRTC/PAGE_FLIP 校验过，
 * 所以这里的 min() 只是防止 selftest 用非面板尺寸的缓冲做验证。
 */
static void drm_scanout(const drm_fb_t *fb) {
    if (!g_drm.have_real_fb || !g_drm.fb_addr || !fb) return;
    drm_dumb_t *dumb = drm_dumb_find(fb->handle);
    if (!dumb || !dumb->phys) return;

    uint32_t rows = fb->height < g_drm.height ? fb->height : g_drm.height;
    uint32_t dst_pitch = g_drm.fb_pitch;
    uint32_t src_pitch = fb->pitch;
    uint32_t bytes = src_pitch < dst_pitch ? src_pitch : dst_pitch;

    const uint8_t *src = (const uint8_t *)(uintptr_t)dumb->phys;
    uint8_t *dst = (uint8_t *)g_drm.fb_addr;
    for (uint32_t y = 0; y < rows; y++)
        memcpy(dst + (uint64_t)y * dst_pitch, src + (uint64_t)y * src_pitch,
               bytes);
}

/* ═══ 初始化 ════════════════════════════════════════════════════════ */

/** 用给定分辨率建立模式表；真实 fb 存在时才允许扫描输出。 */
static void drm_card_configure(uint32_t width, uint32_t height,
                               uint32_t pitch, uint8_t bpp,
                               uint32_t *addr, int have_real_fb) {
    g_drm.width = width;
    g_drm.height = height;
    g_drm.fb_pitch = pitch ? pitch : width * 4U;
    g_drm.fb_bpp = bpp;
    g_drm.fb_addr = addr;
    g_drm.have_real_fb = have_real_fb;
    g_drm.mode_count = 1;
    drm_mode_build(&g_drm.modes[0], width, height);
    g_drm.crtc_active = 0;
    g_drm.crtc_fb_id = 0;
    g_drm.ready = 1;
}

int drm_card_init(void) {
    if (g_drm.ready) return 0;

    fb_info_t info;
    if (fb_get_info(&info) < 0 || !info.addr || info.width == 0 ||
        info.height == 0)
        return -1;

    memset(&g_drm, 0, sizeof(g_drm));
    g_drm.next_handle = 1;
    g_drm.next_fb_id = 1;
    /* 面板分辨率取引导器给的真实尺寸；pitch 是硬件行距，不能假定 w*4。 */
    drm_card_configure((uint32_t)info.width, (uint32_t)info.height,
                       (uint32_t)info.pitch, info.bpp, info.addr, 1);
    return 0;
}

int drm_card_ready(void) {
    return g_drm.ready;
}

int drm_card_register_node(struct vfs_node *node) {
    if (!node) return -1;
    if (!g_drm.ready) return -1;
    node->ops = &g_drm_card_ops;
    node->private_data = (void *)&g_drm;
    node->size = 0;
    return 0;
}

int drm_card_is_card_node(const struct vfs_node *node) {
    return node && node->ops == &g_drm_card_ops;
}

int drm_card_event_pending(const struct vfs_node *node) {
    if (!drm_card_is_card_node(node)) return 0;
    return g_drm.ev_count ? 1 : 0;
}

/** 偏移解码本身与节点无关，单测/自检直接用它，不必伪造 vfs 节点。 */
static uint64_t drm_page_phys_for_offset(uint64_t offset) {
    uint32_t handle = (uint32_t)(offset >> DRM_CARD_DUMB_OFFSET_SHIFT);
    uint64_t inner = offset & 0xFFFFFFFFULL;
    drm_dumb_t *dumb = drm_dumb_find(handle);
    if (!dumb) return 0;
    if (inner & (PAGE_SIZE - 1)) return 0;
    if (inner >= dumb->size) return 0;
    return dumb->phys + inner;
}

uint64_t drm_card_page_phys(const struct vfs_node *node, uint64_t offset) {
    if (!drm_card_is_card_node(node)) return 0;
    return drm_page_phys_for_offset(offset);
}

void drm_card_status(drm_card_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ready = (uint32_t)g_drm.ready;
    out->width = g_drm.width;
    out->height = g_drm.height;
    out->pitch = g_drm.fb_pitch;
    out->crtc_active = (uint32_t)g_drm.crtc_active;
    out->crtc_fb_id = g_drm.crtc_fb_id;
    out->mode_count = g_drm.mode_count;
    out->events_pending = g_drm.ev_count;
    out->flips = g_drm.flips;
    out->client_caps = g_drm.client_caps;
    for (uint32_t i = 0; i < DRM_CARD_MAX_DUMB; i++)
        if (g_drm.dumb[i].used) out->dumb_used++;
    for (uint32_t i = 0; i < DRM_CARD_MAX_FB; i++)
        if (g_drm.fbs[i].used) out->fb_used++;
}

/* ═══ dumb buffer ═══════════════════════════════════════════════════ */

/**
 * 申请一块 dumb buffer。
 *
 * pitch 按 64 字节对齐（扫描输出的常见要求，也让 1920*4 保持原值）。
 * 物理页必须连续：KWin 后续可能把 fb_id 交给别的扫描路径，非连续会让
 * 将来的真实 DMA 无法使用。
 * @return 0 成功；-EINVAL 参数非法；-ENOSPC 槽位满；-ENOMEM 物理页不足。
 */
static int drm_dumb_create(const drm_mode_create_dumb_t *req,
                           drm_mode_create_dumb_t *out) {
    if (!req || !out) return -EINVAL;
    if (req->width == 0 || req->height == 0) return -EINVAL;
    if (req->bpp != 16 && req->bpp != 24 && req->bpp != 32) return -EINVAL;
    if (req->flags != 0) return -EINVAL;

    uint64_t pitch = ((uint64_t)req->width * req->bpp + 7U) / 8U;
    pitch = (pitch + 63U) & ~63ULL;
    uint64_t size = pitch * req->height;
    /* 上限 64 MiB：再大就不是 dumb buffer 的用途了，也容易把 PMM 打穿。 */
    if (size == 0 || size > 64ULL * 1024 * 1024) return -EINVAL;
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);

    uint32_t slot = DRM_CARD_MAX_DUMB;
    for (uint32_t i = 0; i < DRM_CARD_MAX_DUMB; i++)
        if (!g_drm.dumb[i].used) { slot = i; break; }
    if (slot == DRM_CARD_MAX_DUMB) return -ENOSPC;

    uint64_t phys = pmm_alloc_blocks(pages);
    if (!phys) return -ENOMEM;
    /* 恒等映射：VA == PA（见文件头第 2 条）。 */
    memset((void *)(uintptr_t)phys, 0, (uint64_t)pages * PAGE_SIZE);

    drm_dumb_t *dumb = &g_drm.dumb[slot];
    memset(dumb, 0, sizeof(*dumb));
    dumb->used = 1;
    dumb->handle = g_drm.next_handle++;
    if (dumb->handle == 0) dumb->handle = g_drm.next_handle++;
    dumb->width = req->width;
    dumb->height = req->height;
    dumb->pitch = (uint32_t)pitch;
    dumb->bpp = req->bpp;
    dumb->size = size;
    dumb->phys = phys;
    dumb->pages = pages;

    memset(out, 0, sizeof(*out));
    out->handle = dumb->handle;
    out->pitch = dumb->pitch;
    out->size = dumb->size;
    return 0;
}

/** 释放 dumb buffer；仍被 fb 引用时拒绝（-EBUSY）。 */
static int drm_dumb_destroy(uint32_t handle) {
    drm_dumb_t *dumb = drm_dumb_find(handle);
    if (!dumb) return -ENOENT;
    if (drm_fb_find_handle(handle)) return -EBUSY;

    /* 曾被 mmap 的缓冲不立刻 pmm_free_blocks：内核没有 munmap 钩子，无法
     * 判断用户态是否还在写。宁可泄漏到重启，也不能把还在写的页还回去。 */
    if (!dumb->mmapped && dumb->phys)
        pmm_free_blocks(dumb->phys, dumb->pages);
    memset(dumb, 0, sizeof(*dumb));
    return 0;
}

/* ═══ ioctl 实现 ════════════════════════════════════════════════════ */

static int drm_ioctl_version(drm_version_t *v) {
    if (!v) return -EINVAL;
    static const char name[] = "hive-drm";
    static const char date[] = "20261114";
    static const char desc[] = "HBOS single-CRTC KMS (framebuffer passthrough)";

    uint64_t name_len = strlen(name);
    uint64_t date_len = strlen(date);
    uint64_t desc_len = strlen(desc);

    if (v->name && v->name_len) {
        uint64_t n = name_len < v->name_len - 1 ? name_len : v->name_len - 1;
        memcpy(v->name, name, n);
        v->name[n] = '\0';
    }
    if (v->date && v->date_len) {
        uint64_t n = date_len < v->date_len - 1 ? date_len : v->date_len - 1;
        memcpy(v->date, date, n);
        v->date[n] = '\0';
    }
    if (v->desc && v->desc_len) {
        uint64_t n = desc_len < v->desc_len - 1 ? desc_len : v->desc_len - 1;
        memcpy(v->desc, desc, n);
        v->desc[n] = '\0';
    }
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    v->name_len = name_len;
    v->date_len = date_len;
    v->desc_len = desc_len;
    return 0;
}

static int drm_ioctl_get_cap(drm_get_cap_t *cap) {
    if (!cap) return -EINVAL;
    uint64_t value;
    switch (cap->capability) {
        case DRM_CAP_DUMB_BUFFER:           value = 1; break;
        case DRM_CAP_DUMB_PREFERRED_DEPTH:  value = 32; break;
        case DRM_CAP_DUMB_PREFER_SHADOW:    value = 0; break;
        case DRM_CAP_VBLANK_HIGH_CRTC:      value = 0; break;
        case DRM_CAP_PRIME:                 value = 0; break;
        case DRM_CAP_TIMESTAMP_MONOTONIC:   value = 1; break;
        case DRM_CAP_ASYNC_PAGE_FLIP:       value = 0; break;
        case DRM_CAP_CURSOR_WIDTH:          value = 64; break;
        case DRM_CAP_CURSOR_HEIGHT:         value = 64; break;
        case DRM_CAP_ADDFB2_MODIFIERS:      value = 0; break;
        case DRM_CAP_PAGE_FLIP_TARGET:      value = 0; break;
        case DRM_CAP_CRTC_IN_VBLANK_EVENT:  value = 0; break;
        case DRM_CAP_SYNCOBJ:               value = 0; break;
        case DRM_CAP_SYNCOBJ_TIMELINE:      value = 0; break;
        default: return -EINVAL;
    }
    cap->value = value;
    return 0;
}

static int drm_ioctl_set_client_cap(const drm_get_cap_t *cap) {
    if (!cap) return -EINVAL;
    switch (cap->capability) {
        case DRM_CLIENT_CAP_STEREO_3D:
            return 0;
        case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
            if (cap->value) g_drm.client_caps |= (1U << 1);
            else g_drm.client_caps &= ~(1U << 1);
            return 0;
        case DRM_CLIENT_CAP_ASPECT_RATIO:
            return 0;
        case DRM_CLIENT_CAP_ATOMIC:
            /* 见文件头第 4 条：让客户端明确知道要走 legacy 路径。 */
            return -EINVAL;
        default:
            return -EINVAL;
    }
}

/** 数组两段式的通用写回：count 不够时只更新需求数，不算失败。 */
static int drm_ioctl_get_resources(drm_mode_card_res_t *res) {
    if (!res) return -EINVAL;

    uint32_t fb_count = 0;
    for (uint32_t i = 0; i < DRM_CARD_MAX_FB; i++)
        if (g_drm.fbs[i].used) fb_count++;

    if (res->fb_id_ptr && res->count_fbs >= fb_count) {
        uint32_t *out = (uint32_t *)(uintptr_t)res->fb_id_ptr;
        uint32_t n = 0;
        for (uint32_t i = 0; i < DRM_CARD_MAX_FB; i++)
            if (g_drm.fbs[i].used) out[n++] = g_drm.fbs[i].fb_id;
    }
    if (res->crtc_id_ptr && res->count_crtcs >= 1)
        ((uint32_t *)(uintptr_t)res->crtc_id_ptr)[0] = DRM_CARD_CRTC_ID;
    if (res->connector_id_ptr && res->count_connectors >= 1)
        ((uint32_t *)(uintptr_t)res->connector_id_ptr)[0] = DRM_CARD_CONNECTOR_ID;
    if (res->encoder_id_ptr && res->count_encoders >= 1)
        ((uint32_t *)(uintptr_t)res->encoder_id_ptr)[0] = DRM_CARD_ENCODER_ID;

    res->count_fbs = fb_count;
    res->count_crtcs = 1;
    res->count_connectors = 1;
    res->count_encoders = 1;
    res->min_width = 320;
    res->max_width = g_drm.width;
    res->min_height = 200;
    res->max_height = g_drm.height;
    return 0;
}

static int drm_ioctl_get_connector(drm_mode_get_connector_t *c) {
    if (!c) return -EINVAL;
    if (c->connector_id && c->connector_id != DRM_CARD_CONNECTOR_ID)
        return -ENOENT;

    if (c->encoders_ptr && c->count_encoders >= 1)
        ((uint32_t *)(uintptr_t)c->encoders_ptr)[0] = DRM_CARD_ENCODER_ID;
    if (c->modes_ptr && c->count_modes >= g_drm.mode_count)
        memcpy((void *)(uintptr_t)c->modes_ptr, g_drm.modes,
               (uint64_t)g_drm.mode_count * sizeof(drm_mode_modeinfo_t));

    c->connector_id = DRM_CARD_CONNECTOR_ID;
    c->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    c->connector_type_id = 1;
    c->encoder_id = DRM_CARD_ENCODER_ID;
    c->count_encoders = 1;
    c->count_modes = g_drm.mode_count;
    c->count_props = 0;
    c->connection = DRM_MODE_CONNECTED;
    /* 96 DPI 折算物理尺寸（KWin 用它算默认缩放）。 */
    c->mm_width = (uint32_t)((uint64_t)g_drm.width * 254U / 960U);
    c->mm_height = (uint32_t)((uint64_t)g_drm.height * 254U / 960U);
    c->subpixel = 0;
    c->pad = 0;
    return 0;
}

static int drm_ioctl_get_encoder(drm_mode_get_encoder_t *e) {
    if (!e) return -EINVAL;
    if (e->encoder_id && e->encoder_id != DRM_CARD_ENCODER_ID)
        return -ENOENT;
    e->encoder_id = DRM_CARD_ENCODER_ID;
    e->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
    e->crtc_id = DRM_CARD_CRTC_ID;
    e->possible_crtcs = 1U;     /* bit0 = CRTC 索引 0 */
    e->possible_clones = 0;
    return 0;
}

static int drm_ioctl_get_crtc(drm_mode_crtc_t *c) {
    if (!c) return -EINVAL;
    if (c->crtc_id && c->crtc_id != DRM_CARD_CRTC_ID) return -ENOENT;
    if (c->set_connectors_ptr && c->count_connectors >= 1)
        ((uint32_t *)(uintptr_t)c->set_connectors_ptr)[0] = DRM_CARD_CONNECTOR_ID;

    c->crtc_id = DRM_CARD_CRTC_ID;
    c->fb_id = g_drm.crtc_fb_id;
    c->x = 0;
    c->y = 0;
    c->gamma_size = 0;
    c->mode_valid = (uint32_t)(g_drm.crtc_active ? 1 : 0);
    c->count_connectors = 1;
    if (g_drm.crtc_active)
        memcpy(&c->mode, &g_drm.crtc_mode, sizeof(c->mode));
    else
        memset(&c->mode, 0, sizeof(c->mode));
    return 0;
}

static int drm_ioctl_set_crtc(const drm_mode_crtc_t *c) {
    if (!c) return -EINVAL;
    if (c->crtc_id != DRM_CARD_CRTC_ID) return -ENOENT;

    if (c->fb_id == 0) {
        g_drm.crtc_active = 0;
        g_drm.crtc_fb_id = 0;
        return 0;
    }

    drm_fb_t *fb = drm_fb_find(c->fb_id);
    if (!fb) return -ENOENT;
    if (!c->mode_valid) return -EINVAL;

    /* 只支持 1:1 呈现：模式尺寸必须等于 framebuffer 尺寸。 */
    if ((uint32_t)c->mode.hdisplay != fb->width ||
        (uint32_t)c->mode.vdisplay != fb->height)
        return -EINVAL;
    if ((uint32_t)c->mode.hdisplay != g_drm.width ||
        (uint32_t)c->mode.vdisplay != g_drm.height)
        return -EINVAL;

    if (c->set_connectors_ptr && c->count_connectors) {
        const uint32_t *ids = (const uint32_t *)(uintptr_t)c->set_connectors_ptr;
        if (ids[0] != DRM_CARD_CONNECTOR_ID) return -EINVAL;
    } else if (c->count_connectors == 0) {
        /* legacy drmModeSetCrtc 总是带 connector；缺省视为当前唯一连接器。 */
    }

    g_drm.crtc_active = 1;
    g_drm.crtc_fb_id = fb->fb_id;
    memcpy(&g_drm.crtc_mode, &c->mode, sizeof(g_drm.crtc_mode));
    g_drm.sequence++;
    drm_scanout(fb);
    return 0;
}

static int drm_ioctl_page_flip(const drm_mode_crtc_page_flip_t *flip) {
    if (!flip) return -EINVAL;
    if (flip->crtc_id != DRM_CARD_CRTC_ID) return -ENOENT;
    if (flip->flags & ~DRM_MODE_PAGE_FLIP_FLAGS) return -EINVAL;
    if (flip->flags & DRM_MODE_PAGE_FLIP_ASYNC) return -EINVAL;
    if (!g_drm.crtc_active) return -ENODEV;

    drm_fb_t *fb = drm_fb_find(flip->fb_id);
    if (!fb) return -ENOENT;
    if (fb->width != g_drm.width || fb->height != g_drm.height) return -EINVAL;

    g_drm.crtc_fb_id = fb->fb_id;
    g_drm.sequence++;
    g_drm.flips++;
    drm_scanout(fb);
    if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT)
        drm_event_push(DRM_EVENT_FLIP_COMPLETE, flip->user_data,
                       DRM_CARD_CRTC_ID);
    return 0;
}

static int drm_ioctl_addfb2(drm_mode_fb_cmd2_t *cmd) {
    if (!cmd) return -EINVAL;
    if (cmd->flags & (1U << 1)) return -EINVAL;   /* DRM_MODE_FB_MODIFIERS */

    drm_dumb_t *dumb = drm_dumb_find(cmd->handles[0]);
    if (!dumb) return -ENOENT;
    if (cmd->width == 0 || cmd->height == 0) return -EINVAL;
    if (cmd->width > dumb->width || cmd->height > dumb->height) return -EINVAL;
    if (cmd->pitches[0] && cmd->pitches[0] != dumb->pitch) return -EINVAL;
    if (cmd->offsets[0] != 0) return -EINVAL;

    switch (cmd->pixel_format) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_ARGB8888:
            if (dumb->bpp != 32) return -EINVAL;
            break;
        case DRM_FORMAT_RGB565:
            if (dumb->bpp != 16) return -EINVAL;
            break;
        default:
            return -EINVAL;
    }

    uint32_t slot = DRM_CARD_MAX_FB;
    for (uint32_t i = 0; i < DRM_CARD_MAX_FB; i++)
        if (!g_drm.fbs[i].used) { slot = i; break; }
    if (slot == DRM_CARD_MAX_FB) return -ENOSPC;

    drm_fb_t *fb = &g_drm.fbs[slot];
    memset(fb, 0, sizeof(*fb));
    fb->used = 1;
    fb->fb_id = g_drm.next_fb_id++;
    if (fb->fb_id == 0) fb->fb_id = g_drm.next_fb_id++;
    fb->handle = dumb->handle;
    fb->width = cmd->width;
    fb->height = cmd->height;
    fb->pitch = dumb->pitch;
    fb->pixel_format = cmd->pixel_format;
    cmd->fb_id = fb->fb_id;
    return 0;
}

static int drm_ioctl_getfb2(drm_mode_fb_cmd2_t *cmd) {
    if (!cmd) return -EINVAL;
    drm_fb_t *fb = drm_fb_find(cmd->fb_id);
    if (!fb) return -ENOENT;
    drm_dumb_t *dumb = drm_dumb_find(fb->handle);
    if (!dumb) return -ENOENT;

    cmd->width = fb->width;
    cmd->height = fb->height;
    cmd->pixel_format = fb->pixel_format;
    cmd->flags = 0;
    cmd->handles[0] = dumb->handle;
    cmd->pitches[0] = dumb->pitch;
    cmd->offsets[0] = 0;
    cmd->modifier[0] = DRM_FORMAT_MOD_LINEAR;
    for (int i = 1; i < 4; i++) {
        cmd->handles[i] = 0;
        cmd->pitches[i] = 0;
        cmd->offsets[i] = 0;
        cmd->modifier[i] = 0;
    }
    return 0;
}

static int drm_ioctl_map_dumb(drm_mode_map_dumb_t *map) {
    if (!map) return -EINVAL;
    drm_dumb_t *dumb = drm_dumb_find(map->handle);
    if (!dumb) return -ENOENT;
    dumb->mmapped = 1;
    map->offset = (uint64_t)dumb->handle << DRM_CARD_DUMB_OFFSET_SHIFT;
    return 0;
}

static int drm_ioctl_wait_vblank(drm_wait_vblank_t *v) {
    if (!v) return -EINVAL;
    uint32_t type = v->request.type;
    if (type & DRM_VBLANK_EVENT) {
        drm_event_push(DRM_EVENT_VBLANK, 0, DRM_CARD_CRTC_ID);
        v->reply.type = type;
        v->reply.sequence = ++g_drm.sequence;
        v->reply.tval_sec = 0;
        v->reply.tval_usec = 0;
        return 0;
    }
    if (type & DRM_VBLANK_RELATIVE) {
        v->reply.sequence = g_drm.sequence + v->request.sequence;
    } else {
        v->reply.sequence = v->request.sequence ? v->request.sequence
                                               : g_drm.sequence;
    }
    g_drm.sequence = v->reply.sequence;
    v->reply.type = type & ~DRM_VBLANK_FLIP;
    v->reply.tval_sec = 0;
    v->reply.tval_usec = 0;
    return 0;
}

static int drm_card_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    (void)node;
    if (!g_drm.ready) return -ENODEV;
    if (DRM_CMD_TYPE(cmd) != (uint32_t)DRM_IOCTL_BASE) return -ENOTTY;

    switch (DRM_CMD_NR(cmd)) {
        case 0x00: return drm_ioctl_version((drm_version_t *)arg);
        case 0x0c: return drm_ioctl_get_cap((drm_get_cap_t *)arg);
        case 0x0d: return drm_ioctl_set_client_cap((const drm_get_cap_t *)arg);
        case 0x1e: g_drm.master = 1; return 0;    /* SET_MASTER */
        case 0x1f: g_drm.master = 0; return 0;    /* DROP_MASTER */
        case 0x3a: return drm_ioctl_wait_vblank((drm_wait_vblank_t *)arg);
        case 0xA0: return drm_ioctl_get_resources((drm_mode_card_res_t *)arg);
        case 0xA1: return drm_ioctl_get_crtc((drm_mode_crtc_t *)arg);
        case 0xA2: return drm_ioctl_set_crtc((const drm_mode_crtc_t *)arg);
        case 0xA6: return drm_ioctl_get_encoder((drm_mode_get_encoder_t *)arg);
        case 0xA7: return drm_ioctl_get_connector((drm_mode_get_connector_t *)arg);
        case 0xB0: return drm_ioctl_page_flip((const drm_mode_crtc_page_flip_t *)arg);
        case 0xB2: {
            if (!arg) return -EINVAL;
            return drm_dumb_create((const drm_mode_create_dumb_t *)arg,
                                   (drm_mode_create_dumb_t *)arg);
        }
        case 0xB3: return drm_ioctl_map_dumb((drm_mode_map_dumb_t *)arg);
        case 0xB4: {
            if (!arg) return -EINVAL;
            return drm_dumb_destroy(((const drm_mode_destroy_dumb_t *)arg)->handle);
        }
        case 0xB8: return drm_ioctl_addfb2((drm_mode_fb_cmd2_t *)arg);
        case 0xCE: return drm_ioctl_getfb2((drm_mode_fb_cmd2_t *)arg);
        case 0xAF: {   /* RMFB */
            if (!arg) return -EINVAL;
            uint32_t fb_id = *(const uint32_t *)arg;
            if (fb_id == g_drm.crtc_fb_id) return -EBUSY;
            drm_fb_t *fb = drm_fb_find(fb_id);
            if (!fb) return -ENOENT;
            memset(fb, 0, sizeof(*fb));
            return 0;
        }
        case 0x09: {   /* GEM_CLOSE */
            if (!arg) return -EINVAL;
            return drm_dumb_destroy(((const drm_gem_close_t *)arg)->handle);
        }
        case 0xA3:   /* MODE_CURSOR：无硬件光标 */
        case 0xAA:   /* MODE_GETPROPERTY：无属性模型 */
        case 0xAB:   /* MODE_SETPROPERTY */
        case 0xAC:   /* MODE_GETPROPBLOB */
        case 0xAD:   /* MODE_GETFB（legacy 1-plane，直接不支持） */
        case 0xAE:   /* MODE_ADDFB（legacy） */
        case 0xB5:   /* MODE_GETPLANERESOURCES */
        case 0xB6:   /* MODE_GETPLANE */
        case 0xB7:   /* MODE_SETPLANE */
        case 0xB9:   /* MODE_OBJ_GETPROPERTIES */
        case 0xBA:   /* MODE_OBJ_SETPROPERTY */
        case 0xBC:   /* MODE_ATOMIC：明确拒绝，走 legacy */
            return -EINVAL;
        default:
            return -ENOTTY;
    }
}

/** 一次 read 交一个完整事件；没有事件时返回 0（libdrm 视为"暂无"）。 */
static int drm_card_read(vfs_node_t *node, uint32_t offset, void *buf,
                         uint32_t count) {
    (void)node;
    (void)offset;
    if (!buf) return -EINVAL;
    if (!g_drm.ready) return -ENODEV;
    if (g_drm.ev_count == 0) return 0;
    if (count < sizeof(drm_event_vblank_t)) return -EINVAL;

    drm_event_slot_t *e = &g_drm.events[g_drm.ev_head];
    drm_event_vblank_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.base.type = e->type;
    ev.base.length = (uint32_t)sizeof(ev);
    ev.user_data = e->user_data;
    ev.sequence = e->sequence;
    ev.crtc_id = e->crtc_id;
    memcpy(buf, &ev, sizeof(ev));

    memset(e, 0, sizeof(*e));
    g_drm.ev_head = (g_drm.ev_head + 1U) % DRM_CARD_MAX_EVENTS;
    g_drm.ev_count--;
    return (int)sizeof(ev);
}

/* ═══ 自检 ══════════════════════════════════════════════════════════ */

static int g_selftest_failures;

#define DRM_SELFTEST_CHECK(expr)                                          \
    do {                                                                  \
        if (expr) {                                                       \
            console_puts("[DRM] PASS " #expr "\n");                        \
        } else {                                                          \
            console_puts("[DRM] FAIL " #expr "\n");                        \
            g_selftest_failures++;                                        \
        }                                                                 \
    } while (0)

/**
 * 自检不依赖 QEMU：全部走 ioctl 入口，和用户态 libdrm 的调用顺序一致。
 * 若 graphics 尚未初始化，用一个 640x480 的伪造状态跑状态机（不碰屏幕）。
 */
int drm_card_selftest(void) {
    g_selftest_failures = 0;

    if (!g_drm.ready && drm_card_init() < 0) {
        memset(&g_drm, 0, sizeof(g_drm));
        g_drm.next_handle = 1;
        g_drm.next_fb_id = 1;
        drm_card_configure(640, 480, 640 * 4U, 32, NULL, 0);
        console_puts("[DRM] no framebuffer, using synthetic 640x480\n");
    }

    drm_version_t ver;
    memset(&ver, 0, sizeof(ver));
    char name[32];
    ver.name = name;
    ver.name_len = sizeof(name);
    DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_VERSION, &ver) == 0);
    DRM_SELFTEST_CHECK(strcmp(name, "hive-drm") == 0);

    drm_get_cap_t cap;
    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_DUMB_BUFFER;
    DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_GET_CAP, &cap) == 0);
    DRM_SELFTEST_CHECK(cap.value == 1);

    drm_mode_card_res_t res;
    memset(&res, 0, sizeof(res));
    DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0);
    DRM_SELFTEST_CHECK(res.count_crtcs == 1 && res.count_connectors == 1 &&
                       res.count_encoders == 1);

    drm_mode_get_connector_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.connector_id = DRM_CARD_CONNECTOR_ID;
    DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0);
    DRM_SELFTEST_CHECK(conn.connection == DRM_MODE_CONNECTED &&
                       conn.count_modes >= 1 && conn.modes_ptr == 0);

    drm_mode_modeinfo_t mode;
    memset(&mode, 0, sizeof(mode));
    conn.modes_ptr = (uint64_t)(uintptr_t)&mode;
    conn.count_modes = 1;
    DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0);
    DRM_SELFTEST_CHECK(mode.hdisplay == g_drm.width &&
                       mode.vdisplay == g_drm.height);

    drm_mode_create_dumb_t dumb;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width = g_drm.width;
    dumb.height = g_drm.height;
    dumb.bpp = 32;
    DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) == 0);
    DRM_SELFTEST_CHECK(dumb.handle != 0 && dumb.size >= (uint64_t)g_drm.width * 4U * g_drm.height);

    drm_mode_map_dumb_t map;
    memset(&map, 0, sizeof(map));
    map.handle = dumb.handle;
    DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0);
    DRM_SELFTEST_CHECK(drm_page_phys_for_offset(map.offset) != 0);
    DRM_SELFTEST_CHECK(drm_page_phys_for_offset(map.offset + 4096) != 0 ||
                       dumb.size <= 4096);
    /* 节点身份判定：伪造一个只带 ops 的栈上节点。 */
    vfs_node_t probe;
    memset(&probe, 0, sizeof(probe));
    DRM_SELFTEST_CHECK(drm_card_is_card_node(&probe) == 0);
    probe.ops = &g_drm_card_ops;
    DRM_SELFTEST_CHECK(drm_card_is_card_node(&probe) == 1);

    /* 往 dumb buffer 里写可识别的图样（恒等映射，直接写物理地址）。 */
    drm_dumb_t *db = drm_dumb_find(dumb.handle);
    DRM_SELFTEST_CHECK(db != NULL);
    if (db) {
        uint32_t *px = (uint32_t *)(uintptr_t)db->phys;
        for (uint32_t i = 0; i < 16; i++) px[i] = 0x00FF00FFU;

        drm_mode_fb_cmd2_t fb;
        memset(&fb, 0, sizeof(fb));
        fb.width = db->width;
        fb.height = db->height;
        fb.pixel_format = DRM_FORMAT_XRGB8888;
        fb.handles[0] = db->handle;
        fb.pitches[0] = db->pitch;
        DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_ADDFB2, &fb) == 0);
        DRM_SELFTEST_CHECK(fb.fb_id != 0);

        drm_mode_crtc_t crtc;
        memset(&crtc, 0, sizeof(crtc));
        crtc.crtc_id = DRM_CARD_CRTC_ID;
        crtc.fb_id = fb.fb_id;
        crtc.mode_valid = 1;
        crtc.mode = mode;
        DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0);
        DRM_SELFTEST_CHECK(g_drm.crtc_active == 1 &&
                           g_drm.crtc_fb_id == fb.fb_id);

        /* GETCRTC 回读：KWin 会用它确认模式已生效。 */
        drm_mode_crtc_t readback;
        memset(&readback, 0, sizeof(readback));
        readback.crtc_id = DRM_CARD_CRTC_ID;
        DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_GETCRTC, &readback) == 0);
        DRM_SELFTEST_CHECK(readback.mode_valid == 1 &&
                           readback.fb_id == fb.fb_id);

        drm_mode_crtc_page_flip_t flip;
        memset(&flip, 0, sizeof(flip));
        flip.crtc_id = DRM_CARD_CRTC_ID;
        flip.fb_id = fb.fb_id;
        flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
        flip.user_data = 0x12345678ULL;
        DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0);
        DRM_SELFTEST_CHECK(g_drm.ev_count == 1);

        drm_event_vblank_t ev;
        memset(&ev, 0, sizeof(ev));
        DRM_SELFTEST_CHECK(drm_card_read(NULL, 0, &ev, sizeof(ev)) ==
                           (int)sizeof(ev));
        DRM_SELFTEST_CHECK(ev.base.type == DRM_EVENT_FLIP_COMPLETE &&
                           ev.user_data == 0x12345678ULL);

        /* 若接的是真实 framebuffer，第一个像素必须已经是图样色。 */
        if (g_drm.have_real_fb && g_drm.fb_addr) {
            DRM_SELFTEST_CHECK(g_drm.fb_addr[0] == 0x00FF00FFU);
        } else {
            console_puts("[DRM] SKIP scanout check (no real framebuffer)\n");
        }

        /* 清理：RMFB 后才能 DESTROY_DUMB。 */
        uint32_t fb_id = fb.fb_id;
        g_drm.crtc_active = 0;
        g_drm.crtc_fb_id = 0;
        DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_RMFB, &fb_id) == 0);
        drm_mode_destroy_dumb_t destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = db->handle;
        DRM_SELFTEST_CHECK(drm_card_ioctl(NULL, DRM_IOCTL_MODE_DESTROY_DUMB,
                                          &destroy) == 0);
    }

    if (g_selftest_failures == 0) {
        console_puts("[DRM] selftest: all checks passed\n");
        return 0;
    }
    console_puts("[DRM] selftest: failures=");
    char digits[12];
    uint32_t n = 0;
    uint32_t v = (uint32_t)g_selftest_failures;
    do { digits[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 10);
    while (n) console_putchar(digits[--n]);
    console_putchar('\n');
    return -1;
}
