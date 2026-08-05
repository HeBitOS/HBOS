; ============================================================
; HBOS Context Switch — assembly primitive
; ============================================================
; void task_switch(uint64_t *prev_rsp, uint64_t *next_rsp,
;                   void *prev_fpu, void *next_fpu);
;   rdi = address of current task's rsp field (save here)
;   rsi = address of next task's rsp field (restore from here)
;   rdx = current task's 512-byte FXSAVE area (16-byte aligned)
;   rcx = next task's 512-byte FXSAVE area (16-byte aligned)
;
; Saves all callee-saved registers (RBP, RBX, R12-R15) and RSP,
; plus x87/MMX/SSE state (FXSAVE/FXRSTOR), then loads the next
; task's RSP/FPU state and restores its registers.
; Returns to wherever the next task was when it yielded.
; ============================================================

global task_switch
global task_entry_trampoline
global task_enter_ring3
extern task_exit
extern smp_sched_unlock

section .text
bits 64

task_switch:
    ; rdx/rcx (prev_fpu/next_fpu) don't survive the stack juggling below
    ; unscathed in spirit, but they're never touched by push/pop/mov here,
    ; so it's safe to use them directly before and after the RSP swap.
    fxsave [rdx]         ; Save current task's x87/MMX/SSE state

    ; Save callee-saved registers + RFLAGS onto current task's stack
    pushfq              ; Save RFLAGS (IF state)
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save current RSP into *prev_rsp (rdi points to &prev->rsp)
    mov [rdi], rsp

    ; Load next task's RSP from *next_rsp (rsi points to &next->rsp)
    mov rsp, [rsi]

    ; Restore callee-saved registers + RFLAGS of next task
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq               ; Restore RFLAGS (restores IF)

    fxrstor [rcx]        ; Restore next task's x87/MMX/SSE state

    ; Return to where next task yielded (return address on its stack)
    ret

; ============================================================
; Trampoline for newly created tasks.
; Stack layout (from task_create, top to bottom):
;   [RSP+0]  ->  arg (void*)          <-- popped into rdi
;   [RSP+8]  ->  entry (void(*)())    <-- popped into rax
;   [RSP+16] ->  return address (here)<-- ret jumps here
;
; After popping, calls entry(arg). If entry returns, calls task_exit.
; ============================================================
task_entry_trampoline:
    ; First-run tasks enter here while the scheduler handoff lock is held.
    ; Release it before enabling IRQs or calling task code.
    call smp_sched_unlock
    sti
    pop rdi             ; rdi = arg
    pop rax             ; rax = entry function
    call rax            ; call entry(arg)
    call task_exit      ; if entry returns, terminate task
    ; Never reaches here

; ============================================================
; Ring3 entry trampoline — switches to user mode via iretq
; Called from task_entry_trampoline as the "entry function"
; arg (rdi) = pointer to ring3_launch_ctx:
;   ctx[0] = user_entry (RIP)
;   ctx[1] = user_stack (RSP)
; ============================================================
task_enter_ring3:
    mov rax, rdi        ; rax = ctx pointer

    ; Build iretq frame for ring3 on kernel stack
    push 0x23           ; SS = user data selector | RPL=3
    push qword [rax+8]  ; RSP = user stack pointer
    pushfq              ; RFLAGS
    pop rdx
    or rdx, 0x3202      ; IF=1, IOPL=3, always-set bit
    push rdx
    push 0x1B           ; CS = user code selector | RPL=3
    push qword [rax]    ; RIP = user entry point

    ; Set segment registers for user mode
    mov bx, 0x23
    mov ds, bx
    mov es, bx
    ; Keep FS/GS selectors untouched.  Long-mode TLS uses FS.base from
    ; IA32_FS_BASE, which task_switch_tls restored for this task.

    ; _start(argc, argv)
    mov rdi, [rax+16]
    mov rsi, [rax+24]

    iretq                ; Enter ring3!
