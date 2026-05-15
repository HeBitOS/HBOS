#!/bin/bash
# HBOS 自动诊断和迭代脚本

set -e
cd /media/data/hbosv2

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}[1/5] 清理旧构建${NC}"
make clean > /dev/null 2>&1

echo -e "${YELLOW}[2/5] 编译 HBOS${NC}"
if ! make > /tmp/build.log 2>&1; then
    echo -e "${RED}编译失败${NC}"
    tail -20 /tmp/build.log
    exit 1
fi
echo -e "${GREEN}✓ 编译成功${NC}"

echo -e "${YELLOW}[3/5] 验证 ISO 文件${NC}"
if [ ! -f build/hbos.iso ]; then
    echo -e "${RED}ISO 文件不存在${NC}"
    exit 1
fi
ISO_SIZE=$(ls -lh build/hbos.iso | awk '{print $5}')
echo -e "${GREEN}✓ ISO 文件: $ISO_SIZE${NC}"

echo -e "${YELLOW}[4/5] 验证 Multiboot2 头部${NC}"
if readelf -x .multiboot build/hbos_bios.bin 2>/dev/null | grep -q "d65052e8"; then
    echo -e "${GREEN}✓ Multiboot2 头部正确${NC}"
else
    echo -e "${RED}✗ Multiboot2 头部错误${NC}"
fi

echo -e "${YELLOW}[5/5] 启动虚拟机测试 (5秒)${NC}"
echo "期望输出: OK (来自 boot.asm 的串口输出)"
echo ""

timeout 5 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M \
    -boot d \
    -serial stdio \
    -vga none \
    -nographic \
    2>&1 | head -50 || true

echo ""
echo -e "${GREEN}[完成] 诊断测试结束${NC}"
