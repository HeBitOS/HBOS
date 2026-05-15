# HBOS 快速启动指南

## 当前进度（第 2 阶段）

✅ **第 1 阶段：64 位启动修复**
- Multiboot2 协议实现
- 长模式初始化
- 串口调试输出

✅ **第 2 阶段：VGA 图形输出**
- VGA 初始化
- VGA 与串口并行输出
- 启动序列验证

## 快速开始

### 方式 1：自动化测试（推荐）
```bash
./run_test.sh
```
这会自动编译、启动并验证启动过程。

### 方式 2：图形模式启动
```bash
make run
```
启动 HBOS 并显示 VGA 图形界面。同时输出串口调试信息到终端。

### 方式 3：纯文本调试模式
```bash
./test_text.sh
```
禁用 VGA，仅输出串口信息用于调试。

### 方式 4：完整启动验证
```bash
./verify_boot.sh
```
验证编译、ISO 文件和启动序列。

## 启动序列说明

```
GRUB Multiboot2 加载
    ↓
boot.asm _start (32位模式)
    ↓
[B] 检查长模式支持（CPU 检查）
    ↓
[L] 长模式可用（CPUID 确认）
    ↓
设置页表和 GDT
    ↓
[P] 启用分页（CR0.PG = 1）
    ↓
长模式 jmp 指令
    ↓
[OK] 长模式执行（64位代码）
    ↓
kernel.c kmain()
    ↓
初始化串口 (115200 baud)
    ↓
初始化 VGA (80x25 文本模式)
    ↓
显示欢迎信息
    ↓
等待键盘输入（Shell 提示符）
```

## 预期输出

### 串口输出
```
B
L
P
OK

=====================================
      HBOS Kernel Initializing
      64-bit Operating System
=====================================
Serial console initialized
Setting up VGA...
VGA: Clearing screen...
VGA: Screen cleared
VGA initialized, shell starting...
```

### VGA 图形输出
```
========================================
         HBOS - He Bit OS
       64-bit Operating System
========================================

System started!
Type 'help' for commands

hbos# 
```

## 调试技巧

### 查看详细启动日志
```bash
./run_test.sh 2>&1 | grep -E "B|L|P|OK|initialized"
```

### 延长 QEMU 启动时间
编辑 `Makefile`，修改超时值：
```makefile
run: $(ISO_HYBRID)
    timeout 30 qemu-system-x86_64 ...  # 改为 30 秒
```

### 添加调试断点
在 `src/kernel.c` 中添加：
```c
while(1) {
    serial_print("等待中...\n");
    for (volatile int i = 0; i < 1000000; i++);
}
```

## 现在可以做什么

- ✅ 在图形模式下看到启动界面
- ✅ 看到 "hbos#" Shell 提示符
- ⏳ 输入命令并执行
- ⏳ 测试内置命令（help, clear, version 等）

## 下一步（第 3 阶段）

- [ ] 键盘输入处理
- [ ] Shell 命令实现
- [ ] 文件系统基础
- [ ] UEFI 启动支持

