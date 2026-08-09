; Entered through the embedded HAX fast path, then replaces itself with the
; >512 KiB musl executable registered at /linux_musl.  A PASS can therefore
; only come from the VFS-backed streaming execve path and real PT_INTERP.

BITS 64
DEFAULT REL
GLOBAL _start

SECTION .text
_start:
    xor eax, eax
    push rax                       ; envp[0] = NULL
    lea rdi, [path]
    push rdi                       ; argv[0]
    mov rsi, rsp
    lea rdx, [rsp + 8]             ; envp
    mov eax, 59                    ; Linux x86_64 SYS_execve
    syscall

    ; execve only returns on failure.
    mov eax, 1                     ; SYS_write
    mov edi, 1
    lea rsi, [failure]
    mov edx, failure_length
    syscall
    mov eax, 60                    ; SYS_exit
    mov edi, 1
    syscall
    ud2

SECTION .rodata
path: db "/linux_musl", 0
failure: db "LINUX_MUSL_STREAM: EXEC_FAIL", 10
failure_length equ $ - failure
