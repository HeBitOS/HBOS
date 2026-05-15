# HBOS - He Bit OS
![截图](./photo/hbosv0.1.png "v0.1截图")

一个简单的 64 位操作系统，支持命令行交互。

**现已支持 BIOS (Multiboot) 和 UEFI 双启动！** 🎉

## 项目结构

```
hbosv2/
├── src/
│   ├── boot.asm          # Multiboot 引导入口 (32位到64位转换, BIOS)
│   ├── boot_efi.asm      # UEFI 引导入口 (64位, EFI固件)
│   ├── kernel.c          # 内核主代码 (VGA输出, 键盘输入, Shell)
│   ├── fs.c              # 文件系统
│   ├── types.h           # 类型定义
│   └── limine.h          # Limine 引导协议支持
├── linker_bios.ld        # 链接脚本 (BIOS/Multiboot启动)
├── linker_efi.ld         # 链接脚本 (UEFI/EFI启动)
├── limine.cfg            # Limine 配置文件
├── Makefile              # 构建脚本 (支持双启动)
└── README.md             # 本文件
```

## 功能特性

- ✅ 64 位长模式运行
- ✅ BIOS/Multiboot 启动支持
- ✅ **UEFI/EFI 启动支持** ✨
- ✅ **混合 ISO 映像 (二合一)** - 同时支持 BIOS 和 UEFI 启动
- ✅ VGA 文本模式显示
- ✅ 硬件光标支持
- ✅ PS/2 键盘驱动
- ✅ Shift/CapsLock 支持
- ✅ 简易 Shell 命令行

## 可用命令

| 命令 | 说明 |
|------|------|
| `help` | 显示帮助信息 |
| `clear` | 清屏 |
| `version` | 显示系统版本 |
| `reboot` | 重启系统 |
| `poweroff` | 关闭系统 |
| `echo <text>` | 输出文本 |
| `color FG BG` | 设置颜色 (0-15) |

## 快速开始

### 依赖

- `gcc` - C 编译器
- `nasm` - 汇编器
- `ld` - 链接器
- `grub-mkrescue` - ISO 制作工具 (支持 EFI)
- `mtools` - GRUB 依赖
- `qemu-system-x86_64` - 模拟器
- `ovmf` (可选) - UEFI 固件用于 UEFI 模式测试

```bash
# Ubuntu/Debian
sudo apt install build-essential nasm grub-pc-bin mtools qemu-system-x86 ovmf

# Fedora
sudo dnf install gcc nasm grub2-tools mtools qemu-system-x86 ovmf
```

### 构建

```bash
# 构建混合 ISO (支持 BIOS 和 UEFI)
make

# 仅构建 BIOS 内核
make all-bios

# 清理构建输出
make clean
```

### 运行

```bash
# 在 BIOS 模式运行
make run

# 在 UEFI 模式运行 (需要 OVMF)
make run-efi

# 手动运行
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M
```

### 详细的编译选项

```bash
# 查看所有可用的编译目标
make help
```

## 技术细节

### 启动流程对比

#### BIOS/Multiboot 启动流程
```
BIOS → GRUB → Multiboot Header → _start (boot.asm)
      → 设置页表 → 启用长模式 → 跳转64位
      → kmain() (kernel.c) → Shell
```

#### UEFI/EFI 启动流程
```
UEFI 固件 → EFI 启动管理器 → efi_main (boot_efi.asm)
         → 已处于 64 位长模式
         → kmain() (kernel.c) → Shell
```

### 混合 ISO (二合一) 说明

混合 ISO 同时包含：
1. **BIOS 引导部分**: 使用 GRUB Multiboot 协议
   - 存放位置: `/boot/hbos.bin`
   - 启动方式: 传统 BIOS 引导

2. **UEFI 引导部分**: 使用 EFI PE 可执行文件格式
   - 存放位置: `/EFI/BOOT/bootx64.efi`
   - 启动方式: UEFI 固件直接执行
   - 支持 64 位 UEFI (x64)

启动时，firmware 自动选择相应的引导方式：
- **Legacy BIOS**: 使用 Multiboot 模式
- **UEFI**: 使用 EFI 模式

### 内存布局

```
0x100000  - 内核加载地址 (BIOS 模式)
0x100000  - Multiboot Header
0x101000  - 代码段 (.text)
0x102000  - 只读数据 (.rodata)
0x103000  - 数据段 (.data)
0x104000  - BSS段 (.bss)
          - 页表 (12KB)
          - 栈 (64KB)

UEFI 模式: 由 UEFI 固件动态分配和加载
```

### 构建系统架构

```
Makefile:
├── BIOS 目标
│   ├── boot.asm → boot_bios.o → linker_bios.ld → hbos_bios.bin
│   └── kernel.c, fs.c → 编译到 BIOS 内核
├── UEFI 目标
│   ├── boot_efi.asm → boot_efi.o → linker_efi.ld → hbos_efi.efi
│   └── kernel.c, fs.c → 编译到 UEFI 内核
└── ISO 目标
    ├── BIOS ISO: 包含 BIOS 启动部分
    ├── Hybrid ISO: 同时包含 BIOS 和 UEFI 部分
    └── EFI ISO: (未来支持) 仅包含 UEFI 部分
```

### VGA 颜色表

| 编号 | 颜色 | 编号 | 颜色 |
|------|------|------|------|
| 0 | 黑 | 8 | 深灰 |
| 1 | 蓝 | 9 | 亮蓝 |
| 2 | 绿 | 10 | 亮绿 |
| 3 | 青 | 11 | 亮青 |
| 4 | 红 | 12 | 亮红 |
| 5 | 紫 | 13 | 亮紫 |
| 6 | 棕 | 14 | 黄 |
| 7 | 浅灰 | 15 | 白 |




## 许可证

GPL-3.0 license
