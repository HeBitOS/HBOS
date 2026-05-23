; Multiboot2 Header - request high-resolution framebuffer
section .multiboot
align 8
header_start:
    dd 0xE85250D6                        ; magic
    dd 0                                 ; ISA: i386
    dd header_end - header_start         ; header length
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))  ; checksum

    ; Framebuffer request tag (type 5) - optional
    ; Use 0/0/0 so GRUB chooses any available framebuffer mode.
    ; This avoids "no suitable video mode found" on VirtualBox/VMSVGA/VBoxVGA.
    align 8
    dw 5
    dw 1                                 ; flags: 1=optional
    dd 20                                ; size
    dd 0                                 ; preferred width:  no preference
    dd 0                                 ; preferred height: no preference
    dd 0                                 ; preferred depth:  no preference

    ; end tag
    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .text
bits 32
global _start
extern kmain

_start:
    mov al, 'X'
    mov dx, 0x3F8
    out dx, al

    mov esp, stack_top
    mov [saved_mbi], ebx

    mov al, 'B'
    mov dx, 0x3F8
    out dx, al

    ; check long mode
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long

    mov al, 'L'
    mov dx, 0x3F8
    out dx, al

    ; Clear page table memory: P4 + P3 + 4x P2 = 6 pages = 24576 bytes
    mov edi, p4_table
    xor eax, eax
    mov ecx, 24576 / 4                  ; 6144 dwords
    rep stosd

    ; P4[0] → P3
    mov eax, p3_table
    or eax, 3
    mov [p4_table], eax

    ; P3[0..3] → P2_0 .. P2_3  (covers 0-4GB)
    mov edi, p3_table
    mov ebx, p2_0
    mov ecx, 4
.set_p3:
    mov eax, ebx
    or eax, 3
    mov [edi], eax
    add edi, 8
    add ebx, 4096
    loop .set_p3

    ; Fill 4 P2 tables (2048 entries total) with 2MB pages = 4GB
    mov edi, p2_0
    mov eax, 0x83                       ; present + writable + 2MB page
    mov ecx, 2048
.fill_p2:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill_p2

    ; enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax
    ; load page table
    mov eax, p4_table
    mov cr3, eax
    ; enable long mode
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr
    ; enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    mov al, 'P'
    mov dx, 0x3F8
    out dx, al

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode

.no_long:
    hlt
    jmp $

bits 64
default abs
long_mode:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rdi, [saved_mbi]
    mov rsp, stack_top

    mov al, 'O'
    mov dx, 0x3F8
    out dx, al
    mov al, 'K'
    out dx, al
    mov al, 10
    out dx, al

    call kmain

.halt:
    hlt
    jmp .halt

section .rodata
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .bss
align 4096
p4_table: resb 4096
p3_table: resb 4096
p2_0: resb 4096    ; 0GB - 1GB
p2_1: resb 4096    ; 1GB - 2GB
p2_2: resb 4096    ; 2GB - 3GB
p2_3: resb 4096    ; 3GB - 4GB
saved_mbi: resq 1
stack_bottom: resb 65536
stack_top:
