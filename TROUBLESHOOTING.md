# HBOS 启动故障排除指南

## 黑屏问题诊断

### 症状 1：完全黑屏（无任何输出）

**可能原因：**
1. GRUB 未正确加载内核
2. 内核地址配置错误
3. Multiboot2 头部错误

**解决步骤：**

```bash
# 检查 Multiboot2 头部
readelf -x .multiboot build/hbos_bios.bin

# 期望看到:
# d65052e8 (Multiboot2 magic)
# 后面跟随 address tag 和 entry point tag
```

### 症状 2：有 GRUB 菜单，但选项无效

**可能原因：**
1. 内核加载失败
2. 跳转地址错误
3. 长模式初始化失败

**解决步骤：**

查看 build/isodir/boot/grub/grub.cfg：

```bash
cat build/isodir/boot/grub/grub.cfg
```

应该显示：
```
set timeout=5
set default=0
terminal_input console
terminal_output console
menuentry "HBOS - He Bit OS (BIOS/UEFI)" {
    echo Loading HBOS...
    multiboot2 /boot/hbos.bin
}
```

### 症状 3：GRUB 显示但启动后黑屏

**可能原因：**
1. VGA 初始化失败
2. 串口输出被拦截
3. 堆栈配置错误

**解决步骤：**

运行文本模式测试：

```bash
./test_text.sh  # 禁用 VGA，仅用串口输出
```

预期输出序列：
```
X          # _start 入口点
B          # 长模式检查
L          # 长模式可用
P          # 分页启用
OK         # 长模式执行
[串口初始化消息]
```

## 关键诊断点

### 1. Multiboot2 头部验证

```bash
hexdump -C build/hbos_bios.bin | head -5

# 第一个 DWORD 应该是 d6 50 52 e8 (小端序)
```

### 2. ELF 结构验证

```bash
file build/hbos_bios.bin
readelf -h build/hbos_bios.bin
readelf -S build/hbos_bios.bin
```

### 3. 串口输出捕获

```bash
make run 2>&1 | tee boot.log

# 查看输出日志
cat boot.log | grep -E "X|B|L|P|OK|initialized"
```

## 常见问题和解决方案

### Q1: "error: invalid arch-dependent ELF magic"

**原因：** GRUB 不支持该 ELF 格式
**解决：** 确保使用 Multiboot2 (multiboot2 命令)

### Q2: "error: no multiboot header found"

**原因：** Multiboot2 头部不在可加载段的前 32KB
**解决：** 重新编译并检查头部位置

```bash
readelf -x .multiboot build/hbos_bios.bin
```

### Q3: 启动后立即重启

**原因：** 可能是页表或长模式转换错误
**解决：** 检查 boot.asm 的页表设置

### Q4: 黑屏但串口有输出

**原因：** VGA 初始化失败
**解决：** 在 kernel.c vga_clear() 前添加诊断

```c
serial_print("Before VGA clear\n");
vga_clear();
serial_print("After VGA clear\n");
```

## 调试技巧

### 1. 逐步启用功能

注释掉 VGA 初始化来隔离问题：

```c
// vga_clear();
// color = 0x0E;
// vga_print("...");

serial_print("VGA skipped for testing\n");
```

### 2. 添加多个诊断点

```asm
; 每个关键步骤前后输出
mov al, 'A'
mov dx, 0x3F8
out dx, al
```

### 3. 验证内存布局

```bash
readelf -l build/hbos_bios.bin | grep -A 5 "LOAD"

# 验证 load_addr = 0x100000
# 验证 bss_end_addr >= load_end_addr
```

## BIOS vs UEFI

### BIOS 启动流程
```
ROM BIOS → MBR → GRUB(PC) → Multiboot2 → _start(32-bit)
```

### UEFI 启动流程
```
UEFI固件 → EFI启动管理器 → GRUB(EFI) → Multiboot2 → _start(32-bit)
```

两者都使用相同的 Multiboot2 头部和内核代码！

## 快速诊断清单

- [ ] `readelf -x .multiboot` 显示 d65052e8
- [ ] `cat build/isodir/boot/grub/grub.cfg` 显示正确的配置
- [ ] `make clean && make` 成功编译
- [ ] `./test_text.sh` 显示启动序列 (X→B→L→P→OK)
- [ ] `make run` 显示 GRUB 菜单
- [ ] GRUB 菜单加载内核
- [ ] 串口输出启动消息
- [ ] VGA 显示 HBOS 欢迎屏

## 获取帮助

如果仍然有黑屏问题，收集以下信息：

```bash
# 1. 构建输出
make clean > /tmp/build.log 2>&1 && make >> /tmp/build.log 2>&1
tail -50 /tmp/build.log

# 2. Multiboot 头部
readelf -x .multiboot build/hbos_bios.bin

# 3. GRUB 配置
cat build/isodir/boot/grub/grub.cfg

# 4. 启动输出（保存到文件）
timeout 10 qemu-system-x86_64 \
    -cdrom build/hbos.iso \
    -m 512M -boot d \
    -serial file:/tmp/serial.log \
    -vga std 2>&1 | tee /tmp/boot.log

# 查看日志
cat /tmp/serial.log
cat /tmp/boot.log | head -100
```

