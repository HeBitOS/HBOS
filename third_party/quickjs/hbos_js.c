/* HBOS standalone quickjs runtime.
 *
 * Minimal `js` command for HBOS: evaluates a script file or an inline
 * `-e` expression with the standard `print`/`console.log` globals and
 * prints the final result or a formatted exception.  Used by the
 * compatibility smoke suite to prove the vendored quickjs engine runs at
 * ring3 on HBOS (see scripts/test_linux_compat_smoke.sh, linux_js).
 *
 * This file is HBOS-specific; everything else in third_party/quickjs is
 * upstream quickjs.  Build flags must enable real SSE2 (double math),
 * like third_party/tinycc — see the quickjs section of the Makefile.
 */
#include "quickjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void print_exception(JSContext *ctx) {
    JSValue exception = JS_GetException(ctx);
    JSValue stack, msg;
    const char *str;

    if (JS_IsObject(exception)) {
        stack = JS_GetPropertyStr(ctx, exception, "stack");
        if (JS_IsString(stack)) {
            str = JS_ToCString(ctx, stack);
            if (str) {
                fprintf(stderr, "%s\n", str);
                JS_FreeCString(ctx, str);
            }
            JS_FreeValue(ctx, stack);
            JS_FreeValue(ctx, exception);
            return;
        }
        JS_FreeValue(ctx, stack);
        msg = JS_GetPropertyStr(ctx, exception, "message");
        if (JS_IsString(msg)) {
            str = JS_ToCString(ctx, msg);
            if (str) {
                fprintf(stderr, "js: %s\n", str);
                JS_FreeCString(ctx, str);
            }
            JS_FreeValue(ctx, msg);
        } else {
            JS_FreeValue(ctx, msg);
        }
    } else {
        str = JS_ToCString(ctx, exception);
        if (str) {
            fprintf(stderr, "js: %s\n", str);
            JS_FreeCString(ctx, str);
        }
    }
    JS_FreeValue(ctx, exception);
}

static JSValue js_print_internal(JSContext *ctx, int argc, JSValueConst *argv,
                                 int to_stderr) {
    int i;
    for (i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (!str) return JS_EXCEPTION;
        if (to_stderr)
            fprintf(stderr, "%s", str);
        else
            printf("%s", str);
        JS_FreeCString(ctx, str);
        if (i < argc - 1) {
            if (to_stderr)
                fprintf(stderr, " ");
            else
                printf(" ");
        }
    }
    if (to_stderr)
        fprintf(stderr, "\n");
    else
        printf("\n");
    return JS_UNDEFINED;
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    return js_print_internal(ctx, argc, argv, 0);
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    (void)this_val;
    return js_print_internal(ctx, argc, argv, 0);
}

static JSValue js_console_error(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    return js_print_internal(ctx, argc, argv, 1);
}

static void register_globals(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console;

    JS_SetPropertyStr(ctx, global, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));
    console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

static char *read_file(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    char *buf;
    size_t cap, len = 0;

    if (fd < 0) return NULL;
    cap = 4096;
    buf = (char *)malloc(cap);
    if (!buf) {
        close(fd);
        return NULL;
    }
    for (;;) {
        ssize_t n;
        if (len + 4096 > cap) {
            char *nb = (char *)realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        n = read(fd, buf + len, 4096);
        if (n < 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    *out_len = len;
    return buf;
}

/* Built-in self-test for the smoke suite: run a battery of assertions
 * covering arithmetic, Math, strings, arrays, objects, JSON, recursion and
 * exceptions, then print the LINUX_JS PASS/FAIL marker.  Used with
 * `run js -t` so the shell's quoting quirks never reach the JS text. */
static int js_selftest(void) {
    static const char *cases[] = {
        "if (6*7 !== 42) throw Error('arith')",
        "if (Math.sqrt(16) !== 4) throw Error('math')",
        "if (Math.sin(Math.PI/2) !== 1) throw Error('sin')",
        "if (2**10 !== 1024) throw Error('pow')",
        "if ('abc'.toUpperCase() !== 'ABC') throw Error('str')",
        "if ([3,1,2].sort().join('+') !== '1+2+3') throw Error('arr')",
        "if (JSON.stringify({a:1,b:[2,3]}) !== '{\"a\":1,\"b\":[2,3]}') throw Error('json')",
        "if (JSON.parse('{\"k\":5}').k !== 5) throw Error('parse')",
        "function fib(n){return n<2?n:fib(n-1)+fib(n-2)} if (fib(15)!==610) throw Error('fib')",
        "var s=0; for(var i=0;i<1000;i++){s+=i} if (s!==499500) throw Error('loop')",
        "var t=0; try { throw 42 } catch(e) { t=e } if (t!==42) throw Error('catch')",
        "if (typeof Date.now() !== 'number') throw Error('date')",
        "if (String(3.14).indexOf('3.14') !== 0) throw Error('dtoa')",
        NULL
    };
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx;
    JSValue result;
    int i;

    if (!rt) return 1;
    ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return 1;
    }
    for (i = 0; cases[i]; i++) {
        result = JS_Eval(ctx, cases[i], strlen(cases[i]), "<selftest>",
                         JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            print_exception(ctx);
            printf("LINUX_JS: FAIL\n");
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 1;
        }
        JS_FreeValue(ctx, result);
    }
    printf("LINUX_JS: PASS\n");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}

int main(int argc, char **argv) {
    JSRuntime *rt;
    JSContext *ctx;
    JSValue result;
    const char *code = NULL;
    const char *filename = "<eval>";
    char *file_buf = NULL;
    size_t code_len = 0;
    int exit_code = 0;

    if (argc >= 2 && strcmp(argv[1], "-t") == 0)
        return js_selftest();

    if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        code = argv[2];
        code_len = strlen(code);
    } else if (argc >= 2) {
        file_buf = read_file(argv[1], &code_len);
        if (!file_buf) {
            fprintf(stderr, "js: cannot read %s\n", argv[1]);
            return 1;
        }
        code = file_buf;
        filename = argv[1];
    } else {
        fprintf(stderr, "usage: js <file.js> | js -e <code>\n");
        return 1;
    }

    rt = JS_NewRuntime();
    if (!rt) {
        fprintf(stderr, "js: cannot create runtime\n");
        free(file_buf);
        return 1;
    }
    ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "js: cannot create context\n");
        JS_FreeRuntime(rt);
        free(file_buf);
        return 1;
    }
    JS_SetRuntimeInfo(rt, "HBOS quickjs");

    register_globals(ctx);

    result = JS_Eval(ctx, code, code_len, filename, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        print_exception(ctx);
        exit_code = 1;
    } else if (JS_IsUndefined(result)) {
        JS_FreeValue(ctx, result);
    } else {
        const char *str = JS_ToCString(ctx, result);
        if (str) {
            printf("%s\n", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, result);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(file_buf);
    return exit_code;
}
