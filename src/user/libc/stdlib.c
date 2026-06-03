#include "stdlib.h"
#include "string.h"
#include "syscall.h"

typedef struct block {
    size_t size;
    int    free;
    struct block *next;
    struct block *prev;
} block_t;

static block_t *heap_head;
static int heap_inited;

static void heap_init(void) {
    long p = __syscall1(HBOS_SYS_SBRK, 4096);
    if (p < 0) return;
    heap_head = (block_t *)p;
    heap_head->size = 4096 - sizeof(block_t);
    heap_head->free = 1;
    heap_head->next = 0;
    heap_head->prev = 0;
    heap_inited = 1;
}

static block_t *find_block(size_t size) {
    block_t *b = heap_head;
    while (b) {
        if (b->free && b->size >= size) return b;
        b = b->next;
    }
    return 0;
}

static block_t *extend_heap(size_t size) {
    size_t need = size + sizeof(block_t);
    need = (need + 4095) & ~4095;
    long p = __syscall1(HBOS_SYS_SBRK, (long)need);
    if (p < 0) return 0;
    block_t *b = (block_t *)p;

    block_t *last = heap_head;
    while (last && last->next) last = last->next;

    b->size = need - sizeof(block_t);
    b->free = 0;
    b->next = 0;
    b->prev = last;
    if (last) last->next = b;
    else heap_head = b;
    return b;
}

static void split_block(block_t *b, size_t size) {
    if (b->size >= size + sizeof(block_t) + 16) {
        block_t *newb = (block_t *)((unsigned char *)b + sizeof(block_t) + size);
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
    if (size == 0) return 0;
    if (!heap_inited) heap_init();
    if (!heap_head) return 0;
    size = (size + 15) & ~15;
    block_t *b = find_block(size);
    if (!b) b = extend_heap(size);
    if (!b) return 0;
    b->free = 0;
    split_block(b, size);
    return (void *)((unsigned char *)b + sizeof(block_t));
}

void free(void *ptr) {
    if (!ptr || !heap_head) return;
    block_t *b = (block_t *)((unsigned char *)ptr - sizeof(block_t));
    if (b < heap_head) return;
    b->free = 1;
    merge_blocks(b);
    if (b->prev && b->prev->free) merge_blocks(b->prev);
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb && total / nmemb != size) return 0;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }
    block_t *b = (block_t *)((unsigned char *)ptr - sizeof(block_t));
    if (b < heap_head) return 0;
    if (b->size >= size) return ptr;
    void *newp = malloc(size);
    if (!newp) return 0;
    memcpy(newp, ptr, b->size);
    free(ptr);
    return newp;
}

int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

long strtol(const char *s, char **endptr, int base) {
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
    long val = 0;
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
    return (unsigned long)strtol(s, endptr, base);
}

void abort(void) { exit(1); }

void exit(int status) {
    __syscall1(HBOS_SYS_EXIT, status);
    while (1);
}

static unsigned int _rand_seed = 1;
int rand(void) { _rand_seed = _rand_seed * 1103515245 + 12345; return (int)(_rand_seed >> 16) & RAND_MAX; }
void srand(unsigned int seed) { _rand_seed = seed; }

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

/* ========== Environment Variables ========== */

char **environ;

static char *env_find(const char *name) {
    if (!environ || !name) return 0;
    size_t namelen = strlen(name);
    for (char **ep = environ; *ep; ep++) {
        if (strncmp(*ep, name, namelen) == 0 && (*ep)[namelen] == '=') {
            return *ep + namelen + 1;
        }
    }
    return 0;
}

static int env_count(void) {
    if (!environ) return 0;
    int n = 0;
    while (environ[n]) n++;
    return n;
}

char *getenv(const char *name) {
    return env_find(name);
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || !*name || strchr(name, '=')) return -1;

    char *existing = env_find(name);
    if (existing) {
        if (!overwrite) return 0;
        /* replace existing entry */
        size_t namelen = strlen(name);
        char *entry = (char *)(existing - namelen - 1);
        size_t newlen = namelen + 1 + strlen(value) + 1;
        char *newentry = (char *)malloc(newlen);
        if (!newentry) return -1;
        memcpy(newentry, name, namelen);
        newentry[namelen] = '=';
        memcpy(newentry + namelen + 1, value, strlen(value) + 1);
        /* find and replace pointer in environ */
        for (char **ep = environ; *ep; ep++) {
            if (*ep == entry) { *ep = newentry; break; }
        }
        return 0;
    }

    /* add new entry */
    size_t namelen = strlen(name);
    size_t vallen  = strlen(value);
    size_t newlen  = namelen + 1 + vallen + 1;
    char *newentry = (char *)malloc(newlen);
    if (!newentry) return -1;
    memcpy(newentry, name, namelen);
    newentry[namelen] = '=';
    memcpy(newentry + namelen + 1, value, vallen + 1);

    int cnt = env_count();
    char **newenv = (char **)malloc((cnt + 2) * sizeof(char *));
    if (!newenv) { free(newentry); return -1; }
    for (int i = 0; i < cnt; i++) newenv[i] = environ[i];
    newenv[cnt] = newentry;
    newenv[cnt + 1] = 0;
    environ = newenv;
    return 0;
}

int putenv(char *string) {
    if (!string) return -1;
    char *eq = strchr(string, '=');
    if (!eq) return -1;

    size_t namelen = (size_t)(eq - string);
    char *existing = env_find(string);

    if (existing) {
        /* replace entry pointer */
        char *entry = existing - namelen - 1;
        for (char **ep = environ; *ep; ep++) {
            if (*ep == entry) { *ep = string; return 0; }
        }
    }

    int cnt = env_count();
    char **newenv = (char **)malloc((cnt + 2) * sizeof(char *));
    if (!newenv) return -1;
    for (int i = 0; i < cnt; i++) newenv[i] = environ[i];
    newenv[cnt] = string;
    newenv[cnt + 1] = 0;
    environ = newenv;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !environ) return -1;
    size_t namelen = strlen(name);
    int cnt = env_count();
    for (int i = 0; i < cnt; i++) {
        if (strncmp(environ[i], name, namelen) == 0 &&
            environ[i][namelen] == '=') {
            for (int j = i; j < cnt; j++) environ[j] = environ[j + 1];
            return 0;
        }
    }
    return 0;
}