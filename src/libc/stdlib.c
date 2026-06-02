/**
 * @file    stdlib.c
 * @brief   HBOS 标准库实现 — malloc/free/exit/atoi 等
 *
 * malloc 基于 sys_sbrk，从内核堆分配。
 * 使用简单的首次适配链表分配器。
 */

#include "stdlib.h"
#include "string.h"
#include "../user/syscall.h"

typedef struct block {
    size_t size;
    int    free;
    struct block *next;
    struct block *prev;
} block_t;

static block_t *heap_head;
static int heap_inited;

static void heap_init(void) {
    heap_head = (block_t *)sbrk(4096);
    if (!heap_head || heap_head == (block_t *)-1) return;
    heap_head->size = 4096 - sizeof(block_t);
    heap_head->free = 1;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_inited = 1;
}

static block_t *find_block(size_t size) {
    block_t *b = heap_head;
    while (b) {
        if (b->free && b->size >= size) return b;
        b = b->next;
    }
    return NULL;
}

static block_t *extend_heap(size_t size) {
    size_t need = size + sizeof(block_t);
    need = (need + 4095) & ~4095;
    block_t *b = (block_t *)sbrk((int)need);
    if (!b || b == (block_t *)-1) return NULL;

    block_t *last = heap_head;
    while (last && last->next) last = last->next;

    b->size = need - sizeof(block_t);
    b->free = 0;
    b->next = NULL;
    b->prev = last;
    if (last) last->next = b;
    else heap_head = b;
    return b;
}

static void split_block(block_t *b, size_t size) {
    if (b->size >= size + sizeof(block_t) + 16) {
        block_t *newb = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
        newb->size = b->size - size - sizeof(block_t);
        newb->free = 1;
        newb->next = b->next;
        newb->prev = b;
        b->size = size;
        b->next = newb;
        if (newb->next) newb->next->prev = newb;
    }
}

static void merge_blocks(block_t *b) {
    if (b->next && b->next->free) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    if (!heap_inited) heap_init();
    if (!heap_head) return NULL;
    size = (size + 15) & ~15;
    block_t *b = find_block(size);
    if (!b) b = extend_heap(size);
    if (!b) return NULL;
    b->free = 0;
    split_block(b, size);
    return (void *)((uint8_t *)b + sizeof(block_t));
}

void free(void *ptr) {
    if (!ptr || !heap_head) return;
    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if (b < heap_head) return;
    b->free = 1;
    merge_blocks(b);
    if (b->prev && b->prev->free) merge_blocks(b->prev);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb && total / nmemb != size) return NULL;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if (b < heap_head) return NULL;
    if (b->size >= size) return ptr;
    void *newp = malloc(size);
    if (!newp) return NULL;
    memcpy(newp, ptr, b->size);
    free(ptr);
    return newp;
}

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

double atof(const char *s) { return 0.0; (void)s; }

long strtol(const char *s, char **endptr, int base) {
    return strtoll(s, endptr, base);
}

long long strtoll(const char *s, char **endptr, int base) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        base = 10;
        if (*s == '0') {
            base = 8; s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
        }
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    long long val = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    return (unsigned long)strtoll(s, endptr, base);
}

void abort(void) { exit(1); }

void exit(int status) { _exit(status); while (1); }

int atexit(void (*func)(void)) { (void)func; return 0; }

int system(const char *cmd) {
    (void)cmd;
    return -1;
}

static char *env_tmp;
char *getenv(const char *name) { (void)name; return NULL; }
int setenv(const char *name, const char *value, int overwrite) {
    (void)name; (void)value; (void)overwrite; return 0;
}

static unsigned int _rand_seed = 1;
int rand(void) { _rand_seed = _rand_seed * 1103515245 + 12345; return (int)(_rand_seed >> 16) & RAND_MAX; }
void srand(unsigned int seed) { _rand_seed = seed; }

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *)) {
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, (const uint8_t *)base + mid * size);
        if (cmp < 0) hi = mid;
        else if (cmp > 0) lo = mid + 1;
        else return (void *)((const uint8_t *)base + mid * size);
    }
    return NULL;
}

static void _qsort_swap(char *a, char *b, size_t size) {
    for (size_t i = 0; i < size; i++) { char t = a[i]; a[i] = b[i]; b[i] = t; }
}

static void _qsort_rec(char *base, int lo, int hi, size_t size,
                       int (*compar)(const void *, const void *)) {
    if (lo >= hi) return;
    int p = lo + (hi - lo) / 2;
    _qsort_swap(base + p * size, base + hi * size, size);
    int i = lo;
    for (int j = lo; j < hi; j++) {
        if (compar(base + j * size, base + hi * size) < 0) {
            _qsort_swap(base + i * size, base + j * size, size);
            i++;
        }
    }
    _qsort_swap(base + i * size, base + hi * size, size);
    _qsort_rec(base, lo, i - 1, size, compar);
    _qsort_rec(base, i + 1, hi, size, compar);
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb > 1) _qsort_rec((char *)base, 0, (int)(nmemb - 1), size, compar);
}

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }