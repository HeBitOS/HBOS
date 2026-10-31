/* termios/pty/job-control 验收（freestanding，裸 syscall，-nostdlib -static-pie） */
typedef long L;
static L sc(L n, L a, L b, L c) {
  L r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
static void P(const char *s) { int n = 0; while (s[n]) n++; sc(1, 1, (L)s, n); }
static void T(int c, const char *m) { P(c ? "PASS " : "FAIL "); P(m); P("\n"); }
static int eq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
void _start(void) {
  char b[32] = {0}; int u = 0, n = -1; unsigned short w[4] = {30, 100, 0, 0};
  int m = sc(2, (L)"/dev/ptmx", 2, 0); T(m >= 0, "open ptmx");
  T(sc(16, m, 0x40045431, (L)&u) == 0, "unlock");
  T(sc(16, m, 0x80045430, (L)&n) == 0 && n >= 0, "ptn");
  char p[] = "/dev/pts/0"; p[9] = '0' + n;
  int s = sc(2, (L)p, 2, 0); T(s >= 0, "open slave");
  unsigned t[9] = {0}; T(sc(16, s, 0x5401, (L)t) == 0 && (t[3] & 2), "tcgetattr canon");
  sc(1, m, (L)"hi\n", 3); L r = sc(0, s, (L)b, 16); T(r == 3 && eq(b, "hi\n"), "canon read");
  r = sc(0, m, (L)b, 16); T(r >= 3, "echo to master");
  sc(1, s, (L)"x\n", 2); for (int i = 0; i < 32; i++) b[i] = 0;
  r = sc(0, m, (L)b, 16); T(r == 3 && b[1] == '\r', "onlcr");
  sc(16, s, 0x5414, (L)w); w[1] = 0; T(sc(16, s, 0x5413, (L)w) == 0 && w[1] == 100, "winsz");
  L c = sc(57, 0, 0, 0);
  if (c == 0) {
    T(sc(112, 0, 0, 0) > 0, "setsid");
    int q = sc(2, (L)p, 2, 0); int g = 0; L me = sc(39, 0, 0, 0);
    T(sc(16, q, 0x540E, 0) == 0, "tiocsctty");
    T(sc(16, q, 0x540F, (L)&g) == 0 && g == me, "tcgetpgrp");
    for (;;) sc(24, 0, 0, 0);
  }
  for (int i = 0; i < 20; i++) sc(24, 0, 0, 0);
  T(sc(124, c, 0, 0) == c, "child sid");
  sc(62, c, 19, 0); int st = 0;
  T(sc(61, c, (L)&st, 2) == c && (st & 0xff) == 0x7f, "wuntraced");
  sc(62, c, 18, 0); sc(62, c, 9, 0); sc(61, c, (L)&st, 0); T(1, "done");
  sc(60, 0, 0, 0);
}
