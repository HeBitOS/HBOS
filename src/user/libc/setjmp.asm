; ============================================================
; setjmp/longjmp — x86-64 SysV, hand-written (no libc/libgcc
; support for these on HBOS). Mirrors the raw register save/
; restore style of src/core/task_switch.asm; written directly in
; asm rather than C + inline-asm so the compiler's own prologue
; can't interfere with the raw rsp/return-address manipulation.
;
; jmp_buf layout (8 qwords, see setjmp.h):
;   [0] rbx  [8] rbp  [16] r12 [24] r13
;   [32] r14 [40] r15 [48] rsp (caller's, post-call)
;   [56] return address
; ============================================================

section .text
bits 64

global setjmp
setjmp:
    ; rdi = jmp_buf*
    mov [rdi+0],  rbx
    mov [rdi+8],  rbp
    mov [rdi+16], r12
    mov [rdi+24], r13
    mov [rdi+32], r14
    mov [rdi+40], r15
    lea rax, [rsp+8]        ; caller's rsp (rsp here still holds our own return addr)
    mov [rdi+48], rax
    mov rax, [rsp]          ; return address pushed by `call setjmp`
    mov [rdi+56], rax
    xor eax, eax
    ret

global longjmp
longjmp:
    ; rdi = jmp_buf*, esi = val
    mov eax, esi
    test eax, eax
    jnz .nonzero
    mov eax, 1
.nonzero:
    mov rbx, [rdi+0]
    mov rbp, [rdi+8]
    mov r12, [rdi+16]
    mov r13, [rdi+24]
    mov r14, [rdi+32]
    mov r15, [rdi+40]
    mov rsp, [rdi+48]
    mov rcx, [rdi+56]
    jmp rcx
