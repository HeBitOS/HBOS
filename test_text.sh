#!/bin/bash
# 纯文本模式启动测试（调试模式）

cd /media/data/hbosv2

echo "=== HBOS 文本模式启动测试 (10秒超时) ==="
echo "期望输出序列："
echo "  B      - 检查长模式"
echo "  L      - 长模式可用"
echo "  P      - 分页启用"
echo "  OK     - 长模式初始化完成"
echo "  [串口初始化消息]"
echo ""

timeout 10 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M \
    -boot d \
    -serial stdio \
    -vga none \
    -nographic \
    2>&1 | tee /tmp/boot_output.log

echo ""
echo "=== 测试结果分析 ==="
if grep -q "OK" /tmp/boot_output.log; then
    echo "✓ 启动序列完成"
else
    echo "✗ 启动序列不完整"
fi

if grep -q "Serial console initialized" /tmp/boot_output.log; then
    echo "✓ 串口初始化成功"
else
    echo "⚠ 串口初始化消息未见"
fi

if grep -q "VGA initialized" /tmp/boot_output.log; then
    echo "✓ VGA 初始化成功"
else
    echo "⚠ VGA 初始化消息未见"
fi
