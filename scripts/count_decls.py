#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""数一个头文件里导出的函数声明个数。

给 scripts/check_headers.sh 的第 5 项（@brief 计数核对）用。

为什么不写在 shell 里：初版用 `grep -E '^[A-Za-z_].*\\);'` 数，结果
`static inline int foo(\n  ...\n) {` 这种**跨行且以 { 收尾**的声明全被漏掉，
libc 头（pthread/stdio/socket）直接数成 0。反过来只用括号配平又会把结构体里
的函数指针成员算进去，xhci.h 从 13 涨到 18。两个坑都要处理：

  · 跨行：左括号之后的括号要一路配平到闭合
  · 收尾：闭合右括号之后必须紧跟 `;` 或 `{`，这才是一次声明/定义
  · 排除：行首 if/for/while/switch/return 等控制流；`=` 出现在名字之前（赋值）
  · 排除：名字之前必须至少有一个"类型词"，纯 `foo(` 不算声明

用法：count_decls.py <file.h>  →  stdout 打印个数
"""
import re
import sys

# 行首这些关键字开头的 `(` 不是声明
_CTRL = re.compile(r"^[ \t]*(if|for|while|switch|return|else|do|sizeof)[ \t]*\(")
# 注释、预处理指令直接跳过
_SKIP = re.compile(r"^[ \t]*(//|/\*|\*|#)")
# 名字 + 左括号
_NAME_OPEN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(")
# 名字之前允许出现的东西：类型名、修饰词、指针星号、数组方括号
_TYPEISH = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*\[\]]*$")


def count_decls(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")

    total = 0
    for i, s in enumerate(lines):
        if _SKIP.match(s):
            continue
        m = _NAME_OPEN.search(s)
        if not m:
            continue
        if _CTRL.match(s):
            continue

        pre = s[: m.start()]
        if not pre.strip():
            continue
        if re.search(r"=[^=]", pre):      # 赋值语句里的调用
            continue
        if not _TYPEISH.match(pre.strip()):
            continue

        # 从第一个左括号开始配平，允许跨行
        depth = 0
        closed = False
        j = i
        t = s[s.index("(", m.start()):]
        while j < len(lines):
            for c in t:
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
            if j < len(lines):
                t = lines[j]
        if not closed:
            continue

        # 闭合右括号之后必须立刻跟 ; 或 {（可能在同一行，也可能在后续行）
        rest = t[t.find(")") + 1:]
        k = j
        while not rest.strip() and k + 1 < len(lines):
            k += 1
            rest = lines[k]
        if not re.match(r"^[ \t]*[;{]", rest):
            continue

        total += 1
    return total


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.stderr.write("用法: count_decls.py <file.h>\n")
        sys.exit(2)
    print(count_decls(sys.argv[1]))
