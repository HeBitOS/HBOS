bits 64
default rel

section .text
global hbos_test_clone3

; long hbos_test_clone3(void *args, size_t size,
;                       int (*child)(void *), void *argument)
; The child resumes after SYSCALL on its new stack.  Keep its entry function
; and argument in callee-saved registers so the kernel's clone context must
; restore those registers correctly for this test to pass.
hbos_test_clone3:
    push r12
    push r13
    mov r12, rdx
    mov r13, rcx
    mov eax, 435                 ; __NR_clone3 on x86-64
    syscall
    test rax, rax
    jz .child
    pop r13
    pop r12
    ret

.child:
    mov rdi, r13
    call r12
    mov edi, eax
    mov eax, 60                  ; __NR_exit
    syscall
    ud2
