; ============================================================
; HBOS BIOS 启动入口 (Multiboot2)
; ============================================================
;
; 启动流程:
;   1. GRUB 加载内核，解析 Multiboot2 头，跳转到 _start（32 位保护模式）
;   2. 检查 CPU 是否支持 long mode（CPUID 0x80000001 EDX bit 29）
;   3. 构建 4-level 页表（恒等映射 0-4GB，使用 2MB 大页）
;   4. 启用 PAE → 加载 CR3 → 设置 EFER.LME → 启用分页（进入 long mode）
;   5. 加载 64 位 GDT，远跳转刷新 CS
;   6. 在 64 位模式下设置段寄存器，调用 kmain(mbi)
;
; 串口调试输出（COM1 0x3F8）:
;   'X' = 入口点到达
;   'B' = Multiboot2 信息已保存
;   'L' = long mode 可用
;   'P' = 分页已启用
;   'OK' = 进入 64 位模式，即将调用 kmain
; ============================================================

; ---- Multiboot2 头部 ----
; GRUB 在加载内核时解析此头部，获取所需信息
section .multiboot
align 8
header_start:
    dd 0xE85250D6                        ; 魔数
    dd 0                                 ; 架构: i386 (protected mode)
    dd header_end - header_start         ; 头部总长度
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))  ; 校验和

    ; Framebuffer 请求标签 (type 5) — 可选
    ; 请求 1024x768x32 — 所有 VESA BIOS 支持的基础高分辨率
    ; GRUB 配置使用 gfxmode=1440x900x32,1024x768x32,auto + gfxpayload=keep
    align 8
    dw 5                                 ; 标签类型: framebuffer
    dw 1                                 ; 标志: 可选
    dd 20                                ; 标签大小
    dd 1024                              ; 首选宽度 (VirtualBox compatible)
    dd 768                               ; 首选高度
    dd 32                                ; 首选色深 (bpp)

    ; 结束标签
    align 8
    dw 0                                 ; 类型 0 = 结束
    dw 0
    dd 8                                 ; 大小
header_end:

; ---- 32 位保护模式入口 ----
section .text
bits 32
global _start
extern kmain

_start:
    ; 调试: 输出 'X' 表示到达入口点
    mov al, 'X'
    mov dx, 0x3F8
    out dx, al

    ; 设置栈指针并保存 Multiboot2 信息指针（EBX 由 GRUB 传入）
    mov esp, stack_top
    mov [saved_mbi], ebx

    mov al, 'B'
    mov dx, 0x3F8
    out dx, al

    ; ---- 检查 CPU 是否支持 long mode ----
    ; CPUID 0x80000001 EDX bit 29 = long mode 支持
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001                  ; 检查是否有扩展功能
    jb .no_long
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29                    ; 测试 LM (long mode) 位
    jz .no_long

    mov al, 'L'
    mov dx, 0x3F8
    out dx, al

    ; ---- 构建 4-level 页表 (恒等映射 0-4GB) ----
    ; 结构: P4 → P3 → 4×P2 (每个 P2 覆盖 1GB，使用 2MB 大页)
    ; 总共 6 个 4KB 页 = 24576 字节

    ; 清零页表内存
    mov edi, p4_table
    xor eax, eax
    mov ecx, 24576 / 4                   ; 6144 个 dword
    rep stosd

    ; P4[0] → P3 (设置 present + writable 标志)
    mov eax, p3_table
    or eax, 3                            ; bits 0-1: present + writable
    mov [p4_table], eax

    ; P3[0..3] → P2_0 .. P2_3 (每个 P2 表覆盖 1GB)
    mov edi, p3_table
    mov ebx, p2_0
    mov ecx, 4
.set_p3:
    mov eax, ebx
    or eax, 3                            ; present + writable
    mov [edi], eax
    add edi, 8                           ; 每个 P3 条目 8 字节
    add ebx, 4096                        ; 下一个 P2 表
    loop .set_p3

    ; 填充 4 个 P2 表（共 2048 个条目），每个条目映射 2MB
    ; 使用 2MB 大页 (PS=1) 实现恒等映射 0-4GB
    mov edi, p2_0
    mov eax, 0x83                        ; present + writable + 2MB page (bit 7)
    mov ecx, 2048                        ; 4 × 512 = 2048 个条目
.fill_p2:
    mov [edi], eax
    add eax, 0x200000                    ; 下一个 2MB 物理地址
    add edi, 8
    loop .fill_p2

    ; ---- 启用 long mode ----
    ; 步骤: PAE → CR3 → EFER.LME → CR0.PG
    mov eax, cr4
    or eax, 1 << 5                       ; 设置 CR4.PAE
    mov cr4, eax

    mov eax, p4_table                    ; 加载页表基址
    mov cr3, eax

    mov ecx, 0xC0000080                  ; EFER MSR
    rdmsr
    or eax, 1 << 8                       ; 设置 EFER.LME (long mode enable)
    wrmsr

    mov eax, cr0
    or eax, 1 << 31                      ; 设置 CR0.PG (paging enable)
    mov cr0, eax                          ; CPU 现在处于兼容模式

    mov al, 'P'
    mov dx, 0x3F8
    out dx, al

    ; 加载 64 位 GDT 并远跳转进入 long mode
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode

.no_long:
    hlt
    jmp $                                ; CPU 不支持 long mode — 死循环

; ---- 64 位 long mode 入口 ----
bits 64
default abs
long_mode:
    ; 清零所有段寄存器（64 位模式下不使用分段）
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; 恢复 Multiboot2 信息指针并设置 64 位栈
    mov rdi, [saved_mbi]                 ; 第一个参数: mbi (System V ABI)
    mov rsp, stack_top

    ; 调试: 输出 "OK\n"
    mov al, 'O'
    mov dx, 0x3F8
    out dx, al
    mov al, 'K'
    out dx, al
    mov al, 10                           ; '\n'
    out dx, al

    call kmain                           ; 调用 C 内核入口，永不返回

.halt:
    hlt
    jmp .halt

; ---- 64 位 GDT（最小配置，仅用于远跳转） ----
; 内核稍后会在 gdt_idt_init() 中设置完整的 GDT
section .rodata
gdt64:
    dq 0                                 ; NULL 描述符
.code: equ $ - gdt64
    ; 64 位代码段: L=1, D=0, P=1, DPL=0, 代码段类型
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.pointer:
    dw $ - gdt64 - 1                     ; GDT 界限
    dq gdt64                             ; GDT 基址

; ---- BSS 段（未初始化数据） ----
section .bss
align 4096
p4_table: resb 4096                      ; PML4 表
p3_table: resb 4096                      ; PDPT 表
p2_0: resb 4096                          ; PD 表: 0GB - 1GB
p2_1: resb 4096                          ; PD 表: 1GB - 2GB
p2_2: resb 4096                          ; PD 表: 2GB - 3GB
p2_3: resb 4096                          ; PD 表: 3GB - 4GB
saved_mbi: resq 1                        ; 保存的 Multiboot2 信息指针
stack_bottom: resb 65536                 ; 64KB 内核栈
stack_top:
