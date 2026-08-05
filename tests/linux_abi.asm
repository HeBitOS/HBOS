bits 64
default rel

global _start

section .text
_start:
    mov r13d, 11
    ; Linux x86-64 struct stat conversion: '/' must be a directory.
    mov eax, 4
    lea rdi, [root_path]
    lea rsi, [stat_buffer]
    syscall
    test rax, rax
    jnz .failed
    mov r13d, 12
    mov eax, [stat_buffer + 24]
    and eax, 0o170000
    cmp eax, 0o040000
    jne .failed

    mov r13d, 2
    ; Linux getdents64 uses variable, 8-byte-aligned records and fd offsets.
    mov eax, 2
    lea rdi, [root_path]
    mov esi, 0x10000                 ; O_DIRECTORY
    xor edx, edx
    syscall
    test rax, rax
    js .failed
    mov r12, rax

    mov eax, 217
    mov rdi, r12
    lea rsi, [directory_buffer]
    mov edx, 1024
    syscall
    test rax, rax
    jle .failed
    cmp word [directory_buffer + 16], 24
    jb .failed

    mov eax, 3
    mov rdi, r12
    syscall
    test rax, rax
    jnz .failed

    mov r13d, 3
    ; Linux readv/writev layouts and partial-vector accounting.
    mov eax, 22
    lea rdi, [pipe_fds]
    syscall
    test rax, rax
    jnz .failed

    mov eax, 20
    mov edi, [pipe_fds + 4]
    lea rsi, [send_vectors]
    mov edx, 2
    syscall
    cmp rax, 4
    jne .failed

    mov eax, 19
    mov edi, [pipe_fds]
    lea rsi, [receive_vectors]
    mov edx, 2
    syscall
    cmp rax, 4
    jne .failed
    cmp dword [receive_buffer], 0x73756264 ; "dbus"
    jne .failed

    mov eax, 3
    mov edi, [pipe_fds]
    syscall
    mov eax, 3
    mov edi, [pipe_fds + 4]
    syscall

    mov r13d, 4
    ; D-Bus' AF_UNIX sendmsg/recvmsg path.
    mov eax, 53
    mov edi, 1                        ; AF_UNIX
    mov esi, 1                        ; SOCK_STREAM
    xor edx, edx
    lea r10, [socket_fds]
    syscall
    test rax, rax
    jnz .failed

    mov eax, 46
    mov edi, [socket_fds]
    lea rsi, [send_message]
    xor edx, edx
    syscall
    cmp rax, 4
    jne .failed

    mov eax, 47
    mov edi, [socket_fds + 4]
    lea rsi, [receive_message]
    xor edx, edx
    syscall
    cmp rax, 4
    jne .failed
    cmp dword [message_buffer], 0x73756264
    jne .failed

    mov eax, 3
    mov edi, [socket_fds]
    syscall
    mov eax, 3
    mov edi, [socket_fds + 4]
    syscall

    mov r13d, 5
    ; Wayland-style memfd + ftruncate + MAP_SHARED survives remapping.
    mov eax, 319
    lea rdi, [memfd_name]
    xor esi, esi
    syscall
    test rax, rax
    js .failed
    mov r12, rax

    mov eax, 77
    mov rdi, r12
    mov esi, 8192
    syscall
    test rax, rax
    jnz .failed

    mov eax, 5
    mov rdi, r12
    lea rsi, [stat_buffer]
    syscall
    test rax, rax
    jnz .failed
    cmp qword [stat_buffer + 48], 8192
    jne .failed

    mov eax, 9
    xor edi, edi
    mov esi, 8192
    mov edx, 3                       ; PROT_READ | PROT_WRITE
    mov r10d, 1                      ; MAP_SHARED
    mov r8, r12
    xor r9d, r9d
    syscall
    test rax, rax
    js .failed
    mov r14, rax
    mov dword [r14 + 4092], 0x48424f53

    mov eax, 11
    mov rdi, r14
    mov esi, 8192
    syscall
    test rax, rax
    jnz .failed

    mov eax, 9
    xor edi, edi
    mov esi, 8192
    mov edx, 3
    mov r10d, 1
    mov r8, r12
    xor r9d, r9d
    syscall
    test rax, rax
    js .failed
    mov r14, rax
    cmp dword [r14 + 4092], 0x48424f53
    jne .failed

    mov eax, 3
    mov rdi, r12
    syscall
    mov eax, 11
    mov rdi, r14
    mov esi, 8192
    syscall
    test rax, rax
    jnz .failed

    mov r13d, 6
    ; Report success through writev as a final ABI check.
    mov eax, 20
    mov edi, 1
    lea rsi, [result_vectors]
    mov edx, 2
    syscall
    cmp rax, result_length
    jne .failed
    xor edi, edi
    jmp .exit

.failed:
    mov edi, r13d
.exit:
    mov eax, 60
    syscall
    ud2

section .rodata
root_path: db "/", 0
memfd_name: db "wayland-buffer", 0
part_a: db "db"
part_b: db "us"
result_a: db "LINUX_ABI: "
result_a_length equ $ - result_a
result_b: db "PASS", 10
result_b_length equ $ - result_b
result_length equ result_a_length + result_b_length

section .data
align 8
send_vectors:
    dq part_a, 2
    dq part_b, 2
receive_vectors:
    dq receive_buffer, 2
    dq receive_buffer + 2, 2
message_receive_vector:
    dq message_buffer, 8
result_vectors:
    dq result_a, result_a_length
    dq result_b, result_b_length

; Linux x86-64 struct msghdr: name, namelen+pad, iov, iovlen,
; control, controllen, flags+pad.
send_message:
    dq 0
    dq 0
    dq send_vectors
    dq 2
    dq 0
    dq 0
    dq 0
receive_message:
    dq 0
    dq 0
    dq message_receive_vector
    dq 1
    dq 0
    dq 0
    dq 0

section .bss
align 8
stat_buffer: resb 144
directory_buffer: resb 1024
pipe_fds: resd 2
socket_fds: resd 2
receive_buffer: resb 8
message_buffer: resb 8
