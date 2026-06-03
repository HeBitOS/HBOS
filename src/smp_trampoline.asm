; ============================================================
; SMP AP Trampoline — Entry point for Application Processors
; ============================================================
; Executed at physical address 0x8000 in 16-bit real mode.
; Switches to 64-bit long mode and enters ap_trampoline_entry.
; ============================================================

bits 16
section .text

global ap_trampoline_start
global ap_trampoline_end

ap_trampoline_start:
    cli
    cld

    ; Zero data segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000

    ; Load GDT for protected mode
    lgdt [ap_gdt_ptr - ap_trampoline_start + 0x8000]

    ; Enable protected mode
    mov eax, cr0
    or al, 1
    mov cr0, eax

    ; Far jump to 32-bit protected mode
    jmp 0x08:(ap_protected - ap_trampoline_start + 0x8000)

bits 32
ap_protected:
    ; Set up 32-bit segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9000

    ; Set up 4-level paging (use existing kernel page tables)
    ; CR3 already valid from BSP
    mov eax, [0x8FE4]
    mov cr3, eax

    ; Enable PAE
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; Enable long mode via EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; Load 64-bit GDT
    lgdt [ap_gdt64_ptr - ap_trampoline_start + 0x8000]

    ; Far jump to 64-bit long mode
    jmp 0x08:(ap_longmode - ap_trampoline_start + 0x8000)

default abs
bits 64
ap_longmode:
    ; Set up 64-bit segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up stack
    mov rsp, [0x8FE4 + 8]

    ; Set CR3 to kernel page tables
    mov rax, [0x8FE4]
    mov cr3, rax

    ; Call C entry function via pointer stored at 0x8FF0
    mov rax, [0x8FF0]
    call rax

    ; Halt if entry returns
    cli
    hlt
    jmp $ - 2

align 16
ap_gdt:
    dq 0                       ; Null descriptor
    dq 0x00CF9A000000FFFF      ; 32-bit code
    dq 0x00CF92000000FFFF      ; 32-bit data

ap_gdt_ptr:
    dw 23
    dd ap_gdt - ap_trampoline_start + 0x8000

align 16
ap_gdt64:
    dq 0                       ; Null descriptor
    dq 0x00209A0000000000      ; 64-bit code
    dq 0x0000920000000000      ; 64-bit data

ap_gdt64_ptr:
    dw 23
    dd ap_gdt64 - ap_trampoline_start + 0x8000

ap_trampoline_end: