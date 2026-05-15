; UEFI Bootloader for HBOS
; This bootloader is compatible with UEFI systems
; It already runs in 64-bit long mode provided by UEFI firmware

bits 64

section .text
global efi_main
extern kmain

; UEFI Entry Point (efi_main)
; Called by UEFI firmware with:
;   rdi = ImageHandle
;   rsi = SystemTable pointer
;   rdx, rcx, r8, r9 = other parameters
efi_main:
    ; Save callee-saved registers (System V AMD64 ABI)
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; We're already in 64-bit long mode, memory is accessible
    ; Call the kernel main function
    xor rdi, rdi        ; Pass 0 as Multiboot info (we'll handle UEFI differently)
    call kmain

    ; Kernel should not return, but if it does, halt
    cli
    hlt
    jmp $

; Data section (required for ELF format)
section .data
    align 8
    dummy: dq 0

; BSS section
section .bss
    align 4096
    stack_space: resq 4096  ; 32KB stack
