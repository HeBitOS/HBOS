bits 64
default rel

global _start

section .text
_start:
    mov eax, 1
    mov edi, 1
    lea rsi, [message]
    mov edx, message_length
    syscall

    cmp rax, message_length
    jne .failed
    xor edi, edi
    jmp .exit

.failed:
    mov edi, 1
.exit:
    mov eax, 60
    syscall
    ud2

section .rodata
message: db "LINUX_SYSCALL_ELF: PASS", 10
message_length equ $ - message
