#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从一个头文件里提取「声明 + 所在位置 + 一句话职责」。

给 gen_all_headers.py 生成函数清单用。

要点：
  · 声明可能跨行（`int vfs_resolve_path(const char *cwd, const char *path,\n
    char *out, uint32_t cap);`），要按括号配平把整条拼回来
  · 位置必须是**声明起点的行号**，这样 Ctrl+Click 落在定义处/声明处都能对
  · 职责优先取同行 `/**< */` 尾注，其次上方注释块，最后退回头文件索引段
  · 注释里可能混进 `*/`、`\\`，写进块注释前必须消毒，否则会把注释提前闭合
"""
import re

# 控制流关键字：行首是这些的不是声明
_CTRL = re.compile(r"^[ \t]*(if|for|while|switch|return|else|do|sizeof|case)\b")
# 注释、预处理、续行
_SKIP = re.compile(r"^[ \t]*(//|/\*|\*|#)")
_NAME_OPEN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(")
_TYPEISH = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*\[\]]*$")


def sanitize(text):
    """消毒：保证写进 /** */ 块注释不会把注释截断。"""
    if not text:
        return ""
    text = text.replace("*/", "*\\/")
    text = text.replace("\\", "/")
    text = text.replace("@", "(at)")
    # 行首的 * 会被当成注释修饰符
    text = re.sub(r"\n[ \t]*\*", "\n  ", text)
    return text.strip()


def _tail_desc(line):
    """取同行 `/**< ... */` 或 `// ...` 尾注。"""
    m = re.search(r"/\*\*<\s*(.*?)\s*\*/", line)
    if m:
        return m.group(1).strip()
    m = re.search(r"//+\s*(.+)$", line)
    if m:
        return m.group(1).strip()
    m = re.search(r"/\*\s*(?!<)(.*?)\s*\*/", line)
    if m:
        return m.group(1).strip()
    return ""


def _above_comment(lines, idx):
    """取声明正上方**紧邻单行**的注释。

    只在上一行就是注释时才取，不做跨行回溯——回溯很容易一路吃到上一个
    结构体字段的注释（`char name[VFS_MAX_NAME]` 那种），结果职责串味。
    抓不到就返回空，由调用方回退到索引段的描述。
    """
    j = idx - 1
    if j < 0:
        return ""
    s = lines[j].strip()
    if not s:
        return ""
    if s.startswith("//"):
        return s.lstrip("/").strip()
    if s.endswith("*/") and "/**" not in s:
        # 单行块注释 /* xxx */
        return s.replace("/*", "").replace("*/", "").strip()
    return ""


def _skip_body(lines, start, n):
    """从含 `{` 的那一行起，跳到与之配平的 `}` 之后的下一行。

    必须认字符串/字符字面量，否则 `'}'`、`"{"` 会把配平算错。HBOS 里
    有 `fmt` 风格的格式串，这种字面量不少。
    返回下一行下标；扫描到文件尾仍未配平时返回 n（调用方据此收敛）。
    """
    depth = 0
    entered = False
    i = start
    while i < n:
        s = lines[i]
        j = 0
        quote = ""
        while j < len(s):
            c = s[j]
            if quote:
                if c == "\\":
                    j += 2
                    continue
                if c == quote:
                    quote = ""
            elif c in ("'", '"'):
                quote = c
            elif c == "/" and j + 1 < len(s) and s[j + 1] == "/":
                break                    # 行注释吃掉剩下全行
            elif c == "/" and j + 1 < len(s) and s[j + 1] == "*":
                end = s.find("*/", j + 2)
                if end < 0:
                    # 块注释跨行：跳到该注释结束的那一行
                    k = i + 1
                    while k < n and "*/" not in lines[k]:
                        k += 1
                    if k >= n:
                        return n
                    i = k
                    s = lines[i]
                    j = s.find("*/") + 2
                    continue
                j = end + 2
                continue
            elif c == "{":
                depth += 1
                entered = True
            elif c == "}":
                depth -= 1
                if depth == 0 and entered:
                    return i + 1
            j += 1
        i += 1
    return n


def extract(path, relpath):
    """返回 [(行号, 声明单行文本, 职责, 名字, 是否函数原型)]。

    第 5 项为 True 表示以 `;` 收尾的纯声明（可以安全地再抄一遍）；
    为 False 表示以 `{` 开头的带函数体内联定义（只索引，不抄原型）。
    """
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")

    out = []
    i = 0
    n = len(lines)
    while i < n:
        s = lines[i]
        if _SKIP.match(s):
            i += 1
            continue
        m = _NAME_OPEN.search(s)
        if not m or _CTRL.match(s):
            i += 1
            continue
        pre = s[: m.start()]
        if not pre.strip() or not _TYPEISH.match(pre.strip()):
            i += 1
            continue
        if re.search(r"=[^=]", pre):
            i += 1
            continue

        # 配平括号，把跨行声明拼回来。
        # 一定要记下**参数表闭合处**在 body 里的字符下标：行尾可能有函数体
        # （`void cli(void) { __asm__ volatile("cli"); }`），body.rfind(")")
        # 会定位到体内的 `("cli")`，于是把体内的 `;` 当成声明结尾。
        open_col = s.index("(", m.start())
        head = s[:open_col]          # "int vfs_resolve_path"
        body = s[open_col:]          # "(const char *cwd, ..."
        depth = 0
        closed = False
        close_at = -1
        j = i
        cur = body
        while j < n:
            if j > i:
                cur = lines[j]
                body += " " + cur
            for c in cur:
                if c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                    if depth == 0:
                        closed = True
                        break
            if closed or depth < 0:
                break
            j += 1
        if not closed:
            i += 1
            continue
        # body 可能被追加过多行；按字符重新找到配平点
        depth = 0
        for ci, c in enumerate(body):
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    close_at = ci
                    break
        if close_at < 0:
            i += 1
            continue
        params = body[: close_at + 1]

        # 参数表闭合之后必须是 ; 或 {，否则是函数调用/宏不是声明
        after = body[close_at + 1:]
        k = j
        while not after.strip() and k + 1 < n:
            k += 1
            after = lines[k]
        if not re.match(r"^[ \t]*[;{]", after):
            i = j + 1
            continue

        # `;` 才是原型；`{` 是带函数体的内联定义（可能是单行体）。
        # 必须看**第一个**非空白字符——`void cli(void){ f(); }` 的 body 里
        # 也有 `;`，但那是函数体内部的语句，不是声明结尾。
        # 内联定义绝不能再抄一遍原型：会切断定义与体的关系，且 static
        # 复制成普通原型会被判"未定义的静态函数"。
        is_proto = after.lstrip().startswith(";")

        # decl 只取到参数表闭合处——后面可能是函数体
        decl = (head + params).strip()
        decl = re.sub(r"\s+", " ", decl)

        name = m.group(1)
        desc = _tail_desc(s)
        if not desc:
            desc = _above_comment(lines, i)
        out.append((i + 1, decl, sanitize(desc), name, is_proto))

        if is_proto:
            i = j + 1
        else:
            # 跳过整个函数体。不跳的话，体里的 `__asm__ volatile("cli")`
            # 会被当成一条声明抄出来，生成物直接编不过。
            # 起始行取 k：`{` 可能与签名同行（`... (void) { __asm__ ...; }`），
            # 而 body 只拼到 `)` 就停了，从 j 起扫会漏掉同行那个 `{`。
            i = _skip_body(lines, k, n)
            if i <= j:
                i = j + 1

    return out


if __name__ == "__main__":
    import sys
    for rec in extract(sys.argv[1], sys.argv[1]):
        print(rec)
