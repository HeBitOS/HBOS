CC = gcc
AS = nasm
LD = ld

BUILD_DIR = build
SRC_DIR = src
HBOS_VERSION = v0.1-beta3-pre3

KERNEL_BIOS = $(BUILD_DIR)/hbos-bios.bin
ISO_BIOS = $(BUILD_DIR)/hbos-bios.iso
ISO_UEFI = $(BUILD_DIR)/hbos-uefi.iso
UEFI_IMG = $(BUILD_DIR)/hbos_uefi.img
DISK_IMG = $(BUILD_DIR)/hbos_disk.img
INSTALL_IMG = $(BUILD_DIR)/hbos_installed.img
INSTALL_IMG_BIOS = $(BUILD_DIR)/hbos_installed_bios.img
INSTALL_IMG_UEFI = $(BUILD_DIR)/hbos_installed_uefi.img
VMWARE_BIOS_VMDK = $(BUILD_DIR)/hbos_vmware_bios.vmdk
VMWARE_UEFI_VMDK = $(BUILD_DIR)/hbos_vmware_uefi.vmdk
VBOX_BIOS_VDI = $(BUILD_DIR)/hbos_virtualbox_bios.vdi
VBOX_UEFI_VDI = $(BUILD_DIR)/hbos_virtualbox_uefi.vdi
LIMINE_EFI = limine-bin/bin/BOOTX64.EFI
UEFI_CD_IMG = $(BUILD_DIR)/limine_uefi_cd.img
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
QEMU_IMG ?= qemu-img

CFLAGS = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mcmodel=kernel -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra \
         -I$(SRC_DIR) -I$(SRC_DIR)/graphics -I$(SRC_DIR)/shell -I$(SRC_DIR)/core -I$(SRC_DIR)/tools -I$(SRC_DIR)/gui -I$(SRC_DIR)/user

ASFLAGS = -f elf64
LDFLAGS_BIOS = -m elf_x86_64 -static -Bsymbolic -nostdlib -T linker_bios.ld

QEMU = /usr/bin/qemu-system-x86_64
QEMU_ENV = env -u SNAP -u SNAP_NAME -u SNAP_REVISION -u SNAP_ARCH -u SNAP_INSTANCE_NAME \
           -u SNAP_COMMON -u SNAP_DATA -u SNAP_USER_COMMON -u SNAP_USER_DATA \
           -u SNAP_LIBRARY_PATH -u GTK_PATH -u GTK_EXE_PREFIX -u GIO_MODULE_DIR \
           -u GTK_IM_MODULE_FILE -u LOCPATH -u LD_LIBRARY_PATH -u LD_PRELOAD

# 字体文件
FONT_TTF = fonts/ZhengGeDianHei-16.ttf
FONT_BIN = $(BUILD_DIR)/font_cjk.bin
# GUI proportional anti-aliased font (HarmonyOS Sans SC). Swap GUI_FONT_TTF to
# change the GUI typeface; glyph coverage is decided by tools/genfont.py.
GUI_FONT_TTF = fonts/HarmonyOS_Sans_SC_Regular.ttf
GUI_FONT_BIN = $(BUILD_DIR)/gui_font.bin
# Desktop wallpaper. Swap GUI_WALL_IMG to change it; genwall.py decodes/resizes.
GUI_WALL_IMG = photo/壁纸.jpg
GUI_WALL_BIN = $(BUILD_DIR)/gui_wall.bin

# 所有 C 源文件
APP_SRCS = $(wildcard $(SRC_DIR)/apps/*.c)

C_SRCS = \
	$(SRC_DIR)/kernel.c \
	$(SRC_DIR)/selftest.c \
	$(SRC_DIR)/acpi.c \
	$(SRC_DIR)/syscall.c \
	$(SRC_DIR)/elf.c \
	$(SRC_DIR)/devfs.c \
	$(SRC_DIR)/rtc.c \
	$(SRC_DIR)/ipc.c \
	$(SRC_DIR)/pci.c \
	$(SRC_DIR)/net.c \
	$(SRC_DIR)/tls.c \
	$(SRC_DIR)/block.c \
	$(SRC_DIR)/ahci.c \
	$(SRC_DIR)/fs.c \
	$(SRC_DIR)/vfs.c \
	$(SRC_DIR)/ata.c \
	$(SRC_DIR)/fb.c \
	$(SRC_DIR)/flanterm.c \
	$(SRC_DIR)/xhci.c \
	$(SRC_DIR)/usb_hid.c \
	$(SRC_DIR)/usb_msc.c \
	$(SRC_DIR)/smp.c \
	$(SRC_DIR)/ext2.c \
	$(SRC_DIR)/fat32.c \
	$(SRC_DIR)/tty.c \
	$(SRC_DIR)/graphics/graphics.c \
	$(SRC_DIR)/graphics/font_cjk.c \
	$(SRC_DIR)/graphics/gui_font.c \
	$(SRC_DIR)/graphics/gui_wall.c \
	$(SRC_DIR)/input/mouse.c \
	$(SRC_DIR)/shell/shell.c \
	$(SRC_DIR)/core/task.c \
	$(SRC_DIR)/lib/posix.c \
	$(SRC_DIR)/lib/string.c \
	$(SRC_DIR)/crypto/sha256.c \
	$(SRC_DIR)/crypto/x25519.c \
	$(SRC_DIR)/crypto/chacha20_poly1305.c \
	$(SRC_DIR)/user/app_runtime.c \
	$(SRC_DIR)/user/syscall.c \
	$(SRC_DIR)/user/ldso.c \
	$(SRC_DIR)/tools/help.c \
	$(SRC_DIR)/tools/system.c \
	$(SRC_DIR)/tools/debug.c \
	$(SRC_DIR)/tools/history.c \
	$(SRC_DIR)/tools/file.c \
	$(SRC_DIR)/tools/app.c \
	$(SRC_DIR)/tools/ata.c \
	$(SRC_DIR)/tools/disk.c \
	$(SRC_DIR)/tools/net.c \
	$(SRC_DIR)/tools/gui.c \
	$(SRC_DIR)/tools/editor.c \
	$(SRC_DIR)/tools/cc.c \
	$(SRC_DIR)/tools/python.c \
	$(SRC_DIR)/tools/cppe.c \
	$(SRC_DIR)/gui/wm.c \
	$(SRC_DIR)/gui/compositor.c \
	$(SRC_DIR)/gui/effects.c \
	$(SRC_DIR)/gui/gui_dirty.c \
	$(SRC_DIR)/gui/gui_apps.c \
	$(SRC_DIR)/gui/apps/app_calc.c \
	$(SRC_DIR)/gui/apps/app_clock.c \
	$(SRC_DIR)/gpu.c \
	$(APP_SRCS)

C_OBJS = $(C_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# 所有汇编源文件
C_SRCS += \
	$(SRC_DIR)/core/gdt_idt.c \
	$(SRC_DIR)/core/pmm.c \
	$(SRC_DIR)/core/vmm.c \
	$(SRC_DIR)/core/heap.c

ASM_SRCS = \
	$(SRC_DIR)/boot.asm \
	$(SRC_DIR)/core/task_switch.asm \
	$(SRC_DIR)/core/interrupt_asm.asm \
	$(SRC_DIR)/graphics/cjk_glyph.asm \
	$(SRC_DIR)/graphics/gui_glyph.asm \
	$(SRC_DIR)/graphics/gui_wallimg.asm \
	$(SRC_DIR)/smp_trampoline.asm

ASM_OBJS = $(ASM_SRCS:$(SRC_DIR)/%.asm=$(BUILD_DIR)/%.o)

ALL_OBJS = $(C_OBJS) $(ASM_OBJS)

.PHONY: all clean run vm run-bios run-iso run-bios-nodisk run-bios-disk run-bios-ahci install-img vmware-bios vmware-uefi vbox-bios vbox-uefi release smoke run-hdd run-hdd-bios run-hdd-uefi iso bios-iso uefi uefi-iso uefi-img disk-img run-uefi run-iso-uefi run-uefi-nodisk run-uefi-headless run-uefi-disk run-uefi-ahci run-uefi-img limine-uefi help font user-progs user-progs-clean

all: iso

help:
	@echo "HBOS common targets:"
	@echo "  make            Build BIOS and UEFI ISOs"
	@echo "  make run        Boot BIOS hard disk image in QEMU"
	@echo "  make run-uefi   Boot UEFI hard disk image in QEMU"
	@echo "  make release    Build ISOs plus VMware/VirtualBox disk images"
	@echo "  make smoke      Build release and boot-test BIOS/UEFI ISO/HDD/VMDK"
	@echo "  make clean      Clean build files"
	@echo ""
	@echo "Individual artifacts:"
	@echo "  make bios-iso       Build build/hbos-bios.iso"
	@echo "  make uefi-iso       Build build/hbos-uefi.iso"
	@echo "  make install-img    Build BIOS/UEFI bootable hard disk images"
	@echo "  make vmware-bios    Build VMware BIOS VMDK"
	@echo "  make vmware-uefi    Build VMware UEFI VMDK"
	@echo "  make vbox-bios      Build VirtualBox BIOS VDI"
	@echo "  make vbox-uefi      Build VirtualBox UEFI VDI"
	@echo "  make disk-img       Build blank HBFS data disk"
	@echo ""
	@echo "Debug boot targets:"
	@echo "  make run-iso        Boot BIOS ISO with persistent AHCI data disk"
	@echo "  make run-iso-uefi   Boot UEFI ISO with persistent AHCI data disk"
	@echo "  make run-bios-nodisk"
	@echo "  make run-uefi-nodisk"
	@echo "  make run-uefi-headless"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(BUILD_DIR)/graphics $(BUILD_DIR)/input $(BUILD_DIR)/shell $(BUILD_DIR)/core $(BUILD_DIR)/tools $(BUILD_DIR)/lib $(BUILD_DIR)/user $(BUILD_DIR)/apps $(BUILD_DIR)/gui $(BUILD_DIR)/crypto

# Font generation
font: $(FONT_BIN) $(GUI_FONT_BIN) $(GUI_WALL_BIN)

$(FONT_BIN): $(FONT_TTF) tools/genhzk.py
	@echo "[MAKE] Generating CJK font bitmap..."
	python3 tools/genhzk.py $(FONT_TTF) $(FONT_BIN)
	@echo "[MAKE] CJK font: $(FONT_BIN)"

$(GUI_FONT_BIN): $(GUI_FONT_TTF) tools/genfont.py
	@echo "[MAKE] Generating GUI proportional font atlas..."
	python3 tools/genfont.py $(GUI_FONT_TTF) $(GUI_FONT_BIN)
	@echo "[MAKE] GUI font: $(GUI_FONT_BIN)"

$(GUI_WALL_BIN): $(GUI_WALL_IMG) tools/genwall.py
	@echo "[MAKE] Generating desktop wallpaper..."
	python3 tools/genwall.py "$(GUI_WALL_IMG)" $(GUI_WALL_BIN)
	@echo "[MAKE] Wallpaper: $(GUI_WALL_BIN)"

# NASM (.asm) — various directories
$(BUILD_DIR)/boot.o: $(SRC_DIR)/boot.asm | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/smp_trampoline.o: $(SRC_DIR)/smp_trampoline.asm | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/core/%.o: $(SRC_DIR)/core/%.asm | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/graphics/%.o: $(SRC_DIR)/graphics/%.asm | $(BUILD_DIR) $(FONT_BIN) $(GUI_FONT_BIN) $(GUI_WALL_BIN)
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@

# The incbin objects must REASSEMBLE when their embedded blob changes — a normal
# (not order-only) prerequisite, otherwise a regenerated blob is silently stale.
$(BUILD_DIR)/graphics/cjk_glyph.o: $(FONT_BIN)
$(BUILD_DIR)/graphics/gui_glyph.o: $(GUI_FONT_BIN)
$(BUILD_DIR)/graphics/gui_wallimg.o: $(GUI_WALL_BIN)

# C rules — one generic rule for all subdirectories
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/graphics/%.o: $(SRC_DIR)/graphics/%.c | $(BUILD_DIR) $(FONT_BIN)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/shell/%.o: $(SRC_DIR)/shell/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/input/%.o: $(SRC_DIR)/input/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/gui/%.o: $(SRC_DIR)/gui/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/core/%.o: $(SRC_DIR)/core/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/tools/%.o: $(SRC_DIR)/tools/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/user/%.o: $(SRC_DIR)/user/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/apps/%.o: $(SRC_DIR)/apps/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(CFLAGS) $< -o $@

# Link
$(KERNEL_BIOS): $(ALL_OBJS) linker_bios.ld
	$(LD) $(LDFLAGS_BIOS) $(filter-out linker_bios.ld,$^) -o $@
	@echo "✓ BIOS kernel: $@"

$(ISO_BIOS): $(KERNEL_BIOS)
	@rm -rf $(BUILD_DIR)/isodir-bios
	@mkdir -p $(BUILD_DIR)/isodir-bios/boot/grub
	@cp $(KERNEL_BIOS) $(BUILD_DIR)/isodir-bios/boot/hbos.bin
	@printf 'set timeout=5\nset default=0\ninsmod all_video\ninsmod gfxterm\nset gfxmode=1024x768x32,800x600x32,1440x900x32,1280x800x32,auto\n' > $(BUILD_DIR)/isodir-bios/boot/grub/grub.cfg
	@printf 'menuentry "HBOS $(HBOS_VERSION) (graphics auto)" {\n  terminal_output gfxterm\n  set gfxpayload=keep\n  multiboot2 /boot/hbos.bin\n}\n' >> $(BUILD_DIR)/isodir-bios/boot/grub/grub.cfg
	@printf 'menuentry "HBOS $(HBOS_VERSION) (text fallback)" {\n  terminal_output console\n  set gfxpayload=text\n  multiboot2 /boot/hbos.bin\n}\n' >> $(BUILD_DIR)/isodir-bios/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/isodir-bios 2>/dev/null
	@cp $@ "$(BUILD_DIR)/hbos-bios-$(HBOS_VERSION)_$(shell date +%Y%m%d).iso"
	@echo "✓ BIOS ISO: $@"
	@echo "✓ Versioned BIOS ISO: $(BUILD_DIR)/hbos-bios-$(HBOS_VERSION)_$(shell date +%Y%m%d).iso"

$(ISO_UEFI): $(KERNEL_BIOS) $(LIMINE_EFI) $(UEFI_CD_IMG) limine.conf
	@rm -rf $(BUILD_DIR)/isodir-uefi
	@mkdir -p $(BUILD_DIR)/isodir-uefi/EFI/BOOT $(BUILD_DIR)/isodir-uefi/boot
	@cp $(KERNEL_BIOS) $(BUILD_DIR)/isodir-uefi/boot/hbos.bin
	@cp limine.conf $(BUILD_DIR)/isodir-uefi/limine.conf
	@cp $(LIMINE_EFI) $(BUILD_DIR)/isodir-uefi/EFI/BOOT/BOOTX64.EFI
	@cp $(UEFI_CD_IMG) $(BUILD_DIR)/isodir-uefi/boot/uefi-cd.img
	@xorriso -as mkisofs -R -r -J -V HBOS_UEFI \
		-eltorito-alt-boot -e boot/uefi-cd.img -no-emul-boot \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		-o $@ $(BUILD_DIR)/isodir-uefi >/dev/null 2>&1
	@cp $@ "$(BUILD_DIR)/hbos-uefi-$(HBOS_VERSION)_$(shell date +%Y%m%d).iso"
	@echo "✓ UEFI ISO: $@"
	@echo "✓ Versioned UEFI ISO: $(BUILD_DIR)/hbos-uefi-$(HBOS_VERSION)_$(shell date +%Y%m%d).iso"

iso: $(ISO_BIOS) $(ISO_UEFI)

bios-iso: $(ISO_BIOS)

uefi-iso: $(ISO_UEFI)

run: run-hdd-bios

vm: run

run-bios: run

run-iso: run-bios-ahci

run-bios-nodisk: $(ISO_BIOS)
	$(QEMU_ENV) $(QEMU) -cdrom $(ISO_BIOS) -m 512M -boot d -serial stdio -vga std -monitor none

disk-img: $(DISK_IMG)

$(INSTALL_IMG_BIOS): $(KERNEL_BIOS) $(LIMINE_EFI) limine.conf tools/mkhbosdisk.py
	python3 tools/mkhbosdisk.py $@ --mode bios --kernel $(KERNEL_BIOS) --limine-conf limine.conf --limine-dir limine-bin/bin

$(INSTALL_IMG_UEFI): $(KERNEL_BIOS) $(LIMINE_EFI) limine.conf tools/mkhbosdisk.py
	python3 tools/mkhbosdisk.py $@ --mode uefi --kernel $(KERNEL_BIOS) --limine-conf limine.conf --limine-dir limine-bin/bin

$(INSTALL_IMG): $(INSTALL_IMG_UEFI)
	@cp $(INSTALL_IMG_UEFI) $@

install-img: $(INSTALL_IMG_BIOS) $(INSTALL_IMG_UEFI) $(INSTALL_IMG)

$(VMWARE_BIOS_VMDK): $(INSTALL_IMG_BIOS)
	@rm -f $@
	$(QEMU_IMG) convert -f raw -O vmdk -o adapter_type=ide,subformat=monolithicSparse $(INSTALL_IMG_BIOS) $@
	@echo "✓ VMware BIOS VMDK: $@"

$(VMWARE_UEFI_VMDK): $(INSTALL_IMG_UEFI)
	@rm -f $@
	$(QEMU_IMG) convert -f raw -O vmdk -o adapter_type=ide,subformat=monolithicSparse $(INSTALL_IMG_UEFI) $@
	@echo "✓ VMware UEFI VMDK: $@"

$(VBOX_BIOS_VDI): $(INSTALL_IMG_BIOS)
	@rm -f $@
	$(QEMU_IMG) convert -f raw -O vdi $(INSTALL_IMG_BIOS) $@
	@echo "✓ VirtualBox BIOS VDI: $@"

$(VBOX_UEFI_VDI): $(INSTALL_IMG_UEFI)
	@rm -f $@
	$(QEMU_IMG) convert -f raw -O vdi $(INSTALL_IMG_UEFI) $@
	@echo "✓ VirtualBox UEFI VDI: $@"

vmware-bios: $(VMWARE_BIOS_VMDK)

vmware-uefi: $(VMWARE_UEFI_VMDK)

vbox-bios: $(VBOX_BIOS_VDI)

vbox-uefi: $(VBOX_UEFI_VDI)

release: $(ISO_BIOS) $(ISO_UEFI) $(VMWARE_BIOS_VMDK) $(VMWARE_UEFI_VMDK) $(VBOX_BIOS_VDI) $(VBOX_UEFI_VDI)
	@echo "✓ Release artifacts:"
	@echo "  $(ISO_BIOS)"
	@echo "  $(ISO_UEFI)"
	@echo "  $(VMWARE_BIOS_VMDK)"
	@echo "  $(VMWARE_UEFI_VMDK)"
	@echo "  $(VBOX_BIOS_VDI)"
	@echo "  $(VBOX_UEFI_VDI)"

smoke:
	bash scripts/smoke.sh

run-hdd: run-hdd-bios

run-hdd-bios: $(INSTALL_IMG_BIOS)
	$(QEMU_ENV) $(QEMU) -m 512M \
		-device ich9-ahci,id=ahci \
		-drive file=$(INSTALL_IMG_BIOS),format=raw,if=none,id=hd0 \
		-device ide-hd,drive=hd0,bus=ahci.0 \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-boot c -serial stdio -vga std -monitor none

run-hdd-uefi: $(INSTALL_IMG_UEFI)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_HDD.fd
	$(QEMU_ENV) $(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_HDD.fd \
		-device ich9-ahci,id=ahci \
		-drive file=$(INSTALL_IMG_UEFI),format=raw,if=none,id=hd0 \
		-device ide-hd,drive=hd0,bus=ahci.0 \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-boot c -serial stdio -monitor none -vga std -no-reboot

run-bios-disk: $(ISO_BIOS) $(DISK_IMG)
	$(QEMU_ENV) $(QEMU) -m 512M \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-cdrom $(ISO_BIOS) -boot d \
		-serial stdio -vga std -monitor none

run-bios-ahci: $(ISO_BIOS) $(DISK_IMG)
	$(QEMU_ENV) $(QEMU) -m 512M \
		-device ich9-ahci,id=ahci \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device ide-hd,drive=hd0,bus=ahci.0 \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-cdrom $(ISO_BIOS) -boot d \
		-serial stdio -vga std -monitor none

limine-uefi: $(LIMINE_EFI)

$(LIMINE_EFI):
	$(MAKE) -C limine-bin limine-uefi-x86-64 BUILD_UEFI_X86_64=limine-uefi-x86-64

$(UEFI_CD_IMG): $(KERNEL_BIOS) $(LIMINE_EFI) limine.conf
	@rm -f $@
	@dd if=/dev/zero of=$@ bs=512 count=32768 2>/dev/null
	@mformat -i $@ -h 64 -t 32 -s 16 -N 12345678 ::
	@mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot
	@mcopy -D o -m -i $@ $(LIMINE_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -D o -m -i $@ limine.conf ::/limine.conf
	@mcopy -D o -m -i $@ $(KERNEL_BIOS) ::/boot/hbos.bin

$(UEFI_IMG): $(KERNEL_BIOS) $(LIMINE_EFI) limine.conf
	@rm -f $@
	@truncate -s 64M $@
	@mkfs.fat -F 32 $@ >/dev/null
	@mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot
	@mcopy -i $@ $(LIMINE_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i $@ limine.conf ::/limine.conf
	@mcopy -i $@ $(KERNEL_BIOS) ::/boot/hbos.bin
	@echo "✓ UEFI FAT image: $@"

uefi: $(ISO_UEFI)

uefi-img: $(UEFI_IMG)

run-uefi: run-hdd-uefi

run-iso-uefi: run-uefi-ahci

run-uefi-nodisk: $(ISO_UEFI)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU_ENV) $(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-cdrom $(ISO_UEFI) -boot d \
		-serial none -vga std -monitor none -no-reboot

run-uefi-img: $(UEFI_IMG)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU_ENV) $(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-drive file=$(UEFI_IMG),format=raw,if=virtio \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-serial none -vga std -monitor none -no-reboot

run-uefi-headless: $(ISO_UEFI)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU_ENV) $(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-cdrom $(ISO_UEFI) -boot d \
		-serial stdio -monitor none -display none -no-reboot

run-uefi-disk: $(ISO_UEFI) $(DISK_IMG)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU_ENV) $(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-drive file=$(DISK_IMG),format=raw,if=ide,index=0,media=disk \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-cdrom $(ISO_UEFI) -boot d \
		-serial stdio -monitor none -vga std -no-reboot

run-uefi-ahci: $(ISO_UEFI) $(DISK_IMG)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU_ENV) $(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-device ich9-ahci,id=ahci \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 \
		-device ide-hd,drive=hd0,bus=ahci.0 \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-cdrom $(ISO_UEFI) -boot d \
		-serial stdio -monitor none -vga std -no-reboot

# ============================================================
# User-space ring3 programs
# ============================================================

USER_PROG_DIR = $(SRC_DIR)/user/progs
USER_LIBC_DIR = $(SRC_DIR)/user/libc
USER_BUILD_DIR = $(BUILD_DIR)/user

USER_CFLAGS = -m64 -mcmodel=large -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
              -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 -O2 -Wall -Wextra \
              -I$(SRC_DIR)/user/libc -I$(SRC_DIR)/user

USER_LDFLAGS = -m elf_x86_64 -static -nostdlib -T user.ld

USER_LIBC_SRCS = \
	$(USER_LIBC_DIR)/crt0.c \
	$(USER_LIBC_DIR)/syscall.c \
	$(USER_LIBC_DIR)/string.c \
	$(USER_LIBC_DIR)/stdlib.c \
	$(USER_LIBC_DIR)/stdio.c \
	$(USER_LIBC_DIR)/socket.c \
	$(USER_LIBC_DIR)/dlfcn.c \
	$(USER_LIBC_DIR)/unistd.c

USER_LIBC_OBJS = $(USER_LIBC_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

USER_PROG_SRCS = $(wildcard $(USER_PROG_DIR)/*.c)
USER_PROG_BINS = $(USER_PROG_SRCS:$(USER_PROG_DIR)/%.c=$(USER_BUILD_DIR)/%.elf)

$(BUILD_DIR)/user/libc/%.o: $(SRC_DIR)/user/libc/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(USER_CFLAGS) $< -o $@

$(USER_BUILD_DIR)/%.elf: $(USER_PROG_DIR)/%.c $(USER_LIBC_OBJS) | $(BUILD_DIR)
	@mkdir -p $(@D) $(BUILD_DIR)/user/progs
	$(CC) -c $(USER_CFLAGS) $< -o $(BUILD_DIR)/user/progs/$*.o
	$(LD) $(USER_LDFLAGS) $(USER_LIBC_OBJS) $(BUILD_DIR)/user/progs/$*.o -o $@
	@echo "✓ user prog: $@"

user-progs: $(USER_PROG_BINS)
	@echo "✓ All user programs built"

user-progs-clean:
	rm -rf $(USER_BUILD_DIR) $(BUILD_DIR)/user
	@echo "✓ User programs cleaned"

# ── HBFS file injection ──────────────────────────────────────────
# Copy a file into a standalone HBFS image:  make hbfs-copy DISK=build/hbos_disk.img FILE=hello.c
hbfs-copy:
	@test -n "$(FILE)" || { echo "Usage: make hbfs-copy DISK=<image> FILE=<file> [NAME=remote_name]"; exit 1; }
	python3 tools/hbfs-copy.py $(DISK) $(FILE) $(NAME)

# Copy a file into the installed disk image:  make hbfs-install FILE=hello.c
hbfs-install: $(INSTALL_IMG_BIOS)
	python3 tools/hbfs-copy.py $(INSTALL_IMG_BIOS) $(FILE) $(NAME)

# List files in installed disk image:  make hbfs-list
hbfs-list: $(INSTALL_IMG_BIOS)
	python3 tools/hbfs-copy.py $(INSTALL_IMG_BIOS) --list

# Create standalone HBFS data disk:  make hbfs-disk
hbfs-disk: $(DISK_IMG)
$(DISK_IMG): tools/mkhbfs.py | $(BUILD_DIR)
	python3 tools/mkhbfs.py $@ --size-mib 16

clean:
	rm -rf $(BUILD_DIR)
	@echo "✓ Cleaned"
