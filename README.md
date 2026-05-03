# HBOS - He Bit OS
![截图](./photo/hbosv0.1.png "v0.1截图")

一个简单的 64 位操作系统，支持命令行交互。

## 项目结构

```
os-demo/
├── src/
│   ├── boot.asm      # Multiboot 引导入口 (32位到64位转换)
│   ├── kernel.c      # 内核主代码 (VGA输出, 键盘输入, Shell)
│   └── types.h       # 类型定义
├── linker_bios.ld    # 链接脚本 (BIOS启动)
├── Makefile          # 构建脚本
└── README.md         # 本文件
```

## 功能特性

- ✅ 64 位长模式运行
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
- `grub-mkrescue` - ISO 制作工具
- `mtools` - GRUB 依赖
- `qemu-system-x86_64` - 模拟器

```bash
# Ubuntu/Debian
sudo apt install gcc nasm binutils grub-pc-bin mtools qemu-system-x86
```

### 构建

```bash
# 构建内核
make

# 创建 ISO 镜像
make iso

# 运行
make run

# 清理
make clean
```

### 手动运行

```bash
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M
```

## 技术细节

### 启动流程

```
BIOS → GRUB → Multiboot Header → _start (boot.asm)
      → 设置页表 → 启用长模式 → 跳转64位
      → kmain() (kernel.c) → Shell
```

### 内存布局

```
0x100000  - 内核加载地址
0x100000  - Multiboot Header
0x101000  - 代码段 (.text)
0x102000  - 只读数据 (.rodata)
0x103000  - 数据段 (.data)
0x104000  - BSS段 (.bss)
          - 页表 (12KB)
          - 栈 (64KB)
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
