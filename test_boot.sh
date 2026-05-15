#!/bin/bash
set -e

cd /media/data/hbosv2

echo "=== Building HBOS ==="
make clean > /dev/null 2>&1
make > /dev/null 2>&1

echo "=== Running with serial output (5 seconds) ==="
timeout 5 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M \
    -boot d \
    -serial stdio \
    -vga none \
    -nographic \
    2>&1 || true

echo ""
echo "=== Test complete ==="
