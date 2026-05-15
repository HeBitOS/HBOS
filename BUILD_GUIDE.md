# HBOS 构建和使用指南

## 快速开始

### 系统要求

- Linux 系统 (Ubuntu 20.04+, Debian 11+, Fedora 33+ 等)
- 编译工具链
- QEMU (用于测试)
- 2GB+ 磁盘空间

### 安装依赖

```bash
# Ubuntu/Debian
sudo apt install build-essential nasm grub-pc-bin mtools qemu-system-x86 ovmf

# Fedora
sudo dnf install gcc nasm grub2-tools mtools qemu-system-x86 ovmf
```

## 编译

### 最简单的方式

```bash
# 编译一切并创建混合 ISO (支持 BIOS 和 UEFI)
make
```

### 或者选择具体目标

```bash
# 仅编译 BIOS 版本
make all-bios

# 仅编译 UEFI 版本
make all-efi

# 创建 ISO (默认混合 ISO)
make iso

# 创建仅支持 BIOS 的 ISO
make iso-bios
```

### 查看所有可用命令

```bash
make help
```

## 运行

### 在虚拟机中运行

#### BIOS 模式 (推荐用于测试)

```bash
make run
# 或手动运行
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M
```

#### UEFI 模式 (需要 OVMF)

```bash
make run-efi
```

脚本会自动检测 OVMF 位置。如果不可用，会回退到 BIOS 模式。

#### BIOS-only ISO

```bash
make run-bios
```

### 在物理硬件上运行

1. **创建可启动 USB**:
   ```bash
   sudo dd if=build/hbos.iso of=/dev/sdX bs=4M && sync
   ```
   其中 `/dev/sdX` 是你的 USB 设备 (小心不要选错!)

2. **插入 USB，重启计算机**

3. **从 USB 启动**:
   - 按住启动菜单键 (通常是 F2, F10, F12 或 DEL) 进入 BIOS/UEFI
   - 选择从 USB 启动
   - 选择 HBOS 条目

## 开发

### 项目结构

```
.
├── src/
│   ├── boot.asm         # BIOS 启动代码
│   ├── boot_efi.asm     # UEFI 启动代码
│   ├── kernel.c         # 内核主程序
│   ├── kernel.h         # 内核头文件 (如果有)
│   ├── fs.c             # 文件系统
│   ├── fs.h             # 文件系统头文件
│   ├── types.h          # 类型定义
│   └── [其他驱动文件]
├── build/               # 构建输出 (自动生成)
├── Makefile             # 构建配置
├── linker_bios.ld       # BIOS 链接脚本
├── linker_efi.ld        # UEFI 链接脚本
├── UEFI_SUPPORT.md      # UEFI 实现细节
└── README.md            # 本说明文档
```

### 修改代码后重新编译

```bash
# 快速构建
make

# 完全清理后重新构建
make clean && make

# 仅重新编译内核代码 (不重新编译引导)
touch src/kernel.c && make
```

### 编译器标志

#### C 代码编译标志

```
-m64                      # 64 位编译
-ffreestanding            # 独立编译 (无 C 库)
-fno-stack-protector      # 禁用堆栈保护
-fno-pic -fno-pie         # 位置无关代码
-mcmodel=kernel           # 内核代码模型
-mno-red-zone             # 禁用红色区域
-mno-80387 -mno-mmx       # 禁用浮点
-mno-sse -mno-sse2        # 禁用 SSE
-O2                       # 优化级别 2
```

这些标志确保代码可以在裸机上运行，没有任何特殊的运行时支持。

## 常见问题

### 构建错误

#### `nasm: command not found`

```bash
sudo apt install nasm
```

#### `error: 'kmain' not defined`

这是在修改引导代码时常见的错误。确保：
1. `kernel.c` 中定义了 `kmain` 函数
2. 在 `.asm` 文件中用 `extern kmain` 声明

#### linker 错误

如果遇到 linker 错误，检查：
1. 所有 `.o` 文件都存在
2. 链接脚本路径正确
3. 尝试 `make clean && make`

### 运行问题

#### QEMU 无法启动 ISO

确保：
1. ISO 文件存在: `ls -l build/hbos.iso`
2. 使用正确的命令: `qemu-system-x86_64 -cdrom build/hbos.iso -m 512M`

#### UEFI 启动失败

1. 检查 OVMF 安装:
   ```bash
   ls /usr/share/ovmf/OVMF_CODE.fd
   # 或
   ls /usr/share/OVMF/OVMF_CODE.fd
   ```

2. 如果没有安装:
   ```bash
   sudo apt install ovmf
   ```

3. 使用调试选项:
   ```bash
   qemu-system-x86_64 -cdrom build/hbos.iso -m 512M \
     -bios /usr/share/ovmf/OVMF_CODE.fd -d int
   ```

### 性能优化

#### 启用 KVM (需要虚拟化支持)

```bash
make run  # 自动尝试使用 KVM
# 或手动
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M -enable-kvm
```

#### 增加内存

```bash
make QEMU_MEM=1024 run  # 1GB 内存
# 或手动
qemu-system-x86_64 -cdrom build/hbos.iso -m 1024
```

## 进阶用法

### 调试

#### 使用 QEMU 调试器

```bash
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M -s -S
# 在另一个终端运行
gdb
# 在 gdb 中
(gdb) target remote :1234
(gdb) c  # 继续执行
```

#### 查看启动日志

```bash
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M \
  -d int -serial stdio 2>&1 | head -100
```

### 修改 Makefile

你可以自定义：

```makefile
# 修改内存大小
QEMU_MEM = 512M
# 修改 CPU 数量
QEMU_SMP = 2
```

## 提示和技巧

### 快速重构

```bash
# 清理并重新构建一切
make clean && make

# 或简写
make clean make
```

### 观察构建过程

```bash
# 详细输出
make -d

# 显示执行的命令
make --debug=b
```

### 提交更改

构建成功后，可以提交更改：

```bash
git status          # 查看变更
git add .           # 暂存文件
git commit -m "..."  # 提交
```

## 许可证

GPL-3.0 - 详见 LICENSE 文件

## 支持和反馈

如有问题，请：
1. 检查本指南的相关部分
2. 查看 UEFI_SUPPORT.md 了解技术细节
3. 检查构建日志输出
4. 参考 [OSDev Wiki](https://wiki.osdev.org)
