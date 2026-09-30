/* ─────────────────────────────────────────────────────────────────────────
 * @file  lib/string.c
 * @brief HBOS 内核的 freestanding 字符串 / 内存例程（唯一链接进内核的实现，
 *        见 Makefile CORE_SRCS 里的 src/lib/string.c）。
 *
 * 为什么不用编译器内建
 * ────────────────────
 * 内核以 -ffreestanding 编译，编译器不能假设存在 libc，但仍会把 memset/
 * memcpy 的调用改写为对这些符号的调用，因此必须由本文件提供。
 *
 * 性能策略
 * ────────
 * 内核编译带 -mno-sse -mno-sse2 -mno-mmx（中断/异常路径不保存 x87 与向量
 * 寄存器状态，故禁止任何向量指令）。因此本文件采用「先对齐、再按 8 字节
 * 字（long）搬运/比较」的无向量加速：对 memset/memcpy/memmove/memcmp/
 * memchr/strlen 这类全内核最高频例程，字级实现比逐字节实现快约 4~8 倍
 * ——它们被 VFS 目录查找、字体/位图合成、终端网格、ELF 加载等所有路径调用。
 *
 * 两条不变式（修改本文件务必保持）
 * ──────────────────────────────
 *  1. 不构造越界指针：对齐演算先转成 uintptr_t 做整数运算，不写 p + (8 -
 *     (p & 7)) 这类可能越过对象尾部的指针算术；
 *  2. 不跨对象读字：对象头部（到下一个 8 字节边界）与尾部不足 8 字节的
 *     部分一律按字节处理，只有「完整落在对象内」的 8 字节字才整体读写。
 *     8 字节对齐的字永不跨 4 KiB 页边界（4096 % 8 == 0），故对齐后的整字
 *     读取不会触及未映射页——这也是 strlen 能安全整字扫描的原因。
 * ───────────────────────────────────────────────────────────────────────── */

#include <stddef.h>
#include <stdint.h>

#include "../string.h"

/** 短块阈值：低于该长度时对齐演算的开销超过收益，直接逐字节。 */
#define WORD_OP_MIN_BYTES 16u

/** 把单字节模式复制到 8 字节字的每个字节（0xab -> 0xabababab...）。 */
#define BYTE_REP8(b) ((uint64_t)(uint8_t)(b) * 0x0101010101010101ULL)

/**
 * @brief 判断 8 字节字 v 中是否含有值为 0 的字节（经典 SWAR 技巧）。
 *
 * 原理：某字节为 0 时，(v - 1) 在该字节处向高位借位使其符号位置 1，且该
 * 字节原值 0 使 ~v 的符号位也为 1；两者相与即命中。
 *
 * @param v 待检测的 8 字节字
 * @return 非 0 表示至少一个字节等于 0
 */
#define WORD_HAS_ZERO(v)     (((v) - 0x0101010101010101ULL) & ~(v) & 0x8080808080808080ULL)

/* ── 内存操作 ─────────────────────────────────────────────────────────── */

/**
 * @brief 用字节 c 填充 n 字节内存（字级加速版 memset）。
 *
 * 三步：① 逐字节填到 8 字节对齐边界；② 主体按 8 字节字批量写（1 条存储
 * 顶 8 条）；③ 收尾不足 8 字节的尾巴。
 *
 * @param s 目标缓冲区，允许任意对齐
 * @param c 填充字节，仅低 8 位有效（遵循 C 标准 memset 语义）
 * @param n 填充字节数；n == 0 时不做任何写入
 * @return s（C 标准约定）
 */
void *memset(void *s, int c, uint64_t n) {
    uint8_t *p = (uint8_t *)s;
    if (n == 0) return s;
    if (n < WORD_OP_MIN_BYTES) {                       /* 短块：逐字节最快 */
        uint8_t b = (uint8_t)c;
        do { *p++ = b; } while (--n);
        return s;
    }
    const uint64_t word = BYTE_REP8(c);
    uint64_t head = (0u - (uintptr_t)p) & 7u;          /* 到对齐边界的字节数 */
    if (head > n) head = n;
    for (uint64_t i = 0; i < head; i++) p[i] = (uint8_t)c;
    p += head; n -= head;
    uint64_t *w = (uint64_t *)p;                       /* p 现已 8 字节对齐 */
    while (n >= 32) {                                  /* 4 字一批，摊薄循环开销 */
        w[0] = word; w[1] = word; w[2] = word; w[3] = word;
        w += 4; n -= 32;
    }
    while (n >= 8) { *w++ = word; n -= 8; }
    p = (uint8_t *)w;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

/**
 * @brief 拷贝 n 字节（字级加速版 memcpy）。
 *
 * 只有当 src 与 dst 相对 8 字节「同余」时，二者才能同时对齐、整字搬运；
 * 不同余时退回逐字节，反而比反复错位的实现更稳定（编译器通常也会这么做）。
 * 主体一次搬 4 个字（32 字节），摊薄循环与指针自增开销。
 *
 * @param dest 目的缓冲区（与 src 不得重叠，重叠请用 memmove）
 * @param src  源缓冲区
 * @param n    字节数
 * @return dest（C 标准约定）
 */
void *memcpy(void *dest, const void *src, uint64_t n) {
    uint8_t       *d  = (uint8_t *)dest;
    const uint8_t *sp = (const uint8_t *)src;
    if (n == 0) return dest;
    if (n < WORD_OP_MIN_BYTES || (((uintptr_t)d ^ (uintptr_t)sp) & 7u)) {
        do { *d++ = *sp++; } while (--n);              /* 短块 / 不同余 */
        return dest;
    }
    uint64_t head = (0u - (uintptr_t)d) & 7u;
    if (head > n) head = n;
    for (uint64_t i = 0; i < head; i++) d[i] = sp[i];
    d += head; sp += head; n -= head;
    uint64_t       *dw = (uint64_t *)d;
    const uint64_t *sw = (const uint64_t *)sp;
    while (n >= 32) {                                  /* 4 字一批 */
        dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
        dw += 4; sw += 4; n -= 32;
    }
    while (n >= 8) { *dw++ = *sw++; n -= 8; }
    d = (uint8_t *)dw; sp = (const uint8_t *)sw;
    while (n--) *d++ = *sp++;
    return dest;
}

/**
 * @brief 可重叠拷贝（字级转发到 memcpy）。
 *
 * 向后（低地址→高地址）拷贝时源区间在前、目的在后，二者可能交叠但目的写入
 * 永远落后于源读取，可安全交给 memcpy；向前拷贝必须自尾向头逐字节，否则
 * 尚未读取的源字节会被覆盖——这一方向无法整字加速（同一字内前缀会污染
 * 后缀），故保持逐字节。
 *
 * @param dest 目的缓冲区，可与 src 重叠
 * @param src  源缓冲区
 * @param n    字节数
 * @return dest（C 标准约定）
 */
void *memmove(void *dest, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dest;
    if (d < s) return memcpy(dest, src, n);            /* 向后：无覆写风险 */
    d += n; s += n;
    while (n--) *--d = *--s;                           /* 向前：自尾向头 */
    return dest;
}

/**
 * @brief 比较两段内存（字级加速版 memcmp）。
 *
 * 主体一次比 8 字节；整字相等则跳过 8 字节，不相等时再在该字内逐字节定位
 * 第一个差异，保证返回值与逐字节实现完全一致（第一个差异字节的无符号差）。
 *
 * @param s1 第一块内存
 * @param s2 第二块内存
 * @param n  比较字节数；n == 0 视为相等
 * @return 第一个差异字节的无符号差；全等返回 0
 */
int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    while (n && ((uintptr_t)a & 7u)) {                  /* 头部：对齐 a */
        if (*a != *b) return (int)*a - (int)*b;
        a++; b++; n--;
    }
    /* 只有两者相对 8 字节同余，才能同时对齐做整字比较；否则保持逐字节 */
    const uint64_t *aw = (const uint64_t *)a;
    const uint64_t *bw = (const uint64_t *)b;
    if ((((uintptr_t)a ^ (uintptr_t)b) & 7u) != 0) {
        while (n--) {
            if (*a != *b) return (int)*a - (int)*b;
            a++; b++;
        }
        return 0;
    }
    while (n >= 8) {
        uint64_t wa = *aw, wb = *bw;
        if (wa != wb) {                    /* 差异在本字内：定位到具体字节 */
            const uint8_t *pa = (const uint8_t *)&wa;
            const uint8_t *pb = (const uint8_t *)&wb;
            for (int i = 0; i < 8; i++)
                if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
        }
        aw++; bw++; n -= 8;
    }
    a = (const uint8_t *)aw; b = (const uint8_t *)bw;
    while (n--) {
        if (*a != *b) return (int)*a - (int)*b;
        a++; b++;
    }
    return 0;
}

/**
 * @brief 在 n 字节内存中查找字节 c（字级加速版 memchr）。
 *
 * 对齐后用 SWAR 一次检测 8 个字节是否等于 c（先 XOR 广播值把「等于」变成
 * 「零字节」，再用 WORD_HAS_ZERO 判定），命中后在字内逐字节定位下标。
 *
 * @param s 待搜索内存
 * @param c 目标字节（仅低 8 位有效）
 * @param n 搜索长度
 * @return 首次出现的地址；未找到返回 NULL
 */
void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    uint64_t head = (0u - (uintptr_t)p) & 7u;
    if (head > n) head = n;
    for (uint64_t i = 0; i < head; i++)                 /* 头部：对齐前 */
        if (p[i] == (uint8_t)c) return (void *)(p + i);
    p += head; n -= head;
    const uint64_t  pat = BYTE_REP8(c);
    const uint64_t *w   = (const uint64_t *)p;
    while (n >= 8) {
        uint64_t v = *w ^ pat;
        if (WORD_HAS_ZERO(v)) {                         /* 命中：字内定位 */
            const uint8_t *pb = (const uint8_t *)w;
            for (int i = 0; i < 8; i++)
                if (pb[i] == (uint8_t)c) return (void *)(pb + i);
        }
        w++; n -= 8;
    }
    p = (const uint8_t *)w;
    while (n--) {
        if (*p == (uint8_t)c) return (void *)p;
        p++;
    }
    return NULL;
}

/* ── 字符串长度 ───────────────────────────────────────────────────────── */

/**
 * @brief 求 NUL 结尾字符串长度（字级加速版 strlen）。
 *
 * 对齐后每次读 8 字节并用 WORD_HAS_ZERO 检测 NUL，命中后在字内定位。
 * 整字读取安全：对齐的 8 字节字不跨页，且 NUL 之前的字节都属于本字符串。
 *
 * @param s 字符串；本内核约定容忍 NULL（返回 0），保持既有调用点行为不变
 * @return 不含结尾 NUL 的长度
 */
size_t strlen(const char *s) {
    if (!s) return 0;
    const char *p = s;
    uint64_t head = (0u - (uintptr_t)p) & 7u;
    for (uint64_t i = 0; i < head; i++) {               /* 头部：对齐前 */
        if (!p[i]) return (size_t)((uintptr_t)(p + i) - (uintptr_t)s);
    }
    p += head;
    const uint64_t *w = (const uint64_t *)p;
    while (!WORD_HAS_ZERO(*w)) w++;
    const uint8_t *b = (const uint8_t *)w;
    uint64_t extra = 0;
    while (b[extra]) extra++;
    return (size_t)((uintptr_t)w - (uintptr_t)s) + extra;
}

/**
 * @brief 求字符串长度，最多扫 maxlen 字节（字级加速版 strnlen）。
 *
 * 与 strlen 的区别：不得读取 maxlen 之外的字节，因此主体只消费「完整落在
 * maxlen 之内」的 8 字节字，其余退回逐字节——不会为加速而越界读。
 *
 * @param s      字符串；容忍 NULL（返回 0）
 * @param maxlen 扫描上限
 * @return min(实际长度, maxlen)
 */
size_t strnlen(const char *s, size_t maxlen) {
    if (!s) return 0;
    const char *p = s;
    size_t n = maxlen;
    uint64_t head = (0u - (uintptr_t)p) & 7u;
    if (head > n) head = n;
    for (uint64_t i = 0; i < head; i++) {               /* 头部：对齐前 */
        if (!p[i]) return (size_t)((uintptr_t)(p + i) - (uintptr_t)s);
    }
    p += head; n -= head;
    const uint64_t *w = (const uint64_t *)p;
    while (n >= 8) {
        if (WORD_HAS_ZERO(*w)) {
            const uint8_t *b = (const uint8_t *)w;
            uint64_t extra = 0;
            while (b[extra]) extra++;
            return (size_t)((uintptr_t)w - (uintptr_t)s) + extra;
        }
        w++; n -= 8;
    }
    p = (const char *)w;
    while (n--) {
        if (!*p) return (size_t)((uintptr_t)p - (uintptr_t)s);
        p++;
    }
    return maxlen;
}

/* ── 字符串复制 / 拼接 ────────────────────────────────────────────────── */

/**
 * @brief 复制字符串（含结尾 NUL）到 dest。
 * @param dest 目的缓冲区，容量须足够（无边界检查——内核内部用法）
 * @param src  源字符串
 * @return dest（C 标准约定）
 */
char *strcpy(char *dest, const char *src) {
    char *ret = dest;
    while ((*dest++ = *src++));
    return ret;
}

/**
 * @brief 定长复制：不足 n 时用 NUL 填满 dest（C 标准 strncpy 语义）。
 * @param dest 目的缓冲区
 * @param src  源字符串
 * @param n    dest 容量；若 src 长度 ≥ n，结果不以 NUL 结尾
 * @return dest
 */
char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

/**
 * @brief 追加字符串：先定位 dest 尾部再复制 src（含 NUL）。
 * @param dest 目的缓冲区，尾部须有足够空间
 * @param src  待追加的字符串
 * @return dest
 */
char *strcat(char *dest, const char *src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

/**
 * @brief 最多追加 src 的前 n 个字节，并补一个结尾 NUL。
 * @param dest 目的缓冲区，尾部须有足够空间
 * @param src  待追加的字符串
 * @param n    最多追加的字节数
 * @return dest
 */
char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest + strlen(dest);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = '\0';
    return dest;
}

/* ── 字符串比较 ───────────────────────────────────────────────────────── */

/**
 * @brief 按字节字典序比较两个字符串。
 * @param s1 字符串一
 * @param s2 字符串二
 * @return 第一个差异字节的无符号差；相等返回 0
 */
int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

/**
 * @brief 最多比较 n 个字节；遇 NUL 提前结束（VFS/命令解析高频调用）。
 *
 * 与 strncmp 的常见误用点：n 只是上限，遇到任一串的 NUL 即结束，因此
 * strncmp("ab","abc",5) 返回负值而非「前 5 字节不同」。
 *
 * @param s1 字符串一
 * @param s2 字符串二
 * @param n  比较上限
 * @return 第一个差异字节的无符号差；前缀全等返回 0
 */
int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)s1[i];
        unsigned char b = (unsigned char)s2[i];
        if (a != b || a == '\0') return (int)a - (int)b;
    }
    return 0;
}

/* ── 字符串查找 ───────────────────────────────────────────────────────── */

/**
 * @brief 查找字符 c 首次出现的位置（c == 0 时匹配结尾 NUL）。
 * @param s 待搜索字符串
 * @param c 目标字符
 * @return 命中位置的指针；未找到返回 NULL
 */
char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return c == 0 ? (char *)s : NULL;
}

/**
 * @brief 查找字符 c 最后一次出现的位置（c == 0 时匹配结尾 NUL）。
 * @param s 待搜索字符串
 * @param c 目标字符
 * @return 最后一次命中位置的指针；未找到返回 NULL
 */
char *strrchr(const char *s, int c) {
    const char *last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

/**
 * @brief 子串查找（朴素 O(n*m) 扫描）。
 *
 * 内核内 needle 通常极短（扩展名、路径段），朴素实现的最坏情况可接受；
 * needle 首字节预筛见下方 while 短路——多数位置在第二次比较就失败。
 *
 * @param haystack 被搜索串
 * @param needle   目标子串；空串按 C 约定返回 haystack
 * @return 首次出现位置；未找到返回 NULL
 */
char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}
