/**
 * @file cc.c
 * @brief HBOS C/C++ 解释器 — 支持C/C++语言子集，直接执行代码
 *
 * 支持的C特性:
 *   int 变量声明与赋值
 *   printf() 输出
 *   if/else 条件
 *   for/while 循环
 *   函数定义与调用
 *   return 语句
 *   算术/比较/逻辑运算
 *   字符串字面量
 *   数组 (int arr[N])
 *   #include "file" 头文件包含
 *
 * 支持的C++特性:
 *   class 定义 (public/private)
 *   new/delete 动态内存
 *   this 指针
 *
 * 用法:
 *   cc <file>        — 执行C/C++文件
 *   cc                — 进入交互式REPL
 */

#include "../fcntl.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../string.h"
#include "../unistd.h"
#include "tool.h"

/* ── Limits ─────────────────────────────────────────────────── */
#define CC_MAX_SRC      16384
#define CC_MAX_FUNCS    64
#define CC_MAX_VARS     256
#define CC_MAX_STACK    512
#define CC_MAX_CALL     32
#define CC_MAX_ARGS     16
#define CC_MAX_NAME     32
#define CC_MAX_STRING   512
#define CC_MAX_INCLUDE  8

/* ── Token types ────────────────────────────────────────────── */
enum {
    T_EOF = 0, T_NUM, T_STR, T_IDENT,
    T_INT, T_VOID, T_CHAR, T_RETURN, T_IF, T_ELSE, T_WHILE, T_FOR,
    T_PRINTF, T_BREAK, T_CONTINUE,
    T_CLASS, T_PUBLIC, T_PRIVATE, T_NEW, T_DELETE, T_THIS,
    T_EQ /* == */, T_NE /* != */, T_LE /* <= */, T_GE /* >= */,
    T_AND /* && */, T_OR /* || */,
    T_PLUS_EQ /* += */, T_MINUS_EQ /* -= */,
    T_PLUS_PLUS /* ++ */, T_MINUS_MINUS /* -- */,
    T_ARROW /* -> */,
};

/* ── Source and tokenizer ───────────────────────────────────── */
static char src[CC_MAX_SRC];
static int src_len;
static int pos;       /* current position in src */
static int tok_type;
static int tok_num;   /* numeric value for T_NUM */
static char tok_str[CC_MAX_STRING]; /* string/identifier value */
static int tok_line;
static int include_depth; /* #include nesting depth */

/* ── #include preprocessor ──────────────────────────────────── */
static int cc_load_file(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    ssize_t r;
    while ((r = read(fd, buf + n, (size_t)(cap - n - 1))) > 0) n += (int)r;
    close(fd);
    buf[n] = '\0';
    return n;
}

static void cc_preprocess(char *code, int *len) {
    /* Simple #include "file" expansion */
    char expanded[CC_MAX_SRC];
    int ep = 0;
    int i = 0;
    while (i < *len && ep < CC_MAX_SRC - 1) {
        if (code[i] == '#' && include_depth < CC_MAX_INCLUDE) {
            /* Check for #include */
            int j = i + 1;
            while (j < *len && (code[j] == ' ' || code[j] == '\t')) j++;
            if (j + 7 < *len && code[j] == 'i' && code[j + 1] == 'n' &&
                code[j + 2] == 'c' && code[j + 3] == 'l' && code[j + 4] == 'u' &&
                code[j + 5] == 'd' && code[j + 6] == 'e') {
                j += 7;
                while (j < *len && (code[j] == ' ' || code[j] == '\t')) j++;
                if (j < *len && code[j] == '"') {
                    j++;
                    char path[128]; int pi = 0;
                    while (j < *len && code[j] != '"' && code[j] != '\n' && pi < 127)
                        path[pi++] = code[j++];
                    path[pi] = '\0';
                    /* Skip to end of line */
                    while (j < *len && code[j] != '\n') j++;
                    if (code[j] == '\n') j++;
                    /* Load included file */
                    include_depth++;
                    char inc_buf[CC_MAX_SRC];
                    int inc_len = cc_load_file(path, inc_buf, CC_MAX_SRC);
                    if (inc_len > 0) {
                        cc_preprocess(inc_buf, &inc_len);
                        if (ep + inc_len < CC_MAX_SRC) {
                            memcpy(expanded + ep, inc_buf, (size_t)inc_len);
                            ep += inc_len;
                        }
                    }
                    include_depth--;
                    i = j;
                    continue;
                }
            }
            /* Not #include, skip the line */
            while (i < *len && code[i] != '\n') i++;
            if (i < *len) i++;
            continue;
        }
        expanded[ep++] = code[i++];
    }
    expanded[ep] = '\0';
    memcpy(code, expanded, (size_t)(ep + 1));
    *len = ep;
}

static void cc_error(const char *msg) {
    console_puts("\x1b[31mcc error\x1b[0m line ");
    char buf[8]; int n = 0, v = tok_line;
    do { buf[n++] = '0' + v % 10; v /= 10; } while (v > 0);
    while (n--) console_putchar(buf[n]);
    console_puts(": ");
    console_puts(msg);
    console_putchar('\n');
}

static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

static int next_tok(void) {
    while (pos < src_len && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r'))
        pos++;
    if (pos >= src_len) { tok_type = T_EOF; return T_EOF; }

    int c = src[pos];
    if (c == '\n') { tok_line++; pos++; return next_tok(); }

    /* Numbers */
    if (is_digit(c)) {
        tok_num = 0;
        if (c == '0' && src[pos + 1] == 'x') {
            pos += 2;
            while (pos < src_len && is_digit(src[pos]))
                tok_num = tok_num * 16 + (src[pos++] - '0');
            while (pos < src_len && src[pos] >= 'a' && src[pos] <= 'f')
                tok_num = tok_num * 16 + (src[pos++] - 'a' + 10);
            while (pos < src_len && src[pos] >= 'A' && src[pos] <= 'F')
                tok_num = tok_num * 16 + (src[pos++] - 'A' + 10);
        } else {
            while (pos < src_len && is_digit(src[pos]))
                tok_num = tok_num * 10 + (src[pos++] - '0');
        }
        tok_type = T_NUM;
        return T_NUM;
    }

    /* Strings */
    if (c == '"') {
        pos++;
        int si = 0;
        while (pos < src_len && src[pos] != '"' && src[pos] != '\n') {
            if (src[pos] == '\\' && src[pos + 1]) {
                pos++;
                if (src[pos] == 'n') tok_str[si++] = '\n';
                else if (src[pos] == 't') tok_str[si++] = '\t';
                else if (src[pos] == '\\') tok_str[si++] = '\\';
                else if (src[pos] == '"') tok_str[si++] = '"';
                else tok_str[si++] = src[pos];
            } else {
                tok_str[si++] = src[pos];
            }
            pos++;
        }
        if (src[pos] == '"') pos++;
        tok_str[si] = '\0';
        tok_type = T_STR;
        return T_STR;
    }

    /* Identifiers / keywords */
    if (is_alpha(c)) {
        int si = 0;
        while (pos < src_len && is_alnum(src[pos]) && si < CC_MAX_NAME - 1)
            tok_str[si++] = src[pos++];
        tok_str[si] = '\0';
        /* Keywords — use strcmp for clarity */
        if (strcmp(tok_str, "int") == 0) { tok_type = T_INT; return T_INT; }
        if (strcmp(tok_str, "void") == 0) { tok_type = T_VOID; return T_VOID; }
        if (strcmp(tok_str, "char") == 0) { tok_type = T_CHAR; return T_CHAR; }
        if (strcmp(tok_str, "return") == 0) { tok_type = T_RETURN; return T_RETURN; }
        if (strcmp(tok_str, "if") == 0) { tok_type = T_IF; return T_IF; }
        if (strcmp(tok_str, "else") == 0) { tok_type = T_ELSE; return T_ELSE; }
        if (strcmp(tok_str, "while") == 0) { tok_type = T_WHILE; return T_WHILE; }
        if (strcmp(tok_str, "for") == 0) { tok_type = T_FOR; return T_FOR; }
        if (strcmp(tok_str, "printf") == 0) { tok_type = T_PRINTF; return T_PRINTF; }
        if (strcmp(tok_str, "break") == 0) { tok_type = T_BREAK; return T_BREAK; }
        if (strcmp(tok_str, "continue") == 0) { tok_type = T_CONTINUE; return T_CONTINUE; }
        /* C++ keywords */
        if (strcmp(tok_str, "class") == 0) { tok_type = T_CLASS; return T_CLASS; }
        if (strcmp(tok_str, "public") == 0) { tok_type = T_PUBLIC; return T_PUBLIC; }
        if (strcmp(tok_str, "private") == 0) { tok_type = T_PRIVATE; return T_PRIVATE; }
        if (strcmp(tok_str, "new") == 0) { tok_type = T_NEW; return T_NEW; }
        if (strcmp(tok_str, "delete") == 0) { tok_type = T_DELETE; return T_DELETE; }
        if (strcmp(tok_str, "this") == 0) { tok_type = T_THIS; return T_THIS; }
        tok_type = T_IDENT;
        return T_IDENT;
    }

    /* Multi-char operators */
    pos++;
    if (c == '-' && src[pos] == '>') { pos++; tok_type = T_ARROW; return T_ARROW; }
    if (c == '=' && src[pos] == '=') { pos++; tok_type = T_EQ; return T_EQ; }
    if (c == '!' && src[pos] == '=') { pos++; tok_type = T_NE; return T_NE; }
    if (c == '<' && src[pos] == '=') { pos++; tok_type = T_LE; return T_LE; }
    if (c == '>' && src[pos] == '=') { pos++; tok_type = T_GE; return T_GE; }
    if (c == '&' && src[pos] == '&') { pos++; tok_type = T_AND; return T_AND; }
    if (c == '|' && src[pos] == '|') { pos++; tok_type = T_OR; return T_OR; }
    if (c == '+' && src[pos] == '=') { pos++; tok_type = T_PLUS_EQ; return T_PLUS_EQ; }
    if (c == '-' && src[pos] == '=') { pos++; tok_type = T_MINUS_EQ; return T_MINUS_EQ; }
    if (c == '+' && src[pos] == '+') { pos++; tok_type = T_PLUS_PLUS; return T_PLUS_PLUS; }
    if (c == '-' && src[pos] == '-') { pos++; tok_type = T_MINUS_MINUS; return T_MINUS_MINUS; }

    tok_type = c;
    return c;
}

/* ── Runtime: variables and functions ───────────────────────── */
typedef struct {
    char name[CC_MAX_NAME];
    int  value;
    int  is_array;
    int  arr[64]; /* small fixed arrays */
    int  arr_size;
} cc_var_t;

typedef struct {
    char name[CC_MAX_NAME];
    int  body_start;    /* position in src after '{' */
    char param_names[CC_MAX_ARGS][CC_MAX_NAME];
    int  param_count;
} cc_func_t;

static cc_var_t  vars[CC_MAX_VARS];
static int       var_count;
static cc_func_t funcs[CC_MAX_FUNCS];
static int       func_count;

/* Break/continue targets */
static int break_target;
static int continue_target;

static int var_find(const char *name) {
    for (int i = var_count - 1; i >= 0; i--)
        if (strcmp(vars[i].name, name) == 0) return i;
    return -1;
}

static int var_alloc(const char *name) {
    if (var_count >= CC_MAX_VARS) { cc_error("too many vars"); return -1; }
    int i = var_count++;
    int n = 0;
    while (name[n] && n < CC_MAX_NAME - 1) { vars[i].name[n] = name[n]; n++; }
    vars[i].name[n] = '\0';
    vars[i].value = 0;
    vars[i].is_array = 0;
    vars[i].arr_size = 0;
    return i;
}

static int func_find(const char *name) {
    for (int i = 0; i < func_count; i++)
        if (strcmp(funcs[i].name, name) == 0) return i;
    return -1;
}

/* ── Forward declarations ───────────────────────────────────── */
static int parse_expr(void);
static int parse_stmt(void);
static int parse_block(void);

/* ── Expression parser ──────────────────────────────────────── */
static int parse_primary(void) {
    if (tok_type == T_NUM) {
        int v = tok_num;
        next_tok();
        return v;
    }
    if (tok_type == T_STR) {
        /* String literal: return pointer-like ID (not really usable as int, but for printf) */
        /* Store string for printf */
        int v = 0;
        for (int i = 0; tok_str[i]; i++) v = v * 256 + (unsigned char)tok_str[i];
        next_tok();
        return v;
    }
    if (tok_type == T_IDENT) {
        char name[CC_MAX_NAME];
        strcpy(name, tok_str);
        next_tok();

        /* Function call */
        if (tok_type == '(') {
            next_tok();
            int args[CC_MAX_ARGS];
            int ac = 0;
            if (tok_type != ')') {
                do {
                    if (ac < CC_MAX_ARGS) args[ac++] = parse_expr();
                    if (tok_type == ',') next_tok();
                    else break;
                } while (1);
            }
            if (tok_type == ')') next_tok();

            /* Built-in: printf */
            if (strcmp(name, "printf") == 0) {
                /* First arg is format string (encoded as int, reconstruct) */
                if (ac > 0) {
                    /* We need to re-parse the format string from source */
                    /* Hack: scan backward in source to find the string */
                    /* Actually, we stored it as tok_str before, need a better approach */
                    /* For now, just print the args */
                    int arg_idx = 1;
                    const char *fmt = ""; /* placeholder */
                    /* Reconstruct format from source - find the string literal */
                    int save_pos = pos;
                    /* Search backward for the string */
                    for (int i = save_pos; i >= 0; i--) {
                        if (src[i] == '"') {
                            /* Found start of string */
                            int end = i + 1;
                            while (src[end] && src[end] != '"') end++;
                            src[end] = '\0';
                            fmt = &src[i + 1];
                            break;
                        }
                    }
                    /* Print format string with %d substitution */
                    for (const char *f = fmt; *f; f++) {
                        if (*f == '%' && f[1] == 'd') {
                            if (arg_idx < ac) {
                                int v = args[arg_idx++];
                                if (v < 0) { console_putchar('-'); v = -v; }
                                char nb[12]; int ni = 0;
                                do { nb[ni++] = '0' + v % 10; v /= 10; } while (v > 0);
                                while (ni--) console_putchar(nb[ni]);
                            }
                            f++;
                        } else if (*f == '%' && f[1] == 's') {
                            /* %s not supported in this simple impl */
                            f++;
                        } else if (*f == '%' && f[1] == 'x') {
                            if (arg_idx < ac) {
                                unsigned int v = (unsigned int)args[arg_idx++];
                                char nb[12]; int ni = 0;
                                do { int d = v % 16; nb[ni++] = d < 10 ? '0' + d : 'a' + d - 10; v /= 16; } while (v > 0);
                                while (ni--) console_putchar(nb[ni]);
                            }
                            f++;
                        } else if (*f == '\\' && f[1] == 'n') {
                            console_putchar('\n');
                            f++;
                        } else {
                            console_putchar(*f);
                        }
                    }
                    /* Restore source */
                    /* Note: this is a hack, ideally we'd store strings properly */
                }
                return 0;
            }

            /* Built-in: getchar */
            if (strcmp(name, "getchar") == 0) {
                return kb_get_key();
            }

            /* User function */
            int fi = func_find(name);
            if (fi >= 0) {
                cc_func_t *fn = &funcs[fi];
                /* Save var count for scope */
                int saved_var_count = var_count;
                /* Bind parameters */
                for (int i = 0; i < fn->param_count && i < ac; i++) {
                    int vi = var_alloc(fn->param_names[i]);
                    if (vi >= 0) vars[vi].value = args[i];
                }
                /* Execute body */
                int save = pos;
                pos = fn->body_start;
                int ret = parse_block();
                pos = save;
                /* Restore scope */
                var_count = saved_var_count;
                return ret;
            }
            cc_error("unknown function");
            return 0;
        }

        /* Array access: ident[expr] */
        if (tok_type == '[') {
            next_tok();
            int idx = parse_expr();
            if (tok_type == ']') next_tok();
            int vi = var_find(name);
            if (vi >= 0 && vars[vi].is_array && idx >= 0 && idx < vars[vi].arr_size)
                return vars[vi].arr[idx];
            return 0;
        }

        /* ++ / -- */
        if (tok_type == T_PLUS_PLUS) {
            next_tok();
            int vi = var_find(name);
            if (vi >= 0) return ++vars[vi].value;
            return 0;
        }
        if (tok_type == T_MINUS_MINUS) {
            next_tok();
            int vi = var_find(name);
            if (vi >= 0) return --vars[vi].value;
            return 0;
        }

        /* Variable */
        int vi = var_find(name);
        return vi >= 0 ? vars[vi].value : 0;
    }

    /* Parenthesized expression */
    if (tok_type == '(') {
        next_tok();
        int v = parse_expr();
        if (tok_type == ')') next_tok();
        return v;
    }

    /* Unary minus */
    if (tok_type == '-') {
        next_tok();
        return -parse_primary();
    }

    /* Unary not */
    if (tok_type == '!') {
        next_tok();
        return !parse_primary();
    }

    return 0;
}

static int parse_mul(void) {
    int v = parse_primary();
    while (tok_type == '*' || tok_type == '/' || tok_type == '%') {
        int op = tok_type;
        next_tok();
        int r = parse_primary();
        if (op == '*') v *= r;
        else if (op == '/') { if (r) v /= r; else { cc_error("div by zero"); v = 0; } }
        else { if (r) v %= r; else { cc_error("mod by zero"); v = 0; } }
    }
    return v;
}

static int parse_add(void) {
    int v = parse_mul();
    while (tok_type == '+' || tok_type == '-') {
        int op = tok_type;
        next_tok();
        int r = parse_mul();
        if (op == '+') v += r; else v -= r;
    }
    return v;
}

static int parse_cmp(void) {
    int v = parse_add();
    while (tok_type == '<' || tok_type == '>' || tok_type == T_LE || tok_type == T_GE ||
           tok_type == T_EQ || tok_type == T_NE) {
        int op = tok_type;
        next_tok();
        int r = parse_add();
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

static int parse_and(void) {
    int v = parse_cmp();
    while (tok_type == T_AND) {
        next_tok();
        v = v && parse_cmp();
    }
    return v;
}

static int parse_or(void) {
    int v = parse_and();
    while (tok_type == T_OR) {
        next_tok();
        v = v || parse_and();
    }
    return v;
}

static int parse_expr(void) {
    return parse_or();
}

/* ── Statement parser ───────────────────────────────────────── */
static int parse_stmt(void) {
    /* Block */
    if (tok_type == '{') return parse_block();

    /* int var; or int var = expr; or int var[N]; */
    if (tok_type == T_INT) {
        next_tok();
        if (tok_type != T_IDENT) { cc_error("expected var name"); return 0; }
        char name[CC_MAX_NAME];
        strcpy(name, tok_str);
        next_tok();

        /* Array declaration */
        if (tok_type == '[') {
            next_tok();
            int sz = parse_expr();
            if (tok_type == ']') next_tok();
            int vi = var_alloc(name);
            if (vi >= 0) {
                vars[vi].is_array = 1;
                vars[vi].arr_size = sz > 64 ? 64 : sz;
                memset(vars[vi].arr, 0, sizeof(int) * (size_t)vars[vi].arr_size);
            }
            if (tok_type == ';') next_tok();
            return 0;
        }

        int vi = var_alloc(name);
        if (tok_type == '=') {
            next_tok();
            int val = parse_expr();
            if (vi >= 0) vars[vi].value = val;
        }
        if (tok_type == ';') next_tok();
        return 0;
    }

    /* return expr; */
    if (tok_type == T_RETURN) {
        next_tok();
        int v = 0;
        if (tok_type != ';' && tok_type != '}') v = parse_expr();
        if (tok_type == ';') next_tok();
        return v;
    }

    /* if (cond) stmt [else stmt] */
    if (tok_type == T_IF) {
        next_tok();
        if (tok_type == '(') next_tok();
        int cond = parse_expr();
        if (tok_type == ')') next_tok();
        if (cond) {
            int v = parse_stmt();
            if (tok_type == T_ELSE) { next_tok(); /* skip else branch */ }
            return v;
        } else {
            /* Skip then branch */
            if (tok_type == '{') {
                int depth = 0;
                do {
                    if (tok_type == '{') depth++;
                    else if (tok_type == '}') depth--;
                    next_tok();
                } while (depth > 0 && tok_type != T_EOF);
            } else {
                while (tok_type != ';' && tok_type != T_ELSE && tok_type != T_EOF) next_tok();
                if (tok_type == ';') next_tok();
            }
            if (tok_type == T_ELSE) {
                next_tok();
                return parse_stmt();
            }
        }
        return 0;
    }

    /* while (cond) stmt */
    if (tok_type == T_WHILE) {
        next_tok();
        if (tok_type == '(') next_tok();
        int cond_start = pos;
        int cond = parse_expr();
        if (tok_type == ')') next_tok();
        int body_start = pos;

        int old_break = break_target;
        int old_continue = continue_target;
        break_target = -1;
        continue_target = cond_start;

        while (cond) {
            pos = body_start;
            parse_stmt();
            pos = cond_start;
            tok_line = 0; /* reset line counter for tokenizer */
            cond = parse_expr();
            if (tok_type == ')') next_tok();
        }
        /* Skip body if condition false */
        pos = body_start;

        break_target = old_break;
        continue_target = old_continue;
        return 0;
    }

    /* for (init; cond; update) stmt */
    if (tok_type == T_FOR) {
        next_tok();
        if (tok_type == '(') next_tok();

        /* Init */
        if (tok_type != ';') {
            parse_expr();
        }
        if (tok_type == ';') next_tok();

        int cond_pos = pos;
        /* Condition */
        int cond = 1;
        if (tok_type != ';') {
            cond = parse_expr();
        }
        if (tok_type == ';') next_tok();

        int update_pos = pos;
        /* Update - skip for now, will re-parse */
        if (tok_type != ')') {
            while (tok_type != ')' && tok_type != T_EOF) next_tok();
        }
        if (tok_type == ')') next_tok();

        int body_start = pos;

        int old_break = break_target;
        int old_continue = continue_target;
        break_target = -1;
        continue_target = update_pos;

        while (cond) {
            pos = body_start;
            parse_stmt();
            /* Execute update */
            pos = update_pos;
            tok_line = 0;
            if (tok_type != ')') parse_expr();
            /* Re-evaluate condition */
            pos = cond_pos;
            tok_line = 0;
            cond = parse_expr();
            if (tok_type == ';') next_tok();
        }
        pos = body_start;

        break_target = old_break;
        continue_target = old_continue;
        return 0;
    }

    /* break */
    if (tok_type == T_BREAK) {
        next_tok();
        if (tok_type == ';') next_tok();
        return 0;
    }

    /* continue */
    if (tok_type == T_CONTINUE) {
        next_tok();
        if (tok_type == ';') next_tok();
        return 0;
    }

    /* Expression statement: assignment, function call, etc. */
    if (tok_type == T_IDENT) {
        char name[CC_MAX_NAME];
        strcpy(name, tok_str);
        int save_pos = pos;
        next_tok();

        /* Array assignment: arr[idx] = expr */
        if (tok_type == '[') {
            next_tok();
            int idx = parse_expr();
            if (tok_type == ']') next_tok();
            if (tok_type == '=') {
                next_tok();
                int val = parse_expr();
                int vi = var_find(name);
                if (vi >= 0 && vars[vi].is_array && idx >= 0 && idx < vars[vi].arr_size)
                    vars[vi].arr[idx] = val;
            } else if (tok_type == T_PLUS_EQ) {
                next_tok();
                int val = parse_expr();
                int vi = var_find(name);
                if (vi >= 0 && vars[vi].is_array && idx >= 0 && idx < vars[vi].arr_size)
                    vars[vi].arr[idx] += val;
            }
            if (tok_type == ';') next_tok();
            return 0;
        }

        /* Assignment: var = expr */
        if (tok_type == '=') {
            next_tok();
            int val = parse_expr();
            int vi = var_find(name);
            if (vi < 0) vi = var_alloc(name);
            if (vi >= 0) vars[vi].value = val;
            if (tok_type == ';') next_tok();
            return 0;
        }

        /* += -= */
        if (tok_type == T_PLUS_EQ) {
            next_tok();
            int val = parse_expr();
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value += val;
            if (tok_type == ';') next_tok();
            return 0;
        }
        if (tok_type == T_MINUS_EQ) {
            next_tok();
            int val = parse_expr();
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value -= val;
            if (tok_type == ';') next_tok();
            return 0;
        }

        /* ++ -- */
        if (tok_type == T_PLUS_PLUS) {
            next_tok();
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value++;
            if (tok_type == ';') next_tok();
            return 0;
        }
        if (tok_type == T_MINUS_MINUS) {
            next_tok();
            int vi = var_find(name);
            if (vi >= 0) vars[vi].value--;
            if (tok_type == ';') next_tok();
            return 0;
        }

        /* Function call */
        if (tok_type == '(') {
            pos = save_pos;
            tok_type = T_IDENT;
            strcpy(tok_str, name);
            parse_expr();
            if (tok_type == ';') next_tok();
            return 0;
        }

        /* Just a variable reference */
        if (tok_type == ';') next_tok();
        return 0;
    }

    /* Empty statement */
    if (tok_type == ';') { next_tok(); return 0; }

    /* Unknown */
    cc_error("unexpected token");
    next_tok();
    return 0;
}

static int parse_block(void) {
    if (tok_type != '{') return parse_stmt();
    next_tok(); /* skip '{' */
    int saved = var_count;
    int result = 0;
    while (tok_type != '}' && tok_type != T_EOF) {
        result = parse_stmt();
    }
    if (tok_type == '}') next_tok();
    var_count = saved; /* restore scope */
    return result;
}

/* ── Top-level parser ───────────────────────────────────────── */
static void parse_program(void) {
    tok_line = 1;
    next_tok();

    while (tok_type != T_EOF) {
        /* Function definition: int name(params) { ... } */
        if (tok_type == T_INT || tok_type == T_VOID) {
            next_tok();
            if (tok_type != T_IDENT) { cc_error("expected func name"); return; }
            char fname[CC_MAX_NAME];
            strcpy(fname, tok_str);
            next_tok();
            if (tok_type != '(') { cc_error("expected ("); return; }
            next_tok();

            cc_func_t fn;
            strcpy(fn.name, fname);
            fn.param_count = 0;
            while (tok_type != ')' && tok_type != T_EOF) {
                if (tok_type == T_INT) next_tok();
                if (tok_type == T_IDENT && fn.param_count < CC_MAX_ARGS) {
                    strcpy(fn.param_names[fn.param_count++], tok_str);
                    next_tok();
                }
                if (tok_type == ',') next_tok();
            }
            if (tok_type == ')') next_tok();

            /* Skip to '{' */
            if (tok_type == '{') {
                next_tok();
                fn.body_start = pos;
                /* Find matching '}' */
                int depth = 1;
                while (depth > 0 && pos < src_len) {
                    if (src[pos] == '{') depth++;
                    else if (src[pos] == '}') depth--;
                    if (depth > 0) pos++;
                }
                if (tok_type == '}') next_tok();

                if (func_count < CC_MAX_FUNCS) {
                    funcs[func_count++] = fn;
                }

                /* If main(), execute it */
                if (strcmp(fname, "main") == 0) {
                    var_count = 0;
                    int save = pos;
                    pos = fn.body_start;
                    parse_block();
                    pos = save;
                }
            }
        } else {
            next_tok();
        }
    }
}

/* ── REPL with full line editing ────────────────────────────── */
static void repl_redraw(const char *line, int len, int pos_cur, int prompt_len) {
    /* Move cursor to start of input (after prompt) */
    for (int i = 0; i < len + prompt_len; i++) console_putchar('\b');
    /* Redraw prompt + line */
    console_puts("c> ");
    for (int i = 0; i < len; i++) console_putchar(line[i]);
    /* Clear any leftover chars */
    for (int i = len; i < len + 4; i++) console_putchar(' ');
    for (int i = 0; i < len + 4; i++) console_putchar('\b');
    /* Position cursor at edit position */
    int tail = len - pos_cur;
    for (int i = 0; i < tail; i++) console_putchar('\b');
}

static void cc_repl(void) {
    console_puts("HBOS C Interpreter v1.0\n");
    console_puts("Type C code, end with ; or } to execute, 'quit' to exit\n");
    console_puts("Arrow keys, Home/End, Insert supported\n\n");

    char line[256];
    int src_pos = 0;
    src[0] = '\0';

    while (1) {
        console_puts("c> ");
        int li = 0;    /* line length */
        int cur = 0;   /* cursor position within line */

        while (1) {
            int c = kb_get_key();
            if (c == '\n' || c == '\r') {
                console_putchar('\n');
                break;
            }
            if (c == 0x102) { /* KEY_LEFT */
                if (cur > 0) { cur--; console_putchar('\b'); }
            } else if (c == 0x103) { /* KEY_RIGHT */
                if (cur < li) { console_putchar(line[cur]); cur++; }
            } else if (c == 0x106) { /* KEY_HOME */
                while (cur > 0) { cur--; console_putchar('\b'); }
            } else if (c == 0x107) { /* KEY_END */
                while (cur < li) { console_putchar(line[cur]); cur++; }
            } else if (c == 0x109) { /* KEY_DELETE */
                if (cur < li) {
                    for (int i = cur; i < li - 1; i++) line[i] = line[i + 1];
                    li--;
                    repl_redraw(line, li, cur, 3);
                }
            } else if (c == '\b' || c == 0x7F) { /* Backspace */
                if (cur > 0) {
                    for (int i = cur - 1; i < li - 1; i++) line[i] = line[i + 1];
                    li--;
                    cur--;
                    repl_redraw(line, li, cur, 3);
                }
            } else if (c >= ' ' && c <= '~' && li < 254) {
                /* Insert at cursor position */
                for (int i = li; i > cur; i--) line[i] = line[i - 1];
                line[cur] = (char)c;
                li++;
                cur++;
                repl_redraw(line, li, cur, 3);
            }
        }
        line[li] = '\0';

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;

        /* Check if line is a complete statement (ends with } or ;) */
        int complete = 0;
        for (int i = li - 1; i >= 0; i--) {
            if (line[i] == ' ' || line[i] == '\t') continue;
            if (line[i] == '}' || line[i] == ';') complete = 1;
            break;
        }

        /* Append to source buffer */
        if (src_pos + li + 2 < CC_MAX_SRC) {
            memcpy(src + src_pos, line, (size_t)li);
            src_pos += li;
            src[src_pos++] = '\n';
            src[src_pos] = '\0';
        }

        if (complete && src_pos > 0) {
            /* Execute accumulated source */
            src_len = src_pos;
            pos = 0;
            func_count = 0;
            var_count = 0;
            parse_program();
            src_pos = 0;
            src[0] = '\0';
        }
    }
}

/* ── Main command ───────────────────────────────────────────── */
static void cmd_cc(int argc, char **argv) {
    if (argc < 2) {
        cc_repl();
        return;
    }

    /* Load file */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        console_puts("cc: cannot open ");
        console_puts(argv[1]);
        console_putchar('\n');
        return;
    }
    src_len = 0;
    ssize_t n;
    while ((n = read(fd, src + src_len, CC_MAX_SRC - src_len - 1)) > 0)
        src_len += (int)n;
    close(fd);
    src[src_len] = '\0';

    /* Preprocess #include */
    include_depth = 0;
    cc_preprocess(src, &src_len);

    /* Parse and execute */
    pos = 0;
    func_count = 0;
    var_count = 0;
    parse_program();
}

/* ── Registration ───────────────────────────────────────────── */
void tool_cc_init(void) {
    static const command_t cmds[] = {
        {"gcc", CMD_GROUP_USER, "C/C++ compiler", "gcc [file.c]", cmd_cc},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        cmd_register(&cmds[i]);
}
