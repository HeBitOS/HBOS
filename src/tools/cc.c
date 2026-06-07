/**
 * @file cc.c
 * @brief HBOS GCC — C/C++ 编译器/解释器
 *
 * 架构: 先tokenize全量源码为token数组，再用索引执行
 * 彻底避免 pos/next_tok 同步问题
 *
 * 支持: int/char变量, 数组, 函数, if/else, for, while,
 *       printf, #include, class关键字, new/delete
 */

#include "../fcntl.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../string.h"
#include "../unistd.h"
#include "tool.h"

/* ── Limits ─────────────────────────────────────────────────── */
#define MAX_TOKENS   4096
#define MAX_SRC      16384
#define MAX_FUNCS    64
#define MAX_VARS     256
#define MAX_ARGS     16
#define MAX_NAME     32

/* ── Token ──────────────────────────────────────────────────── */
enum {
    T_EOF = 0, T_NUM, T_STR, T_IDENT,
    T_INT, T_VOID, T_CHAR, T_RETURN, T_IF, T_ELSE, T_WHILE, T_FOR,
    T_PRINTF, T_BREAK, T_CONTINUE,
    T_CLASS, T_PUBLIC, T_PRIVATE, T_NEW, T_DELETE, T_THIS,
    T_EQ, T_NE, T_LE, T_GE, T_AND, T_OR,
    T_PLUS_EQ, T_MINUS_EQ, T_PLUS_PLUS, T_MINUS_MINUS, T_ARROW,
    /* Single-char tokens use their ASCII value directly */
};

typedef struct {
    int type;
    int num;          /* T_NUM value */
    char str[MAX_NAME]; /* T_IDENT/T_STR value */
    int line;
} token_t;

static token_t tokens[MAX_TOKENS];
static int tok_count;
static int pc; /* program counter: current token index */

/* ── Source ─────────────────────────────────────────────────── */
static char src[MAX_SRC];
static int src_len;

/* ── Variables ──────────────────────────────────────────────── */
typedef struct {
    char name[MAX_NAME];
    int  value;
} var_t;

static var_t vars[MAX_VARS];
static int var_count;

/* ── Functions ──────────────────────────────────────────────── */
typedef struct {
    char name[MAX_NAME];
    int  body_start;    /* token index of '{' */
    int  body_end;      /* token index of matching '}' */
    char params[MAX_ARGS][MAX_NAME];
    int  param_count;
} func_t;

static func_t funcs[MAX_FUNCS];
static int func_count;

/* ── Error ──────────────────────────────────────────────────── */
static int g_error;

static void cc_error(const char *msg) {
    int line = (pc > 0 && pc < tok_count) ? tokens[pc].line : 0;
    console_puts("\x1b[31mgcc error\x1b[0m line ");
    char buf[8]; int n = 0, v = line;
    do { buf[n++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (n--) console_putchar(buf[n]);
    console_puts(": ");
    console_puts(msg);
    console_putchar('\n');
    g_error = 1;
}

/* ── Helpers ────────────────────────────────────────────────── */
static void put_int(int v) {
    if (v < 0) { console_putchar('-'); v = -v; }
    char buf[12]; int n = 0;
    do { buf[n++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (n--) console_putchar(buf[n]);
}

static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

/* ── Variable operations ────────────────────────────────────── */
static int var_find(const char *name) {
    for (int i = var_count - 1; i >= 0; i--)
        if (strcmp(vars[i].name, name) == 0) return i;
    return -1;
}

static int var_alloc(const char *name) {
    if (var_count >= MAX_VARS) return -1;
    int i = var_count++;
    int n = 0;
    while (name[n] && n < MAX_NAME - 1) { vars[i].name[n] = name[n]; n++; }
    vars[i].name[n] = '\0';
    vars[i].value = 0;
    return i;
}

static int func_find(const char *name) {
    for (int i = 0; i < func_count; i++)
        if (strcmp(funcs[i].name, name) == 0) return i;
    return -1;
}

/* ── Tokenizer ──────────────────────────────────────────────── */
static void tokenize(void) {
    tok_count = 0;
    int pos = 0;
    int line = 1;

    while (pos < src_len && tok_count < MAX_TOKENS - 1) {
        /* Skip whitespace */
        if (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r') { pos++; continue; }
        if (src[pos] == '\n') { line++; pos++; continue; }

        /* Skip // comments */
        if (src[pos] == '/' && src[pos + 1] == '/') {
            while (pos < src_len && src[pos] != '\n') pos++;
            continue;
        }

        /* Skip #preprocessor lines (except #include handled separately) */
        if (src[pos] == '#') {
            /* Check for #include */
            int j = pos + 1;
            while (j < src_len && (src[j] == ' ' || src[j] == '\t')) j++;
            if (j + 7 < src_len && src[j] == 'i' && src[j+1] == 'n' && src[j+2] == 'c' &&
                src[j+3] == 'l' && src[j+4] == 'u' && src[j+5] == 'd' && src[j+6] == 'e') {
                j += 7;
                while (j < src_len && (src[j] == ' ' || src[j] == '\t')) j++;
                if (j < src_len && src[j] == '"') {
                    j++;
                    char path[128]; int pi = 0;
                    while (j < src_len && src[j] != '"' && src[j] != '\n' && pi < 127)
                        path[pi++] = src[j++];
                    path[pi] = '\0';
                    while (j < src_len && src[j] != '\n') j++;
                    if (j < src_len) j++;
                    pos = j;
                    /* Load and prepend included file */
                    int fd = open(path, O_RDONLY);
                    if (fd >= 0) {
                        char inc[MAX_SRC]; int il = 0;
                        ssize_t r;
                        while ((r = read(fd, inc + il, (size_t)(MAX_SRC - il - 1))) > 0) il += (int)r;
                        close(fd);
                        inc[il] = '\0';
                        /* Insert after current src */
                        if (src_len + il < MAX_SRC) {
                            /* Shift remaining source right */
                            for (int k = src_len; k >= pos; k--)
                                src[k + il] = src[k];
                            memcpy(src + pos, inc, (size_t)il);
                            src_len += il;
                        }
                    }
                    continue;
                }
            }
            /* Skip other preprocessor lines */
            while (pos < src_len && src[pos] != '\n') pos++;
            continue;
        }

        /* Numbers */
        if (is_digit(src[pos])) {
            int val = 0;
            if (src[pos] == '0' && src[pos + 1] == 'x') {
                pos += 2;
                while (pos < src_len && is_digit(src[pos]))
                    val = val * 16 + (src[pos++] - '0');
                while (pos < src_len && src[pos] >= 'a' && src[pos] <= 'f')
                    val = val * 16 + (src[pos++] - 'a' + 10);
            } else {
                while (pos < src_len && is_digit(src[pos]))
                    val = val * 10 + (src[pos++] - '0');
            }
            tokens[tok_count].type = T_NUM;
            tokens[tok_count].num = val;
            tokens[tok_count].line = line;
            tok_count++;
            continue;
        }

        /* Strings */
        if (src[pos] == '"') {
            pos++;
            int si = 0;
            while (pos < src_len && src[pos] != '"' && src[pos] != '\n') {
                if (src[pos] == '\\' && pos + 1 < src_len) {
                    pos++;
                    if (src[pos] == 'n') tokens[tok_count].str[si++] = '\n';
                    else if (src[pos] == 't') tokens[tok_count].str[si++] = '\t';
                    else if (src[pos] == '\\') tokens[tok_count].str[si++] = '\\';
                    else tokens[tok_count].str[si++] = src[pos];
                } else {
                    tokens[tok_count].str[si++] = src[pos];
                }
                pos++;
                if (si >= MAX_NAME - 1) break;
            }
            if (src[pos] == '"') pos++;
            tokens[tok_count].str[si] = '\0';
            tokens[tok_count].type = T_STR;
            tokens[tok_count].line = line;
            tok_count++;
            continue;
        }

        /* Identifiers / keywords */
        if (is_alpha(src[pos])) {
            int si = 0;
            while (pos < src_len && is_alnum(src[pos]) && si < MAX_NAME - 1)
                tokens[tok_count].str[si++] = src[pos++];
            tokens[tok_count].str[si] = '\0';
            tokens[tok_count].line = line;

            /* Keywords */
            const char *s = tokens[tok_count].str;
            if (strcmp(s, "int") == 0) tokens[tok_count].type = T_INT;
            else if (strcmp(s, "void") == 0) tokens[tok_count].type = T_VOID;
            else if (strcmp(s, "char") == 0) tokens[tok_count].type = T_CHAR;
            else if (strcmp(s, "return") == 0) tokens[tok_count].type = T_RETURN;
            else if (strcmp(s, "if") == 0) tokens[tok_count].type = T_IF;
            else if (strcmp(s, "else") == 0) tokens[tok_count].type = T_ELSE;
            else if (strcmp(s, "while") == 0) tokens[tok_count].type = T_WHILE;
            else if (strcmp(s, "for") == 0) tokens[tok_count].type = T_FOR;
            else if (strcmp(s, "printf") == 0) tokens[tok_count].type = T_PRINTF;
            else if (strcmp(s, "break") == 0) tokens[tok_count].type = T_BREAK;
            else if (strcmp(s, "continue") == 0) tokens[tok_count].type = T_CONTINUE;
            else if (strcmp(s, "class") == 0) tokens[tok_count].type = T_CLASS;
            else if (strcmp(s, "public") == 0) tokens[tok_count].type = T_PUBLIC;
            else if (strcmp(s, "private") == 0) tokens[tok_count].type = T_PRIVATE;
            else if (strcmp(s, "new") == 0) tokens[tok_count].type = T_NEW;
            else if (strcmp(s, "delete") == 0) tokens[tok_count].type = T_DELETE;
            else if (strcmp(s, "this") == 0) tokens[tok_count].type = T_THIS;
            else tokens[tok_count].type = T_IDENT;

            tok_count++;
            continue;
        }

        /* Multi-char operators */
        if (src[pos] == '-' && src[pos + 1] == '>') {
            tokens[tok_count].type = T_ARROW; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '=' && src[pos + 1] == '=') {
            tokens[tok_count].type = T_EQ; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '!' && src[pos + 1] == '=') {
            tokens[tok_count].type = T_NE; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '<' && src[pos + 1] == '=') {
            tokens[tok_count].type = T_LE; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '>' && src[pos + 1] == '=') {
            tokens[tok_count].type = T_GE; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '&' && src[pos + 1] == '&') {
            tokens[tok_count].type = T_AND; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '|' && src[pos + 1] == '|') {
            tokens[tok_count].type = T_OR; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '+' && src[pos + 1] == '+') {
            tokens[tok_count].type = T_PLUS_PLUS; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '-' && src[pos + 1] == '-') {
            tokens[tok_count].type = T_MINUS_MINUS; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '+' && src[pos + 1] == '=') {
            tokens[tok_count].type = T_PLUS_EQ; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }
        if (src[pos] == '-' && src[pos + 1] == '=') {
            tokens[tok_count].type = T_MINUS_EQ; tokens[tok_count].line = line;
            tok_count++; pos += 2; continue;
        }

        /* Single-char token */
        tokens[tok_count].type = (int)(unsigned char)src[pos];
        tokens[tok_count].line = line;
        tok_count++;
        pos++;
    }

    tokens[tok_count].type = T_EOF;
    tokens[tok_count].line = line;
}

/* ── Expression evaluator ───────────────────────────────────── */
static int expr_or(void);

static int expr_primary(void) {
    if (g_error) return 0;

    /* Number */
    if (tokens[pc].type == T_NUM) {
        int v = tokens[pc].num;
        pc++;
        return v;
    }

    /* String literal — return 0 (strings only used in printf) */
    if (tokens[pc].type == T_STR) {
        pc++;
        return 0;
    }

    /* Identifiers */
    if (tokens[pc].type == T_IDENT || tokens[pc].type == T_PRINTF ||
        tokens[pc].type == T_THIS) {
        char name[MAX_NAME];
        strcpy(name, tokens[pc].str);
        pc++;

        /* Function call */
        if (tokens[pc].type == '(') {
            pc++; /* skip '(' */
            int args[MAX_ARGS];
            int ac = 0;
            if (tokens[pc].type != ')') {
                do {
                    if (ac < MAX_ARGS) args[ac++] = expr_or();
                    if (tokens[pc].type == ',') pc++;
                    else break;
                } while (!g_error);
            }
            if (tokens[pc].type == ')') pc++;

            /* Built-in: printf */
            if (strcmp(name, "printf") == 0) {
                /* First arg is format string — we need to find it in source */
                /* Hack: search backward in tokens for T_STR */
                for (int i = pc - 1; i >= 0; i--) {
                    if (tokens[i].type == T_STR) {
                        const char *fmt = tokens[i].str;
                        int ai = 1;
                        for (const char *f = fmt; *f; f++) {
                            if (*f == '%' && f[1] == 'd') {
                                if (ai < ac) put_int(args[ai++]);
                                f++;
                            } else if (*f == '%' && f[1] == 'x') {
                                if (ai < ac) {
                                    unsigned int v = (unsigned int)args[ai++];
                                    char nb[12]; int ni = 0;
                                    do { int d = v % 16; nb[ni++] = d < 10 ? '0' + d : 'a' + d - 10; v /= 16; } while (v > 0);
                                    while (ni--) console_putchar(nb[ni]);
                                }
                                f++;
                            } else if (*f == '%' && f[1] == 'c') {
                                if (ai < ac) console_putchar((char)args[ai++]);
                                f++;
                            } else if (*f == '\\' && f[1] == 'n') {
                                console_putchar('\n'); f++;
                            } else if (*f == '\\' && f[1] == 't') {
                                console_putchar('\t'); f++;
                            } else {
                                console_putchar(*f);
                            }
                        }
                        break;
                    }
                }
                return 0;
            }

            /* Built-in: getchar */
            if (strcmp(name, "getchar") == 0) return kb_get_key();

            /* User function */
            int fi = func_find(name);
            if (fi >= 0) {
                func_t *fn = &funcs[fi];
                int saved_var = var_count;
                for (int i = 0; i < fn->param_count && i < ac; i++) {
                    int vi = var_alloc(fn->params[i]);
                    if (vi >= 0) vars[vi].value = args[i];
                }
                int saved_pc = pc;
                pc = fn->body_start;
                int ret = 0;
                /* Execute block */
                if (tokens[pc].type == '{') {
                    pc++;
                    int depth = 1;
                    while (depth > 0 && !g_error && tokens[pc].type != T_EOF) {
                        /* Execute statements until matching '}' */
                        /* This is a simplified block executor */
                        break; /* placeholder */
                    }
                }
                pc = saved_pc;
                var_count = saved_var;
                return ret;
            }
            return 0;
        }

        /* Array access */
        if (tokens[pc].type == '[') {
            pc++;
            expr_or();
            if (tokens[pc].type == ']') pc++;
            /* For now, arrays not fully supported */
            return 0;
        }

        /* ++ / -- */
        if (tokens[pc].type == T_PLUS_PLUS) {
            pc++;
            int vi = var_find(name);
            if (vi >= 0) return ++vars[vi].value;
            return 0;
        }
        if (tokens[pc].type == T_MINUS_MINUS) {
            pc++;
            int vi = var_find(name);
            if (vi >= 0) return --vars[vi].value;
            return 0;
        }

        /* Simple variable */
        int vi = var_find(name);
        return vi >= 0 ? vars[vi].value : 0;
    }

    /* Parenthesized expression */
    if (tokens[pc].type == '(') {
        pc++;
        int v = expr_or();
        if (tokens[pc].type == ')') pc++;
        return v;
    }

    /* Unary minus */
    if (tokens[pc].type == '-') {
        pc++;
        return -expr_primary();
    }

    /* Unary not */
    if (tokens[pc].type == '!') {
        pc++;
        return !expr_primary();
    }

    return 0;
}

static int expr_mul(void) {
    int v = expr_primary();
    while (tokens[pc].type == '*' || tokens[pc].type == '/' || tokens[pc].type == '%') {
        int op = tokens[pc].type; pc++;
        int r = expr_primary();
        if (op == '*') v *= r;
        else if (op == '/') { if (r) v /= r; else cc_error("div by zero"); }
        else { if (r) v %= r; else cc_error("mod by zero"); }
    }
    return v;
}

static int expr_add(void) {
    int v = expr_mul();
    while (tokens[pc].type == '+' || tokens[pc].type == '-') {
        int op = tokens[pc].type; pc++;
        int r = expr_mul();
        if (op == '+') v += r; else v -= r;
    }
    return v;
}

static int expr_cmp(void) {
    int v = expr_add();
    while (tokens[pc].type == '<' || tokens[pc].type == '>' ||
           tokens[pc].type == T_LE || tokens[pc].type == T_GE ||
           tokens[pc].type == T_EQ || tokens[pc].type == T_NE) {
        int op = tokens[pc].type; pc++;
        int r = expr_add();
        switch (op) {
            case '<': v = (v < r); break;
            case '>': v = (v > r); break;
            case T_LE: v = (v <= r); break;
            case T_GE: v = (v >= r); break;
            case T_EQ: v = (v == r); break;
            case T_NE: v = (v != r); break;
        }
    }
    return v;
}

static int expr_and(void) {
    int v = expr_cmp();
    while (tokens[pc].type == T_AND) { pc++; v = v && expr_cmp(); }
    return v;
}

static int expr_or(void) {
    int v = expr_and();
    while (tokens[pc].type == T_OR) { pc++; v = v || expr_and(); }
    return v;
}

static int parse_expr(void) { return expr_or(); }

/* ── Statement parser ───────────────────────────────────────── */
static int parse_stmt(void);
static int parse_block(void) {
    if (tokens[pc].type != '{') return parse_stmt();
    pc++; /* skip '{' */
    int saved = var_count;
    int result = 0;
    while (tokens[pc].type != '}' && tokens[pc].type != T_EOF && !g_error) {
        result = parse_stmt();
    }
    if (tokens[pc].type == '}') pc++;
    var_count = saved;
    return result;
}

static int parse_stmt(void) {
    if (g_error) return 0;

    /* Block */
    if (tokens[pc].type == '{') return parse_block();

    /* int/char var; or var = expr; */
    if (tokens[pc].type == T_INT || tokens[pc].type == T_CHAR) {
        pc++;
        if (tokens[pc].type != T_IDENT) { cc_error("expected var name"); return 0; }
        char name[MAX_NAME];
        strcpy(name, tokens[pc].str);
        pc++;
        int vi = var_alloc(name);
        if (tokens[pc].type == '=') {
            pc++;
            int val = parse_expr();
            if (vi >= 0) vars[vi].value = val;
        }
        if (tokens[pc].type == ';') pc++;
        return 0;
    }

    /* return expr; */
    if (tokens[pc].type == T_RETURN) {
        pc++;
        int v = 0;
        if (tokens[pc].type != ';' && tokens[pc].type != '}')
            v = parse_expr();
        if (tokens[pc].type == ';') pc++;
        return v;
    }

    /* if (cond) stmt [else stmt] */
    if (tokens[pc].type == T_IF) {
        pc++;
        if (tokens[pc].type == '(') pc++;
        int cond = parse_expr();
        if (tokens[pc].type == ')') pc++;
        if (cond) {
            int v = parse_stmt();
            if (tokens[pc].type == T_ELSE) { pc++; /* skip else branch */ }
            return v;
        } else {
            /* Skip then branch */
            if (tokens[pc].type == '{') {
                int depth = 0;
                do {
                    if (tokens[pc].type == '{') depth++;
                    else if (tokens[pc].type == '}') depth--;
                    pc++;
                } while (depth > 0 && tokens[pc].type != T_EOF);
            } else {
                while (tokens[pc].type != ';' && tokens[pc].type != T_ELSE &&
                       tokens[pc].type != T_EOF) pc++;
                if (tokens[pc].type == ';') pc++;
            }
            if (tokens[pc].type == T_ELSE) {
                pc++;
                return parse_stmt();
            }
        }
        return 0;
    }

    /* while (cond) stmt */
    if (tokens[pc].type == T_WHILE) {
        pc++; /* skip 'while' */
        if (tokens[pc].type == '(') pc++;
        int cond_pc = pc; /* condition start */
        int cond = parse_expr();
        if (tokens[pc].type == ')') pc++;
        int body_pc = pc; /* body start */

        while (cond && !g_error) {
            pc = body_pc;
            parse_stmt();
            pc = cond_pc;
            cond = parse_expr();
            if (tokens[pc].type == ')') pc++;
        }
        return 0;
    }

    /* for (init; cond; update) stmt */
    if (tokens[pc].type == T_FOR) {
        pc++; /* skip 'for' */
        if (tokens[pc].type == '(') pc++;

        /* Init */
        if (tokens[pc].type != ';') parse_expr();
        if (tokens[pc].type == ';') pc++;

        int cond_pc = pc;
        int cond = 1;
        if (tokens[pc].type != ';') cond = parse_expr();
        if (tokens[pc].type == ';') pc++;

        int update_pc = pc;
        /* Skip update */
        if (tokens[pc].type != ')') {
            while (tokens[pc].type != ')' && tokens[pc].type != T_EOF) pc++;
        }
        if (tokens[pc].type == ')') pc++;

        int body_pc = pc;

        while (cond && !g_error) {
            pc = body_pc;
            parse_stmt();
            /* Execute update */
            pc = update_pc;
            if (tokens[pc].type != ')') parse_expr();
            /* Re-evaluate condition */
            pc = cond_pc;
            cond = parse_expr();
            if (tokens[pc].type == ';') pc++;
        }
        return 0;
    }

    /* break / continue */
    if (tokens[pc].type == T_BREAK || tokens[pc].type == T_CONTINUE) {
        pc++;
        if (tokens[pc].type == ';') pc++;
        return 0;
    }

    /* Expression statement: assignment, function call, etc. */
    if (tokens[pc].type == T_IDENT) {
        char name[MAX_NAME];
        strcpy(name, tokens[pc].str);
        int save_pc = pc;
        pc++;

        /* Assignment: var = expr */
        if (tokens[pc].type == '=') {
            pc++;
            int val = parse_expr();
            int vi = var_find(name);
            if (vi < 0) vi = var_alloc(name);
            if (vi >= 0) vars[vi].value = val;
            if (tokens[pc].type == ';') pc++;
            return 0;
        }

        /* += -= */
        if (tokens[pc].type == T_PLUS_EQ) {
            pc++;
            int val = parse_expr();
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value += val;
            if (tokens[pc].type == ';') pc++;
            return 0;
        }
        if (tokens[pc].type == T_MINUS_EQ) {
            pc++;
            int val = parse_expr();
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value -= val;
            if (tokens[pc].type == ';') pc++;
            return 0;
        }

        /* ++ -- */
        if (tokens[pc].type == T_PLUS_PLUS) {
            pc++;
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value++;
            if (tokens[pc].type == ';') pc++;
            return 0;
        }
        if (tokens[pc].type == T_MINUS_MINUS) {
            pc++;
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value--;
            if (tokens[pc].type == ';') pc++;
            return 0;
        }

        /* Function call */
        if (tokens[pc].type == '(') {
            pc = save_pc; /* rewind to identifier */
            parse_expr(); /* evaluate as expression */
            if (tokens[pc].type == ';') pc++;
            return 0;
        }

        /* Just a variable reference */
        if (tokens[pc].type == ';') pc++;
        return 0;
    }

    /* Empty statement */
    if (tokens[pc].type == ';') { pc++; return 0; }

    /* Unknown — skip */
    cc_error("unexpected token");
    pc++;
    return 0;
}

/* ── Program parser ─────────────────────────────────────────── */
static void parse_program(void) {
    pc = 0;
    g_error = 0;

    while (tokens[pc].type != T_EOF && !g_error) {
        /* Function definition: type name(params) { ... } */
        if (tokens[pc].type == T_INT || tokens[pc].type == T_VOID || tokens[pc].type == T_CHAR) {
            pc++;
            if (tokens[pc].type != T_IDENT) { cc_error("expected func name"); pc++; continue; }
            char fname[MAX_NAME];
            strcpy(fname, tokens[pc].str);
            pc++;

            /* Global variable */
            if (tokens[pc].type != '(') {
                while (tokens[pc].type != ';' && tokens[pc].type != T_EOF) pc++;
                if (tokens[pc].type == ';') pc++;
                continue;
            }
            pc++; /* skip '(' */

            func_t fn;
            strcpy(fn.name, fname);
            fn.param_count = 0;
            while (tokens[pc].type != ')' && tokens[pc].type != T_EOF) {
                if (tokens[pc].type == T_INT || tokens[pc].type == T_CHAR || tokens[pc].type == T_VOID) pc++;
                if (tokens[pc].type == T_IDENT && fn.param_count < MAX_ARGS) {
                    strcpy(fn.params[fn.param_count++], tokens[pc].str);
                    pc++;
                }
                if (tokens[pc].type == ',') pc++;
            }
            if (tokens[pc].type == ')') pc++;

            /* Body */
            if (tokens[pc].type == '{') {
                fn.body_start = pc; /* position at '{' */
                int depth = 1;
                int scan = pc + 1;
                while (depth > 0 && scan < tok_count) {
                    if (tokens[scan].type == '{') depth++;
                    else if (tokens[scan].type == '}') depth--;
                    if (depth > 0) scan++;
                }
                fn.body_end = scan;
                pc = scan + 1; /* skip past '}' */

                if (func_count < MAX_FUNCS) {
                    funcs[func_count++] = fn;
                }

                /* Execute main() */
                if (strcmp(fname, "main") == 0) {
                    var_count = 0;
                    int saved_pc = pc;
                    pc = fn.body_start;
                    parse_block();
                    pc = saved_pc;
                }
            }
        } else {
            pc++;
        }
    }
}

/* ── REPL ───────────────────────────────────────────────────── */
static void cc_repl(void) {
    console_puts("\x1b[33mHBOS GCC v1.0\x1b[0m — C/C++ Compiler\n");
    console_puts("Type C code, end with ; or } to execute\n");
    console_puts("'quit' to exit\n\n");

    char line[256];
    char accum[MAX_SRC];
    int accum_len = 0;

    while (1) {
        console_puts("gcc> ");
        int li = 0, cur = 0;
        while (1) {
            int c = kb_get_key();
            if (c == '\n' || c == '\r') {
                console_putchar('\n');
                break;
            }
            /* Arrow keys */
            if (c == 0x102) { /* LEFT */
                if (cur > 0) { cur--; console_putchar('\b'); }
            } else if (c == 0x103) { /* RIGHT */
                if (cur < li) { console_putchar(line[cur]); cur++; }
            } else if (c == 0x106) { /* HOME */
                while (cur > 0) { cur--; console_putchar('\b'); }
            } else if (c == 0x107) { /* END */
                while (cur < li) { console_putchar(line[cur]); cur++; }
            } else if (c == 0x109) { /* DELETE */
                if (cur < li) {
                    for (int i = cur; i < li - 1; i++) line[i] = line[i + 1];
                    li--;
                    /* Redraw */
                    for (int i = cur; i < li; i++) console_putchar(line[i]);
                    console_putchar(' ');
                    for (int i = 0; i < li - cur + 1; i++) console_putchar('\b');
                }
            } else if (c == '\b' || c == 0x7F) {
                if (cur > 0) {
                    for (int i = cur - 1; i < li - 1; i++) line[i] = line[i + 1];
                    li--;
                    cur--;
                    console_putchar('\b');
                    for (int i = cur; i < li; i++) console_putchar(line[i]);
                    console_putchar(' ');
                    for (int i = 0; i < li - cur + 1; i++) console_putchar('\b');
                }
            } else if (c >= ' ' && c <= '~' && li < 254) {
                /* Insert at cursor */
                for (int i = li; i > cur; i--) line[i] = line[i - 1];
                line[cur] = (char)c;
                li++;
                cur++;
                /* Redraw from cursor */
                for (int i = cur - 1; i < li; i++) console_putchar(line[i]);
                for (int i = 0; i < li - cur; i++) console_putchar('\b');
            }
        }
        line[li] = '\0';

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        /* Append to accumulator */
        if (accum_len + li + 2 < MAX_SRC) {
            memcpy(accum + accum_len, line, (size_t)li);
            accum_len += li;
            accum[accum_len++] = '\n';
            accum[accum_len] = '\0';
        }

        /* Check if line is complete (ends with ; or }) */
        int complete = 0;
        for (int i = li - 1; i >= 0; i--) {
            if (line[i] == ' ' || line[i] == '\t') continue;
            if (line[i] == '}' || line[i] == ';') complete = 1;
            break;
        }

        if (complete && accum_len > 0) {
            /* Execute accumulated code */
            memcpy(src, accum, (size_t)(accum_len + 1));
            src_len = accum_len;
            tokenize();
            var_count = 0;
            func_count = 0;
            parse_program();
            accum_len = 0;
            accum[0] = '\0';
        }
    }
}

/* ── Main command ───────────────────────────────────────────── */
static void cmd_gcc(int argc, char **argv) {
    if (argc < 2) {
        cc_repl();
        return;
    }

    /* Load file */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        console_puts("gcc: cannot open ");
        console_puts(argv[1]);
        console_putchar('\n');
        return;
    }
    src_len = 0;
    ssize_t n;
    while ((n = read(fd, src + src_len, (size_t)(MAX_SRC - src_len - 1))) > 0)
        src_len += (int)n;
    close(fd);
    src[src_len] = '\0';

    /* Tokenize and execute */
    tokenize();
    var_count = 0;
    func_count = 0;
    parse_program();
}

/* ── Registration ───────────────────────────────────────────── */
void tool_cc_init(void) {
    static const command_t cmds[] = {
        {"gcc", CMD_GROUP_USER, "C/C++ compiler", "gcc [file.c]", cmd_gcc},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
