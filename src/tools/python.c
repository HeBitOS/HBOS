/**
 * @file python.c
 * @brief HBOS Python — minimal Python 3 interpreter
 *
 * Supports: int/string variables, if/elif/else, while, for-in-range,
 * def/return/break/continue, print, and graphics builtins (rect, text,
 * rgb, clear, present, get_key, wait_key, screen_w, screen_h).
 */

#include "../fcntl.h"
#include "../graphics/graphics.h"
#include "../shell/shell.h"
#include "../string.h"
#include "../unistd.h"
#include "../vfs.h"
#include "tool.h"
#include "cc.h"

/* ── Limits ─────────────────────────────────────────────────── */
#define PY_MAX_TOKENS   2048
#define PY_MAX_SRC      8192
#define PY_MAX_VARS     128
#define PY_MAX_FUNCS    32
#define PY_MAX_ARGS     8
#define PY_MAX_STRINGS  256
#define PY_STR_CAP      128
#define PY_MAX_NAME     48
#define PY_CALL_DEPTH   32

/* ── Token types ─────────────────────────────────────────────── */
enum {
    PY_EOF = 0,
    PY_NEWLINE = 1,
    PY_NUM = 300,
    PY_STR,
    PY_IDENT,
    /* keywords */
    PY_IF, PY_ELIF, PY_ELSE,
    PY_WHILE, PY_FOR, PY_IN,
    PY_DEF, PY_RETURN, PY_BREAK, PY_CONTINUE,
    PY_AND, PY_OR, PY_NOT,
    PY_TRUE, PY_FALSE, PY_NONE_KW,
    /* compound operators */
    PY_EQ, PY_NE, PY_LE, PY_GE,
    PY_PLUSEQ, PY_MINUSEQ, PY_MULEQ, PY_DIVEQ,
    PY_FLOORDIV, PY_POW,
};

typedef struct {
    int  type;
    int  ival;
    char name[PY_MAX_NAME];
    int  col;        /* column of this token */
    int  line_col;   /* indentation of its line */
    int  line;
} py_tok_t;

/* ── Value ───────────────────────────────────────────────────── */
#define PY_INT  0
#define PY_SVAL 1
#define PY_NONE 2

typedef struct { int type; int v; } py_val_t;

static const py_val_t PY_NONE_VAL = {PY_NONE, 0};
static const py_val_t PY_ZERO_VAL = {PY_INT,  0};
static const py_val_t PY_ONE_VAL  = {PY_INT,  1};

/* ── String table ────────────────────────────────────────────── */
static char py_strtab[PY_MAX_STRINGS][PY_STR_CAP];
static int  py_nstr;

static int py_str_intern(const char *s) {
    for (int i = 0; i < py_nstr; i++)
        if (strcmp(py_strtab[i], s) == 0) return i;
    if (py_nstr >= PY_MAX_STRINGS) return 0;
    int idx = py_nstr++;
    int i = 0;
    while (s[i] && i + 1 < PY_STR_CAP) { py_strtab[idx][i] = s[i]; i++; }
    py_strtab[idx][i] = 0;
    return idx;
}

static const char *py_str_get(int idx) {
    return (idx >= 0 && idx < py_nstr) ? py_strtab[idx] : "";
}

static int py_str_concat(int a, int b) {
    char buf[PY_STR_CAP * 2];
    const char *sa = py_str_get(a), *sb = py_str_get(b);
    int i = 0;
    while (sa[i] && i < PY_STR_CAP - 1) { buf[i] = sa[i]; i++; }
    int j = 0;
    while (sb[j] && i < PY_STR_CAP * 2 - 1) { buf[i++] = sb[j++]; }
    buf[i] = 0;
    return py_str_intern(buf);
}

/* ── Variables ───────────────────────────────────────────────── */
typedef struct {
    char     name[PY_MAX_NAME];
    py_val_t val;
    int      depth; /* call depth this var belongs to */
} py_var_t;

static py_var_t py_vars[PY_MAX_VARS];
static int      py_nvar;
static int      py_depth; /* current call depth */

static py_val_t py_var_get(const char *nm) {
    for (int i = py_nvar - 1; i >= 0; i--)
        if (strcmp(py_vars[i].name, nm) == 0) return py_vars[i].val;
    return PY_NONE_VAL;
}

static void py_var_set(const char *nm, py_val_t v) {
    for (int i = py_nvar - 1; i >= 0; i--) {
        if (strcmp(py_vars[i].name, nm) == 0) { py_vars[i].val = v; return; }
    }
    if (py_nvar >= PY_MAX_VARS) return;
    int idx = py_nvar++;
    int i = 0;
    while (nm[i] && i + 1 < PY_MAX_NAME) { py_vars[idx].name[i] = nm[i]; i++; }
    py_vars[idx].name[i] = 0;
    py_vars[idx].val = v;
    py_vars[idx].depth = py_depth;
}

static void py_pop_scope(int saved_nvar) {
    py_nvar = saved_nvar;
}

/* ── Functions ───────────────────────────────────────────────── */
typedef struct {
    char name[PY_MAX_NAME];
    char params[PY_MAX_ARGS][PY_MAX_NAME];
    int  nparams;
    int  body_tok;   /* index of first token in body */
    int  body_col;   /* indentation of body */
    int  def_col;    /* indentation of def line */
} py_func_t;

static py_func_t py_funcs[PY_MAX_FUNCS];
static int       py_nfunc;

static py_func_t *py_func_find(const char *nm) {
    for (int i = 0; i < py_nfunc; i++)
        if (strcmp(py_funcs[i].name, nm) == 0) return &py_funcs[i];
    return 0;
}

/* ── Tokens ──────────────────────────────────────────────────── */
static py_tok_t py_toks[PY_MAX_TOKENS];
static int      py_ntok;
static int      py_pc;

/* ── Source ──────────────────────────────────────────────────── */
static char py_src[PY_MAX_SRC];
static int  py_src_len;

/* ── Control flow ────────────────────────────────────────────── */
static int      py_g_return;
static py_val_t py_g_retval;
static int      py_g_break;
static int      py_g_cont;
static int      py_g_err;
static char     py_errmsg[96];
static int      py_errline;

/* ── GFX hooks ───────────────────────────────────────────────── */
static const cc_gfx_t *py_gfx;
void py_set_gfx(const cc_gfx_t *h) { py_gfx = h; }

/* ─────────────────────────────────────────────────────────────── */
/* Tokenizer                                                       */
/* ─────────────────────────────────────────────────────────────── */

static void py_tok_add(int type, int ival, const char *name, int col, int line_col, int line) {
    if (py_ntok >= PY_MAX_TOKENS) return;
    py_tok_t *t = &py_toks[py_ntok++];
    t->type = type;
    t->ival = ival;
    t->col  = col;
    t->line_col = line_col;
    t->line = line;
    t->name[0] = 0;
    if (name) {
        int i = 0;
        while (name[i] && i + 1 < PY_MAX_NAME) { t->name[i] = name[i]; i++; }
        t->name[i] = 0;
    }
}

static int py_kw(const char *s) {
    if (strcmp(s,"if")==0)       return PY_IF;
    if (strcmp(s,"elif")==0)     return PY_ELIF;
    if (strcmp(s,"else")==0)     return PY_ELSE;
    if (strcmp(s,"while")==0)    return PY_WHILE;
    if (strcmp(s,"for")==0)      return PY_FOR;
    if (strcmp(s,"in")==0)       return PY_IN;
    if (strcmp(s,"def")==0)      return PY_DEF;
    if (strcmp(s,"return")==0)   return PY_RETURN;
    if (strcmp(s,"break")==0)    return PY_BREAK;
    if (strcmp(s,"continue")==0) return PY_CONTINUE;
    if (strcmp(s,"and")==0)      return PY_AND;
    if (strcmp(s,"or")==0)       return PY_OR;
    if (strcmp(s,"not")==0)      return PY_NOT;
    if (strcmp(s,"True")==0)     return PY_TRUE;
    if (strcmp(s,"False")==0)    return PY_FALSE;
    if (strcmp(s,"None")==0)     return PY_NONE_KW;
    return PY_IDENT;
}

static void py_tokenize(void) {
    py_ntok = 0;
    int pos  = 0;
    int line = 1;
    while (pos < py_src_len) {
        /* count indentation */
        int line_col = 0;
        while (pos < py_src_len && (py_src[pos]==' '||py_src[pos]=='\t')) {
            line_col += (py_src[pos]=='\t') ? 4 : 1;
            pos++;
        }
        /* blank or comment line */
        if (pos >= py_src_len || py_src[pos]=='\n' || py_src[pos]=='#') {
            while (pos < py_src_len && py_src[pos]!='\n') pos++;
            if (pos < py_src_len) { pos++; line++; }
            continue;
        }
        /* tokenize rest of logical line */
        while (pos < py_src_len && py_src[pos]!='\n') {
            char c = py_src[pos];
            int  col = pos; /* approximate column */

            /* skip spaces mid-line */
            if (c==' '||c=='\t') { pos++; continue; }
            /* comment */
            if (c=='#') { while (pos<py_src_len&&py_src[pos]!='\n') pos++; break; }

            /* string literal */
            if (c=='"'||c=='\'') {
                char q = c; pos++;
                char buf[PY_STR_CAP]; int bi=0;
                while (pos<py_src_len && py_src[pos]!=q && py_src[pos]!='\n') {
                    char sc = py_src[pos++];
                    if (sc=='\\' && pos<py_src_len) {
                        sc = py_src[pos++];
                        if (sc=='n') sc='\n';
                        else if (sc=='t') sc='\t';
                        else if (sc=='\\') sc='\\';
                    }
                    if (bi+1<PY_STR_CAP) buf[bi++]=sc;
                }
                if (pos<py_src_len&&py_src[pos]==q) pos++;
                buf[bi]=0;
                py_tok_add(PY_STR, py_str_intern(buf), buf, col, line_col, line);
                continue;
            }

            /* number */
            if (c>='0'&&c<='9') {
                int v=0;
                while (pos<py_src_len&&py_src[pos]>='0'&&py_src[pos]<='9')
                    v=v*10+(py_src[pos++]-'0');
                py_tok_add(PY_NUM, v, 0, col, line_col, line);
                continue;
            }

            /* identifier / keyword */
            if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_') {
                char nm[PY_MAX_NAME]; int ni=0;
                while (pos<py_src_len && ((py_src[pos]>='a'&&py_src[pos]<='z')||
                       (py_src[pos]>='A'&&py_src[pos]<='Z')||
                       (py_src[pos]>='0'&&py_src[pos]<='9')||py_src[pos]=='_')) {
                    if (ni+1<PY_MAX_NAME) nm[ni++]=py_src[pos];
                    pos++;
                }
                nm[ni]=0;
                int kw=py_kw(nm);
                py_tok_add(kw, 0, nm, col, line_col, line);
                continue;
            }

            /* compound operators */
            if (c=='='&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_EQ,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='!'&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_NE,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='<'&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_LE,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='>'&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_GE,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='+'&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_PLUSEQ,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='-'&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_MINUSEQ,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='*'&&pos+1<py_src_len&&py_src[pos+1]=='*'){py_tok_add(PY_POW,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='*'&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_MULEQ,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='/'&&pos+1<py_src_len&&py_src[pos+1]=='/'){py_tok_add(PY_FLOORDIV,0,0,col,line_col,line);pos+=2;continue;}
            if (c=='/'&&pos+1<py_src_len&&py_src[pos+1]=='='){py_tok_add(PY_DIVEQ,0,0,col,line_col,line);pos+=2;continue;}

            /* single-char token */
            py_tok_add((int)(unsigned char)c, 0, 0, col, line_col, line);
            pos++;
        }
        py_tok_add(PY_NEWLINE, 0, 0, pos, line_col, line);
        if (pos<py_src_len&&py_src[pos]=='\n') { pos++; line++; }
    }
    py_tok_add(PY_EOF, 0, 0, pos, 0, line);
}

/* ─────────────────────────────────────────────────────────────── */
/* Token helpers                                                   */
/* ─────────────────────────────────────────────────────────────── */

static py_tok_t *py_peek(void) {
    /* skip newlines mid-expression (but not for block detection) */
    int i = py_pc;
    while (i < py_ntok && py_toks[i].type == PY_NEWLINE) i++;
    return &py_toks[i < py_ntok ? i : py_ntok-1];
}

static py_tok_t *py_cur(void) {
    return &py_toks[py_pc < py_ntok ? py_pc : py_ntok-1];
}

static py_tok_t py_eat(void) {
    while (py_pc < py_ntok && py_toks[py_pc].type == PY_NEWLINE) py_pc++;
    if (py_pc < py_ntok) return py_toks[py_pc++];
    return py_toks[py_ntok-1]; /* EOF */
}

/* skip to next newline (consume newline too) */
static void py_skip_line(void) {
    while (py_pc < py_ntok && py_toks[py_pc].type != PY_NEWLINE
           && py_toks[py_pc].type != PY_EOF)
        py_pc++;
    if (py_pc < py_ntok && py_toks[py_pc].type == PY_NEWLINE) py_pc++;
}

/* ─────────────────────────────────────────────────────────────── */
/* Error                                                           */
/* ─────────────────────────────────────────────────────────────── */

static void py_error(const char *msg) {
    if (!py_g_err) {
        py_g_err = 1;
        py_errline = py_cur()->line;
        int i = 0;
        while (msg[i] && i+1 < (int)sizeof(py_errmsg)) { py_errmsg[i]=msg[i]; i++; }
        py_errmsg[i]=0;
    }
}

/* ─────────────────────────────────────────────────────────────── */
/* Expression evaluator                                            */
/* ─────────────────────────────────────────────────────────────── */

static py_val_t py_eval_expr(void);

static int py_val_truthy(py_val_t v) {
    if (v.type == PY_INT)  return v.v != 0;
    if (v.type == PY_SVAL) return py_str_get(v.v)[0] != 0;
    return 0;
}

static int py_val_cmp(py_val_t a, py_val_t b) {
    if (a.type==PY_INT && b.type==PY_INT) return a.v - b.v;
    if (a.type==PY_SVAL && b.type==PY_SVAL) return strcmp(py_str_get(a.v), py_str_get(b.v));
    return a.v - b.v;
}

static py_val_t py_eval_args(py_val_t *args, int *nargs_out) {
    /* parse '(' arglist ')' */
    int nargs = 0;
    py_eat(); /* '(' */
    while (py_peek()->type != ')' && py_peek()->type != PY_EOF) {
        if (nargs < PY_MAX_ARGS) args[nargs++] = py_eval_expr();
        if (py_peek()->type == ',') py_eat();
        else break;
    }
    py_eat(); /* ')' */
    *nargs_out = nargs;
    return PY_NONE_VAL;
}

/* print(val,...) helper: convert val to string and emit */
static void py_print_val(py_val_t v) {
    if (v.type == PY_SVAL) {
        console_puts(py_str_get(v.v));
    } else if (v.type == PY_INT) {
        char buf[24]; int i=23; buf[i]=0;
        int iv=v.v; int neg=(iv<0);
        if (neg) iv=-iv;
        if (iv==0) buf[--i]='0';
        while (iv>0) { buf[--i]=(char)('0'+iv%10); iv/=10; }
        if (neg) buf[--i]='-';
        console_puts(buf+i);
    } else {
        console_puts("None");
    }
}

/* str(val) conversion */
static int py_val_to_str(py_val_t v) {
    if (v.type == PY_SVAL) return v.v;
    char buf[24]; int i=23; buf[i]=0;
    int iv=v.v; int neg=(iv<0);
    if (neg) iv=-iv;
    if (iv==0) buf[--i]='0';
    while (iv>0) { buf[--i]=(char)('0'+iv%10); iv/=10; }
    if (neg) buf[--i]='-';
    return py_str_intern(buf+i);
}

static py_val_t py_eval_primary(void) {
    py_tok_t *t = py_peek();

    /* integer literal */
    if (t->type == PY_NUM) {
        py_eat();
        return (py_val_t){PY_INT, t->ival};
    }
    /* string literal */
    if (t->type == PY_STR) {
        py_eat();
        return (py_val_t){PY_SVAL, t->ival};
    }
    /* True / False / None */
    if (t->type == PY_TRUE)    { py_eat(); return PY_ONE_VAL; }
    if (t->type == PY_FALSE)   { py_eat(); return PY_ZERO_VAL; }
    if (t->type == PY_NONE_KW) { py_eat(); return PY_NONE_VAL; }

    /* parenthesized expression */
    if (t->type == '(') {
        py_eat();
        py_val_t v = py_eval_expr();
        if (py_peek()->type == ')') py_eat();
        return v;
    }

    /* unary minus */
    if (t->type == '-') {
        py_eat();
        py_val_t v = py_eval_primary();
        return (py_val_t){PY_INT, -v.v};
    }

    /* not */
    if (t->type == PY_NOT) {
        py_eat();
        py_val_t v = py_eval_primary();
        return (py_val_t){PY_INT, !py_val_truthy(v)};
    }

    /* function call or variable */
    if (t->type == PY_IDENT || t->type == PY_IF   ||
        t->type == PY_WHILE || t->type == PY_FOR   ||
        t->type == PY_IN    || t->type == PY_AND   ||
        t->type == PY_OR    || t->type == PY_NOT) {
        /* identifier: peek further */
        py_eat();
        char nm[PY_MAX_NAME];
        int ni=0;
        while (t->name[ni]&&ni+1<PY_MAX_NAME) { nm[ni]=t->name[ni]; ni++; }
        nm[ni]=0;

        /* function call? */
        if (py_peek()->type == '(') {
            py_val_t args[PY_MAX_ARGS]; int nargs=0;
            py_eval_args(args, &nargs);

            /* ── builtins ── */
            if (strcmp(nm,"print")==0) {
                for (int i=0;i<nargs;i++) { if(i)console_putchar(' '); py_print_val(args[i]); }
                console_putchar('\n');
                return PY_NONE_VAL;
            }
            if (strcmp(nm,"int")==0)
                return nargs>0 ? (py_val_t){PY_INT,args[0].v} : PY_ZERO_VAL;
            if (strcmp(nm,"str")==0)
                return nargs>0 ? (py_val_t){PY_SVAL,py_val_to_str(args[0])} : PY_ZERO_VAL;
            if (strcmp(nm,"len")==0) {
                if (nargs>0&&args[0].type==PY_SVAL)
                    return (py_val_t){PY_INT,(int)strlen(py_str_get(args[0].v))};
                return PY_ZERO_VAL;
            }
            if (strcmp(nm,"abs")==0)
                return nargs>0 ? (py_val_t){PY_INT, args[0].v<0?-args[0].v:args[0].v} : PY_ZERO_VAL;

            /* ── graphics builtins ── */
            if (strcmp(nm,"rgb")==0) {
                if (nargs>=3)
                    return (py_val_t){PY_INT,(int)(
                        ((uint32_t)(args[0].v&0xFF)<<16)|
                        ((uint32_t)(args[1].v&0xFF)<<8)|
                         (uint32_t)(args[2].v&0xFF))};
                return PY_ZERO_VAL;
            }
            if (strcmp(nm,"rect")==0) {
                if (py_gfx&&py_gfx->rect&&nargs>=5)
                    py_gfx->rect(args[0].v,args[1].v,args[2].v,args[3].v,(uint32_t)args[4].v);
                return PY_NONE_VAL;
            }
            if (strcmp(nm,"text")==0) {
                if (py_gfx&&py_gfx->text&&nargs>=4) {
                    const char *s=(args[2].type==PY_SVAL)?py_str_get(args[2].v):"";
                    py_gfx->text(args[0].v,args[1].v,s,(uint32_t)args[3].v,nargs>=5?args[4].v:1);
                }
                return PY_NONE_VAL;
            }
            if (strcmp(nm,"clear")==0) {
                if (py_gfx&&py_gfx->rect&&py_gfx->screen_w&&py_gfx->screen_h) {
                    uint32_t c=nargs>=1?(uint32_t)args[0].v:0;
                    py_gfx->rect(0,0,py_gfx->screen_w(),py_gfx->screen_h(),c);
                }
                return PY_NONE_VAL;
            }
            if (strcmp(nm,"present")==0) {
                if (py_gfx&&py_gfx->present) py_gfx->present();
                return PY_NONE_VAL;
            }
            if (strcmp(nm,"screen_w")==0)
                return (py_val_t){PY_INT,(py_gfx&&py_gfx->screen_w)?py_gfx->screen_w():800};
            if (strcmp(nm,"screen_h")==0)
                return (py_val_t){PY_INT,(py_gfx&&py_gfx->screen_h)?py_gfx->screen_h():600};
            if (strcmp(nm,"get_key")==0)
                return (py_val_t){PY_INT,(py_gfx&&py_gfx->get_key)?py_gfx->get_key():0};
            if (strcmp(nm,"wait_key")==0)
                return (py_val_t){PY_INT,(py_gfx&&py_gfx->wait_key)?py_gfx->wait_key():0};

            /* ── user function ── */
            py_func_t *fn = py_func_find(nm);
            if (fn) {
                int saved_nvar = py_nvar;
                int saved_depth = py_depth;
                int saved_pc = py_pc;
                int saved_ret = py_g_return;
                py_val_t saved_retval = py_g_retval;

                py_depth++;
                for (int i=0;i<fn->nparams&&i<nargs;i++)
                    py_var_set(fn->params[i], args[i]);

                py_pc = fn->body_tok;
                py_g_return = 0;
                py_g_retval = PY_NONE_VAL;

                /* execute body block */
                while (py_pc<py_ntok && !py_g_err) {
                    while (py_pc<py_ntok && py_toks[py_pc].type==PY_NEWLINE) py_pc++;
                    if (py_pc>=py_ntok||py_toks[py_pc].type==PY_EOF) break;
                    if (py_toks[py_pc].line_col < fn->body_col) break;
                    if (py_toks[py_pc].line_col > fn->body_col) break;
                    /* execute one stmt - forward decl needed, handled below */
                    extern void py_exec_stmt(void);
                    py_exec_stmt();
                    if (py_g_return||py_g_err) break;
                }

                py_val_t ret = py_g_retval;
                py_pc = saved_pc;
                py_pop_scope(saved_nvar);
                py_depth = saved_depth;
                py_g_return = saved_ret;
                py_g_retval = saved_retval;
                return ret;
            }
            return PY_NONE_VAL;
        }

        /* plain variable reference */
        return py_var_get(nm);
    }

    py_eat(); /* skip unknown */
    return PY_NONE_VAL;
}

static py_val_t py_eval_pow(void) {
    py_val_t v = py_eval_primary();
    if (py_peek()->type==PY_POW) {
        py_eat();
        py_val_t e = py_eval_primary();
        int base=v.v, exp=e.v, res=1;
        while (exp-->0) res*=base;
        return (py_val_t){PY_INT,res};
    }
    return v;
}

static py_val_t py_eval_mul(void) {
    py_val_t v = py_eval_pow();
    for (;;) {
        int op = py_peek()->type;
        if (op!='*'&&op!='/'&&op!='%'&&op!=PY_FLOORDIV) break;
        py_eat();
        py_val_t r = py_eval_pow();
        if (op=='*') v.v *= r.v;
        else if (op=='/'||op==PY_FLOORDIV) { if(r.v) v.v/=r.v; else py_error("div by zero"); }
        else if (op=='%') { if(r.v) v.v%=r.v; else py_error("mod by zero"); }
    }
    return v;
}

static py_val_t py_eval_add(void) {
    py_val_t v = py_eval_mul();
    for (;;) {
        int op = py_peek()->type;
        if (op!='+' && op!='-') break;
        py_eat();
        py_val_t r = py_eval_mul();
        if (op=='+') {
            if (v.type==PY_SVAL||r.type==PY_SVAL)
                v=(py_val_t){PY_SVAL,py_str_concat(py_val_to_str(v),py_val_to_str(r))};
            else v.v+=r.v;
        } else v.v-=r.v;
    }
    return v;
}

static py_val_t py_eval_cmp(void) {
    py_val_t v = py_eval_add();
    for (;;) {
        int op=py_peek()->type;
        if (op!=PY_EQ&&op!=PY_NE&&op!='<'&&op!='>'&&op!=PY_LE&&op!=PY_GE) break;
        py_eat();
        py_val_t r=py_eval_add();
        int c=py_val_cmp(v,r);
        if (op==PY_EQ) v=(py_val_t){PY_INT,c==0};
        else if(op==PY_NE) v=(py_val_t){PY_INT,c!=0};
        else if(op=='<')  v=(py_val_t){PY_INT,c<0};
        else if(op=='>')  v=(py_val_t){PY_INT,c>0};
        else if(op==PY_LE) v=(py_val_t){PY_INT,c<=0};
        else if(op==PY_GE) v=(py_val_t){PY_INT,c>=0};
    }
    return v;
}

static py_val_t py_eval_not(void) {
    if (py_peek()->type==PY_NOT) {
        py_eat();
        py_val_t v=py_eval_not();
        return (py_val_t){PY_INT,!py_val_truthy(v)};
    }
    return py_eval_cmp();
}

static py_val_t py_eval_and(void) {
    py_val_t v=py_eval_not();
    while (py_peek()->type==PY_AND) {
        py_eat();
        py_val_t r=py_eval_not();
        v=(py_val_t){PY_INT,py_val_truthy(v)&&py_val_truthy(r)};
    }
    return v;
}

static py_val_t py_eval_expr(void) {
    py_val_t v=py_eval_and();
    while (py_peek()->type==PY_OR) {
        py_eat();
        py_val_t r=py_eval_and();
        v=(py_val_t){PY_INT,py_val_truthy(v)||py_val_truthy(r)};
    }
    return v;
}

/* ─────────────────────────────────────────────────────────────── */
/* Block skip (when condition is false)                            */
/* ─────────────────────────────────────────────────────────────── */

static void py_skip_block(int parent_col) {
    /* advance past any newlines to see block indent */
    while (py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE) py_pc++;
    if (py_pc>=py_ntok) return;
    int block_col=py_toks[py_pc].line_col;
    if (block_col<=parent_col) return;
    while (py_pc<py_ntok) {
        while (py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE) py_pc++;
        if (py_pc>=py_ntok||py_toks[py_pc].type==PY_EOF) break;
        if (py_toks[py_pc].line_col<=parent_col) break;
        py_skip_line();
    }
}

/* ─────────────────────────────────────────────────────────────── */
/* Statement executor                                              */
/* ─────────────────────────────────────────────────────────────── */

static void py_exec_block(int parent_col);

void py_exec_stmt(void) {
    /* skip leading newlines */
    while (py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE) py_pc++;
    if (py_pc>=py_ntok||py_toks[py_pc].type==PY_EOF) return;

    py_tok_t *t = &py_toks[py_pc];
    int stmt_col = t->line_col;

    /* ── return ── */
    if (t->type==PY_RETURN) {
        py_pc++;
        py_val_t v=PY_NONE_VAL;
        /* check if there's a value on the same line */
        if (py_pc<py_ntok && py_toks[py_pc].type!=PY_NEWLINE
            && py_toks[py_pc].type!=PY_EOF)
            v=py_eval_expr();
        py_g_return=1; py_g_retval=v;
        py_skip_line();
        return;
    }
    /* ── break ── */
    if (t->type==PY_BREAK) { py_pc++; py_g_break=1; py_skip_line(); return; }
    /* ── continue ── */
    if (t->type==PY_CONTINUE) { py_pc++; py_g_cont=1; py_skip_line(); return; }

    /* ── if / elif / else ── */
    if (t->type==PY_IF) {
        py_pc++;
        py_val_t cond=py_eval_expr();
        if (py_peek()->type==':') py_eat();
        if (py_val_truthy(cond)) {
            py_exec_block(stmt_col);
            /* skip elif/else chains */
            while (py_pc<py_ntok) {
                while (py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE) py_pc++;
                if (py_pc>=py_ntok) break;
                int nc=py_toks[py_pc].line_col;
                if (nc!=stmt_col) break;
                if (py_toks[py_pc].type==PY_ELIF||py_toks[py_pc].type==PY_ELSE) {
                    py_pc++;
                    if (py_toks[py_pc-1].type==PY_ELIF) py_eval_expr(); /* skip cond */
                    if (py_peek()->type==':') py_eat();
                    py_skip_block(stmt_col);
                } else break;
            }
        } else {
            py_skip_block(stmt_col);
            /* handle elif / else */
            while (py_pc<py_ntok&&!py_g_err) {
                while (py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE) py_pc++;
                if (py_pc>=py_ntok) break;
                int nc=py_toks[py_pc].line_col;
                if (nc!=stmt_col) break;
                if (py_toks[py_pc].type==PY_ELIF) {
                    py_pc++;
                    py_val_t c2=py_eval_expr();
                    if (py_peek()->type==':') py_eat();
                    if (py_val_truthy(c2)) {
                        py_exec_block(stmt_col);
                        /* skip rest */
                        while (py_pc<py_ntok) {
                            while(py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE)py_pc++;
                            if(py_pc>=py_ntok)break;
                            if(py_toks[py_pc].line_col!=stmt_col)break;
                            if(py_toks[py_pc].type==PY_ELIF||py_toks[py_pc].type==PY_ELSE){
                                py_pc++;
                                if(py_toks[py_pc-1].type==PY_ELIF)py_eval_expr();
                                if(py_peek()->type==':')py_eat();
                                py_skip_block(stmt_col);
                            } else break;
                        }
                        return;
                    } else { py_skip_block(stmt_col); }
                } else if (py_toks[py_pc].type==PY_ELSE) {
                    py_pc++;
                    if (py_peek()->type==':') py_eat();
                    py_exec_block(stmt_col);
                    return;
                } else break;
            }
        }
        return;
    }

    /* ── while ── */
    if (t->type==PY_WHILE) {
        py_pc++;
        int cond_tok=py_pc;
        while (!py_g_err) {
            py_pc=cond_tok;
            py_val_t cond=py_eval_expr();
            if (py_peek()->type==':') py_eat();
            if (!py_val_truthy(cond)) { py_skip_block(stmt_col); break; }
            py_exec_block(stmt_col);
            if (py_g_return||py_g_err) break;
            if (py_g_break) { py_g_break=0; break; }
            if (py_g_cont)  { py_g_cont=0; continue; }
        }
        return;
    }

    /* ── for i in range(...) ── */
    if (t->type==PY_FOR) {
        py_pc++;
        char ivar[PY_MAX_NAME]; int iv=0;
        py_tok_t *vt=&py_toks[py_pc<py_ntok?py_pc:py_ntok-1];
        while (vt->name[iv]&&iv+1<PY_MAX_NAME) { ivar[iv]=vt->name[iv]; iv++; }
        ivar[iv]=0;
        py_pc++;
        if (py_peek()->type==PY_IN) py_eat();
        /* expect range(...) */
        if (py_peek()->type==PY_IDENT&&strcmp(py_peek()->name,"range")==0) {
            py_eat();
            py_val_t rargs[3]; int nra=0;
            py_eval_args(rargs,&nra);
            int rstart=0,rstop=0,rstep=1;
            if (nra==1){rstop=rargs[0].v;}
            else if(nra==2){rstart=rargs[0].v;rstop=rargs[1].v;}
            else if(nra>=3){rstart=rargs[0].v;rstop=rargs[1].v;rstep=rargs[2].v;}
            if(rstep==0)rstep=1;
            if(py_peek()->type==':')py_eat();
            int body_tok=py_pc;
            for (int ri=rstart;rstep>0?ri<rstop:ri>rstop;ri+=rstep) {
                py_var_set(ivar,(py_val_t){PY_INT,ri});
                py_pc=body_tok;
                py_exec_block(stmt_col);
                if(py_g_return||py_g_err)break;
                if(py_g_break){py_g_break=0;break;}
                if(py_g_cont){py_g_cont=0;continue;}
            }
            /* ensure pc is past the block */
            py_pc=body_tok;
            py_skip_block(stmt_col);
        } else {
            /* skip unsupported iterable */
            py_skip_line();
            py_skip_block(stmt_col);
        }
        return;
    }

    /* ── def ── */
    if (t->type==PY_DEF) {
        py_pc++;
        char fnm[PY_MAX_NAME]; int fi=0;
        py_tok_t *nt=&py_toks[py_pc<py_ntok?py_pc:py_ntok-1];
        while (nt->name[fi]&&fi+1<PY_MAX_NAME){fnm[fi]=nt->name[fi];fi++;}
        fnm[fi]=0; py_pc++;
        /* parse params */
        if (py_peek()->type=='(') py_eat();
        char params[PY_MAX_ARGS][PY_MAX_NAME]; int nparams=0;
        while (py_peek()->type!=')' && py_peek()->type!=PY_EOF) {
            py_tok_t *pt=py_peek(); py_eat();
            int pi=0;
            while(pt->name[pi]&&pi+1<PY_MAX_NAME){params[nparams][pi]=pt->name[pi];pi++;}
            params[nparams][pi]=0; nparams++;
            if(py_peek()->type==',')py_eat(); else break;
        }
        if(py_peek()->type==')')py_eat();
        if(py_peek()->type==':')py_eat();
        py_skip_line();
        /* record body start and indent */
        while(py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE)py_pc++;
        int body_tok=py_pc;
        int body_col=(py_pc<py_ntok)?py_toks[py_pc].line_col:stmt_col+4;
        /* register */
        if (py_nfunc < PY_MAX_FUNCS) {
            py_func_t *fn=&py_funcs[py_nfunc++];
            int xi=0; while(fnm[xi]&&xi+1<PY_MAX_NAME){fn->name[xi]=fnm[xi];xi++;} fn->name[xi]=0;
            for(int pi=0;pi<nparams&&pi<PY_MAX_ARGS;pi++){
                int qi=0; while(params[pi][qi]&&qi+1<PY_MAX_NAME){fn->params[pi][qi]=params[pi][qi];qi++;}
                fn->params[pi][qi]=0;
            }
            fn->nparams=nparams;
            fn->body_tok=body_tok;
            fn->body_col=body_col;
            fn->def_col=stmt_col;
        }
        /* skip body */
        py_skip_block(stmt_col);
        return;
    }

    /* ── assignment or augmented assignment or expression ── */
    if ((t->type==PY_IDENT) && py_pc+1<py_ntok) {
        int nxt=py_toks[py_pc+1].type;
        if (nxt=='='||nxt==PY_PLUSEQ||nxt==PY_MINUSEQ||
            nxt==PY_MULEQ||nxt==PY_DIVEQ) {
            char nm[PY_MAX_NAME]; int ni=0;
            while(t->name[ni]&&ni+1<PY_MAX_NAME){nm[ni]=t->name[ni];ni++;}
            nm[ni]=0;
            py_pc+=2;
            py_val_t rhs=py_eval_expr();
            if (nxt=='=') {
                py_var_set(nm,rhs);
            } else {
                py_val_t cur=py_var_get(nm);
                if(nxt==PY_PLUSEQ)  cur.v+=rhs.v;
                else if(nxt==PY_MINUSEQ) cur.v-=rhs.v;
                else if(nxt==PY_MULEQ)  cur.v*=rhs.v;
                else if(nxt==PY_DIVEQ)  { if(rhs.v) cur.v/=rhs.v; }
                py_var_set(nm,cur);
            }
            py_skip_line();
            return;
        }
    }

    /* expression statement (e.g. function call) */
    py_eval_expr();
    py_skip_line();
}

static void py_exec_block(int parent_col) {
    /* skip newlines, determine block col */
    while (py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE) py_pc++;
    if (py_pc>=py_ntok||py_toks[py_pc].type==PY_EOF) return;
    int block_col=py_toks[py_pc].line_col;
    if (block_col<=parent_col) return;
    while (py_pc<py_ntok&&!py_g_err) {
        while (py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE) py_pc++;
        if (py_pc>=py_ntok||py_toks[py_pc].type==PY_EOF) break;
        if (py_toks[py_pc].line_col<block_col) break;
        if (py_toks[py_pc].line_col>block_col) { py_skip_line(); continue; }
        py_exec_stmt();
        if (py_g_return||py_g_break||py_g_cont||py_g_err) break;
    }
}

/* ─────────────────────────────────────────────────────────────── */
/* Public API                                                      */
/* ─────────────────────────────────────────────────────────────── */

static int py_load_file(const char *path) {
    int fd=open(path,0 /* O_RDONLY */);
    if (fd<0) return -1;
    py_src_len=0;
    ssize_t n;
    while ((n=read(fd,py_src+py_src_len,PY_MAX_SRC-py_src_len-1))>0)
        py_src_len+=(int)n;
    close(fd);
    py_src[py_src_len]=0;
    return 0;
}

int py_run_file(const char *path) {
    py_nstr=0; py_nvar=0; py_nfunc=0; py_ntok=0; py_pc=0;
    py_g_return=0; py_g_retval=PY_NONE_VAL;
    py_g_break=0; py_g_cont=0; py_g_err=0;
    py_errmsg[0]=0; py_errline=0;
    py_depth=0;

    if (py_load_file(path)<0) {
        py_g_err=1;
        py_errmsg[0]='n';py_errmsg[1]='o';py_errmsg[2]=' ';
        py_errmsg[3]='f';py_errmsg[4]='i';py_errmsg[5]='l';
        py_errmsg[6]='e';py_errmsg[7]=0;
        return -1;
    }
    py_tokenize();
    while (py_pc<py_ntok&&!py_g_err) {
        while(py_pc<py_ntok&&py_toks[py_pc].type==PY_NEWLINE)py_pc++;
        if(py_pc>=py_ntok||py_toks[py_pc].type==PY_EOF)break;
        py_exec_stmt();
        if(py_g_return||py_g_break)break;
    }
    return py_g_err ? -1 : 0;
}

const char *py_last_error(void)      { return py_errmsg; }
int         py_last_error_line(void) { return py_errline; }

/* ─────────────────────────────────────────────────────────────── */
/* Shell command                                                   */
/* ─────────────────────────────────────────────────────────────── */

static int py_write_sample(const char *path) {
    static const char sample[] =
        "# HBOS Python GUI App\n"
        "# Builtins: rect(x,y,w,h,col) text(x,y,s,col,scale)\n"
        "#           rgb(r,g,b) clear(col) present()\n"
        "#           get_key() wait_key() screen_w() screen_h()\n"
        "\n"
        "x = 40\n"
        "col = rgb(0, 180, 255)\n"
        "k = 0\n"
        "while k != 27:\n"
        "    clear(rgb(20, 25, 35))\n"
        "    rect(x, 80, 120, 80, col)\n"
        "    text(40, 40, \"Hello HBOS Python!\", rgb(255, 255, 255), 2)\n"
        "    text(40, 200, \"A/D move  ESC quit\", rgb(160, 200, 220), 1)\n"
        "    present()\n"
        "    k = wait_key()\n"
        "    if k == 100:\n"
        "        x = x + 8\n"
        "    if k == 97:\n"
        "        x = x - 8\n"
        "    if x < 0:\n"
        "        x = 0\n"
        "    if x > screen_w() - 120:\n"
        "        x = screen_w() - 120\n";
    int fd=open(path, 0x201 /* O_CREAT|O_WRONLY|O_TRUNC */);
    if (fd<0) { console_puts("python: cannot create "); console_puts(path); console_putchar('\n'); return -1; }
    int left=(int)sizeof(sample)-1, off=0;
    while(left>0){ ssize_t n=write(fd,sample+off,(size_t)left); if(n<=0)break; off+=(int)n; left-=(int)n; }
    close(fd);
    console_puts("python: created "); console_puts(path); console_putchar('\n');
    return 0;
}

static void cmd_python(int argc, char **argv) {
    if (argc<2) { console_puts("Usage: python [--new] <file.py>\n"); return; }
    int do_new=0; const char *file=0;
    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--new")==0) do_new=1;
        else file=argv[i];
    }
    if (!file) { console_puts("python: missing file\n"); return; }
    if (do_new) { py_write_sample(file); return; }

    if (py_run_file(file)<0) {
        console_puts("python: error");
        if (py_errline>0) {
            console_puts(" line ");
            char buf[8]; int i=7; buf[i]=0;
            int v=py_errline; if(!v)v=1;
            do{buf[--i]=(char)('0'+v%10);v/=10;}while(v&&i>0);
            console_puts(buf+i);
        }
        console_puts(": "); console_puts(py_errmsg[0]?py_errmsg:"unknown");
        console_putchar('\n');
    }
}

void tool_python_init(void) {
    static const command_t cmds[] = {
        {"python",  CMD_GROUP_USER, "Python interpreter",  "python [--new] <file.py>", cmd_python},
        {"python3", CMD_GROUP_USER, "Python interpreter",  "python3 [--new] <file.py>", cmd_python},
        {"py",      CMD_GROUP_USER, "Python interpreter",  "py [--new] <file.py>",     cmd_python},
    };
    for (size_t i=0;i<sizeof(cmds)/sizeof(cmds[0]);i++)
        cmd_register(&cmds[i]);
}
