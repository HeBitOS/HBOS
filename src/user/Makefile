# HBOS User-Space Build System
#
# Build instructions:
#   1. make          — build all user programs into bin/
#   2. make clean    — remove build artifacts
#   3. make install  — copy binaries to filesystem
#
# Programs are compiled as static ELF64 executables for HBOS.

CC       = gcc
LD       = ld
CFLAGS   = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
           -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
           -O2 -Wall -Wextra -I../libc

LDFLAGS  = -m elf_x86_64 -T ../../user.ld

BUILD_DIR = build
BIN_DIR   = bin

SRCS = $(wildcard *.c)
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)

CRT0_SRC = crt0.c
CRT0_OBJ = $(BUILD_DIR)/crt0.o

LIBC_SRCS = $(wildcard ../libc/*.c)
LIBC_OBJS = $(LIBC_SRCS:../libc/%.c=$(BUILD_DIR)/libc_%.o)

.PHONY: all clean install

all: $(BIN_DIR) $(CRT0_OBJ) libc.a $(patsubst %.c,$(BIN_DIR)/%,$(filter-out crt0.c,$(wildcard *.c)))

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/crt0.o: crt0.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/libc_%.o: ../libc/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

libc.a: $(LIBC_OBJS)
	ar rcs $@ $^

$(BIN_DIR)/%: %.c $(CRT0_OBJ) libc.a | $(BIN_DIR) $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $(BUILD_DIR)/$*.o
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(BUILD_DIR)/$*.o libc.a -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) libc.a