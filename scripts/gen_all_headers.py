#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 hbos_all.h —— 一个把 src/ 下全部头文件索引进来的聚合头。

用途：编辑器里从这个文件就能看到 HBOS 全部对外符号（类型/宏/函数）的定义，
Ctrl+Click 直接跳到实现或声明处。

为什么不简单地把所有 #include 平铺一遍：

HBOS 有 16 组同名头文件，几乎全都落在同一条分界线上——**内核侧 vs 用户态侧**。
两边的同名函数签名按设计就不同：

    src/string.h            memcpy(void *, const void *, uint64_t n)
    src/user/libc/string.h  memcpy(void *, const void *, size_t n)

把它们放进同一个翻译单元必然 redefinition。这不是可以"合并"的重复代码，
而是两个编译环境。所以本头文件用互斥开关把它们分到两条分支里：

    HBOS_ALL_WITH_USER   （默认 0）1 = 编用户态那一支

默认只编内核侧，因为内核代码占绝大多数，编辑内核时看到的就是内核的名字空间。
定义 HBOS_ALL_WITH_USER 就以用户态那一支为准。

另外有两种真实冲突必须在头文件侧解决，这里逐条注释说明：
  1) `_private.h` 自带 #error，需要在 include 前定义对应的 IN_* 宏
  2) 同一侧内部签名不一致（src/unistd.h 的 select/poll 用裸 void *，
     而 src/sys/select.h 用 fd_set/struct pollfd）——以 sys/select.h 为准，
     unistd.h 里那两行重复声明跳过

用法：
    make all-headers          # 生成 src/hbos_all.h
    gcc -fsyntax-only -Isrc src/hbos_all.h   # 自检
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_decls import extract as extract_decls  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "hbos_all.h")

# 第三方源码树不进索引：跳转过去看到的是别人的命名空间，且它们靠各自的
# include 路径（lwip/src/include、mbedtls/include）解析，混进来只会坏。
SKIP_DIR = re.compile(r"(^|/)(third_party|build|\.git)(/|$)")

# net/ 下这 7 个是 lwip / mbedtls 的配置文件与 shim，必须配第三方 include
# 路径和 `-include mbedtls_port.h` 才能展开，单独编会找不到 lwip/init.h。
# 归成第三支 HBOS_ALL_WITH_NET，配合 mk/secure_net.mk 的参数使用。
NET_HEADS = [
    "src/net/arch/cc.h",
    "src/net/lwipopts.h",
    "src/net/mbedtls_config.h",
    "src/net/mbedtls_port.h",
    "src/net/mbedtls_shim/arpa/inet.h",
    "src/net/mbedtls_shim/sys/socket.h",
    "src/net/psa_crypto_config.h",
]

# 自带 #error 的私头：include 前必须先定义守护宏。
# 这不是 hack——上游 flanterm 就是这么设计的，正式用法也是如此。
GUARD_MACROS = {
    "src/fb_private.h": "FLANTERM_IN_FLANTERM",
    "src/flanterm_private.h": "FLANTERM_IN_FLANTERM",
}

# 这些头必须在别的头之前：它们的 struct 定义被后续头按值使用（不完整类型
# 会直接报错，而不是前向声明能糊过去的那种）。
PRIORITY = [
    "src/types.h",              # 基础 typedef
    "src/flanterm_private.h",   # struct flanterm_context 的定义
    "src/fb_private.h",         # 按值内嵌 struct flanterm_context
]

# 解析器会读歪、或者故意不想出现在索引里的声明。
# 键是 (头文件相对路径, 函数名)。宁可少印一条，也不要用错签名污染整个 TU。
SKIP_DECLS = {
    # src/unistd.h 里这两个是裸 void * 版本的兜底声明，真正有类型的版本在
    # src/sys/select.h。两处都印出来会 conflicting types，以 sys/select.h 为准。
    ("src/unistd.h", "select"),
    ("src/unistd.h", "poll"),
}


def order(rel_list):
    """把 PRIORITY 里的头提到最前，其余保持字典序。

    只做"提到最前"，不做完整拓扑排序：HBOS 的头文件互相 include 得比较密，
    绝大多数已经自带所需声明。真正需要强制先行的只有 PRIORITY 这几条—
    struct 按值内嵌时前向声明救不了。
    """
    head = [p for p in PRIORITY if p in rel_list]
    rest = [r for r in rel_list if r not in head]
    return head + rest


def collect():
    """收集 src/ 下全部头文件，按三支分组：内核 / 用户态 libc / 安全网络配置。

    分组**不能只看目录**。`src/user/` 下混着两种用途：

      · src/user/syscall.h / app.h / ldso.h / hax_app.h
        引的是内核侧 `../sys/types.h`，给内核内置应用和 selftest 用
      · src/user/libc/**
        独立的用户态 libc 环境，自带 syscall.h、sys/ 全套

    只有 libc 那一棵才是"另一套环境"。按 `src/user/` 前缀判会把这四个也归
    到用户态，于是它们引内核头时绕过分支隔离，撞出 useconds_t 重定义。

    net/ 下那 7 个 lwip/mbedtls 配置头单独成一支：它们必须配 lwip 和 mbedtls
    的第三方 include 路径才能展开，和内核侧一起编会找不到 lwip/init.h。
    """
    kernel, user, net = [], [], []
    net_set = set(NET_HEADS)
    for root, dirs, files in os.walk(os.path.join(ROOT, "src")):
        dirs[:] = [d for d in dirs if d not in ("build", ".git", "third_party")]
        for f in sorted(files):
            if not f.endswith(".h"):
                continue
            rel = os.path.relpath(os.path.join(root, f), ROOT).replace("\\", "/")
            if rel.endswith("hbos_all.h"):
                continue
            if SKIP_DIR.search(rel):
                continue
            if rel in net_set:
                net.append(rel)
            elif rel.startswith("src/user/libc/"):
                user.append(rel)
            else:
                kernel.append(rel)
    return order(sorted(kernel)), order(sorted(user)), sorted(net)


def _sig_key(decl):
    """把原型压成一个可比较的签名键：去掉空白差异，只留结构。

    `int  vfs_init(void);` 和 `int vfs_init(void)` 应视为同一条。
    """
    return re.sub(r"\s+", " ", decl).strip().rstrip(";").strip()


def emit_decls(sink, rel_list, root_rel=""):
    """为每个头文件输出一节：文件位置 + 该文件里每条函数原型 + 职责。

    为什么要把原型抄一遍而不是只 #include：
      只 include 的话，跳转要靠编译数据库和索引服务；一旦索引没建好或者
      编辑器不支持，就什么都跳不了。把原型连同 `文件:行号` 直接印在文本里，
      是最朴素的兜底——搜函数名就能看见它在哪个文件第几行，双击就过去了。
      代价是原型出现两份（这里和原头），但本文件是生成物，不会漂。

    位置写成 `src/bsp.h:142` 这种形态，多数编辑器（VSCode / CLion /
    JetBrains 全家 / vim+grep）都能直接识别并跳转。

    重复声明在 C 里合法（identical redeclaration），但一旦解析出的文本和
    真实声明的签名有细微出入，就会变成 `conflicting types` 硬错误。所以：
      · 先用 SKIP_DECLS 人工登记已知会被解析器读歪的条目，直接跳过
      · 再按 `名字 + 规范化签名` 去重，同一 (名字,签名) 只印一次
      · 带函数体的 static inline 只印索引行、不抄原型——抄了会切断
        定义与函数体，且 static 被复制成普通原型会被判"未定义静态函数"
    """
    seen = {}          # (name, sigkey) -> rel
    n_proto = 0
    for rel in rel_list:
        full = os.path.join(ROOT, rel)
        try:
            recs = extract_decls(full, rel)
        except Exception:
            recs = []
        funcs = recs
        inc = rel[4:] if rel.startswith("src/") else rel
        guard = GUARD_MACROS.get(rel)

        body = []
        for line_no, decl, desc, name, is_proto in funcs:
            if (rel, name) in SKIP_DECLS:
                continue
            key = (name, _sig_key(decl))
            if key in seen:
                continue
            seen[key] = rel
            where = f"{rel}:{line_no}"
            if not is_proto:
                # 带函数体的 static inline：不能抄原型（会切断定与体，
                # 且 static 复制成普通原型会被判"未定义的静态函数"）。
                # 只留索引行，让编辑器靠 include + 索引去跳。
                note = f" — {desc}" if desc else ""
                body.append(f"/* inline 定义 {name}{note}  ── 见 {where} */")
                continue
            if desc:
                body.append(f"/* {desc}  ── 见 {where} */")
            else:
                body.append(f"/* 见 {where} */")
            body.append(decl + ";")
            n_proto += 1

        # 没有任何函数声明时（纯 typedef / 宏 / 结构体头，如 types.h、
        # net/lwipopts.h），仍然要输出 #include：类型和宏才是这类头的全部
        # 价值，漏掉就等于这个头没被索引。
        if not body:
            body.append(f"/* 该头只有类型/宏，无函数声明  ── 见 {rel} */")

        sink.append("")
        sink.append(f"/* ── {rel} ──────────────────────────────── */")
        sink.append(f"/* 真实声明位于 {rel} */")
        if guard:
            # 上游用 #error 禁止直接 include，标准做法是先定义守护宏。
            # 只在还没定义时临时开，include 完再还原，避免污染后续翻译单元。
            sink.append(f"#ifndef {guard}")
            sink.append(f"#  define {guard} 1")
            sink.append(f"#  define HBOS_ALL_LOCAL_{guard} 1")
            sink.append("#endif")
        sink.append(f"#include \"{inc}\"")
        if guard:
            sink.append(f"#ifdef HBOS_ALL_LOCAL_{guard}")
            sink.append(f"#  undef {guard}")
            sink.append(f"#  undef HBOS_ALL_LOCAL_{guard}")
            sink.append("#endif")
        sink.append("")
        sink.extend(body)
    return n_proto


def main():
    kernel, user, net = collect()
    total = len(kernel) + len(user) + len(net)
    if total == 0:
        sys.exit("没找到任何头文件")

    lines = []
    lines.append("/**")
    lines.append(" * @file    hbos_all.h")
    lines.append(f" * @brief   聚合索引：src/ 下全部 {total} 个头文件的入口")
    lines.append(" *")
    lines.append(" * @layer   top")
    lines.append(" * @owner   scripts/gen_all_headers.py")
    lines.append(" * @stable  generated（自动生成，勿手改）")
    lines.append(" *")
    lines.append(" * 用途")
    lines.append(" *   **仅供编辑器建立索引**（Ctrl+Click 跳转 / 找引用 / 补全），")
    lines.append(" *   不是编译契约。任何 .c 都不应 include 本文件——它会把你")
    lines.append(" *   拉进整个内核的依赖图，编译时间翻倍且极易撞上符号冲突。")
    lines.append(" *")
    lines.append(" * 为什么分三支")
    lines.append(" *   HBOS 有 16 组同名头，几乎全部落在「内核侧 / 用户态侧」这条")
    lines.append(" *   分界线上。两侧同名函数签名按设计不同，例如：")
    lines.append(" *     src/string.h            memcpy(…, uint64_t n)")
    lines.append(" *     src/user/libc/string.h  memcpy(…, size_t n)")
    lines.append(" *   这不是可合并的重复代码，而是两个编译环境。硬塞进同一个")
    lines.append(" *   翻译单元会产生上百个 redefinition。")
    lines.append(" *")
    lines.append(" *   net/ 下的 lwip / mbedtls 配置文件是第三种情况：它们本身不冲突，")
    lines.append(" *   但必须配第三方 include 路径才能展开，所以单独一支。")
    lines.append(" *")
    lines.append(" * 怎么用")
    lines.append(" *   在编辑器里为每一支各建一个编译配置即可（或用下面的宏切换）：")
    lines.append(" *     默认                                内核侧        "
                 f"{len(kernel):3d} 个头")
    lines.append(" *     HBOS_ALL_WITH_USER=1              用户态 libc   "
                 f"{len(user):3d} 个头")
    lines.append(" *     HBOS_ALL_WITH_NET=1               安全网络配置  "
                 f"{len(net):3d} 个头")
    lines.append(" *   三支互斥，同时打开多个只有第一个生效。")
    lines.append(" *")
    lines.append(" * 编译参数（复制即用）")
    lines.append(" *   内核侧： -Isrc -Isrc/graphics -Isrc/shell -Isrc/core "
                 "-Isrc/tools -Isrc/gui -Isrc/user -Isrc/net")
    lines.append(" *   用户态： -Isrc/user/libc -Isrc/user/libc/sys -Isrc/user")
    lines.append(" *   网络：   见 mk/secure_net.mk 的 LWIP_CFLAGS / "
                 "MBEDTLS_APP_CFLAGS")
    lines.append(" *")
    lines.append(" * 索引头文件")
    lines.append(" *   宏    HBOS_ALL_WITH_USER    1 = 走用户态 libc 侧")
    lines.append(" *   宏    HBOS_ALL_WITH_NET     1 = 走安全网络配置侧")
    lines.append(f" *   宏    HBOS_ALL_KERNEL_N     {len(kernel)}（内核侧头文件数，默认支）")
    lines.append(f" *   宏    HBOS_ALL_USER_N       {len(user)}（用户态头文件数）")
    lines.append(f" *   宏    HBOS_ALL_NET_N        {len(net)}（网络配置头文件数）")
    lines.append(" */")
    lines.append("#ifndef HBOS_ALL_H")
    lines.append("#define HBOS_ALL_H 1")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("#include <stddef.h>")
    lines.append("#include <stdbool.h>")
    lines.append("")
    lines.append("#define HBOS_ALL_KERNEL_N %d" % len(kernel))
    lines.append("#define HBOS_ALL_USER_N %d" % len(user))
    lines.append("#define HBOS_ALL_NET_N %d" % len(net))
    lines.append("")
    # 三支互斥。默认走内核侧；显式给 USER/NET 才切过去，避免"同时定义多个
    # 时谁赢"这种不确定行为。
    lines.append("#if defined(HBOS_ALL_WITH_USER) && HBOS_ALL_WITH_USER")
    lines.append("/* ══ 用户态 libc 侧 ══════════════════════════════════════ */")
    n_user = emit_decls(lines, user, "")
    lines.append("")
    lines.append("#elif defined(HBOS_ALL_WITH_NET) && HBOS_ALL_WITH_NET")
    lines.append("/* ══ 安全网络配置侧（lwip / mbedtls）═════════════════════ */")
    n_net = emit_decls(lines, net, "")
    lines.append("")
    lines.append("#else")
    lines.append("/* ══ 内核侧（默认）═══════════════════════════════════════ */")
    n_kernel = emit_decls(lines, kernel, "")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* HBOS_ALL_H */")
    lines.append("")

    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    print(f"✓ {OUT}")
    print(f"  内核侧 {len(kernel)} / 用户态 {len(user)} / 网络 {len(net)}"
          f" = {total} 个头文件")
    print(f"  原型条目  内核 {n_kernel} / 用户态 {n_user} / 网络 {n_net}")


if __name__ == "__main__":
    main()
