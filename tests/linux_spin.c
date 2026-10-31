/* 作业控制手测：循环读 stdin，便于 ^C/^Z（-nostdlib -static-pie） */
static long sc(long n,long a,long b,long c){long r;__asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");return r;}
void _start(void){char b[64];for(;;){sc(24,0,0,0);}(void)b;}
