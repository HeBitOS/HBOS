; Verify that a Linux rt_sigaction handler runs at ring 3 and returns through
; rt_sigreturn to the instruction following kill(2).

BITS 64
DEFAULT REL
GLOBAL _start

%define SYS_write        1
%define SYS_rt_sigaction 13
%define SYS_rt_sigreturn 15
%define SYS_mmap         9
%define SYS_mprotect     10
%define SYS_getpid       39
%define SYS_kill         62
%define SYS_exit         60
%define SIGTERM          15
%define SIGSEGV          11
%define SA_RESTORER      0x04000000
%define PROT_NONE        0
%define PROT_READ        1
%define PROT_WRITE       2
%define MAP_PRIVATE      2
%define MAP_ANONYMOUS    0x20

SECTION .text
_start:
    mov eax, SYS_rt_sigaction
    mov edi, SIGTERM
    lea rsi, [action]
    xor edx, edx
    mov r10d, 8                    ; Linux x86-64 sigset_t size
    syscall
    test rax, rax
    js .failed

    mov eax, SYS_rt_sigaction
    mov edi, SIGSEGV
    lea rsi, [action]
    xor edx, edx
    mov r10d, 8
    syscall
    test rax, rax
    js .failed

    mov eax, SYS_getpid
    syscall
    mov edi, eax
    mov esi, SIGTERM
    mov eax, SYS_kill
    syscall
    test rax, rax
    js .failed

    cmp byte [handled], 1
    jne .failed

    ; A protection fault must enter the same ring3 handler, allow it to
    ; repair the page, then resume and retry this exact write.  RCX/R11 are
    ; checked because rt_sigreturn previously lost those registers.
    mov eax, SYS_mmap
    xor edi, edi
    mov esi, 4096
    mov edx, PROT_NONE
    mov r10d, MAP_PRIVATE | MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    js .failed
    mov [fault_page], rax
    mov rcx, 0x1122334455667788
    mov r11, 0x7766554433221100
    mov byte [rax], 0x5a
    cmp byte [fault_handled], 1
    jne .failed
    cmp byte [rax], 0x5a
    jne .failed
    mov rdx, 0x1122334455667788
    cmp rcx, rdx
    jne .failed
    mov rdx, 0x7766554433221100
    cmp r11, rdx
    jne .failed
    lea rsi, [pass_message]
    mov edx, pass_length
    xor edi, edi
    jmp .print

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

signal_handler:
    cmp edi, SIGTERM
    je .term
    cmp edi, SIGSEGV
    jne .handler_return
    mov rdi, [fault_page]
    mov esi, 4096
    mov edx, PROT_READ | PROT_WRITE
    mov eax, SYS_mprotect
    syscall
    test rax, rax
    js .handler_return
    mov byte [fault_handled], 1
    jmp .handler_return
.term:
    mov byte [handled], 1
.handler_return:
    ret

signal_restorer:
    mov eax, SYS_rt_sigreturn
    syscall
    ud2

SECTION .data
align 8
action:
    dq signal_handler
    dq SA_RESTORER
    dq signal_restorer
    dq 0
handled: db 0
fault_handled: db 0
align 8
fault_page: dq 0

SECTION .rodata
pass_message: db "LINUX_SIGNAL: PASS", 10
pass_length equ $ - pass_message
fail_message: db "LINUX_SIGNAL: FAIL", 10
fail_length equ $ - fail_message
