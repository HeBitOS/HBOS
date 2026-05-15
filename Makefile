CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy

BUILD_DIR = build
SRC_DIR = src

# BIOS/Multiboot targets
KERNEL_BIOS = $(BUILD_DIR)/hbos_bios.bin
KERNEL_EFI = $(BUILD_DIR)/hbos_efi.efi

# ISO targets
ISO_BIOS = $(BUILD_DIR)/hbos_bios.iso
ISO_HYBRID = $(BUILD_DIR)/hbos.iso

CFLAGS = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mcmodel=kernel -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra

ASFLAGS = -f elf64
LDFLAGS_BIOS = -m elf_x86_64 -static -Bsymbolic -nostdlib -T linker_bios.ld
LDFLAGS_EFI = --subsystem=10 -shared --no-undefined -Bsymbolic

QEMU = qemu-system-x86_64

.PHONY: all clean run iso run-bios run-efi all-bios all-efi help

all: iso

help:
	@echo "HBOS Build System"
	@echo "=================="
	@echo "Targets:"
	@echo "  all          - Build hybrid ISO (BIOS + UEFI)"
	@echo "  all-bios     - Build BIOS version only"
	@echo "  all-efi      - Build UEFI version only"
	@echo "  iso          - Create hybrid bootable ISO (default)"
	@echo "  run          - Build and run hybrid ISO in QEMU (BIOS mode)"
	@echo "  run-bios     - Build and run BIOS ISO in QEMU"
	@echo "  run-efi      - Build and run UEFI ISO in QEMU (requires OVMF)"
	@echo "  clean        - Remove all build files"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ============ BIOS/Multiboot Build ============

$(BUILD_DIR)/boot.o: $(SRC_DIR)/boot.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/boot_efi.o: $(SRC_DIR)/boot_efi.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/kernel.o: $(SRC_DIR)/kernel.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/fs.o: $(SRC_DIR)/fs.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# BIOS Kernel
$(KERNEL_BIOS): $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/fs.o
	$(LD) $(LDFLAGS_BIOS) $^ -o $@
	@echo "✓ BIOS kernel built: $@"

# EFI Kernel (kept for reference, not used in default hybrid ISO)
# Hybrid GRUB ISO includes EFI support automatically
$(KERNEL_EFI): $(BUILD_DIR)/boot_efi.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/fs.o
	$(LD) -m elf_x86_64 -static -Bsymbolic -nostdlib -T $(SRC_DIR)/linker_efi.ld $^ -o $@
	@echo "✓ EFI kernel built: $@"

all-bios: $(KERNEL_BIOS)
	@echo "✓ BIOS build complete"

all-efi: $(KERNEL_BIOS)
	@echo "⚠ UEFI support is built into the hybrid ISO via GRUB"
	@echo "✓ UEFI-compatible build complete"

# ============ ISO Generation ============

$(ISO_BIOS): $(KERNEL_BIOS)
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	@cp $(KERNEL_BIOS) $(BUILD_DIR)/isodir/boot/hbos.bin
	@echo 'set timeout=3' > $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'set default=0' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'insmod all_video' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'set gfxmode=auto' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'menuentry "HBOS - He Bit OS (BIOS)" {' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/hbos.bin' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo '}' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/isodir 2>/dev/null || \
	 grub-mkrescue -o $@ $(BUILD_DIR)/isodir
	@echo "✓ BIOS ISO created: $@"

$(ISO_HYBRID): $(KERNEL_BIOS)
	@echo "Creating hybrid ISO with BIOS and UEFI support..."
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	@cp $(KERNEL_BIOS) $(BUILD_DIR)/isodir/boot/hbos.bin
	@echo 'set timeout=5' > $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'set default=0' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'insmod all_video' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'set gfxmode=auto' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'menuentry "HBOS - He Bit OS" {' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/hbos.bin' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo '}' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@grub-mkrescue --efi-boot-part --efi-boot-image -o $@ $(BUILD_DIR)/isodir 2>/dev/null || \
	 grub-mkrescue -o $@ $(BUILD_DIR)/isodir 2>/dev/null
	@echo "✓ Hybrid ISO created: $@"

iso: $(ISO_HYBRID)

# Legacy target
run: $(ISO_HYBRID)
	@echo "Booting hybrid ISO (BIOS mode)..."
	$(QEMU) -cdrom $(ISO_HYBRID) -m 512M -boot d -serial stdio -vga none

run-bios: $(ISO_BIOS)
	@echo "Booting BIOS-only ISO..."
	$(QEMU) -cdrom $(ISO_BIOS) -m 512M -boot d

run-efi: $(ISO_HYBRID)
	@echo "Booting hybrid ISO (UEFI mode with OVMF)..."
	@echo "Checking for OVMF firmware..."
	@if [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then \
		$(QEMU) -cdrom $(ISO_HYBRID) -m 512M -boot d \
			-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd; \
	elif [ -f /usr/share/ovmf/OVMF.fd ]; then \
		$(QEMU) -cdrom $(ISO_HYBRID) -m 512M -boot d \
			-drive if=pflash,format=raw,readonly=on,file=/usr/share/ovmf/OVMF.fd; \
	elif [ -f /usr/share/qemu/OVMF.fd ]; then \
		$(QEMU) -cdrom $(ISO_HYBRID) -m 512M -boot d \
			-drive if=pflash,format=raw,readonly=on,file=/usr/share/qemu/OVMF.fd; \
	else \
		echo "⚠ OVMF firmware not found. Install with:"; \
		echo "  sudo apt install ovmf"; \
		echo "Falling back to BIOS mode..."; \
		$(QEMU) -cdrom $(ISO_HYBRID) -m 512M -boot d; \
	fi

clean:
	rm -rf $(BUILD_DIR)
	@echo "✓ Build directory cleaned"