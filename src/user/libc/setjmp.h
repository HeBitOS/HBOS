/**
 * @file    setjmp.h
 * @brief   导出 3 个接口（1 类型 / 0 宏 / 2 函数）
 *
 * @layer   user
 * @owner   src/user/libc/
 * @stable  internal
 *
 * 依赖    见文件正文 #include；本文件刻意不引入更高层的头
 * 被依赖  见 docs/agents/ 对应子系统地图
 *
 * 索引头文件
 *   类型  jmp_buf     rbx, rbp, r12-r15, rs…
 *   函数  setjmp      保存跳转点
 *         longjmp     跳回保存点
 */
#ifndef HBOS_USER_LIBC_SETJMP_H
#define HBOS_USER_LIBC_SETJMP_H

/* rbx, rbp, r12-r15, rsp, return address — see setjmp.asm */
typedef long jmp_buf[8];

/* Hand-written in setjmp.asm (x86-64 SysV), not C — see that file for why. */
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
