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
#include "syscall.h"

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
        } else {
            JS_FreeValue(ctx, stack);
        }
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
static char g_js_out[16384];
static size_t g_js_out_len;
static char g_js_title[256];
static char g_js_href[512] = "about:blank";
static char g_js_host[256] = "";
static char g_js_proto[16] = "";
static char g_js_path[512] = "";
static int g_js_out_file = -1;

/* 从 URL 拆出 location 部件（内核浏览器传 -u <当前地址>） */
static void split_url(const char *url) {
    g_js_host[0] = 0;
    g_js_proto[0] = 0;
    g_js_path[0] = 0;
    const char *scheme = strstr(url, "://");
    if (!scheme) return;
    size_t sl = (size_t)(scheme - url);
    if (sl + 1 < sizeof(g_js_proto)) {
        memcpy(g_js_proto, url, sl + 1);
        g_js_proto[sl + 1] = 0;
    }
    const char *host = scheme + 3;
    const char *pslash = strchr(host, '/');
    size_t hl = pslash ? (size_t)(pslash - host) : strlen(host);
    if (hl >= sizeof(g_js_host)) hl = sizeof(g_js_host) - 1;
    memcpy(g_js_host, host, hl);
    g_js_host[hl] = 0;
    if (pslash) {
        size_t pl = strlen(pslash);
        if (pl >= sizeof(g_js_path)) pl = sizeof(g_js_path) - 1;
        memcpy(g_js_path, pslash, pl);
        g_js_path[pl] = 0;
    }
}

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

/* DOM 平台（third_party/quickjs/hbos_dom.js，构建期由 tools/genjsheader.py
 * 转成字符串嵌入）：真实 DOM 树 + HTML 解析器 + 选择器 + 事件 + 渲染器。
 * register_globals 只提供 console/navigator/location/window，document 由
 * hbos_dom.js 在 JS 侧装配。 */
#include "hbos_dom_inc.h"

/* -w 输出文件的格式：第一行 document.title，第二行起是渲染结果。 */
static const char *g_page_buf = NULL;   /* -p 页面原始 HTML */
static size_t g_page_len = 0;

#define HBOS_FETCH_CAP (256U * 1024U)

static int parse_fetch_url(const char *url, char *host, size_t host_cap,
                           char *path, size_t path_cap, uint16_t *port,
                           uint32_t *flags) {
    const char *p;
    if (strncmp(url, "https://", 8) == 0) {
        p = url + 8;
        *flags = HBOS_WEB_FETCH_HTTPS;
        *port = 443;
    } else if (strncmp(url, "http://", 7) == 0) {
        p = url + 7;
        *flags = 0;
        *port = 80;
    } else {
        return -1;
    }
    const char *slash = strchr(p, '/');
    const char *end = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *q = p; q < end; q++)
        if (*q == ':') colon = q;
    size_t host_len = (size_t)((colon ? colon : end) - p);
    if (!host_len || host_len >= host_cap) return -1;
    memcpy(host, p, host_len);
    host[host_len] = 0;
    if (colon) {
        unsigned value = 0;
        for (const char *q = colon + 1; q < end; q++) {
            if (*q < '0' || *q > '9') return -1;
            value = value * 10U + (unsigned)(*q - '0');
            if (value > 65535U) return -1;
        }
        if (!value) return -1;
        *port = (uint16_t)value;
    }
    const char *src_path = slash ? slash : "/";
    size_t plen = strlen(src_path);
    if (plen >= path_cap) return -1;
    memcpy(path, src_path, plen + 1);
    return 0;
}

static JSValue js_fetch_raw(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fetch requires a URL");
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    char host[256], path[2048];
    uint16_t port;
    uint32_t flags;
    if (parse_fetch_url(url, host, sizeof(host), path, sizeof(path),
                        &port, &flags) < 0) {
        JS_FreeCString(ctx, url);
        return JS_ThrowTypeError(ctx, "fetch supports absolute http(s) URLs");
    }
    char *response = (char *)malloc(HBOS_FETCH_CAP);
    if (!response) {
        JS_FreeCString(ctx, url);
        return JS_ThrowInternalError(ctx, "fetch: out of memory");
    }
    hbos_web_fetch_request_t req = {
        .version = HBOS_WEB_FETCH_VERSION,
        .flags = flags,
        .host = host,
        .path = path,
        .output = response,
        .output_capacity = HBOS_FETCH_CAP,
        .port = port,
        .reserved = 0,
    };
    long len = __syscall1(HBOS_SYS_WEB_FETCH, (long)&req);
    JS_FreeCString(ctx, url);
    if (len < 0) {
        free(response);
        return JS_ThrowInternalError(ctx, "fetch transport failed (%ld)", len);
    }
    JSValue result = JS_NewStringLen(ctx, response, (size_t)len);
    free(response);
    return result;
}

static void register_globals(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console;
    JSValue nav, loc;

    JS_SetPropertyStr(ctx, global, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));
    console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log",
                      JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console, "error",
                      JS_NewCFunction(ctx, js_console_error, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    nav = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nav, "userAgent",
                      JS_NewString(ctx,
                                   "Mozilla/5.0 (X11; Linux x86_64) "
                                   "HBOS/0.1"));
    JS_SetPropertyStr(ctx, global, "navigator", nav);

    loc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, g_js_href));
    JS_SetPropertyStr(ctx, loc, "hostname",
                      JS_NewString(ctx, g_js_host));
    JS_SetPropertyStr(ctx, loc, "host", JS_NewString(ctx, g_js_host));
    JS_SetPropertyStr(ctx, loc, "protocol",
                      JS_NewString(ctx, g_js_proto));
    JS_SetPropertyStr(ctx, loc, "pathname",
                      JS_NewString(ctx, g_js_path));
    JS_SetPropertyStr(ctx, global, "location", loc);
    JS_SetPropertyStr(ctx, global, "__hbosFetchRaw",
                      JS_NewCFunction(ctx, js_fetch_raw,
                                      "__hbosFetchRaw", 1));

    /* window === globalThis */
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_FreeValue(ctx, global);
}

/* eval 一小段脚本并吞掉异常（返回 0 成功） */
static int eval_quiet(JSContext *ctx, const char *code, size_t len,
                      const char *file) {
    JSValue r = JS_Eval(ctx, code, len, file, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        print_exception(ctx);
        JS_FreeValue(ctx, r);
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}

/* 运行已经入队的 Promise continuation。传输当前是阻塞式内核代理，但
 * fetch() 返回标准 Promise；脚本之间清空 job 队列即可支持 then/await
 * 的短任务链。轮次上限防止页面自行制造无限微任务。 */
static void drain_jobs(JSRuntime *rt) {
    for (int i = 0; i < 64 && JS_IsJobPending(rt); i++) {
        JSContext *job_ctx = NULL;
        if (JS_ExecutePendingJob(rt, &job_ctx) < 0) {
            if (job_ctx) print_exception(job_ctx);
            break;
        }
    }
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
    /* QuickJS lexer performs sentinel look-ahead at the end of a source
     * buffer. JS_Eval also receives len, but the backing storage must still
     * have a NUL byte at buf[len]; otherwise a trailing newline can expose
     * allocator metadata as a bogus token (observed as 0x03 at line 3). */
    if (len + 1 > cap) {
        char *nb = (char *)realloc(buf, cap + 1);
        if (!nb) { free(buf); return NULL; }
        buf = nb;
    }
    buf[len] = 0;
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
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    /* DOM 平台自检：解析 → 查询 → 事件 → 渲染。selftest 的 JS 上下文
     * 与主流程分开，这里单独建一个跑 hbos_dom.js。 */
    {
        JSRuntime *drt = JS_NewRuntime();
        JSContext *dctx = drt ? JS_NewContext(drt) : NULL;
        int dom_ok = 0;
        if (dctx) {
            static const char env_code[] =
                "window = this;"
                "print = function (s) { };"
                "location = { href: 'about:blank', hostname: '', host: '',"
                "  protocol: '', pathname: '/', search: '', hash: '',"
                "  origin: 'about:blank' };";
            JSValue sr = JS_Eval(dctx, env_code, sizeof(env_code) - 1,
                                 "<dom-selftest-env>", JS_EVAL_TYPE_GLOBAL);
            JS_FreeValue(dctx, sr);
            JSValue dr = JS_Eval(dctx, hbos_dom_js, sizeof(hbos_dom_js) - 1,
                                 "<hbos-dom-selftest>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(dr)) {
                print_exception(dctx);
                JS_FreeValue(dctx, dr);
            } else {
                JS_FreeValue(dctx, dr);
            }
            const char *script =
                "var __fail = 0;"
                "function __ck(c, n) { if (!c) { __fail = 1; print('DOM-FAIL ' + n + '\\n'); } }"
                "HBOS.parsePage('<html><head><title>T1</title></head><body>"
                "<div id=\"a\"><p class=\"x\">hi</p><ul><li>1</li><li>2</li></ul>"
                "<script>var s = 1<\/script></div></body></html>');"
                "__ck(document.title === 'T1', 'title');"
                "var el = document.querySelector('#a p.x');"
                "__ck(el && el.textContent === 'hi', 'query');"
                "__ck(document.querySelectorAll('li').length === 2, 'qall');"
                "el.classList.add('y');"
                "__ck(el.className === 'x y', 'classlist');"
                "var fired = 0; document.addEventListener('DOMContentLoaded',"
                " function () { fired = 1; });"
                "HBOS.flushDeferred();"
                "__ck(fired === 1, 'event');"
                "var r = HBOS.renderPage();"
                "__ck(r.indexOf('hi') >= 0 && r.indexOf('<li>1</li>') >= 0, 'render');"
                "__ck(r.indexOf('T1') < 0, 'render-no-title');"
                "0";
            JSValue r = JS_Eval(dctx, script, strlen(script), "<dom-selftest>",
                                JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(r)) {
                print_exception(dctx);
                JS_FreeValue(dctx, r);
            } else {
                JS_FreeValue(dctx, r);
                JSValue gf = JS_GetGlobalObject(dctx);
                JSValue fv = JS_GetPropertyStr(dctx, gf, "__fail");
                int32_t fv32 = 0;
                dom_ok = JS_ToInt32(dctx, &fv32, fv) == 0 && fv32 == 0;
                JS_FreeValue(dctx, fv);
                JS_FreeValue(dctx, gf);
            }
            JS_FreeContext(dctx);
        }
        if (drt) JS_FreeRuntime(drt);
        if (!dom_ok) {
            printf("LINUX_JS: FAIL (dom)\n");
            return 1;
        }
    }

    printf("LINUX_JS: PASS\n");
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
    const char *page_path = NULL;
    int argi = 1;

    if (argc >= 2 && strcmp(argv[1], "-t") == 0)
        return js_selftest();

    if (argc >= 3 && strcmp(argv[argi], "-w") == 0) {
        out_path = argv[argi + 1];
        argi += 2;
    }

    if (argc >= argi + 2 && strcmp(argv[argi], "-p") == 0) {
        page_path = argv[argi + 1];
        argi += 2;
    }

    if (argc >= argi + 2 && strcmp(argv[argi], "-u") == 0) {
        if (strlen(argv[argi + 1]) < sizeof(g_js_href)) {
            strcpy(g_js_href, argv[argi + 1]);
            split_url(g_js_href);
        }
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

    /* DOM 平台：真实 DOM 树 + HTML 解析器 + 渲染器（hbos_dom.js） */
    if (eval_quiet(ctx, hbos_dom_js, sizeof(hbos_dom_js) - 1,
                   "<hbos-dom>") != 0) {
        fprintf(stderr, "js: DOM platform failed to load\n");
        return 1;
    }

    /* 页面原始 HTML → 解析成 document 树（脚本执行前） */
    if (page_path) {
        g_page_buf = read_file(page_path, &g_page_len);
        if (!g_page_buf) {
            fprintf(stderr, "js: cannot read page %s\n", page_path);
            return 1;
        }
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "g_page_html",
                          JS_NewStringLen(ctx, g_page_buf, g_page_len));
        JS_FreeValue(ctx, global);
        char parse_code[64];
        snprintf(parse_code, sizeof(parse_code),
                 "HBOS.parsePage(g_page_html); 0");
        if (eval_quiet(ctx, parse_code, strlen(parse_code),
                       "<hbos-parse>") != 0)
            return 1;
    }

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
            drain_jobs(rt);
            free(fbuf);
        }
    }

    if (out_path) {
        /* 浏览器语义收尾：DOMContentLoaded → 零延时回调/rAF（有轮次上限）
         * → load；然后渲染最终 DOM。 */
        if (eval_quiet(ctx, "HBOS.flushDeferred(); 0", 23, "<hbos-flush>") != 0) {
            /* 事件回调里抛异常不阻塞渲染 */
        }
        drain_jobs(rt);
        if (eval_quiet(ctx, "g_render_out = HBOS.renderPage(); 0", 33,
                       "<hbos-render>") != 0) {
            JSValue g2 = JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx, g2, "g_render_out",
                              JS_NewString(ctx, ""));
            JS_FreeValue(ctx, g2);
        }

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

        JSValue rv = JS_GetPropertyStr(ctx, global, "g_render_out");
        const char *render_str = NULL;
        if (JS_IsString(rv))
            render_str = JS_ToCString(ctx, rv);
        JS_FreeValue(ctx, global);

        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)write(fd, g_js_title, strlen(g_js_title));
            (void)write(fd, "\n", 1);
            if (render_str) {
                (void)write(fd, render_str, strlen(render_str));
                JS_FreeCString(ctx, render_str);
            }
            close(fd);
        }
        JS_FreeValue(ctx, rv);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(file_buf);
    free((void *)g_page_buf);
    return exit_code;
}
