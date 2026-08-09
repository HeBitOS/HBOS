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

/* Captured script output: console.log/document.write text plus the final
 * document.title, written to the -w file (and the console).  The browser
 * backend reads the file back to render script results into the page. */
static char g_js_out[8192];
static size_t g_js_out_len;
static char g_js_title[256];
static char g_js_href[512] = "about:blank";
static int g_js_out_file = -1;

static void capture_out(const char *s, size_t n) {
    if (!s || n == 0) return;
    if (g_js_out_len + n < sizeof(g_js_out)) {
        memcpy(g_js_out + g_js_out_len, s, n);
        g_js_out_len += n;
    }
    if (g_js_out_file >= 0)
        (void)write(g_js_out_file, s, n);
    (void)write(1, s, n);
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
            capture_out(str, strlen(str));
        JS_FreeCString(ctx, str);
        if (i < argc - 1) {
            if (to_stderr)
                fprintf(stderr, " ");
            else
                capture_out(" ", 1);
        }
    }
    if (to_stderr)
        fprintf(stderr, "\n");
    else
        capture_out("\n", 1);
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

/* DOM stubs so real-world inline scripts (e.g. bilibili's boot scripts)
 * parse and run instead of throwing: no DOM tree exists in v1, so
 * querySelector returns null and navigator/location carry fixed values. */
static JSValue js_document_querySelector(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NULL;
}

static JSValue js_document_getElementById(JSContext *ctx,
                                          JSValueConst this_val, int argc,
                                          JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NULL;
}

static JSValue js_document_createElement(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NULL;
}

static JSValue js_document_write(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            capture_out(str, strlen(str));
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

static const char dom_stub_js[] =
    "var _hbos_el = function(){ return {"
    " innerHTML:'', textContent:'', value:'', className:'', id:'',"
    " style:{}, dataset:{}, classList:{add:function(){},"
    "  remove:function(){}, contains:function(){return false},"
    "  toggle:function(){}},"
    " setAttribute:function(){}, getAttribute:function(){return null},"
    " removeAttribute:function(){},"
    " appendChild:function(){return arguments[0]},"
    " removeChild:function(){}, insertBefore:function(){},"
    " replaceChild:function(){}, cloneNode:function(){return _hbos_el()},"
    " addEventListener:function(){}, removeEventListener:function(){},"
    " dispatchEvent:function(){return false},"
    " querySelector:function(){return _hbos_el()},"
    " querySelectorAll:function(){return []},"
    " getElementsByTagName:function(){return []},"
    " getBoundingClientRect:function(){return {top:0,left:0,width:0,"
    "  height:0,right:0,bottom:0}},"
    " contains:function(){return false}, focus:function(){},"
    " blur:function(){}, click:function(){},"
    " parentNode:null, parentElement:null, firstChild:null,"
    " lastChild:null, nextSibling:null, previousSibling:null,"
    " children:[], childNodes:[], nodeType:1, nodeName:'DIV',"
    " tagName:'DIV', offsetWidth:0, offsetHeight:0,"
    " scrollIntoView:function(){}, remove:function(){}"
    " }; };"
    "document.querySelector=function(){return _hbos_el()};"
    "document.querySelectorAll=function(){return []};"
    "document.getElementById=function(){return _hbos_el()};"
    "document.getElementsByTagName=function(){return []};"
    "document.getElementsByClassName=function(){return []};"
    "document.createElement=function(){return _hbos_el()};"
    "document.createTextNode=function(){return {nodeType:3,"
    " textContent:arguments[0]||''}};"
    "document.body=_hbos_el();document.documentElement=_hbos_el();"
    "document.head=_hbos_el();"
    "document.addEventListener=function(){};"
    "document.removeEventListener=function(){};"
    "document.readyState='complete';"
    "document.documentURI=window.location.href;"
    "document.referrer='';"
    "window.addEventListener=function(){};"
    "window.removeEventListener=function(){};"
    "window.setTimeout=function(fn){return 0};"
    "window.clearTimeout=function(){};"
    "window.setInterval=function(){return 0};"
    "window.clearInterval=function(){};"
    "window.requestAnimationFrame=function(fn){return 0};"
    "window.getComputedStyle=function(){return {display:'block',"
    " visibility:'visible', opacity:'1', color:'rgb(0, 0, 0)',"
    " backgroundColor:'rgba(0, 0, 0, 0)', position:'static',"
    " width:'0px', height:'0px'}};"
    "window.scrollTo=function(){};window.scrollBy=function(){};"
    "window.alert=function(){};window.confirm=function(){return false};"
    "window.prompt=function(){return null};"
    "window.open=function(){return null};"
    "window.fetch=function(){return Promise.reject(new Error("
    " 'fetch: not implemented'))};"
    "window.XMLHttpRequest=function(){};"
    "window.console=console;";

static void register_globals(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console;
    JSValue doc;

    JS_SetPropertyStr(ctx, global, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));
    console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    /* Minimal document object: title (plain property; read back after the
     * script runs and reported through the -w file) and write() (appends
     * to the captured output). */
    JSValue nav, loc;

    doc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, doc, "title", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, doc, "write",
                      JS_NewCFunction(ctx, js_document_write, "write", 1));
    JS_SetPropertyStr(ctx, doc, "querySelector",
                      JS_NewCFunction(ctx, js_document_querySelector,
                                      "querySelector", 1));
    JS_SetPropertyStr(ctx, doc, "getElementById",
                      JS_NewCFunction(ctx, js_document_getElementById,
                                      "getElementById", 1));
    JS_SetPropertyStr(ctx, doc, "createElement",
                      JS_NewCFunction(ctx, js_document_createElement,
                                      "createElement", 1));
    /* Plain properties instead of getter/setter pairs: JS_DefineProperty
     * with JS_CFUNC_getter/setter does not bind reliably in this quickjs
     * build (the assignment silently becomes a plain property and the
     * runtime leaks the function objects).  Fixed values are fine for the
     * v1 DOM stub surface. */
    JS_SetPropertyStr(ctx, doc, "cookie", JS_NewString(ctx, ""));
    JS_SetPropertyStr(ctx, global, "document", doc);

    nav = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "userAgent",
                      JS_NewString(ctx,
                                   "Mozilla/5.0 (X11; Linux x86_64) "
                                   "HBOS/0.1"));
    JS_SetPropertyStr(ctx, global, "navigator", nav);

    loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, g_js_href));
    JS_SetPropertyStr(ctx, global, "location", loc);

    /* window === globalThis */
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_FreeValue(ctx, global);

    /* Lazy DOM element stub: every property/method that real pages touch
     * resolves to a harmless value so scripts run to completion instead of
     * throwing (v1 has no DOM tree to operate on).  Defined in JS — far
     * shorter than the equivalent C object construction. */
    JSValue stub_ret = JS_Eval(ctx, dom_stub_js,
                               sizeof(dom_stub_js) - 1, "<dom-stub>",
                               JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(stub_ret)) {
        JSValue e = JS_GetException(ctx);
        const char *estr = JS_ToCString(ctx, e);
        fprintf(stderr, "dom-stub eval error: %s\n",
                estr ? estr : "?");
        if (estr) JS_FreeCString(ctx, estr);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, stub_ret);
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

    const char *out_path = NULL;
    int argi = 1;

    if (argc >= 2 && strcmp(argv[1], "-t") == 0)
        return js_selftest();

    if (argc >= 3 && strcmp(argv[argi], "-w") == 0) {
        out_path = argv[argi + 1];
        argi += 2;
    }

    if (argc >= argi + 2 && strcmp(argv[argi], "-e") == 0) {
        code = argv[argi + 1];
        code_len = strlen(code);
    } else if (argc >= argi + 1) {
        file_buf = NULL;   /* multi-file mode: each file evals in its own
                            * unit so a SyntaxError in one (e.g. Vite
                            * import.meta chunks) never blocks the rest */
    } else {
        fprintf(stderr, "usage: js <file.js>... | js -e <code>\n");
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

    if (code) {
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
    } else {
        int f;
        for (f = argi; f < argc; f++) {
            size_t flen;
            char *fbuf = read_file(argv[f], &flen);
            if (!fbuf) {
                fprintf(stderr, "js: cannot read %s\n", argv[f]);
                exit_code = 1;
                continue;
            }
            result = JS_Eval(ctx, fbuf, flen, argv[f], JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(result)) {
                print_exception(ctx);
                exit_code = 1;
            } else {
                JS_FreeValue(ctx, result);
            }
            free(fbuf);
        }
    }

    if (out_path) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue docv = JS_GetPropertyStr(ctx, global, "document");
        JSValue tv = JS_GetPropertyStr(ctx, docv, "title");
        if (JS_IsString(tv)) {
            const char *ts = JS_ToCString(ctx, tv);
            if (ts) {
                size_t n = strlen(ts);
                if (n >= sizeof(g_js_title)) n = sizeof(g_js_title) - 1;
                memcpy(g_js_title, ts, n);
                g_js_title[n] = 0;
                JS_FreeCString(ctx, ts);
            }
        }
        JS_FreeValue(ctx, tv);
        JS_FreeValue(ctx, docv);
        JS_FreeValue(ctx, global);

        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)write(fd, g_js_title, strlen(g_js_title));
            (void)write(fd, "\n", 1);
            (void)write(fd, g_js_out, g_js_out_len);
            close(fd);
        }
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(file_buf);
    return exit_code;
}
