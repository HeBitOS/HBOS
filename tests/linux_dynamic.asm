; The PT_INTERP smoke loader should run before this executable entry point.
; Reaching this code means the kernel ignored or misrouted PT_INTERP.

BITS 64
GLOBAL _start

SECTION .text
_start:
    mov eax, 1                     ; SYS_write
    mov edi, 1
    lea rsi, [rel fail_message]
    mov edx, fail_length
    syscall
    mov eax, 60                    ; SYS_exit
    mov edi, 1
    syscall
    ud2

SECTION .rodata
fail_message: db "LINUX_INTERP: MAIN_ENTRY_FAIL", 10
fail_length equ $ - fail_message
