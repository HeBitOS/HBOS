CC = gcc
AS = nasm
LD = ld

BUILD_DIR = build
SRC_DIR = src

KERNEL_BIOS = $(BUILD_DIR)/hbos_bios.bin
ISO_BIOS = $(BUILD_DIR)/hbos_bios.iso
ISO_UEFI = $(BUILD_DIR)/hbos_uefi.iso
ISO_HYBRID = $(BUILD_DIR)/hbos.iso
UEFI_IMG = $(BUILD_DIR)/hbos_uefi.img
LIMINE_EFI = limine-bin/bin/BOOTX64.EFI
UEFI_CD_IMG = $(BUILD_DIR)/limine_uefi_cd.img
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

CFLAGS = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mcmodel=kernel -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra \
         -I$(SRC_DIR) -I$(SRC_DIR)/graphics -I$(SRC_DIR)/shell -I$(SRC_DIR)/core -I$(SRC_DIR)/tools

ASFLAGS = -f elf64
LDFLAGS_BIOS = -m elf_x86_64 -static -Bsymbolic -nostdlib -T linker_bios.ld

QEMU = /usr/bin/qemu-system-x86_64

# 字体文件
FONT_TTF = fonts/ZhengGeDianHei-16.ttf
FONT_BIN = $(BUILD_DIR)/font_cjk.bin

# 所有 C 源文件
C_SRCS = \
	$(SRC_DIR)/kernel.c \
	$(SRC_DIR)/selftest.c \
	$(SRC_DIR)/fs.c \
	$(SRC_DIR)/vfs.c \
	$(SRC_DIR)/fb.c \
	$(SRC_DIR)/flanterm.c \
	$(SRC_DIR)/graphics/graphics.c \
	$(SRC_DIR)/graphics/font_cjk.c \
	$(SRC_DIR)/shell/shell.c \
	$(SRC_DIR)/core/task.c \
	$(SRC_DIR)/lib/posix.c \
	$(SRC_DIR)/lib/string.c \
	$(SRC_DIR)/tools/help.c \
	$(SRC_DIR)/tools/system.c \
	$(SRC_DIR)/tools/debug.c \
	$(SRC_DIR)/tools/history.c \
	$(SRC_DIR)/tools/file.c

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
	$(SRC_DIR)/graphics/cjk_glyph.asm

ASM_OBJS = $(ASM_SRCS:$(SRC_DIR)/%.asm=$(BUILD_DIR)/%.o)

ALL_OBJS = $(C_OBJS) $(ASM_OBJS)

.PHONY: all clean run run-bios iso bios-iso uefi uefi-iso uefi-img run-uefi run-uefi-headless run-uefi-img limine-uefi help font

all: iso

help:
	@echo "HBOS Build Targets:"
	@echo "  make all       - Build BIOS and UEFI ISOs"
	@echo "  make bios-iso  - Build BIOS ISO"
	@echo "  make uefi-iso  - Build UEFI ISO"
	@echo "  make run       - Build and run BIOS ISO"
	@echo "  make run-uefi  - Build and run UEFI ISO with OVMF"
	@echo "  make uefi-img  - Build UEFI FAT boot image"
	@echo "  make run-uefi-headless - Run UEFI ISO with serial output only"
	@echo "  make clean     - Clean build files"
	@echo "  make font      - Regenerate CJK font binary"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(BUILD_DIR)/graphics $(BUILD_DIR)/shell $(BUILD_DIR)/core $(BUILD_DIR)/tools $(BUILD_DIR)/lib

# Font generation
font: $(FONT_BIN)

$(FONT_BIN): $(FONT_TTF) tools/genhzk.py
	@echo "[MAKE] Generating CJK font bitmap..."
	python3 tools/genhzk.py $(FONT_TTF) $(FONT_BIN)
	@echo "[MAKE] CJK font: $(FONT_BIN)"

# NASM (.asm) — various directories
$(BUILD_DIR)/boot.o: $(SRC_DIR)/boot.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/core/%.o: $(SRC_DIR)/core/%.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/graphics/%.o: $(SRC_DIR)/graphics/%.asm | $(BUILD_DIR) $(FONT_BIN)
	$(AS) $(ASFLAGS) $< -o $@

# C rules — one generic rule for all subdirectories
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/graphics/%.o: $(SRC_DIR)/graphics/%.c | $(BUILD_DIR) $(FONT_BIN)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/shell/%.o: $(SRC_DIR)/shell/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/core/%.o: $(SRC_DIR)/core/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/tools/%.o: $(SRC_DIR)/tools/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/lib/%.o: $(SRC_DIR)/lib/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# Link
$(KERNEL_BIOS): $(ALL_OBJS)
	$(LD) $(LDFLAGS_BIOS) $^ -o $@
	@echo "✓ BIOS kernel: $@"

$(ISO_BIOS): $(KERNEL_BIOS)
	@rm -rf $(BUILD_DIR)/isodir-bios
	@mkdir -p $(BUILD_DIR)/isodir-bios/boot/grub
	@cp $(KERNEL_BIOS) $(BUILD_DIR)/isodir-bios/boot/hbos.bin
	@printf 'set timeout=5\nset default=0\ninsmod all_video\ninsmod gfxterm\nset gfxmode=auto\n' > $(BUILD_DIR)/isodir-bios/boot/grub/grub.cfg
	@printf 'menuentry "HBOS beta1 (graphics auto)" {\n  terminal_output gfxterm\n  set gfxpayload=keep\n  multiboot2 /boot/hbos.bin\n}\n' >> $(BUILD_DIR)/isodir-bios/boot/grub/grub.cfg
	@printf 'menuentry "HBOS beta1 (text fallback)" {\n  terminal_output console\n  set gfxpayload=text\n  multiboot2 /boot/hbos.bin\n}\n' >> $(BUILD_DIR)/isodir-bios/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/isodir-bios 2>/dev/null
	@cp $@ $(ISO_HYBRID)
	@echo "✓ BIOS ISO: $@"

$(ISO_UEFI): $(KERNEL_BIOS) $(LIMINE_EFI) $(UEFI_CD_IMG) limine.conf
	@rm -rf $(BUILD_DIR)/isodir-uefi
	@mkdir -p $(BUILD_DIR)/isodir-uefi/EFI/BOOT $(BUILD_DIR)/isodir-uefi/boot
	@cp $(KERNEL_BIOS) $(BUILD_DIR)/isodir-uefi/boot/hbos.bin
	@cp limine.conf $(BUILD_DIR)/isodir-uefi/limine.conf
	@cp $(LIMINE_EFI) $(BUILD_DIR)/isodir-uefi/EFI/BOOT/BOOTX64.EFI
	@cp $(UEFI_CD_IMG) $(BUILD_DIR)/isodir-uefi/boot/uefi-cd.img
	@xorriso -as mkisofs -R -r -J \
		--efi-boot boot/uefi-cd.img \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		-o $@ $(BUILD_DIR)/isodir-uefi >/dev/null 2>&1
	@echo "✓ UEFI ISO: $@"

$(ISO_HYBRID): $(ISO_BIOS)

iso: $(ISO_BIOS) $(ISO_UEFI)

bios-iso: $(ISO_BIOS)

uefi-iso: $(ISO_UEFI)

run: run-bios

run-bios: $(ISO_BIOS)
	$(QEMU) -cdrom $(ISO_BIOS) -m 512M -boot d -serial stdio -vga std -monitor none

limine-uefi: $(LIMINE_EFI)

$(LIMINE_EFI):
	$(MAKE) -C limine-bin limine-uefi-x86-64 BUILD_UEFI_X86_64=limine-uefi-x86-64

$(UEFI_CD_IMG): $(KERNEL_BIOS) $(LIMINE_EFI) limine.conf
	@rm -f $@
	@truncate -s 64M $@
	@mkfs.fat -F 32 $@ >/dev/null
	@mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot
	@mcopy -i $@ $(LIMINE_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i $@ limine.conf ::/limine.conf
	@mcopy -i $@ $(KERNEL_BIOS) ::/boot/hbos.bin

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

run-uefi: $(ISO_UEFI)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-cdrom $(ISO_UEFI) -boot d \
		-serial none -vga std -monitor none -no-reboot

run-uefi-img: $(UEFI_IMG)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-drive file=$(UEFI_IMG),format=raw,if=virtio \
		-serial none -vga std -monitor none -no-reboot

run-uefi-headless: $(ISO_UEFI)
	@cp $(OVMF_VARS) $(BUILD_DIR)/OVMF_VARS_UEFI.fd
	$(QEMU) -machine q35 -m 512M \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD_DIR)/OVMF_VARS_UEFI.fd \
		-cdrom $(ISO_UEFI) -boot d \
		-serial stdio -monitor none -display none -no-reboot

clean:
	rm -rf $(BUILD_DIR)
	@echo "✓ Cleaned"
