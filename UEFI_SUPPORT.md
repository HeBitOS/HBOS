# UEFI 支持 & 双启动实现指南

## 概述

HBOS 现已支持通过 GRUB 的 UEFI 启动。混合 ISO 镜像包含：
- BIOS/Multiboot 启动支持 (传统模式)
- UEFI 启动支持 (通过 GRUB 的 EFI 引导程序)

这个实现在单个 ISO 中提供了最大的兼容性。

## 技术架构

### 启动流程

#### BIOS 模式
```
计算机启动 (BIOS)
    ↓
ISO 的 MBR 启动扇区
    ↓
GRUB 引导加载程序 (BIOS 模式)
    ↓
Multiboot 协议加载 hbos.bin
    ↓
boot.asm (32位 → 64位 转换)
    ↓
kmain() (内核)
    ↓
Shell
```

#### UEFI 模式
```
计算机启动 (UEFI 固件)
    ↓
EFI 启动管理器
    ↓
GRUB 引导加载程序 (UEFI/EFI 模式)
    ↓
Multiboot 协议加载 hbos.bin
    ↓
boot.asm (32位 → 64位 转换)
    ↓
kmain() (内核)
    ↓
Shell
```

### 关键优势

1. **单一内核代码**: BIOS 和 UEFI 都运行相同的内核
2. **兼容性最大化**: 适用于任何支持 BIOS 或 UEFI 的系统
3. **简化的构建**: 不需要维护两套完整的启动代码
4. **GRUB 处理细节**: GRUB 负责处理 UEFI 特定的复杂性

## 添加的新文件

### 1. `src/boot_efi.asm` - UEFI 引导代码 (参考实现)

这是一个参考实现，展示如何编写直接的 UEFI 启动程序。
当前的混合 ISO 使用 GRUB 作为 UEFI 加载程序，但这个文件可用于未来的直接 UEFI 启动实现。

特点：
- 64 位模式 (UEFI 已初始化)
- 简化的初始化
- 可选的 UEFI 固件调用支持

### 2. `src/linker_efi.ld` - EFI 链接脚本

用于将 EFI 启动代码链接为 ELF 可执行文件。

## 修改的文件

### `Makefile` - 构建系统

主要改动：
1. 简化的编译目标
2. GRUB 自动处理 UEFI 支持
3. 单一的混合 ISO 目标

编译目标：
```
make              # 构建混合 ISO (默认)
make all-bios     # 仅编译 BIOS 版本
make all-efi      # 验证 UEFI 兼容性
make clean        # 清理构建文件
```

### `README.md` - 文档更新

添加了 UEFI 支持说明和简化的构建指令。

## GRUB 的 UEFI 支持

`grub-mkrescue` 工具使用 `--efi-boot-part` 和 `--efi-boot-image` 选项自动：

1. 创建可启动的 ISO 9660 文件系统
2. 添加 BIOS MBR 启动部分
3. 添加 UEFI El Torito 启动部分
4. 包含 GRUB 的 UEFI 启动加载程序

结果是一个真正的混合 ISO，既支持 BIOS 也支持 UEFI。

## 工作原理

### 编译流程

```
源代码 (boot.asm + kernel.c + fs.c)
    ↓
编译为 ELF 对象文件
    ↓
链接到单一的 hbos_bios.bin (Multiboot 格式)
    ↓
创建 ISO 文件系统
    ↓
使用 grub-mkrescue 添加 BIOS 和 UEFI 启动支持
    ↓
最终的 hbos.iso (混合 ISO)
```

### ISO 结构

```
hbos.iso
├── [MBR 启动扇区 - BIOS]
├── [El Torito 启动目录]
├── [GRUB BIOS 启动代码]
├── [GRUB UEFI 启动代码]
└── ISO 9660 文件系统
    ├── boot/
    │   ├── grub/
    │   │   ├── grub.cfg
    │   │   ├── i386-pc/ (BIOS GRUB 模块)
    │   │   └── x86_64-efi/ (UEFI GRUB 模块)
    │   └── hbos.bin
    └── [其他文件]
```

## 运行和测试

### BIOS 模式测试

```bash
make run
# 或
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M
```

### UEFI 模式测试

需要 OVMF (UEFI 固件模拟):

```bash
make run-efi
```

或手动指定固件：
```bash
qemu-system-x86_64 -cdrom build/hbos.iso -m 512M \
  -bios /usr/share/ovmf/OVMF_CODE.fd
```

## 注意事项

### 代码兼容性

两种启动方式都加载和执行相同的内核 (hbos.bin)，所以内核代码必须兼容两种模式。

当前支持：
- ✅ VGA 输出 - 内存映射地址相同
- ✅ 键盘输入 - 端口 I/O 相同
- ✅ 中断处理 - 架构相同

### 未来改进

可能的增强：
1. **直接 UEFI 启动**: 使用 boot_efi.asm 和 linker_efi.ld 创建直接的 EFI 应用程序
2. **UEFI 服务**: 利用 UEFI Boot Services 获取系统信息
3. **EFI 驱动**: 实现 UEFI 驱动程序接口
4. **Secure Boot**: 支持 UEFI Secure Boot

## 参考资源

- [UEFI 规范](https://uefi.org/sites/default/files/resources/UEFI_Spec_2_8_final.pdf)
- [GRUB 2 手册](https://www.gnu.org/software/grub/manual/grub/)
- [OSDev UEFI](https://wiki.osdev.org/UEFI)
- [OSDev Multiboot](https://wiki.osdev.org/Multiboot)
- [OVMF 文档](https://github.com/tianocore/tianocore.github.io/wiki/OVMF)
