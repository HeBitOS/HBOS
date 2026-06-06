/**
 * @file basic.c
 * @brief HBOS BASIC 解释器 — 支持变量、算术、条件、循环、输入输出
 *
 * 支持的命令:
 *   PRINT expr         — 输出表达式
 *   LET var = expr     — 赋值
 *   INPUT var          — 从键盘读取
 *   IF expr THEN cmd   — 条件
 *   FOR var = a TO b   — 循环
 *   NEXT               — 循环结束
 *   GOTO linenum       — 跳转
 *   GOSUB linenum      — 子程序调用
 *   RETURN             — 子程序返回
 *   END                — 程序结束
 *   RUN                — 执行程序
 *   LIST               — 列出程序
 *   NEW                — 清除程序
 *   REM                — 注释
 */

#include "../fcntl.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../string.h"
#include "../unistd.h"
#include "tool.h"

/* ── Limits ─────────────────────────────────────────────────── */
#define BASIC_MAX_LINES   256
#define BASIC_LINE_LEN    128
#define BASIC_MAX_VARS    64
#define BASIC_MAX_FOR     16
#define BASIC_MAX_GOSUB   16
#define BASIC_EXPR_DEPTH  32

/* ── Program storage ────────────────────────────────────────── */
typedef struct {
    uint16_t num;                    /* line number, 0 = deleted */
    char     text[BASIC_LINE_LEN];   /* line content */
} basic_line_t;

static basic_line_t program[BASIC_MAX_LINES];
static int prog_count;

/* ── Variables ──────────────────────────────────────────────── */
typedef struct {
    char name[16];
    int  value;
} basic_var_t;

static basic_var_t vars[BASIC_MAX_VARS];
static int var_count;

/* ── FOR stack ──────────────────────────────────────────────── */
typedef struct {
    int  line_idx;   /* index in program[] of FOR line */
    char var[16];    /* loop variable */
    int  limit;      /* TO value */
} basic_for_t;

static basic_for_t for_stack[BASIC_MAX_FOR];
static int for_sp;

/* ── GOSUB stack ────────────────────────────────────────────── */
static int gosub_stack[BASIC_MAX_GOSUB];
static int gosub_sp;

/* ── Helpers ────────────────────────────────────────────────── */
static void put_str(const char *s) { console_puts(s); }
static void put_int(int v) {
    if (v < 0) { console_putchar('-'); v = -v; }
    char buf[12]; int n = 0;
    do { buf[n++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (n--) console_putchar(buf[n]);
}
static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* Find variable by name, return index or -1 */
static int var_find(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (str_eq(vars[i].name, name)) return i;
    return -1;
}

/* Get or create variable, return index */
static int var_get(const char *name) {
    int idx = var_find(name);
    if (idx >= 0) return idx;
    if (var_count >= BASIC_MAX_VARS) return -1;
    idx = var_count++;
    int i = 0;
    while (name[i] && i < 15) { vars[idx].name[i] = name[i]; i++; }
    vars[idx].name[i] = '\0';
    vars[idx].value = 0;
    return idx;
}

/* Find program line by number, return index or -1 */
static int line_find(uint16_t num) {
    for (int i = 0; i < prog_count; i++)
        if (program[i].num == num) return i;
    return -1;
}

/* Insert or replace a program line */
static void line_store(uint16_t num, const char *text) {
    /* If text is empty or just whitespace, delete the line */
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') {
        int idx = line_find(num);
        if (idx >= 0) {
            for (int i = idx; i < prog_count - 1; i++) program[i] = program[i + 1];
            prog_count--;
        }
        return;
    }
    int idx = line_find(num);
    if (idx >= 0) {
        program[idx].num = num;
        int i = 0;
        while (text[i] && i < BASIC_LINE_LEN - 1) { program[idx].text[i] = text[i]; i++; }
        program[idx].text[i] = '\0';
    } else {
        if (prog_count >= BASIC_MAX_LINES) { put_str("basic: program full\n"); return; }
        /* Insert in sorted order */
        int pos = prog_count;
        for (int i = 0; i < prog_count; i++) {
            if (program[i].num > num) { pos = i; break; }
        }
        for (int i = prog_count; i > pos; i--) program[i] = program[i - 1];
        program[pos].num = num;
        int i = 0;
        while (text[i] && i < BASIC_LINE_LEN - 1) { program[pos].text[i] = text[i]; i++; }
        program[pos].text[i] = '\0';
        prog_count++;
    }
}

/* ── Expression evaluator ───────────────────────────────────── */
/* Supports: +, -, *, /, %, ==, !=, <, >, <=, >=, AND, OR, NOT
   Variables are int. Literals are decimal. */

static const char *expr_ptr; /* current parse position */
static int expr_error;

static int expr_skip_space(void) {
    while (*expr_ptr == ' ' || *expr_ptr == '\t') expr_ptr++;
    return *expr_ptr;
}

/* Forward declarations */
static int expr_or(void);

static int expr_atom(void) {
    expr_skip_space();
    if (*expr_ptr == '(') {
        expr_ptr++;
        int val = expr_or();
        expr_skip_space();
        if (*expr_ptr == ')') expr_ptr++;
        return val;
    }
    if (*expr_ptr == '-' && (expr_ptr[1] >= '0' && expr_ptr[1] <= '9')) {
        expr_ptr++;
        return -expr_atom();
    }
    if (*expr_ptr >= '0' && *expr_ptr <= '9') {
        int val = 0;
        while (*expr_ptr >= '0' && *expr_ptr <= '9') { val = val * 10 + (*expr_ptr - '0'); expr_ptr++; }
        return val;
    }
    if (*expr_ptr == '-') {
        expr_ptr++;
        return -expr_atom();
    }
    /* Variable */
    if ((*expr_ptr >= 'A' && *expr_ptr <= 'Z') || (*expr_ptr >= 'a' && *expr_ptr <= 'z')) {
        char name[16]; int ni = 0;
        while (((*expr_ptr >= 'A' && *expr_ptr <= 'Z') || (*expr_ptr >= 'a' && *expr_ptr <= 'z') ||
                (*expr_ptr >= '0' && *expr_ptr <= '9')) && ni < 15)
            name[ni++] = *expr_ptr++;
        name[ni] = '\0';
        int idx = var_find(name);
        return idx >= 0 ? vars[idx].value : 0;
    }
    expr_error = 1;
    return 0;
}

static int expr_unary(void) {
    expr_skip_space();
    if (expr_ptr[0] == 'N' && expr_ptr[1] == 'O' && expr_ptr[2] == 'T' &&
        (expr_ptr[3] < 'A' || expr_ptr[3] > 'Z')) {
        expr_ptr += 3;
        return !expr_unary();
    }
    return expr_atom();
}

static int expr_mul(void) {
    int val = expr_unary();
    while (1) {
        expr_skip_space();
        char op = *expr_ptr;
        if (op != '*' && op != '/' && op != '%') break;
        expr_ptr++;
        int rhs = expr_unary();
        if (op == '*') val *= rhs;
        else if (op == '/') { if (rhs) val /= rhs; else expr_error = 1; }
        else { if (rhs) val %= rhs; else expr_error = 1; }
    }
    return val;
}

static int expr_add(void) {
    int val = expr_mul();
    while (1) {
        expr_skip_space();
        char op = *expr_ptr;
        if (op != '+' && op != '-') break;
        expr_ptr++;
        int rhs = expr_mul();
        if (op == '+') val += rhs; else val -= rhs;
    }
    return val;
}

static int expr_cmp(void) {
    int val = expr_add();
    while (1) {
        expr_skip_space();
        int eq = 0, ne = 0, lt = 0, gt = 0;
        if (expr_ptr[0] == '=' && expr_ptr[1] == '=') { eq = 1; expr_ptr += 2; }
        else if (expr_ptr[0] == '!' && expr_ptr[1] == '=') { ne = 1; expr_ptr += 2; }
        else if (expr_ptr[0] == '<' && expr_ptr[1] == '=') { lt = 2; expr_ptr += 2; }
        else if (expr_ptr[0] == '>' && expr_ptr[1] == '=') { gt = 2; expr_ptr += 2; }
        else if (expr_ptr[0] == '<') { lt = 1; expr_ptr++; }
        else if (expr_ptr[0] == '>') { gt = 1; expr_ptr++; }
        else break;
        int rhs = expr_add();
        if (eq) val = (val == rhs);
        else if (ne) val = (val != rhs);
        else if (lt == 2) val = (val <= rhs);
        else if (gt == 2) val = (val >= rhs);
        else if (lt) val = (val < rhs);
        else if (gt) val = (val > rhs);
    }
    return val;
}

static int expr_and(void) {
    int val = expr_cmp();
    while (1) {
        expr_skip_space();
        if (expr_ptr[0] == 'A' && expr_ptr[1] == 'N' && expr_ptr[2] == 'D' &&
            (expr_ptr[3] < 'A' || expr_ptr[3] > 'Z')) {
            expr_ptr += 3;
            val = val && expr_cmp();
        } else break;
    }
    return val;
}

static int expr_or(void) {
    int val = expr_and();
    while (1) {
        expr_skip_space();
        if (expr_ptr[0] == 'O' && expr_ptr[1] == 'R' &&
            (expr_ptr[2] < 'A' || expr_ptr[2] > 'Z')) {
            expr_ptr += 2;
            val = val || expr_and();
        } else break;
    }
    return val;
}

static int eval_expr(const char *s) {
    expr_ptr = s;
    expr_error = 0;
    int val = expr_or();
    return val;
}

/* ── Line execution ─────────────────────────────────────────── */
static int g_running;  /* flag: is RUN active */
static int g_stopped;  /* flag: stop requested */

static int exec_line(const char *line);

/* Skip leading spaces, return pointer to first non-space */
static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static int exec_line(const char *line) {
    const char *p = skip_ws(line);
    if (*p == '\0' || *p == '\n') return 0;

    /* REM — comment */
    if (p[0] == 'R' && p[1] == 'E' && p[2] == 'M' && (p[3] < 'A' || p[3] > 'Z'))
        return 0;

    /* PRINT */
    if (p[0] == 'P' && p[1] == 'R' && p[2] == 'I' && p[3] == 'N' && p[4] == 'T') {
        p = skip_ws(p + 5);
        if (*p == '\0' || *p == '\n') { console_putchar('\n'); return 0; }
        /* Handle string literals */
        while (*p && *p != '\n') {
            p = skip_ws(p);
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && *p != '\n') { console_putchar(*p); p++; }
                if (*p == '"') p++;
            } else {
                int val = eval_expr(p);
                put_int(val);
                /* Advance p past the expression */
                while (*p && *p != '\n' && *p != ';') p++;
            }
            p = skip_ws(p);
            if (*p == ';') { p++; }
            else if (*p == ',') { console_putchar('\t'); p++; }
            else break;
        }
        console_putchar('\n');
        return 0;
    }

    /* LET */
    if (p[0] == 'L' && p[1] == 'E' && p[2] == 'T' && (p[3] == ' ' || p[3] == '\t')) {
        p = skip_ws(p + 3);
        char name[16]; int ni = 0;
        while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9')) && ni < 15)
            name[ni++] = *p++;
        name[ni] = '\0';
        p = skip_ws(p);
        if (*p == '=') {
            p++;
            int val = eval_expr(p);
            int idx = var_get(name);
            if (idx >= 0) vars[idx].value = val;
        }
        return 0;
    }

    /* Implicit LET: A = 5 */
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        const char *start = p;
        char name[16]; int ni = 0;
        while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9')) && ni < 15)
            name[ni++] = *p++;
        name[ni] = '\0';
        const char *after = skip_ws(p);
        if (*after == '=') {
            p = after + 1;
            int val = eval_expr(p);
            int idx = var_get(name);
            if (idx >= 0) vars[idx].value = val;
            return 0;
        }
        p = start; /* not an assignment, fall through */
    }

    /* INPUT */
    if (p[0] == 'I' && p[1] == 'N' && p[2] == 'P' && p[3] == 'U' && p[4] == 'T') {
        p = skip_ws(p + 5);
        char name[16]; int ni = 0;
        while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9')) && ni < 15)
            name[ni++] = *p++;
        name[ni] = '\0';
        put_str("? ");
        /* Read a line from keyboard */
        char ibuf[32]; int ii = 0;
        while (1) {
            int c = kb_get_key();
            if (c == '\n' || c == '\r') { console_putchar('\n'); break; }
            if (c == '\b' || c == 0x7F) {
                if (ii > 0) { ii--; console_putchar('\b'); console_putchar(' '); console_putchar('\b'); }
            } else if (c >= ' ' && c <= '~' && ii < 30) {
                ibuf[ii++] = (char)c;
                console_putchar((char)c);
            }
        }
        ibuf[ii] = '\0';
        int val = 0;
        const char *ip = ibuf;
        int neg = 0;
        if (*ip == '-') { neg = 1; ip++; }
        while (*ip >= '0' && *ip <= '9') { val = val * 10 + (*ip - '0'); ip++; }
        if (neg) val = -val;
        int idx = var_get(name);
        if (idx >= 0) vars[idx].value = val;
        return 0;
    }

    /* IF ... THEN ... */
    if (p[0] == 'I' && p[1] == 'F' && (p[2] == ' ' || p[2] == '\t')) {
        p = skip_ws(p + 2);
        /* Find THEN */
        const char *then_ptr = p;
        while (*then_ptr && *then_ptr != '\n') {
            if (then_ptr[0] == 'T' && then_ptr[1] == 'H' && then_ptr[2] == 'E' && then_ptr[3] == 'N' &&
                (then_ptr[4] == ' ' || then_ptr[4] == '\t' || then_ptr[4] == '\0' || then_ptr[4] == '\n'))
                break;
            then_ptr++;
        }
        if (*then_ptr == '\0' || *then_ptr == '\n') return 0;
        /* Temporarily null-terminate the condition */
        char saved = *then_ptr;
        *(char *)then_ptr = '\0';
        int cond = eval_expr(p);
        *(char *)then_ptr = saved;
        if (cond) {
            p = skip_ws(then_ptr + 4);
            /* Check if THEN is followed by a line number (GOTO) */
            if (*p >= '0' && *p <= '9') {
                int target = 0;
                while (*p >= '0' && *p <= '9') { target = target * 10 + (*p - '0'); p++; }
                int li = line_find((uint16_t)target);
                if (li >= 0) return li + 1; /* return next line to execute */
                return -1;
            }
            return exec_line(p);
        }
        return 0;
    }

    /* FOR var = a TO b */
    if (p[0] == 'F' && p[1] == 'O' && p[2] == 'R' && (p[3] == ' ' || p[3] == '\t')) {
        p = skip_ws(p + 3);
        char name[16]; int ni = 0;
        while (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                (*p >= '0' && *p <= '9')) && ni < 15)
            name[ni++] = *p++;
        name[ni] = '\0';
        p = skip_ws(p);
        if (*p == '=') p++;
        int start_val = eval_expr(p);
        /* Skip to TO */
        while (*p && *p != 'T') p++;
        if (*p == 'T' && p[1] == 'O') p += 2;
        int end_val = eval_expr(p);
        int idx = var_get(name);
        if (idx >= 0) vars[idx].value = start_val;
        if (for_sp < BASIC_MAX_FOR) {
            for_stack[for_sp].line_idx = -1; /* will be set by caller */
            int vi = 0;
            while (name[vi] && vi < 15) { for_stack[for_sp].var[vi] = name[vi]; vi++; }
            for_stack[for_sp].var[vi] = '\0';
            for_stack[for_sp].limit = end_val;
            for_sp++;
        }
        return 0;
    }

    /* NEXT */
    if (p[0] == 'N' && p[1] == 'E' && p[2] == 'X' && p[3] == 'T') {
        if (for_sp <= 0) return 0;
        basic_for_t *f = &for_stack[for_sp - 1];
        int idx = var_find(f->var);
        if (idx >= 0) {
            vars[idx].value++;
            if (vars[idx].value <= f->limit) {
                return f->line_idx; /* loop back */
            }
        }
        for_sp--;
        return 0;
    }

    /* GOTO */
    if (p[0] == 'G' && p[1] == 'O' && p[2] == 'T' && p[3] == 'O') {
        p = skip_ws(p + 4);
        int target = 0;
        while (*p >= '0' && *p <= '9') { target = target * 10 + (*p - '0'); p++; }
        int li = line_find((uint16_t)target);
        return li >= 0 ? li : -1;
    }

    /* GOSUB */
    if (p[0] == 'G' && p[1] == 'O' && p[2] == 'S' && p[3] == 'U' && p[4] == 'B') {
        p = skip_ws(p + 5);
        int target = 0;
        while (*p >= '0' && *p <= '9') { target = target * 10 + (*p - '0'); p++; }
        int li = line_find((uint16_t)target);
        if (li >= 0 && gosub_sp < BASIC_MAX_GOSUB) {
            gosub_stack[gosub_sp++] = -1; /* caller fills in return addr */
            return li;
        }
        return -1;
    }

    /* RETURN */
    if (p[0] == 'R' && p[1] == 'E' && p[2] == 'T' && p[3] == 'U' && p[4] == 'R' && p[5] == 'N') {
        if (gosub_sp > 0) {
            gosub_sp--;
            return gosub_stack[gosub_sp];
        }
        return -1;
    }

    /* END */
    if (p[0] == 'E' && p[1] == 'N' && p[2] == 'D' && (p[3] < 'A' || p[3] > 'Z'))
        return -1;

    /* Unknown command */
    put_str("basic: unknown: ");
    put_str(line);
    console_putchar('\n');
    return 0;
}

/* ── RUN ────────────────────────────────────────────────────── */
static void cmd_run(int argc, char **argv) {
    (void)argc; (void)argv;
    if (prog_count == 0) { put_str("basic: no program\n"); return; }

    /* Reset state */
    var_count = 0;
    for_sp = 0;
    gosub_sp = 0;
    g_running = 1;
    g_stopped = 0;

    int pc = 0; /* program counter: index into program[] */
    int steps = 0;

    while (pc >= 0 && pc < prog_count && !g_stopped) {
        /* Set FOR line_idx to current position */
        if (for_sp > 0 && for_stack[for_sp - 1].line_idx < 0)
            for_stack[for_sp - 1].line_idx = pc;

        int result = exec_line(program[pc].text);
        steps++;
        if (steps > 100000) { put_str("\nbasic: too many steps\n"); break; }

        if (result < 0) break; /* END or error */
        if (result > 0) pc = result; /* GOTO/GOSUB/FOR target */
        else pc++;
    }

    g_running = 0;
}

/* ── LIST ───────────────────────────────────────────────────── */
static void cmd_list(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < prog_count; i++) {
        put_int(program[i].num);
        console_putchar(' ');
        put_str(program[i].text);
        console_putchar('\n');
    }
}

/* ── NEW ────────────────────────────────────────────────────── */
static void cmd_new(int argc, char **argv) {
    (void)argc; (void)argv;
    prog_count = 0;
    var_count = 0;
    for_sp = 0;
    gosub_sp = 0;
    put_str("basic: cleared\n");
}

/* ── LOAD ───────────────────────────────────────────────────── */
static void cmd_load(int argc, char **argv) {
    if (argc < 2) { put_str("Usage: load <file>\n"); return; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { put_str("basic: cannot open "); put_str(argv[1]); console_putchar('\n'); return; }
    prog_count = 0;
    char buf[BASIC_LINE_LEN];
    int bi = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            buf[bi] = '\0';
            if (bi > 0) {
                /* Parse line number */
                const char *p = buf;
                while (*p == ' ' || *p == '\t') p++;
                if (*p >= '0' && *p <= '9') {
                    int num = 0;
                    while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
                    while (*p == ' ' || *p == '\t') p++;
                    line_store((uint16_t)num, p);
                }
            }
            bi = 0;
        } else {
            if (bi < BASIC_LINE_LEN - 1) buf[bi++] = c;
        }
    }
    if (bi > 0) {
        buf[bi] = '\0';
        const char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p >= '0' && *p <= '9') {
            int num = 0;
            while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
            while (*p == ' ' || *p == '\t') p++;
            line_store((uint16_t)num, p);
        }
    }
    close(fd);
    put_str("basic: loaded ");
    put_str(argv[1]);
    console_putchar('\n');
}

/* ── SAVE ───────────────────────────────────────────────────── */
static void cmd_save(int argc, char **argv) {
    if (argc < 2) { put_str("Usage: save <file>\n"); return; }
    int fd = open(argv[1], O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) { put_str("basic: cannot create "); put_str(argv[1]); console_putchar('\n'); return; }
    for (int i = 0; i < prog_count; i++) {
        char num[8]; int ni = 0; int v = program[i].num;
        do { num[ni++] = '0' + v % 10; v /= 10; } while (v > 0);
        while (ni > 0) { char c = num[--ni]; write(fd, &c, 1); }
        char sp = ' '; write(fd, &sp, 1);
        write(fd, program[i].text, (size_t)str_len(program[i].text));
        char nl = '\n'; write(fd, &nl, 1);
    }
    close(fd);
    put_str("basic: saved ");
    put_str(argv[1]);
    console_putchar('\n');
}

/* ── REPL mode ──────────────────────────────────────────────── */
static void cmd_basic(int argc, char **argv) {
    (void)argc; (void)argv;
    put_str("HBOS BASIC v1.0\n");
    put_str("Type RUN to execute, LIST to view, NEW to clear\n");
    put_str("Type EXIT to quit\n\n");

    char line[BASIC_LINE_LEN];
    while (1) {
        put_str("basic> ");
        /* Read a line */
        int li = 0;
        while (1) {
            int c = kb_get_key();
            if (c == '\n' || c == '\r') { console_putchar('\n'); break; }
            if (c == '\b' || c == 0x7F) {
                if (li > 0) { li--; console_putchar('\b'); console_putchar(' '); console_putchar('\b'); }
            } else if (c >= ' ' && c <= '~' && li < BASIC_LINE_LEN - 1) {
                line[li++] = (char)c;
                console_putchar((char)c);
            }
        }
        line[li] = '\0';

        const char *p = skip_ws(line);
        if (*p == '\0') continue;

        /* EXIT */
        if (p[0] == 'E' && p[1] == 'X' && p[2] == 'I' && p[3] == 'T') break;

        /* RUN */
        if (p[0] == 'R' && p[1] == 'U' && p[2] == 'N' && (p[3] < 'A' || p[3] > 'Z')) {
            cmd_run(0, NULL);
            continue;
        }
        /* LIST */
        if (p[0] == 'L' && p[1] == 'I' && p[2] == 'S' && p[3] == 'T') {
            cmd_list(0, NULL);
            continue;
        }
        /* NEW */
        if (p[0] == 'N' && p[1] == 'E' && p[2] == 'W' && (p[3] < 'A' || p[3] > 'Z')) {
            cmd_new(0, NULL);
            continue;
        }
        /* LOAD */
        if (p[0] == 'L' && p[1] == 'O' && p[2] == 'A' && p[3] == 'D') {
            char *av[2] = { "load", (char *)skip_ws(p + 4) };
            cmd_load(2, av);
            continue;
        }
        /* SAVE */
        if (p[0] == 'S' && p[1] == 'A' && p[2] == 'V' && p[3] == 'E') {
            char *av[2] = { "save", (char *)skip_ws(p + 4) };
            cmd_save(2, av);
            continue;
        }

        /* Check if line starts with a number → store in program */
        if (*p >= '0' && *p <= '9') {
            int num = 0;
            while (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); p++; }
            while (*p == ' ' || *p == '\t') p++;
            line_store((uint16_t)num, p);
        } else {
            /* Execute immediately */
            exec_line(line);
        }
    }
}

/* ── Registration ───────────────────────────────────────────── */
void tool_basic_init(void) {
    static const command_t cmds[] = {
        {"basic", CMD_GROUP_USER, "BASIC interpreter", "basic", cmd_basic},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
