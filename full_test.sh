#!/bin/bash
# HBOS 完整启动测试和诊断

set -e
cd /media/data/hbosv2

echo "════════════════════════════════════"
echo "   HBOS 启动诊断测试"
echo "════════════════════════════════════"
echo ""

# Step 1: Build
echo "[1/5] 编译系统..."
make clean > /dev/null 2>&1
if ! make > /tmp/make.log 2>&1; then
    echo "❌ 编译失败"
    tail -20 /tmp/make.log
    exit 1
fi
echo "✅ 编译成功"

# Step 2: Verify Multiboot2 header
echo "[2/5] 验证 Multiboot2 头部..."
MAGIC=$(readelf -x .multiboot build/hbos_bios.bin 2>/dev/null | grep -A 1 "multiboot" | tail -1 | awk '{print $2}')
if [ "$MAGIC" = "d65052e8" ]; then
    echo "✅ Multiboot2 magic 正确: 0x$MAGIC"
else
    echo "❌ Multiboot2 magic 错误: 0x$MAGIC"
    exit 1
fi

# Step 3: Check GRUB config
echo "[3/5] 验证 GRUB 配置..."
if grep -q "multiboot2" build/isodir/boot/grub/grub.cfg; then
    echo "✅ GRUB 使用 multiboot2 命令"
else
    echo "❌ GRUB 配置错误"
    exit 1
fi

# Step 4: Check ISO
echo "[4/5] 验证 ISO 文件..."
if [ -f build/hbos.iso ]; then
    SIZE=$(stat -f%z build/hbos.iso 2>/dev/null || stat -c%s build/hbos.iso)
    SIZE_MB=$((SIZE / 1024 / 1024))
    echo "✅ ISO 文件: ${SIZE_MB}MB"
else
    echo "❌ ISO 文件不存在"
    exit 1
fi

# Step 5: Boot test
echo "[5/5] 启动虚拟机测试 (10秒)..."
echo "预期输出序列: X B L P OK [初始化消息]"
echo ""

QEMU_LOG="/tmp/qemu_test.log"
timeout 10 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M \
    -boot d \
    -serial stdio \
    -vga std \
    -monitor none \
    2>&1 | tee "$QEMU_LOG" || true

echo ""
echo "════════════════════════════════════"
echo "   诊断结果"
echo "════════════════════════════════════"

# Analyze results
FOUND_X=0
FOUND_B=0
FOUND_L=0
FOUND_P=0
FOUND_OK=0

if grep -q "^X" "$QEMU_LOG"; then FOUND_X=1; fi
if grep -q "^B" "$QEMU_LOG"; then FOUND_B=1; fi
if grep -q "^L" "$QEMU_LOG"; then FOUND_L=1; fi
if grep -q "^P" "$QEMU_LOG"; then FOUND_P=1; fi
if grep -q "^OK" "$QEMU_LOG"; then FOUND_OK=1; fi

echo "启动序列:"
[ $FOUND_X -eq 1 ] && echo "✅ X - _start 入口点" || echo "⚠️  X - _start 入口点 (未见)"
[ $FOUND_B -eq 1 ] && echo "✅ B - 长模式检查" || echo "⚠️  B - 长模式检查"
[ $FOUND_L -eq 1 ] && echo "✅ L - 长模式可用" || echo "⚠️  L - 长模式可用"
[ $FOUND_P -eq 1 ] && echo "✅ P - 分页启用" || echo "⚠️  P - 分页启用"
[ $FOUND_OK -eq 1 ] && echo "✅ OK - 长模式执行" || echo "⚠️  OK - 长模式执行"

if grep -q "Serial console initialized" "$QEMU_LOG"; then
    echo "✅ 串口初始化成功"
else
    echo "⚠️  串口初始化 (未见)"
fi

if grep -q "VGA: Screen cleared" "$QEMU_LOG"; then
    echo "✅ VGA 初始化成功"
else
    echo "⚠️  VGA 初始化 (未见)"
fi

if grep -q "hbos#" "$QEMU_LOG"; then
    echo "✅ Shell 提示符显示"
else
    echo "⚠️  Shell 提示符 (未见)"
fi

echo ""
if [ $((FOUND_X + FOUND_B + FOUND_L + FOUND_P + FOUND_OK)) -eq 5 ]; then
    echo "🎉 完整启动序列成功！"
    echo "系统已准备好使用"
else
    echo "⚠️  启动序列不完整，请查看上面的输出"
fi
