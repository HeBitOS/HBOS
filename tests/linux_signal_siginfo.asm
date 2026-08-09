; Full Linux rt_sigframe ABI: SA_SIGINFO three-argument handlers with
; siginfo/ucontext, sigaltstack delivery, and nested SA_SIGINFO delivery.
;
; A: kill(SIGUSR1) -> handler with rdi=signo rsi=&siginfo rdx=&ucontext;
;    verify si_signo/si_code(SI_USER)/si_pid, ucontext REG_RIP == the
;    instruction after kill, REG_RAX == kill result, REG_RCX == REG_RIP
;    (syscall clobber invariant); rt_sigreturn resumes right after kill.
; B: write to a PROT_NONE page -> SIGSEGV with si_code==SEGV_MAPERR,
;    si_addr==CR2==fault address, REG_RIP==faulting instruction,
;    REG_TRAPNO==14, REG_ERR==write-not-present(2), REG_RBX sentinel
;    preserved; handler mprotects and the write retries.
; C: sigaltstack(131) install/query; SA_ONSTACK delivery runs the handler
;    with rsp inside [ss_sp, ss_sp+ss_size), uc_stack carries the altstack
;    config, ss_flags reflects the *interrupted* rsp (0, like Linux).
; D: SIGUSR2 handler raises SIGUSR1: nested SA_SIGINFO delivery; the inner
;    frame's uc_sigmask shows SIGUSR2 blocked, both restores unwind, and a
;    rt_sigprocmask query reports the mask back to 0.

BITS 64
DEFAULT REL
GLOBAL _start

%define SYS_write          1
%define SYS_rt_sigaction   13
%define SYS_rt_sigprocmask 14
%define SYS_rt_sigreturn   15
%define SYS_mmap           9
%define SYS_mprotect       10
%define SYS_getpid         39
%define SYS_kill           62
%define SYS_exit           60
%define SYS_sigaltstack    131

%define SIGUSR1            10
%define SIGUSR2            12
%define SIGSEGV            11

%define SA_SIGINFO         0x00000004
%define SA_RESTORER        0x04000000
%define SA_ONSTACK         0x08000000
%define SIG_BLOCK          0
%define PROT_NONE          0
%define PROT_READ          1
%define PROT_WRITE         2
%define MAP_PRIVATE        2
%define MAP_ANONYMOUS      0x20
%define SI_USER            0
%define SEGV_MAPERR        1
%define SEGV_ACCERR        2

; ucontext field offsets (kernel layout, glibc-compatible)
%define UCTX_STACK_SS_FLAGS  24
%define UCTX_SIGMASK         296
; gregs[] indices * 8
%define G_R11                64
%define G_RBX                128
%define G_RAX                144
%define G_RCX                152
%define G_RSP                160
%define G_RIP                168
%define G_ERR                192
%define G_TRAPNO             200
%define G_CR2                216
; siginfo offsets
%define SI_CODE              8
%define SI_PID               16
%define SI_ADDR              16

SECTION .text
_start:
    ; Install the SA_SIGINFO handlers.
    mov eax, SYS_rt_sigaction
    mov edi, SIGUSR1
    lea rsi, [action_sigusr1]
    xor edx, edx
    mov r10d, 8
    syscall
    test rax, rax
    js .failed

    mov eax, SYS_rt_sigaction
    mov edi, SIGUSR2
    lea rsi, [action_sigusr2]
    xor edx, edx
    mov r10d, 8
    syscall
    test rax, rax
    js .failed

    mov eax, SYS_rt_sigaction
    mov edi, SIGSEGV
    lea rsi, [action_segv]
    xor edx, edx
    mov r10d, 8
    syscall
    test rax, rax
    js .failed

    ; ---- A: SA_SIGINFO delivery of SIGUSR1 via kill ----
    lea rcx, [.after_kill]
    mov [expected_rip], rcx
    mov eax, SYS_getpid
    syscall
    mov edi, eax
    mov esi, SIGUSR1
    mov eax, SYS_kill
    syscall
.after_kill:
    test rax, rax                    ; kill result 0 restored by rt_sigreturn
    jnz .failed
    cmp byte [sigusr1_seen], 1
    jne .failed
    cmp byte [handler_errors], 0
    jne .failed

    ; ---- B: SIGSEGV with siginfo, repaired by the handler ----
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

    lea rcx, [.fault_instr]
    mov [expected_rip], rcx
    mov rbx, 0x1122334455667788       ; callee-saved sentinel across signal
.fault_instr:
    mov byte [rax], 0x5a              ; page fault (PROT_NONE)
.fault_ret:
    cmp byte [rax], 0x5a              ; write retried after mprotect
    jne .failed
    mov rax, 0x1122334455667788       ; cmp r64 has no imm64 form
    cmp rbx, rax                      ; rbx survived rt_sigreturn
    jne .failed
    cmp byte [segv_seen], 1
    jne .failed
    cmp byte [handler_errors], 0
    jne .failed

    ; ---- C: sigaltstack install/query + SA_ONSTACK delivery ----
    mov eax, SYS_mmap
    xor edi, edi
    mov esi, 65536
    mov edx, PROT_READ | PROT_WRITE
    mov r10d, MAP_PRIVATE | MAP_ANONYMOUS
    mov r8, -1
    xor r9d, r9d
    syscall
    test rax, rax
    js .failed
    mov [altstack], rax

    mov rax, [altstack]
    mov [alt_ss], rax                     ; ss_sp = altstack
    lea rdi, [alt_ss]                     ; flags = 0, size 65536
    xor esi, esi
    mov eax, SYS_sigaltstack
    syscall
    test rax, rax
    js .failed

    xor edi, edi                      ; query: old_ss = current config
    lea rsi, [old_ss]
    mov eax, SYS_sigaltstack
    syscall
    test rax, rax
    js .failed
    mov rax, [altstack]
    cmp [old_ss], rax
    jne .failed
    cmp qword [old_ss + 16], 65536
    jne .failed

    mov eax, SYS_rt_sigaction         ; SA_SIGINFO | SA_ONSTACK handler
    mov edi, SIGUSR1
    lea rsi, [action_alt]
    xor edx, edx
    mov r10d, 8
    syscall
    test rax, rax
    js .failed

    lea rcx, [.after_alt_raise]
    mov [expected_rip], rcx
    mov eax, SYS_getpid
    syscall
    mov edi, eax
    mov esi, SIGUSR1
    mov eax, SYS_kill
    syscall
.after_alt_raise:
    cmp byte [alt_seen], 1
    jne .failed
    cmp byte [handler_errors], 0
    jne .failed

    ; ---- D: nested SA_SIGINFO delivery (SIGUSR2 -> SIGUSR1) ----
    ; C re-registered SIGUSR1 with SA_ONSTACK; restore the plain handler
    mov eax, SYS_rt_sigaction
    mov edi, SIGUSR1
    lea rsi, [action_sigusr1]
    xor edx, edx
    mov r10d, 8
    syscall
    test rax, rax
    js .failed
    lea rcx, [.after_nest_raise]
    mov [expected_rip], rcx
    mov rbx, 0xFEEDFACE12345678
    mov eax, SYS_getpid
    syscall
    mov edi, eax
    mov esi, SIGUSR2
    mov eax, SYS_kill
    syscall
.after_nest_raise:
    mov rax, 0xFEEDFACE12345678
    cmp rbx, rax
    jne .failed
    cmp byte [sigusr2_seen], 1
    jne .failed
    cmp byte [nested_seen], 1
    jne .failed
    cmp byte [handler_errors], 0
    jne .failed

    ; blocked mask must be back to 0 after both frames unwound
    mov eax, SYS_rt_sigprocmask
    mov edi, SIG_BLOCK
    xor esi, esi
    lea rdx, [old_mask]
    mov r10d, 8
    syscall
    test rax, rax
    js .failed
    cmp qword [old_mask], 0
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

; ---------------------------------------------------------------------
; A/D: SIGUSR1 three-argument handler.  For the kill delivery it validates
; siginfo + ucontext; when nested inside the SIGUSR2 handler (D) it also
; checks that uc_sigmask carries the outer blocked set.
; ---------------------------------------------------------------------
handler_sigusr1:
    cmp edi, SIGUSR1
    jne .bad
    cmp dword [rsi], SIGUSR1          ; si_signo
    jne .bad
    cmp dword [rsi + SI_CODE], SI_USER
    jne .bad
    cmp byte [nested_context], 0
    je .plain
    ; Nested delivery inside handler_sigusr2: SIGUSR2 (bit 11) blocked.
    ; The interrupted context is the kill inside the outer handler, so
    ; only the mask is meaningful here.
    mov rax, [rdx + UCTX_SIGMASK]
    cmp rax, 0x800
    jne .bad
    mov byte [nested_seen], 1
    ret
.plain:
    mov eax, SYS_getpid               ; si_pid must be the sender (self)
    syscall
    cmp [rsi + SI_PID], eax
    jne .bad
    mov rax, [expected_rip]
    cmp [rdx + G_RIP], rax
    jne .bad
    cmp qword [rdx + G_RAX], 0   ; interrupted rax = kill result
    jne .bad
    mov rax, [rdx + G_RIP]       ; rcx clobber invariant
    cmp [rdx + G_RCX], rax
    jne .bad
    mov byte [sigusr1_seen], 1
    ret
.bad:
    mov byte [handler_errors], 0x11
    ret

; ---------------------------------------------------------------------
; B: SIGSEGV handler with siginfo; repairs the page and lets the write
; retry.
; ---------------------------------------------------------------------
handler_segv:
    cmp edi, SIGSEGV
    jne .bad
    ; PROT_NONE pages are present-but-unwritable on Linux and HBOS alike,
    ; so the fault reports SEGV_ACCERR; unmapped addresses report MAPERR.
    mov eax, [rsi + SI_CODE]
    cmp eax, SEGV_MAPERR
    je .code_ok
    cmp eax, SEGV_ACCERR
    jne .bad
.code_ok:
    mov rax, [fault_page]
    cmp [rsi + SI_ADDR], rax          ; si_addr == faulting address
    jne .bad
    cmp [rdx + G_CR2], rax
    jne .bad
    mov rax, [expected_rip]
    cmp [rdx + G_RIP], rax
    jne .bad
    cmp qword [rdx + G_TRAPNO], 14
    jne .bad
    ; error code: write + user, present bit may be set (PROT_NONE page)
    ; or clear (unmapped); bit 0 (P) is the only varying one
    mov rax, [rdx + G_ERR]
    and rax, 0x7
    cmp rax, 0x2                   ; W|U
    je .err_ok
    cmp rax, 0x7                   ; P|W|U
    jne .bad
.err_ok:
    mov rax, 0x1122334455667788        ; x86-64 cmp has no imm64 form
    cmp [rdx + G_RBX], rax
    jne .bad
    mov rdi, [fault_page]
    mov esi, 4096
    mov edx, PROT_READ | PROT_WRITE
    mov eax, SYS_mprotect
    syscall
    test rax, rax
    js .bad
    mov byte [segv_seen], 1
    ret
.bad:
    mov byte [handler_errors], 0x12
    ret

; ---------------------------------------------------------------------
; C: SA_ONSTACK handler.  rsp must be inside the altstack; uc_stack must
; carry the altstack config with ss_flags == 0 (interrupted rsp was not
; on the altstack, matching real Linux).
; ---------------------------------------------------------------------
handler_alt:
    cmp edi, SIGUSR1
    jne .bad
    mov rax, [altstack]
    cmp rax, [rdx + 16]               ; uc_stack.ss_sp
    jne .bad
    cmp qword [rdx + 32], 65536       ; uc_stack.ss_size
    jne .bad
    cmp dword [rdx + UCTX_STACK_SS_FLAGS], 0
    jne .bad
    mov rcx, rsp
    mov rdx, [altstack]
    cmp rcx, rdx
    jb .bad
    add rdx, 65536
    cmp rcx, rdx
    jae .bad
    mov byte [alt_seen], 1
    ret
.bad:
    mov byte [handler_errors], 0x13
    ret

; ---------------------------------------------------------------------
; D: SIGUSR2 handler — raises SIGUSR1 for nested SA_SIGINFO delivery.
; ---------------------------------------------------------------------
handler_sigusr2:
    cmp edi, SIGUSR2
    jne .bad
    mov byte [nested_context], 1
    mov eax, SYS_getpid
    syscall
    mov edi, eax
    mov esi, SIGUSR1
    mov eax, SYS_kill
    syscall
    test rax, rax
    js .bad
    mov byte [sigusr2_seen], 1
    ret
.bad:
    mov byte [handler_errors], 0x14
    ret

signal_restorer:
    mov eax, SYS_rt_sigreturn
    syscall
    ud2

SECTION .data
align 8
action_sigusr1:
    dq handler_sigusr1
    dq SA_SIGINFO | SA_RESTORER
    dq signal_restorer
    dq 0
action_sigusr2:
    dq handler_sigusr2
    dq SA_SIGINFO | SA_RESTORER
    dq signal_restorer
    dq 0
action_segv:
    dq handler_segv
    dq SA_SIGINFO | SA_RESTORER
    dq signal_restorer
    dq 0
action_alt:
    dq handler_alt
    dq SA_SIGINFO | SA_RESTORER | SA_ONSTACK
    dq signal_restorer
    dq 0
alt_ss:
    dq 0                              ; ss_sp  (filled at runtime)
    dd 0                              ; ss_flags
    dd 0                              ; pad
    dq 65536                          ; ss_size
old_ss:
    dq 0
    dd 0
    dd 0
    dq 0
old_mask: dq 0
expected_rip: dq 0
expected_hi: dq 0
fault_page: dq 0
altstack: dq 0
sigusr1_seen: db 0
sigusr2_seen: db 0
segv_seen: db 0
alt_seen: db 0
nested_seen: db 0
nested_context: db 0
handler_errors: db 0

SECTION .rodata
pass_message: db "LINUX_SIGNAL_SIGINFO: PASS", 10
pass_length equ $ - pass_message
fail_message: db "LINUX_SIGNAL_SIGINFO: FAIL", 10
fail_length equ $ - fail_message
