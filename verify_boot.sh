#!/bin/bash
# HBOS 完整启动验证

set -e
cd /media/data/hbosv2

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔════════════════════════════════════╗${NC}"
echo -e "${BLUE}║   HBOS 启动验证系统              ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════╝${NC}"
echo ""

# Step 1: Build
echo -e "${YELLOW}[1/3] 编译系统...${NC}"
if make clean > /dev/null 2>&1 && make > /dev/null 2>&1; then
    echo -e "${GREEN}✓ 编译成功${NC}"
else
    echo -e "${RED}✗ 编译失败${NC}"
    exit 1
fi

# Step 2: Verify ISO
echo -e "${YELLOW}[2/3] 验证 ISO 文件...${NC}"
if [ ! -f build/hbos.iso ]; then
    echo -e "${RED}✗ ISO 文件不存在${NC}"
    exit 1
fi
ISO_SIZE=$(stat -f%z build/hbos.iso 2>/dev/null || stat -c%s build/hbos.iso)
ISO_SIZE_MB=$((ISO_SIZE / 1024 / 1024))
echo -e "${GREEN}✓ ISO 文件大小: ${ISO_SIZE_MB}MB${NC}"

# Step 3: Boot test
echo -e "${YELLOW}[3/3] 启动测试 (5秒)...${NC}"
echo -e "${BLUE}预期输出: B L P OK [启动消息]${NC}"
echo ""

timeout 5 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M \
    -boot d \
    -serial stdio \
    -vga std \
    2>&1 | head -30 || true

echo ""
echo -e "${BLUE}════════════════════════════════════${NC}"
echo -e "${GREEN}启动验证完成${NC}"
echo ""
echo -e "下一步:"
echo -e "  图形模式:  ${YELLOW}make run${NC}"
echo -e "  文本模式:  ${YELLOW}./test_text.sh${NC}"
echo -e "  UEFI 模式: ${YELLOW}make run-efi${NC}"
