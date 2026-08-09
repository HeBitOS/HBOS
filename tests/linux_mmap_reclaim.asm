; Validate mmap/munmap semantics and physical-frame reclamation through the
; native Linux x86-64 ABI. sysinfo(2) supplies the observable PMM counter.

BITS 64
DEFAULT REL
GLOBAL _start

%define SYS_write       1
%define SYS_mmap        9
%define SYS_mprotect   10
%define SYS_munmap     11
%define SYS_brk        12
%define SYS_exit       60
%define SYS_sysinfo    99
%define PROT_READ       1
%define PROT_WRITE      2
%define MAP_PRIVATE     2
%define MAP_ANON        0x20
%define MAP_FIXED_NOREPLACE 0x100000
%define ENOMEM          12
%define EEXIST          17
%define PAGE_SIZE       4096
%define BULK_SIZE       (2 * 1024 * 1024)
%define RECLAIM_SLOP    (64 * 1024)
%define SYSINFO_FREERAM 40

SECTION .text
_start:
    lea rdi, [before_info]
    mov eax, SYS_sysinfo
    syscall
    test rax, rax
    js .failed
    mov r12, [before_info + SYSINFO_FREERAM]

    xor edi, edi
    mov esi, BULK_SIZE
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE | MAP_ANON
    mov r8, -1
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed
    mov rbx, rax
    mov byte [rbx], 0x5a
    mov byte [rbx + BULK_SIZE - 1], 0xa5

    lea rdi, [mapped_info]
    mov eax, SYS_sysinfo
    syscall
    test rax, rax
    js .failed_bulk
    mov r13, [mapped_info + SYSINFO_FREERAM]
    cmp r12, r13
    jbe .failed_bulk
    mov rax, r12
    sub rax, r13
    cmp rax, (1024 * 1024)
    jb .failed_bulk

    mov rdi, rbx
    mov esi, BULK_SIZE
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed

    lea rdi, [after_info]
    mov eax, SYS_sysinfo
    syscall
    test rax, rax
    js .failed
    mov rax, [after_info + SYSINFO_FREERAM]
    add rax, RECLAIM_SLOP
    cmp rax, r12
    jb .failed

    ; brk shrink must release full pages above the new break.
    lea rdi, [before_info]
    mov eax, SYS_sysinfo
    syscall
    test rax, rax
    js .failed
    mov r14, [before_info + SYSINFO_FREERAM]
    xor edi, edi
    mov eax, SYS_brk
    syscall
    test rax, rax
    js .failed
    mov r15, rax
    lea rdi, [r15 + 1024 * 1024]
    mov eax, SYS_brk
    syscall
    lea rcx, [r15 + 1024 * 1024]
    cmp rax, rcx
    jne .failed
    lea rdi, [mapped_info]
    mov eax, SYS_sysinfo
    syscall
    test rax, rax
    js .failed_heap
    mov rax, r14
    sub rax, [mapped_info + SYSINFO_FREERAM]
    cmp rax, (512 * 1024)
    jb .failed_heap
    mov rdi, r15
    mov eax, SYS_brk
    syscall
    cmp rax, r15
    jne .failed
    lea rdi, [after_info]
    mov eax, SYS_sysinfo
    syscall
    test rax, rax
    js .failed
    mov rax, [after_info + SYSINFO_FREERAM]
    add rax, RECLAIM_SLOP
    cmp rax, r14
    jb .failed

    ; Punch a one-page hole in a three-page VMA. Both outer fragments must
    ; stay mapped, while mprotect on the hole must report ENOMEM.
    xor edi, edi
    mov esi, 3 * PAGE_SIZE
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE | MAP_ANON
    mov r8, -1
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed
    mov rbx, rax
    mov byte [rbx], 1
    mov byte [rbx + 2 * PAGE_SIZE], 3

    lea rdi, [rbx + PAGE_SIZE]
    mov esi, PAGE_SIZE
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed_three

    lea rdi, [rbx + PAGE_SIZE]
    mov esi, PAGE_SIZE
    mov edx, PROT_READ
    mov eax, SYS_mprotect
    syscall
    cmp rax, -ENOMEM
    jne .failed_three

    ; Refill precisely the punched hole without replacing its neighbours.
    lea rdi, [rbx + PAGE_SIZE]
    mov esi, PAGE_SIZE
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE
    mov r8, -1
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    lea rcx, [rbx + PAGE_SIZE]
    cmp rax, rcx
    jne .failed_three
    ; A second no-replace request over the same page must fail with EEXIST.
    lea rdi, [rbx + PAGE_SIZE]
    mov esi, PAGE_SIZE
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE
    mov r8, -1
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    cmp rax, -EEXIST
    jne .failed_three

    mov rdi, rbx
    mov esi, PAGE_SIZE
    mov edx, PROT_READ | PROT_WRITE
    mov eax, SYS_mprotect
    syscall
    test rax, rax
    js .failed_three
    lea rdi, [rbx + 2 * PAGE_SIZE]
    mov esi, PAGE_SIZE
    mov edx, PROT_READ | PROT_WRITE
    mov eax, SYS_mprotect
    syscall
    test rax, rax
    js .failed_three

    mov rdi, rbx
    mov esi, 3 * PAGE_SIZE
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed
    ; Linux accepts a repeated munmap over an already-empty interval.
    mov rdi, rbx
    mov esi, 3 * PAGE_SIZE
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed

    lea rsi, [pass_message]
    mov edx, pass_length
    xor edi, edi
    jmp .print

.failed_bulk:
    mov rdi, rbx
    mov esi, BULK_SIZE
    mov eax, SYS_munmap
    syscall
    jmp .failed
.failed_heap:
    mov rdi, r15
    mov eax, SYS_brk
    syscall
    jmp .failed
.failed_three:
    mov rdi, rbx
    mov esi, 3 * PAGE_SIZE
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
pass_message: db "LINUX_MMAP_RECLAIM: PASS", 10
pass_length equ $ - pass_message
fail_message: db "LINUX_MMAP_RECLAIM: FAIL", 10
fail_length equ $ - fail_message

SECTION .bss
align 8
before_info: resb 112
mapped_info: resb 112
after_info: resb 112
