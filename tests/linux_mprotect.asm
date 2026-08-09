; Exercise a JIT-shaped W^X transition through native Linux syscalls:
; RW (write code) -> RX (execute) -> RW (write again), plus ENOMEM for an
; unmapped range so a no-op mprotect implementation cannot pass.

BITS 64
DEFAULT REL
GLOBAL _start

%define SYS_write    1
%define SYS_mmap     9
%define SYS_mprotect 10
%define SYS_munmap   11
%define SYS_exit     60
%define PROT_READ    1
%define PROT_WRITE   2
%define PROT_EXEC    4
%define MAP_PRIVATE  2
%define MAP_ANON     0x20
%define ENOMEM       12

SECTION .text
_start:
    xor edi, edi
    mov esi, 4096
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE | MAP_ANON
    mov r8, -1
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed
    mov rbx, rax

    ; mov eax, 42; ret
    mov byte [rbx + 0], 0xb8
    mov dword [rbx + 1], 42
    mov byte [rbx + 5], 0xc3

    mov rdi, rbx
    mov esi, 4096
    mov edx, PROT_READ | PROT_EXEC
    mov eax, SYS_mprotect
    syscall
    test rax, rax
    js .failed_unmap
    call rbx
    cmp eax, 42
    jne .failed_unmap

    mov rdi, rbx
    mov esi, 4096
    mov edx, PROT_READ | PROT_WRITE
    mov eax, SYS_mprotect
    syscall
    test rax, rax
    js .failed_unmap
    mov byte [rbx], 0xc3             ; write permission restored

    mov rdi, 0x0000400000000000
    mov esi, 4096
    mov edx, PROT_READ
    mov eax, SYS_mprotect
    syscall
    cmp rax, -ENOMEM
    jne .failed_unmap

    mov rdi, rbx
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
    lea rsi, [pass_message]
    mov edx, pass_length
    xor edi, edi
    jmp .print

.failed_unmap:
    mov rdi, rbx
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
.failed:
    lea rsi, [fail_message]
    mov edx, fail_length
    mov edi, 1
.print:
    push rdi
    mov eax, SYS_write
    mov edi, 1
    syscall
    pop rdi
    mov eax, SYS_exit
    syscall
    ud2

SECTION .rodata
pass_message: db "LINUX_MPROTECT: PASS", 10
pass_length equ $ - pass_message
fail_message: db "LINUX_MPROTECT: FAIL", 10
fail_length equ $ - fail_message
