; Linux x86-64 regular-file mmap compatibility smoke test.
; Maps this executable through the HBOS VFS and verifies private snapshot,
; close-after-map lifetime, page offsets, zero-filled EOF tail, and the
; explicit read-only MAP_SHARED baseline.

BITS 64
DEFAULT REL
GLOBAL _start

%define SYS_read      0
%define SYS_write     1
%define SYS_open      2
%define SYS_close     3
%define SYS_fstat     5
%define SYS_lseek     8
%define SYS_mmap      9
%define SYS_munmap   11
%define SYS_madvise  28
%define SYS_exit     60

%define PROT_READ     1
%define PROT_WRITE    2
%define MAP_SHARED    1
%define MAP_PRIVATE   2
%define SEEK_SET      0
%define MADV_DONTNEED 4
%define EOPNOTSUPP    95

SECTION .text
_start:
    xor ebx, ebx                    ; optional final-tail mapping
    mov byte [fail_stage], '1'
    lea rdi, [self_path]
    xor esi, esi                    ; O_RDONLY
    xor edx, edx
    mov eax, SYS_open
    syscall
    test rax, rax
    js .failed
    mov r12, rax
    mov byte [fail_stage], '2'

    mov edi, r12d
    lea rsi, [stat_buffer]
    mov eax, SYS_fstat
    syscall
    test rax, rax
    js .failed_close
    mov r13, [stat_buffer + 48]      ; Linux x86-64 struct stat.st_size
    mov byte [fail_stage], '3'
    cmp r13, 4096
    jbe .failed_close

    ; MAP_PRIVATE must copy file bytes into owned pages.
    xor edi, edi
    mov esi, 4096
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE
    mov r8d, r12d
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed_close
    mov r14, rax
    mov byte [fail_stage], '4'
    cmp dword [r14], 0x464c457f     ; ELF magic
    jne .failed_private

    ; Linux mappings outlive their source fd. Private writes must not alter
    ; the underlying VFS file.
    mov edi, r12d
    mov eax, SYS_close
    syscall
    test rax, rax
    js .failed_private
    mov byte [r14], 'H'
    mov byte [fail_stage], '5'
    cmp byte [r14], 'H'
    jne .failed_private

    lea rdi, [self_path]
    xor esi, esi
    xor edx, edx
    mov eax, SYS_open
    syscall
    test rax, rax
    js .failed_private
    mov r12, rax
    mov byte [fail_stage], '6'
    mov edi, r12d
    lea rsi, [first_byte]
    mov edx, 1
    mov eax, SYS_read
    syscall
    cmp rax, 1
    jne .failed_all
    cmp byte [first_byte], 0x7f
    jne .failed_all
    mov byte [fail_stage], '7'

    ; A read-only MAP_SHARED snapshot is sufficient for executable/library
    ; segment loading. Writable VFS sharing is rejected until writeback and
    ; coherent page-cache support exist.
    xor edi, edi
    mov esi, 4096
    mov edx, PROT_READ
    mov r10d, MAP_SHARED
    mov r8d, r12d
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed_all
    mov r15, rax
    mov byte [fail_stage], '8'
    cmp dword [r15], 0x464c457f
    jne .failed_shared

    xor edi, edi
    mov esi, 4096
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_SHARED
    mov r8d, r12d
    xor r9d, r9d
    mov eax, SYS_mmap
    syscall
    cmp rax, -EOPNOTSUPP
    jne .failed_shared
    mov byte [fail_stage], '9'

    ; A nonzero page offset must select the matching file bytes.
    mov edi, r12d
    mov esi, 4096
    mov edx, SEEK_SET
    mov eax, SYS_lseek
    syscall
    cmp rax, 4096
    jne .failed_shared
    mov edi, r12d
    lea rsi, [offset_byte]
    mov edx, 1
    mov eax, SYS_read
    syscall
    cmp rax, 1
    jne .failed_shared
    mov byte [fail_stage], 'A'

    xor edi, edi
    mov esi, 4096
    mov edx, PROT_READ
    mov r10d, MAP_PRIVATE
    mov r8d, r12d
    mov r9d, 4096
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed_shared
    mov rbp, rax
    mov byte [fail_stage], 'B'
    mov al, [offset_byte]
    cmp byte [rbp], al
    jne .failed_offset

    ; Until demand paging can fault file contents back in, MADV_DONTNEED is
    ; advisory for file snapshots and must never zero a mapped ELF segment.
    mov rdi, rbp
    mov esi, 4096
    mov edx, MADV_DONTNEED
    mov eax, SYS_madvise
    syscall
    test rax, rax
    js .failed_offset
    mov al, [offset_byte]
    cmp byte [rbp], al
    jne .failed_offset

    ; The final partial page is initialized to zero after st_size. HBOS does
    ; not yet synthesize SIGBUS for whole pages beyond EOF, so it rejects
    ; mappings whose starting offset is at/after EOF.
    mov rax, r13
    and rax, -4096
    mov [tail_offset], rax
    cmp rax, r13
    je .cleanup                    ; page-aligned file: no partial tail
    mov byte [fail_stage], 'C'
    xor edi, edi
    mov esi, 4096
    mov edx, PROT_READ
    mov r10d, MAP_PRIVATE
    mov r8d, r12d
    mov r9, [tail_offset]
    mov eax, SYS_mmap
    syscall
    test rax, rax
    js .failed_offset
    mov rbx, rax
    mov rcx, r13
    sub rcx, [tail_offset]
    cmp byte [rbx + rcx], 0
    jne .failed_tail

.cleanup:
    mov byte [fail_stage], 'D'
    test rbx, rbx
    jz .cleanup_offset
    mov rdi, rbx
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed_all
.cleanup_offset:
    mov rdi, rbp
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed_all
    mov rdi, r15
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed_all
    mov rdi, r14
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
    test rax, rax
    js .failed_all
    mov edi, r12d
    mov eax, SYS_close
    syscall
    test rax, rax
    js .failed

    mov edi, 1
    lea rsi, [pass_message]
    mov edx, pass_message_end - pass_message
    mov eax, SYS_write
    syscall
    xor edi, edi
    mov eax, SYS_exit
    syscall

.failed_tail:
    mov rdi, rbx
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
.failed_offset:
    mov rdi, rbp
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
.failed_shared:
    mov rdi, r15
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
.failed_all:
    mov edi, r12d
    mov eax, SYS_close
    syscall
.failed_private:
    mov rdi, r14
    mov esi, 4096
    mov eax, SYS_munmap
    syscall
    jmp .failed
.failed_close:
    mov edi, r12d
    mov eax, SYS_close
    syscall
.failed:
    mov edi, 2
    lea rsi, [fail_message]
    mov edx, fail_message_end - fail_message
    mov eax, SYS_write
    syscall
    mov edi, 1
    mov eax, SYS_exit
    syscall

SECTION .rodata
self_path: db "/linux_file_mmap", 0
pass_message: db "LINUX_FILE_MMAP: PASS", 10
pass_message_end:

SECTION .data
fail_message: db "LINUX_FILE_MMAP: FAIL stage="
fail_stage: db "?", 10
fail_message_end:

SECTION .bss
align 8
stat_buffer: resb 144
tail_offset: resq 1
first_byte: resb 1
offset_byte: resb 1
