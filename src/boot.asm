; Multiboot2 Header - minimal, let GRUB read ELF headers directly
section .multiboot
align 8
header_start:
    dd 0xE85250D6                        ; magic
    dd 0                                 ; ISA: i386
    dd header_end - header_start         ; header length
    dd 0x100000000 - (0xE85250D6 + 0 + (header_end - header_start))  ; checksum
    ; end tag
    dw 0
    dw 0
    dd 8
header_end:

section .text
bits 32
global _start
extern kmain

_start:
    ; Very early output to serial
    mov al, 'X'
    mov dx, 0x3F8
    out dx, al

    mov esp, stack_top
    push ebx                     ; multiboot info

    ; Serial debug: marker for GRUB/Multiboot
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

    ; Serial: long mode available
    mov al, 'L'
    mov dx, 0x3F8
    out dx, al

    ; clear page tables
    mov edi, p4_table
    xor eax, eax
    mov ecx, 3072                ; 3 pages * 1024 dwords
    rep stosd

    ; set up page tables
    mov eax, p3_table
    or eax, 3
    mov [p4_table], eax

    mov eax, p2_table
    or eax, 3
    mov [p3_table], eax

    ; map 1GB using 2MB pages
    mov edi, p2_table
    mov eax, 0x83
    mov ecx, 512
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

    ; Serial: paging enabled
    mov al, 'P'
    mov dx, 0x3F8
    out dx, al

    ; load 64-bit GDT and jump
    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode

.no_long:
    hlt
    jmp $

bits 64
long_mode:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Setup stack for 64-bit
    mov rsp, stack_top

    ; Debug: serial output "OK"
    mov al, 'O'
    mov dx, 0x3F8
    out dx, al
    mov al, 'K'
    out dx, al
    mov al, 10
    out dx, al

    pop rdi                      ; multiboot info
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
p2_table: resb 4096
stack_bottom: resb 65536
stack_top:
