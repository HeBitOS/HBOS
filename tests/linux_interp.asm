; Minimal ET_DYN PT_INTERP smoke loader.
; The kernel enters this image with Linux's initial stack.  Validate the
; auxiliary vector contract that a real musl/glibc loader needs, then exit.

BITS 64
GLOBAL _start

SECTION .text
_start:
    mov rbx, rsp
    mov rcx, [rbx]                 ; argc
    lea rsi, [rbx + 8]
    lea rsi, [rsi + rcx * 8 + 8]   ; after argv NULL

.skip_env:
    mov rax, [rsi]
    add rsi, 8
    test rax, rax
    jnz .skip_env

    xor r12d, r12d                 ; bit 0 = AT_PHDR
                                      ; bit 1 = AT_BASE
                                      ; bit 2 = AT_ENTRY
.scan_aux:
    mov rax, [rsi]
    mov rdx, [rsi + 8]
    add rsi, 16
    test rax, rax
    jz .check
    cmp rax, 3                     ; AT_PHDR
    jne .not_phdr
    test rdx, rdx
    jz .fail
    or r12d, 1
.not_phdr:
    cmp rax, 7                     ; AT_BASE
    jne .not_base
    test rdx, rdx
    jz .fail
    or r12d, 2
.not_base:
    cmp rax, 9                     ; AT_ENTRY
    jne .scan_aux
    test rdx, rdx
    jz .fail
    or r12d, 4
    jmp .scan_aux

.check:
    cmp r12d, 7
    jne .fail
    lea rsi, [rel pass_message]
    mov edx, pass_length
    jmp .write

.fail:
    lea rsi, [rel fail_message]
    mov edx, fail_length

.write:
    mov eax, 1                     ; SYS_write
    mov edi, 1
    syscall
    mov eax, 60                    ; SYS_exit
    xor edi, edi
    syscall
    ud2

SECTION .rodata
pass_message: db "LINUX_INTERP: PASS", 10
pass_length equ $ - pass_message
fail_message: db "LINUX_INTERP: FAIL", 10
fail_length equ $ - fail_message
