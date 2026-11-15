/**
 * @file drm_host_test.c
 * @brief DRM/KMS 状态机的宿主单测骨架（不参与内核构建）
 *
 * 用法（在仓库根目录）：
 *   gcc -O2 -Wall -Wextra -Isrc -Isrc/graphics -Isrc/core -Isrc/shell \
 *       -Isrc/gui -Isrc/user -Isrc/graphics/drm \
 *       -DDRM_HOST_TEST=1 \
 *       src/graphics/drm/drm_host_test.c src/graphics/drm/drm_card.c \
 *       -o build/drm_host_test && ./build/drm_host_test
 *
 * 为什么单独放一个文件而不是 tests/host/：drm 模块的归属目录是
 * src/graphics/drm/，测试与实现放一起便于"改一行就能重跑"；整份文件用
 * DRM_HOST_TEST 包住，任何人不小心把它加进 C_SRCS 也只会得到一个空 TU，
 * 不会和内核符号冲突。是否迁到 tests/host/ 由 Lead 决定。
 */
#ifdef DRM_HOST_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "drm.h"
#include "evdev.h"
#include "graphics/graphics.h"
#include "vfs.h"
#include "input/mouse.h"

/* ═══ 被替换的内核符号 ══════════════════════════════════════════════
 * 宿主上用 malloc 代替 PMM；"物理地址"就是 malloc 返回的指针值，因为
 * drm_card.c 依赖恒等映射（VA == PA）直接读写它。
 */

void *pmm_alloc_blocks_stub(size_t count) {
    return calloc(1, count * 4096U);
}

uint64_t pmm_alloc_blocks(size_t count) {
    return (uint64_t)(uintptr_t)pmm_alloc_blocks_stub(count);
}

void pmm_free_blocks(uint64_t phys_addr, size_t count) {
    (void)count;
    free((void *)(uintptr_t)phys_addr);
}

uint64_t pmm_alloc_page(void) {
    return (uint64_t)(uintptr_t)pmm_alloc_blocks_stub(1);
}

void pmm_free_page(uint64_t phys_addr) {
    free((void *)(uintptr_t)phys_addr);
}

void console_puts(const char *s) { fputs(s, stdout); }
void console_putchar(char c) { fputc(c, stdout); }

/* 伪造 64x64 的"真实 framebuffer"：drm_card_selftest 会检查第一个像素
 * 是否真的被 dumb buffer 的图样覆盖，这是"一次关键 modeset"的证据。 */
static uint32_t g_fake_fb[64 * 64];

/* evdev 的降级轮询源：宿主上不产生事件。 */
int kb_poll_key(void) { return 0; }
int kb_ctrl_pressed(void) { return 0; }
int kb_alt_pressed(void) { return 0; }
int kb_super_pressed(void) { return 0; }
int mouse_poll(mouse_event_t *ev) { (void)ev; return 0; }

int fb_get_info(fb_info_t *info) {
    if (!info) return -1;
    info->addr = g_fake_fb;
    info->width = 64;
    info->height = 64;
    info->pitch = 64U * 4U;
    info->bpp = 32;
    return 0;
}

int main(void) {
    printf("== drm_card_selftest (host) ==\n");
    int rc = drm_card_selftest();
    drm_card_status_t st;
    drm_card_status(&st);
    printf("status: ready=%u %ux%u pitch=%u active=%u fb=%u flips=%u dumb=%u fb_used=%u ev=%u\n",
           st.ready, st.width, st.height, st.pitch, st.crtc_active,
           st.crtc_fb_id, st.flips, st.dumb_used, st.fb_used, st.events_pending);

    /* 自检只覆盖"一次成功路径"。再补三条负例，防止状态机变得过于宽松。 */
    int failures = 0;
    if (st.crtc_active != 0 || st.dumb_used != 0 || st.fb_used != 0) {
        printf("FAIL: selftest 未清理干净 (active=%u dumb=%u fb=%u)\n",
               st.crtc_active, st.dumb_used, st.fb_used);
        failures++;
    }

    drm_mode_create_dumb_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.width = 0;
    bad.height = 10;
    bad.bpp = 32;
    if (drm_card_selftest() != 0) { /* 重置内部状态（重复调用必须幂等） */
        printf("FAIL: selftest 第二次调用失败\n");
        failures++;
    }
    printf("== evdev_selftest (host) ==\n");
    int ev_rc = evdev_selftest();

    printf("== %s ==\n",
           (rc == 0 && ev_rc == 0 && failures == 0) ? "ALL PASS" : "FAILED");
    return (rc == 0 && ev_rc == 0 && failures == 0) ? 0 : 1;
}

#endif /* DRM_HOST_TEST */
