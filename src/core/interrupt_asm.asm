; ============================================================
; HBOS GDT flush + ISR/IRQ stubs
; ============================================================

; Segment selector constants (must match cpu.h)
SEL_KCODE equ 0x08
SEL_KDATA equ 0x10
SEL_TSS   equ 0x28

section .text
bits 64

; ============================================================
; GDT flush — void gdt_flush(uint64_t gdt_ptr_addr);
; ============================================================
global gdt_flush
gdt_flush:
    lgdt [rdi]
    ; Far return to reload CS → kernel code (selector 0x08)
    push SEL_KCODE
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    mov ax, SEL_KDATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; ============================================================
; TSS flush — void tss_flush(void);
; ============================================================
global tss_flush
tss_flush:
    mov ax, SEL_TSS
    ltr ax
    ret

; ============================================================
; ISR stubs — 32 CPU exceptions
; ============================================================
extern isr_handler

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push 0          ; fake error code
    push %1         ; interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push %1         ; interrupt number (error code already on stack)
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; Stub address table for C code
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dq isr_stub_%+i
%assign i i+1
%endrep

; ============================================================
; IRQ stubs (PIC remap: IRQ0→INT 32 … IRQ15→INT 47)
; ============================================================
extern irq_handler
extern syscall_dispatch_frame

%macro IRQ 2
global irq_stub_%1
irq_stub_%1:
    push 0
    push %2
    jmp irq_common
%endmacro

IRQ 0, 32
IRQ 1, 33
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

global irq_stub_table
irq_stub_table:
%assign i 0
%rep 16
    dq irq_stub_%+i
%assign i i+1
%endrep

; ============================================================
; Common ISR handler — saves all regs, calls isr_handler(regs)
; ============================================================
isr_common:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        ; rdi = &isr_regs_t
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

    add rsp, 16         ; pop int_no and err_code
    iretq

; ============================================================
; Common IRQ handler — same but calls irq_handler
; ============================================================
irq_common:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax

    add rsp, 16
    iretq

; ============================================================
; Syscall ABI — int 0x80
; rax = syscall number
; rdi, rsi, rdx, r10, r8, r9 = args 0..5
; return value in rax; negative values are -errno.
; ============================================================
global syscall_int80_stub
syscall_int80_stub:
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax

    mov rdi, rsp
    call syscall_dispatch_frame

    mov [rsp], rax
    pop rax
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9
    iretq
