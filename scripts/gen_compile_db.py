#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 compile_commands.json，让编辑器（clangd / VSCode C/C++ / CLion）
能对 HBOS 源码做「跳转到定义」「找引用」「补全」。

为什么需要这个文件：HBOS 的编译参数（-Isrc -Isrc/gui … 以及 freestanding
那一堆坑）只写在 Makefile 里，编辑器不知道。没有编译数据库时，编辑器只能
瞎猜 include 路径，`#include "vfs.h"` 常常跳到错的同名头文件，或者直接标红。

为什么不做一个「包含全部头文件」的汇总头：**做不到，而且不该做**。内核
`src/string.h` 和用户态 `src/user/libc/string.h` 里的 `memcpy` 同名但签名
不同（`uint64_t n` vs `size_t n`），这是内核/用户态分离的设计，不是笔误。
把 173 个头文件塞进同一个翻译单元会立刻炸出 182 个 redefinition /
conflicting types。编辑器要的是「每个 .c 各自的真实编译环境」，不是把它们
混成一锅。所以正解是编译数据库，不是汇总头。

做法：
  1. 用 `make` 自己展开 C_SRCS 和 CFLAGS（别手写 Makefile 解析，会跟丢）
  2. 每个 .c 一条 command，directory 是仓库根（这样相对 -I 才成立）
  3. 头文件没有独立编译单元，给它们各挂一条「借用某个 .c 参数」的条目，
     借用谁按 .d 依赖文件里的真实包含关系中选，这样头文件也能跳转

用法：python3 scripts/gen_compile_db.py [--out compile_commands.json]
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def make_var(target_echo):
    """借 make 展开变量。比解析 Makefile 稳，因为 C_SRCS 是增量 += 出来的。

    同时把 mk/secure_net.mk 里的两套特殊编译参数一起导出来：lwip 系列和
    mbedtls 系列的文件用的是 LWIP_CFLAGS / MBEDTLS_APP_CFLAGS，不是全局
    CFLAGS。漏了它们，net6.c / secure_https.c / mbedtls_port.c 在编辑器里
    会整个标红（找不到 lwip/init.h、mbedtls/ssl.h）。
    """
    mk = os.path.join(ROOT, "build", ".dump-vars.mk")
    os.makedirs(os.path.dirname(mk), exist_ok=True)
    with open(mk, "w", encoding="utf-8") as fh:
        fh.write("include Makefile\n")
        fh.write(".PHONY: __dump\n")
        fh.write("__dump:\n")
        fh.write("\t@true\n")
        # $(info) 在 make **解析阶段**就展开并打印，不经过 shell，所以引号
        # 原样保留。它不需要也不该放在 recipe 里（放 recipe 里会对每个目标
        # 重复打印），直接写在顶层即可。
        for tag, var in (("CFLAGS", "CFLAGS"),
                         ("LWIP", "LWIP_CFLAGS"),
                         ("MBEDTLS", "MBEDTLS_APP_CFLAGS")):
            fh.write("$(info %s_BEGIN)\n" % tag)
            fh.write("$(info $(%s))\n" % var)
            fh.write("$(info %s_END)\n" % tag)
        fh.write("$(info SRCS_BEGIN)\n")
        fh.write("$(foreach f,$(C_SRCS),$(info $(f)))\n")
        fh.write("$(info SRCS_END)\n")
    env = dict(os.environ)
    env["TMPDIR"] = os.path.join(ROOT, "build", "tmp")
    os.makedirs(env["TMPDIR"], exist_ok=True)
    out = subprocess.run(
        ["make", "-s", "-f", mk, "__dump"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    if out.returncode != 0:
        sys.stderr.write(out.stdout + out.stderr)
        raise SystemExit("make 变量展开失败")
    txt = out.stdout

    def grab(tag):
        # 必须锚定整行：tag 用 re.escape，否则 "MBEDTLS" 会前缀命中
        # "MBEDTLS_APP_CFLAGS_BEGIN" 之外的东西，回退逻辑再一搅就串了。
        m = re.search(r"(?m)^" + re.escape(tag) + r"_BEGIN$(.*?)^" + re.escape(tag) + r"_END$",
                      txt, re.S)
        return m.group(1).strip() if m else ""

    srcs = re.search(r"SRCS_BEGIN\n(.*?)\nSRCS_END", txt, re.S).group(1).split()
    os.unlink(mk)
    return {"cflags": grab("CFLAGS"),
            "lwip": grab("LWIP") or grab("CFLAGS"),
            "mbedtls": grab("MBEDTLS") or grab("CFLAGS")}, srcs


def header_owner_map():
    """从 build/*.d 读出「头文件 → 包含它的 .c」的映射。

    .d 是 -MMD 顺手生成的，内容是 "<obj>: <src> <headers...>"，正是编辑器
    需要的真实依赖关系。比扫 #include 准，因为它已经过预处理器展开。

    注意 .d 里的路径有两种形态：`src/net.h` 和 `src/core/../fd.h`。后者是
    相对某个 -I 目录解析出来的，os.path.normpath 之后才是同一个文件。不归一
    化就会出现"src/fd.h 已收录，src/core/../fd.h 又收录一次"的重复条目，
    而且指向的是同一个物理文件，编辑器会困惑。
    """
    owner = {}
    build = os.path.join(ROOT, "build")
    if not os.path.isdir(build):
        return owner
    for name in os.listdir(build):
        if not name.endswith(".d"):
            continue
        try:
            with open(os.path.join(build, name), encoding="utf-8", errors="replace") as fh:
                txt = fh.read()
        except OSError:
            continue
        txt = txt.replace("\\\n", " ")
        if not txt.strip():
            continue
        head, _, deps = txt.partition(":")
        obj = head.strip()
        src = os.path.splitext(os.path.basename(obj))[0] + ".c"
        for d in deps.split():
            d = os.path.normpath(d.strip().replace("\\", "/")).replace("\\", "/")
            if d.endswith(".h"):
                owner.setdefault(d, set()).add(src)
    return owner


def all_src_headers():
    """src/ 下所有头文件（相对仓库根）。用来给没被任何 .c 包含的头兜底。

    GUI 内部头、libc 的同名成员、types/stdint.h 这类被间接包含但不出现在
    .d 首层的头，都可能不在 owner 表里。它们照样得有一条数据库条目，
    否则编辑器打开时完全没有上下文。
    """
    out = []
    for root, dirs, files in os.walk(os.path.join(ROOT, "src")):
        dirs[:] = [d for d in dirs if d not in ("build", ".git")]
        for f in files:
            if f.endswith(".h"):
                rel = os.path.relpath(os.path.join(root, f), ROOT)
                out.append(rel.replace("\\", "/"))
    return sorted(out)


def guess_owner(header_abs, srcs):
    """给没有直接包含者的头文件挑一个"最像它"的 .c，只为借用编译参数。

    规则按可靠性排序：
      1) 同目录同名 .c          —— src/net/psa_crypto_config.h → src/net/? 没有就往下
      2) 头文件剥掉 .h 后的名字正好有同名 .c（gui_draw.h → gui_draw.c）
      3) 同目录下任一 .c        —— 至少 -I 是同一套
      4) 第一个 .c 兜底
    只影响编辑器给的 include 搜索路径，不影响任何构建产物。
    """
    rel = os.path.relpath(header_abs, ROOT).replace("\\", "/")
    stem = rel[:-2] if rel.endswith(".h") else rel
    cand = stem + ".c"
    if cand in srcs or os.path.exists(os.path.join(ROOT, cand)):
        return cand
    d = os.path.dirname(rel)
    same_dir = [s for s in srcs if os.path.dirname(s) == d]
    if same_dir:
        return sorted(same_dir)[0]
    # 逐级往上找，比如 src/gui/wayland/core/x.h 找不到就用 src/gui/ 下的
    while d:
        up = [s for s in srcs if os.path.dirname(s) == d]
        if up:
            return sorted(up)[0]
        nd = os.path.dirname(d)
        if nd == d:
            break
        d = nd
    return srcs[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "compile_commands.json"))
    args = ap.parse_args()

    cflag_sets, srcs = make_var(None)
    # 编辑器的静态分析不需要这些：-MMD/-MP 生成依赖文件会污染目录，
    # -O2 会让 clangd 慢一倍且对跳转没帮助。-include 保留，mbedtls 靠它。
    drop = {"-MMD", "-MP", "-O2", "-O1", "-O3", "-Os"}

    def strip_deps(raw):
        return " ".join(t for t in raw.split() if t not in drop)

    # 按文件前缀挑参数：secure_net.mk 里这三类各自有专属 include 路径。
    def flags_for(relpath):
        base = os.path.basename(relpath)
        if base in ("net6.c",):
            return strip_deps(cflag_sets["lwip"])
        if base in ("secure_https.c", "mbedtls_port.c"):
            return strip_deps(cflag_sets["mbedtls"])
        return strip_deps(cflag_sets["cflags"])

    entries = []
    for s in srcs:
        src = os.path.join(ROOT, s) if not os.path.isabs(s) else s
        if not os.path.exists(src):
            continue
        # 参数串原样拼在命令里，dunder 引号不做任何处理
        cmd = ("gcc " + flags_for(s) + " -xc -std=gnu11"
               + " -c " + src + " -o /dev/null")
        entries.append({
            "directory": ROOT,
            "file": src,
            "command": cmd,
        })

    # 头文件：挂一个借用别人参数的条目，否则编辑器打开 .h 时没有上下文。
    owner = header_owner_map()
    by_basename = {}
    for s in srcs:
        by_basename.setdefault(os.path.basename(s), s)

    # 两张表求并集：.d 里出现过的（有真实包含者）+ src/ 下全部头（兜底）。
    # 只用 .d 会漏掉 GUI 内部头、被间接包含的 types/stdint.h、以及 libc 里
    # 那些没人直接 include 的同名成员；只用文件系统扫描又会丢掉"谁包含它"
    # 这个信息（决定借哪套编译参数）。
    hdr_set = set()
    for h in owner:
        hp = os.path.join(ROOT, h)
        if os.path.exists(hp):
            hdr_set.add(os.path.normpath(hp))
    for h in all_src_headers():
        hdr_set.add(os.path.normpath(os.path.join(ROOT, h)))

    # 同名头（src/string.h 与 src/user/libc/string.h 等 16 组）靠路径区分，
    # 这里统一用绝对规范路径做 key，避免叠成一条。
    dedup = {}
    for hdr, owners in owner.items():
        hp = os.path.normpath(os.path.join(ROOT, hdr))
        dedup.setdefault(hp, set()).update(owners)

    added = 0
    src_set = set(srcs)
    for hp in sorted(hdr_set):
        owners = dedup.get(hp, set())
        pick_rel = None
        for o in sorted(owners):
            if o in by_basename:
                pick_rel = by_basename[o]
                break
        if not pick_rel:
            # 没有直接包含者：按路径猜一个最相关的 .c，只为拿到 -I 环境。
            pick_rel = guess_owner(hp, srcs)
        # 关键：借用的 .c 必须真的参与构建。dead code（src/libc/*.c、
        # src/app/api/hal.c）自己都编不过，借它的参数会让头文件在编辑器里
        # 满屏飘红。这种情况退回一个确定能编过的 .c。
        if pick_rel not in src_set:
            pick_rel = srcs[0]
        flags = flags_for(pick_rel)
        pick = os.path.join(ROOT, pick_rel)
        cmd = ("gcc " + flags + " -xc -std=gnu11"
               + " -c " + pick + " -o /dev/null")
        entries.append({
            "directory": ROOT,
            "file": hp,
            "command": cmd,
        })
        added += 1

    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(entries, fh, ensure_ascii=False, indent=1)

    print(f"✓ {args.out}")
    print(f"  {len(entries) - added} 个 .c + {added} 个 .h = {len(entries)} 条")


if __name__ == "__main__":
    main()
