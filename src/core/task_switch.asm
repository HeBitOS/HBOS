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
global task_resume_linux_syscall
; 排错用：非 0 时给 clone 子进程的首次 iret 打上单步标志（TF），
; 让 CPU 在执行完第一条用户指令后抛 #DB。见下方 task_resume_linux_syscall。
global task_debug_single_step
; 排错用：非 0 时在 task_resume_linux_syscall 里打印子进程的恢复现场。
global task_debug_trace_resume
extern task_exit
extern smp_sched_unlock
extern task_debug_report_resume

section .text
bits 64

section .data
align 4
task_debug_single_step: dd 0
task_debug_trace_resume: dd 0
section .text

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

; Resume the child side of a native Linux clone3 syscall.  The context is a
; hbos_linux_clone_context_t; all registers match the parent at syscall entry
; except Linux's specified child return value RAX=0 and the requested RSP/TLS.
task_resume_linux_syscall:
    mov rax, rdi
    ; 诊断：只在 task_debug_trace_resume 打开时才打（shell 里
    ; `setenv resume 1`）。默认静默——串口在 QEMU 里是逐字符模拟的，
    ; fork 热路径上每多打一行都可能把 zygote 握手拖过超时。
    cmp dword [rel task_debug_trace_resume], 0
    je .no_resume_trace
    push rax
    push rdi
    mov rdi, [rax]          ; arg1 = 上下文里的目标 rip
    mov rsi, [rax+8]        ; arg2 = 目标 rsp
    call task_debug_report_resume
    pop rdi
    pop rax
.no_resume_trace:

    ; iretq 帧：SS, RSP, RFLAGS, CS, RIP（栈顶是 RIP）。
    ; RFLAGS 先算进 r11，按 task_debug_single_step 决定是否打 TF。
    mov r11, [rax+16]
    cmp dword [rel task_debug_single_step], 0
    je .step_done
    or r11, 0x100               ; TF：执行完第一条用户指令后抛 #DB
.step_done:
    push 0x23                   ; SS
    push qword [rax+8]          ; RSP
    push r11                    ; RFLAGS（可能带 TF）
    push 0x1B                   ; CS
    push qword [rax]            ; RIP

    ; Context layout (all 8-byte slots):
    ;   0 rip   8 rsp   16 rflags   24 rdi   32 rsi   40 rdx   48 r10
    ;  56 r8   64 r9    72 rbx     80 rbp   88 r12   96 r13  104 r14  112 r15
    mov rbx, [rax+72]
    mov rbp, [rax+80]
    mov r12, [rax+88]
    mov r13, [rax+96]
    mov r14, [rax+104]
    mov r15, [rax+112]
    mov r10, [rax+48]
    mov r8,  [rax+56]
    mov r9,  [rax+64]
    mov rdx, [rax+40]
    mov rsi, [rax+32]
    mov rcx, [rax]
    mov r11, [rax+16]
    mov rdi, [rax+24]
    xor eax, eax
    iretq
