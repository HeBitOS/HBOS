CC = gcc
AS = nasm
LD = ld

BUILD_DIR = build
SRC_DIR = src

KERNEL = $(BUILD_DIR)/hbos.bin
ISO = $(BUILD_DIR)/hbos.iso

CFLAGS = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mcmodel=kernel -mno-red-zone -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra

ASFLAGS = -f elf64
LDFLAGS = -m elf_x86_64 -static -Bsymbolic -nostdlib -T linker_bios.ld

QEMU = qemu-system-x86_64

.PHONY: all clean run iso

all: $(KERNEL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: $(SRC_DIR)/boot.asm | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/kernel.o: $(SRC_DIR)/kernel.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(KERNEL): $(BUILD_DIR)/boot.o $(BUILD_DIR)/kernel.o
	$(LD) $(LDFLAGS) $^ -o $@
	@echo "HBOS kernel built: $@"

$(ISO): $(KERNEL)
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	@cp $(KERNEL) $(BUILD_DIR)/isodir/boot/hbos.bin
	@echo 'set timeout=3' > $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'set default=0' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo 'menuentry "HBOS - He Bit OS" {' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo '    multiboot /boot/hbos.bin' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@echo '}' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@grub-mkrescue -o $@ $(BUILD_DIR)/isodir 2>/dev/null || \
	 grub-mkrescue -o $@ $(BUILD_DIR)/isodir
	@echo "ISO created: $@"

iso: $(ISO)

run: $(ISO)
	$(QEMU) -cdrom $(ISO) -m 512M

clean:
	sudo rm -rf $(BUILD_DIR)

info:
	@echo "HBOS - He Bit OS"
	@echo ""
	@echo "Targets:"
	@echo "  all   - Build kernel (default)"
	@echo "  iso   - Create bootable ISO"
	@echo "  run   - Build and run in QEMU"
	@echo "  clean - Remove build files"
