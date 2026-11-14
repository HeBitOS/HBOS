/**
 * @file    fbdev.c
 * @brief   /dev/fb0 Linux fbdev 兼容设备实现
 */
#include "fbdev.h"
#include "graphics.h"
#include "../core/vmm.h"
#include "../core/task.h"
#include "../devfs.h"
#include "../errno.h"
#include "../string.h"
#include "../sys/stat.h"

#define FBIOGET_VSCREENINFO 0x4600U
#define FBIOPUT_VSCREENINFO 0x4601U
#define FBIOGET_FSCREENINFO 0x4602U
#define FBIOPAN_DISPLAY     0x4606U
#define FBIOBLANK           0x4611U
#define FBIO_WAITFORVSYNC   0x40044620U

#define FB_TYPE_PACKED_PIXELS 0U
#define FB_VISUAL_TRUECOLOR   2U

typedef struct {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
} linux_fb_bitfield_t;

typedef struct {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    linux_fb_bitfield_t red;
    linux_fb_bitfield_t green;
    linux_fb_bitfield_t blue;
    linux_fb_bitfield_t transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
} linux_fb_var_screeninfo_t;

typedef struct {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint16_t pad;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
    uint16_t tail_pad;
} linux_fb_fix_screeninfo_t;

_Static_assert(sizeof(linux_fb_var_screeninfo_t) == 160,
               "linux_fb_var_screeninfo_t must match x86_64 Linux ABI (160B)");
_Static_assert(sizeof(linux_fb_fix_screeninfo_t) == 80,
               "linux_fb_fix_screeninfo_t must match x86_64 Linux ABI (80B)");

static vfs_node_t *g_fbdev_node = NULL;

static uint32_t fbdev_smem_len(const fb_info_t *info) {
    uint64_t bytes = info->pitch * info->height;
    return (uint32_t)((bytes + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
}

static void fbdev_fill_var(const fb_info_t *info,
                           linux_fb_var_screeninfo_t *var) {
    memset(var, 0, sizeof(*var));
    var->xres = (uint32_t)info->width;
    var->yres = (uint32_t)info->height;
    var->xres_virtual = (uint32_t)info->width;
    var->yres_virtual = (uint32_t)info->height;
    var->bits_per_pixel = info->bpp ? info->bpp : 32U;
    var->red.offset = 16;
    var->red.length = 8;
    var->green.offset = 8;
    var->green.length = 8;
    var->blue.offset = 0;
    var->blue.length = 8;
    var->transp.offset = 24;
    var->transp.length = 8;
    var->height = UINT32_MAX;
    var->width = UINT32_MAX;
}

static int fbdev_read(vfs_node_t *node, uint32_t offset, void *buf,
                      uint32_t count) {
    (void)node;
    fb_info_t info;
    if (!buf || fb_get_info(&info) < 0 || !info.addr) return -EIO;
    uint32_t total = fbdev_smem_len(&info);
    if (offset >= total) return 0;
    if (count > total - offset) count = total - offset;
    memcpy(buf, (const uint8_t *)info.addr + offset, count);
    return (int)count;
}

static int fbdev_write(vfs_node_t *node, uint32_t offset, const void *buf,
                       uint32_t count) {
    (void)node;
    fb_info_t info;
    if (!buf || fb_get_info(&info) < 0 || !info.addr) return -EIO;
    uint32_t total = fbdev_smem_len(&info);
    if (offset >= total) return 0;
    if (count > total - offset) count = total - offset;
    memcpy((uint8_t *)info.addr + offset, buf, count);
    return (int)count;
}

static int fbdev_ioctl(vfs_node_t *node, uint32_t cmd, void *arg) {
    fb_info_t info;
    if (fb_get_info(&info) < 0 || !info.addr) return -ENODEV;
    if (node) node->size = fbdev_smem_len(&info);

    switch (cmd) {
    case FBIOGET_VSCREENINFO: {
        if (!arg) return -EFAULT;
        fbdev_fill_var(&info, (linux_fb_var_screeninfo_t *)arg);
        return 0;
    }
    case FBIOPUT_VSCREENINFO: {
        if (!arg) return -EFAULT;
        linux_fb_var_screeninfo_t *var = (linux_fb_var_screeninfo_t *)arg;
        if (var->xres && var->yres &&
            (var->xres != info.width || var->yres != info.height)) {
            if (fb_set_resolution(var->xres, var->yres) == 0)
                fb_get_info(&info);
        }
        if (node) node->size = fbdev_smem_len(&info);
        fbdev_fill_var(&info, var);
        return 0;
    }
    case FBIOGET_FSCREENINFO: {
        if (!arg) return -EFAULT;
        linux_fb_fix_screeninfo_t *fix = (linux_fb_fix_screeninfo_t *)arg;
        memset(fix, 0, sizeof(*fix));
        memcpy(fix->id, "hbosfb", 7);
        fix->smem_start = vmm_get_phys((uint64_t)(uintptr_t)info.addr);
        fix->smem_len = fbdev_smem_len(&info);
        fix->type = FB_TYPE_PACKED_PIXELS;
        fix->visual = FB_VISUAL_TRUECOLOR;
        fix->line_length = (uint32_t)info.pitch;
        return 0;
    }
    case FBIOPAN_DISPLAY:
    case FBIOBLANK:
        return 0;
    case FBIO_WAITFORVSYNC:
        task_yield();
        return 0;
    default:
        return -ENOTTY;
    }
}

static const vfs_ops_t fbdev_ops = {
    .read = fbdev_read,
    .write = fbdev_write,
    .ioctl = fbdev_ioctl,
};

void fbdev_init(void) {
    vfs_node_t *node = devfs_new_node();
    if (!node) return;
    strncpy(node->name, "/dev/fb0", VFS_MAX_NAME - 1);
    node->name[VFS_MAX_NAME - 1] = '\0';
    node->type = VFS_NODE_CHARDEV;
    node->mode = S_IFCHR | 0666;
    node->ops = &fbdev_ops;
    fb_info_t info;
    if (fb_get_info(&info) == 0 && info.addr)
        node->size = fbdev_smem_len(&info);
    g_fbdev_node = node;
}

int fbdev_is_node(vfs_node_t *node) {
    return node && node == g_fbdev_node;
}

uint64_t fbdev_page_phys(vfs_node_t *node, uint64_t page_index) {
    if (!fbdev_is_node(node)) return 0;
    fb_info_t info;
    if (fb_get_info(&info) < 0 || !info.addr) return 0;
    uint32_t total = fbdev_smem_len(&info);
    if (page_index * PAGE_SIZE >= total) return 0;
    return vmm_get_phys((uint64_t)(uintptr_t)info.addr + page_index * PAGE_SIZE);
}
