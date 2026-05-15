#!/bin/bash
# HBOS 启动自动化测试和状态监控

cd /media/data/hbosv2

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# 编译
echo -e "${YELLOW}编译...${NC}"
make clean > /dev/null 2>&1
if ! make > /tmp/make.log 2>&1; then
    echo -e "${RED}编译失败${NC}"
    tail -10 /tmp/make.log
    exit 1
fi
echo -e "${GREEN}✓ 编译成功${NC}"

# 启动并捕获输出
echo -e "${YELLOW}启动虚拟机 (15秒)...${NC}"
echo ""

# 使用 tee 同时显示和保存输出
timeout 15 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M \
    -boot d \
    -serial stdio \
    -vga std \
    -monitor none \
    2>&1 | tee /tmp/boot.log

BOOT_LOG="/tmp/boot.log"

# 分析结果
echo ""
echo -e "${YELLOW}═══ 启动分析 ═══${NC}"

CHECKS=(
    "B:检查长模式|Boot stage B detected"
    "L:长模式可用|Boot stage L detected"
    "P:分页启用|Boot stage P detected"
    "OK:长模式跳转|Boot sequence OK"
    "Serial:串口初始化|Serial console initialized"
    "VGA:VGA初始化|VGA initialized"
)

for check in "${CHECKS[@]}"; do
    IFS="|" read -r desc pattern <<< "$check"
    if grep -q "$pattern" "$BOOT_LOG" 2>/dev/null; then
        echo -e "${GREEN}✓${NC} $desc"
    else
        echo -e "${YELLOW}⚠${NC} $desc"
    fi
done

echo ""
echo -e "${YELLOW}═══ 启动完成 ═══${NC}"
