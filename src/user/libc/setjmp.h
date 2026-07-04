#ifndef HBOS_USER_LIBC_SETJMP_H
#define HBOS_USER_LIBC_SETJMP_H

/* rbx, rbp, r12-r15, rsp, return address — see setjmp.asm */
typedef long jmp_buf[8];

/* Hand-written in setjmp.asm (x86-64 SysV), not C — see that file for why. */
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
