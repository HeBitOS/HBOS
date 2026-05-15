#!/bin/bash
# 快速启动诊断

cd /media/data/hbosv2

echo "【HBOS 启动诊断】"
echo ""
echo "编译..."
make clean > /dev/null 2>&1
if ! make > /tmp/build.log 2>&1; then
    echo "❌ 编译失败"
    tail -20 /tmp/build.log
    exit 1
fi
echo "✓ 编译成功"

echo ""
echo "启动 QEMU (10秒)..."
echo "预期: GRUB菜单 -> HBOS启动消息 -> Shell提示"
echo ""

timeout 10 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M \
    -boot d \
    -serial stdio \
    -vga std \
    -monitor none \
    2>&1 | head -50

echo ""
echo "【诊断完成】"
