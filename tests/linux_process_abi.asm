; Chromium/Qt-facing Linux process/resource ABI smoke test.

BITS 64
DEFAULT REL
GLOBAL _start

%define SYS_write            1
%define SYS_mmap             9
%define SYS_munmap          11
%define SYS_madvise         28
%define SYS_getpid          39
%define SYS_fork            57
%define SYS_exit            60
%define SYS_wait4           61
%define SYS_getrusage       98
%define SYS_prctl          157
%define SYS_gettid         186
%define SYS_clock_gettime  228
%define SYS_clock_nanosleep 230
%define SYS_tgkill         234
%define SYS_prlimit64      302

%define PROT_READ            1
%define PROT_WRITE           2
%define MAP_PRIVATE          2
%define MAP_ANON             0x20
%define RLIMIT_NOFILE        7
%define PR_SET_PDEATHSIG     1
%define PR_GET_PDEATHSIG     2
%define PR_GET_DUMPABLE      3
%define PR_SET_DUMPABLE      4
%define PR_SET_NAME         15
%define PR_GET_NAME         16
%define PR_SET_NO_NEW_PRIVS 38
%define PR_GET_NO_NEW_PRIVS 39
%define MADV_DONTNEED        4
%define EINVAL              22
%define ENOMEM              12
%define EPERM                1
%define ESRCH                3

SECTION .text
_start:
    ; prlimit64 query exposes the real fixed fd-table limit.
    xor edi, edi
    mov esi, RLIMIT_NOFILE
    xor edx, edx
    lea r10, [old_limit]
    mov eax, SYS_prlimit64
    syscall
    test rax, rax
    js .failed
    cmp qword [old_limit], 128
    jne .failed
    cmp qword [old_limit + 8], 128
    jne .failed
    xor edi, edi
    mov esi, RLIMIT_NOFILE
    lea rdx, [old_limit]
    xor r10d, r10d
    mov eax, SYS_prlimit64
    syscall
    test rax, rax
    js .failed
    mov qword [new_limit], 64
    mov qword [new_limit + 8], 64
    xor edi, edi
    mov esi, RLIMIT_NOFILE
    lea rdx, [new_limit]
    xor r10d, r10d
    mov eax, SYS_prlimit64
    syscall
    cmp rax, -EPERM
    jne .failed

    xor edi, edi
    lea rsi, [usage]
    mov eax, SYS_getrusage
    syscall
    test rax, rax
    js .failed
    mov edi, 2
    lea rsi, [usage]
    mov eax, SYS_getrusage
    syscall
    cmp rax, -EINVAL
    jne .failed

    mov edi, PR_SET_NAME
    lea rsi, [process_name]
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    mov eax, SYS_prctl
    syscall
    test rax, rax
    js .failed
    mov edi, PR_GET_NAME
    lea rsi, [name_buffer]
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    mov eax, SYS_prctl
    syscall
    test rax, rax
    js .failed
    mov rax, [process_name]
    cmp rax, [name_buffer]
    jne .failed

    mov edi, PR_SET_NO_NEW_PRIVS
    mov esi, 1
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    mov eax, SYS_prctl
    syscall
    test rax, rax
    js .failed
    mov edi, PR_GET_NO_NEW_PRIVS
    xor esi, esi
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    mov eax, SYS_prctl
    syscall
    cmp rax, 1
    jne .failed

    mov edi, PR_SET_PDEATHSIG
    mov esi, 10
    xor edx, edx
    xor r10d, r10d
    xor r8d, r8d
    mov eax, SYS_prctl
    syscall
    test rax, rax
    js .failed
    mov dword [signal_value], 0
    mov edi, PR_GET_PDEATHSIG
    lea rsi, [signal_value]
    mov eax, SYS_prctl
    syscall
    test rax, rax
    js .failed
    cmp dword [signal_value], 10
    jne .failed

    mov edi, PR_GET_DUMPABLE
    mov eax, SYS_prctl
    syscall
    cmp rax, 1
    jne .failed
    mov edi, PR_SET_DUMPABLE
    xor esi, esi
    mov eax, SYS_prctl
    syscall
    test rax, rax
    js .failed
    mov edi, PR_GET_DUMPABLE
    mov eax, SYS_prctl
    syscall
    test rax, rax
    jne .failed

    mov eax, SYS_getpid
    syscall
    mov r12d, eax
    mov eax, SYS_gettid
    syscall
    mov r13d, eax
    mov edi, r12d
    mov esi, r13d
    xor edx, edx
    mov eax, SYS_tgkill
    syscall
    test rax, rax
    js .failed
    lea edi, [r12d + 1]
    mov esi, r13d
    xor edx, edx
    mov eax, SYS_tgkill
    syscall
    cmp rax, -ESRCH
    jne .failed

    mov edi, 1
    lea rsi, [before_time]
    mov eax, SYS_clock_gettime
    syscall
    test rax, rax
    js .failed
    xor edi, edi
    lea rsi, [realtime]
    mov eax, SYS_clock_gettime
    syscall
    test rax, rax
    js .failed
    cmp qword [realtime], 1500000000
    jb .failed

    mov qword [sleep_time], 0
    ; PIT runs at 100 Hz. Five ticks avoids a host/QEMU scheduling boundary
    ; making this monotonic-clock assertion intermittently observe no change.
    mov qword [sleep_time + 8], 50000000
    mov edi, 1
    xor esi, esi
    lea rdx, [sleep_time]
    lea r10, [remaining_time]
    mov eax, SYS_clock_nanosleep
    syscall
    test rax, rax
    js .failed
    mov edi, 1
    lea rsi, [after_time]
    mov eax, SYS_clock_gettime
    syscall
    test rax, rax
    js .failed
    mov rax, [after_time]
    cmp rax, [before_time]
    ja .clock_advanced
    jb .failed
    mov rax, [after_time + 8]
    cmp rax, [before_time + 8]
    jbe .failed
.clock_advanced:
    mov qword [sleep_time + 8], 1000000000
    mov edi, 1
    xor esi, esi
    lea rdx, [sleep_time]
    xor r10d, r10d
    mov eax, SYS_clock_nanosleep
    syscall
    cmp rax, -EINVAL
    jne .failed

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
    mov byte [rbx], 0x7f
    mov rdi, rbx
    mov esi, 4096
    mov edx, MADV_DONTNEED
    mov eax, SYS_madvise
    syscall
    test rax, rax
    js .failed_unmap
    cmp byte [rbx], 0
    jne .failed_unmap
    mov rdi, rbx
    mov esi, 4096
    mov edx, 0x7fffffff
    mov eax, SYS_madvise
    syscall
    cmp rax, -EINVAL
    jne .failed_unmap
    mov rdi, rbx
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed
    mov rdi, rbx
    mov esi, 4096
    xor edx, edx
    mov eax, SYS_madvise
    syscall
    cmp rax, -ENOMEM
    jne .failed

    ; Native fork must resume the child at this syscall boundary and share
    ; private owned pages read-only until either side writes.  The child's
    ; write must not change the parent; after child exit the parent's sole
    ; reference should become writable without another 4 KiB copy.
    xor edi, edi
    mov esi, 12288
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE | MAP_ANON
    mov r8, -1
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed
    mov rbx, rax
    mov r15d, 8
.cow_round:
    mov byte [rbx], 0x11
    mov byte [rbx + 4096], 0x44
    mov byte [rbx + 8192], 0x66
    mov eax, SYS_fork
    syscall
    test rax, rax
    js .failed_unmap
    jz .cow_child
    mov r14, rax
    mov rdi, r14
    lea rsi, [child_status]
    xor edx, edx
    xor r10d, r10d
    mov eax, SYS_wait4
    syscall
    cmp rax, r14
    jne .failed_unmap
    cmp dword [child_status], 37 << 8
    jne .failed_unmap
    cmp byte [rbx], 0x11
    jne .failed_unmap
    cmp byte [rbx + 4096], 0x44
    jne .failed_unmap
    cmp byte [rbx + 8192], 0x66
    jne .failed_unmap
    mov byte [rbx], 0x33
    cmp byte [rbx], 0x33
    jne .failed_unmap
    dec r15d
    jnz .cow_round
    mov rdi, rbx
    mov esi, 12288
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed

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

.cow_child:
    cmp byte [rbx], 0x11
    jne .cow_child_fail
    mov byte [rbx], 0x22
    cmp byte [rbx], 0x22
    jne .cow_child_fail
    ; Explicitly re-enabling write access must split a COW page before the
    ; PTE becomes writable.
    lea rdi, [rbx + 4096]
    mov esi, 4096
    mov edx, PROT_READ | PROT_WRITE
    mov eax, 10                    ; mprotect
    syscall
    test rax, rax
    js .cow_child_fail
    mov byte [rbx + 4096], 0x55
    ; Kernel-side MADV_DONTNEED zeroing must also privatize first instead of
    ; clearing the physical page still visible to the parent.
    lea rdi, [rbx + 8192]
    mov esi, 4096
    mov edx, MADV_DONTNEED
    mov eax, SYS_madvise
    syscall
    test rax, rax
    js .cow_child_fail
    cmp byte [rbx + 8192], 0
    jne .cow_child_fail
    mov edi, 37
    mov eax, SYS_exit
    syscall
    ud2
.cow_child_fail:
    mov edi, 99
    mov eax, SYS_exit
    syscall
    ud2

SECTION .rodata
process_name: db "hbos-abi", 0
pass_message: db "LINUX_PROCESS_ABI: PASS", 10
pass_length equ $ - pass_message
fail_message: db "LINUX_PROCESS_ABI: FAIL", 10
fail_length equ $ - fail_message

SECTION .bss
alignb 8
old_limit: resb 16
new_limit: resb 16
usage: resb 144
name_buffer: resb 16
signal_value: resd 1
alignb 8
sleep_time: resb 16
remaining_time: resb 16
before_time: resb 16
after_time: resb 16
realtime: resb 16
child_status: resd 1
