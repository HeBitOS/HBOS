CC = gcc
AS = nasm
LD = ld

BUILD_DIR = build
SRC_DIR = src

KERNEL_BIOS = $(BUILD_DIR)/hbos_bios.bin
ISO_HYBRID = $(BUILD_DIR)/hbos.iso

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
	$(SRC_DIR)/fs.c \
	$(SRC_DIR)/fb.c \
	$(SRC_DIR)/flanterm.c \
	$(SRC_DIR)/graphics/graphics.c \
	$(SRC_DIR)/graphics/font_cjk.c \
	$(SRC_DIR)/shell/shell.c \
	$(SRC_DIR)/core/task.c \
	$(SRC_DIR)/tools/help.c \
	$(SRC_DIR)/tools/system.c \
	$(SRC_DIR)/tools/debug.c \
	$(SRC_DIR)/tools/history.c

C_OBJS = $(C_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# 所有汇编源文件
ASM_SRCS = \
	$(SRC_DIR)/boot.asm \
	$(SRC_DIR)/core/task_switch.asm \
	$(SRC_DIR)/graphics/cjk_glyph.asm

ASM_OBJS = $(ASM_SRCS:$(SRC_DIR)/%.asm=$(BUILD_DIR)/%.o)

ALL_OBJS = $(C_OBJS) $(ASM_OBJS)

.PHONY: all clean run iso help font

all: iso

help:
	@echo "HBOS Build Targets:"
	@echo "  make all       - Build hybrid ISO"
	@echo "  make run       - Build and run in QEMU"
	@echo "  make clean     - Clean build files"
	@echo "  make font      - Regenerate CJK font binary"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(BUILD_DIR)/graphics $(BUILD_DIR)/shell $(BUILD_DIR)/core $(BUILD_DIR)/tools

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

# Link
$(KERNEL_BIOS): $(ALL_OBJS)
	$(LD) $(LDFLAGS_BIOS) $^ -o $@
	@echo "✓ BIOS kernel: $@"

$(ISO_HYBRID): $(KERNEL_BIOS)
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	@cp $(KERNEL_BIOS) $(BUILD_DIR)/isodir/boot/hbos.bin
	@printf 'set timeout=5\nset default=0\ninsmod all_video\ninsmod gfxterm\nset gfxmode=auto\n' > $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@printf 'menuentry "HBOS bata1 (graphics auto)" {\n  terminal_output gfxterm\n  set gfxpayload=keep\n  multiboot2 /boot/hbos.bin\n}\n' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@printf 'menuentry "HBOS bata1 (text fallback)" {\n  terminal_output console\n  set gfxpayload=text\n  multiboot2 /boot/hbos.bin\n}\n' >> $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	@grub-mkrescue --efi-boot-part --efi-boot-image -o $@ $(BUILD_DIR)/isodir 2>/dev/null || \
	 grub-mkrescue -o $@ $(BUILD_DIR)/isodir 2>/dev/null
	@echo "✓ Hybrid ISO: $@"

iso: $(ISO_HYBRID)

run: $(ISO_HYBRID)
	$(QEMU) -cdrom $(ISO_HYBRID) -m 512M -boot d -serial stdio -vga std -monitor none

clean:
	rm -rf $(BUILD_DIR)
	@echo "✓ Cleaned"