; ============================================================
; GRUB BIOS 引导块（boot.img + core.img）— 经 .incbin 嵌入内核
;
; `install auto` 让磁盘具备独立引导能力需要两类东西：
;   1. MBR 引导代码（GRUB 的 boot.img 前 446 字节）：BIOS 读完 MBR 后
;      跳进来，由它加载 core.img；
;   2. core.img（LBA1 起，落在 MBR 后的空隙里）：GRUB 的核心镜像，
;      内嵌了搜寻 /boot/grub/grub.cfg 并 chainload 内核的最小配置。
;
; 这两块由宿主在构建期用 grub-mkimage / grub-bios-setup 的原料生成
; （见 Makefile 的 grub-boot-blobs 规则）。guest 内核里没有 ISO9660
; 也没有 ATAPI，拿不到 CD 上的文件，所以必须在构建期嵌进来。
;
; 布局约定（install 侧照此写入）：
;   LBA 0          : boot.img 前 446 字节（引导代码；分区表与 55AA 另写）
;   LBA 1 .. N     : core.img 原始字节
; ============================================================
section .rodata
bits 64

%ifndef HBOS_BUILD_DIR
%define HBOS_BUILD_DIR "build"
%endif
%strcat HBOS_GRUB_BOOTIMG_PATH HBOS_BUILD_DIR, "/grub/boot.img"
%strcat HBOS_GRUB_COREIMG_PATH HBOS_BUILD_DIR, "/grub/core.img"

global _binary_build_grub_boot_img_start
_binary_build_grub_boot_img_start:
incbin HBOS_GRUB_BOOTIMG_PATH

global _binary_build_grub_boot_img_end
_binary_build_grub_boot_img_end:

global _binary_build_grub_core_img_start
_binary_build_grub_core_img_start:
incbin HBOS_GRUB_COREIMG_PATH

global _binary_build_grub_core_img_end
_binary_build_grub_core_img_end:
