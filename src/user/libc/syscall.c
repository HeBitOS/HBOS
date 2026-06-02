#include "syscall.h"

long __syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long r10 __asm__("r10") = a3;
    register long r8  __asm__("r8")  = a4;
    register long r9  __asm__("r9")  = a5;

    __asm__ volatile(
        "int $0x80"
        : "+a"(nr)
        : "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");

    return nr;
}

long __syscall3(long nr, long a0, long a1, long a2) {
    return __syscall6(nr, a0, a1, a2, 0, 0, 0);
}

long __syscall1(long nr, long a0) {
    return __syscall3(nr, a0, 0, 0);
}