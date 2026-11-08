#!/usr/bin/env python3
"""HBOS 分层依赖门禁（对标 OpenCubeOS C6「分层依赖门禁」）。

用法:
    python3 scripts/check_deps.py              # 打印违规并以退出码 1 失败
    python3 scripts/check_deps.py --json       # 机器可读输出
    python3 scripts/check_deps.py --layer core # 只看某个层的出边

设计:
  * 目录 -> 层 的映射是静态表（LAYERS），依赖方向是静态 DAG（ALLOWED）。
  * 只检查 `#include "..."`（仓库内头）。系统头 `<...>` 不参与分层。
  * `src/hbos_all.h` 是刻意的伞形头，按设计包含全部层，豁免。
  * 历史上确实需要越层的行，可以在 include 行上写
        /* deps-allow: <layer> */
    或在文件里写
        /* deps-allow: <layer> -- <原因> */
    豁免必须带原因，否则本工具拒绝豁免（防止例外清单变成垃圾场）。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, List, Optional, Tuple

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(REPO_ROOT, "src")

# ---------------------------------------------------------------------------
# 分层定义
# ---------------------------------------------------------------------------
# 路径 -> 层。用最长前缀匹配。根级 *.h 归 base，根级 *.c 归 kern。
PATH_LAYERS: List[Tuple[str, str]] = [
    ("src/user/libc", "ulibc"),          # ring3 用户态 libc
    ("src/user/progs", "progs"),        # ring3 用户程序示例
    ("src/user/crt0.c", "ulibc"),       # ring3 入口（与 libc 同层）
    ("src/user/syscall.h", "abi"),      # 系统调用编号 ABI（内核与 ring3 共用）
    ("src/user", "uruntime"),           # 内核侧用户态运行时（ldso/hax/app_runtime）
    ("src/libc", "klibc"),              # 内核侧 libc 头
    ("src/sys", "klibc"),               # 内核侧 POSIX 头
    ("src/types", "klibc"),             # 内核侧 freestanding 头
    ("src/core", "core"),               # 内核核心
    ("src/crypto", "crypto"),
    ("src/lib", "lib"),
    ("src/graphics/blob_unpack.h", "lib"),  # 通用资源编解码，不属于图形层
    ("src/graphics/blob_unpack.c", "lib"),
    ("src/graphics", "graphics"),
    ("src/input", "input"),
    ("src/net", "net"),
    ("src/boot", "boot"),
    ("src/ui", "ui"),
    ("src/app/api", "api"),
    ("src/app", "app"),
    ("src/gui", "gui"),
    ("src/shell", "shell"),
    ("src/tools", "tools"),
]

LAYER_DESC = {
    "ulibc": "ring3 用户态 libc（与内核完全隔离）",
    "progs": "ring3 用户程序示例",
    "abi": "系统调用编号 ABI（内核 <-> ring3 共用）",
    "klibc": "内核侧 libc / POSIX 头",
    "base": "根级共享头（类型 / 错误码 / 文件接口）",
    "core": "内核核心：调度 / 任务 / 内存 / 异常",
    "crypto": "加密原语",
    "lib": "内核侧通用库（json / utf8 / ext4 / posix / blob 编解码）",
    "graphics": "framebuffer / 控制台 / 字体",
    "input": "输入设备",
    "net": "网络栈",
    "boot": "引导代码",
    "ui": "HAX 控件运行时",
    "api": "GUI 服务 ABI（内核 <-> 桌面）",
    "app": "应用框架",
    "uruntime": "内核侧用户态运行时（ldso / hax / app_runtime）",
    "kern": "内核外层：驱动 / fs / syscall / elf / linux_compat",
    "gui": "桌面 / WM / 合成器 / 内置应用",
    "shell": "Shell",
    "tools": "工具层（cc / gui 等命令）",
}

# 允许的依赖方向：from -> 允许出现的 to 层集合。
ALLOWED: Dict[str, set] = {
    "ulibc": {"ulibc"},
    "progs": {"ulibc"},
    "abi": {"abi"},
    "klibc": {"klibc", "abi"},
    "base": {"base", "core", "abi", "klibc"},
    "core": {"base", "core", "abi"},
    "crypto": {"base", "core", "crypto"},
    "lib": {"base", "core", "crypto", "lib"},
    "graphics": {"base", "core", "crypto", "lib", "graphics"},
    "input": {"base", "core", "crypto", "lib", "graphics", "input"},
    "net": {"base", "core", "crypto", "lib", "net"},
    "boot": {"base", "core", "crypto", "lib", "boot"},
    "ui": {"base", "core", "lib", "graphics", "gui", "ui"},
    "api": {"base", "core", "api"},
    "app": {"base", "core", "api", "app"},
    "uruntime": {"base", "core", "crypto", "lib", "uruntime", "abi", "klibc"},
    "kern": {"base", "core", "crypto", "lib", "graphics", "input", "net", "boot",
             "uruntime", "api", "abi", "klibc", "kern"},
    "gui": {"base", "core", "crypto", "lib", "graphics", "input", "api", "ui",
            "uruntime", "abi", "gui"},
    "shell": {"base", "core", "crypto", "lib", "graphics", "input", "uruntime",
              "abi", "shell"},
    "tools": {"base", "core", "crypto", "lib", "graphics", "input", "api", "gui",
              "shell", "ui", "uruntime", "abi", "klibc", "net", "tools"},
}

# 伞形头：按设计包含所有层，豁免。
UMBRELLA_HEADERS = {"hbos_all.h"}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')
ALLOW_RE = re.compile(r"deps-allow:\s*([A-Za-z0-9_,\s]+?)\s*(?:--\s*(.*))?$")


class Violation:
    __slots__ = ("source", "line", "source_layer", "target_layer", "target", "reason")

    def __init__(self, source: str, line: int, source_layer: str,
                 target_layer: str, target: str, reason: str = ""):
        self.source = source
        self.line = line
        self.source_layer = source_layer
        self.target_layer = target_layer
        self.target = target
        self.reason = reason

    def as_dict(self) -> dict:
        return {
            "source": self.source,
            "line": self.line,
            "source_layer": self.source_layer,
            "target_layer": self.target_layer,
            "target": self.target,
            "reason": self.reason,
        }

    def __str__(self) -> str:
        note = f"  // {self.reason}" if self.reason else ""
        return (f"{self.source}:{self.line}: [{self.source_layer} -> "
                f"{self.target_layer}] #include \"{self.target}\"{note}")


def _norm(rel: str) -> str:
    return os.path.normpath(rel).replace(os.sep, "/")


def layer_of(rel_path: str) -> str:
    """把仓库相对路径映射到层名。"""
    rel = _norm(rel_path)
    if rel.startswith("src/"):
        rel = rel[4:]
    parts = rel.split("/")
    if len(parts) == 1:
        # 根级：*.h -> base，*.c -> kern
        return "base" if parts[0].endswith(".h") else "kern"
    # 先匹配更具体的目录（libc / progs / api 在 user / app 之下）
    for name, kind, _ in LAYERS:
        if kind.startswith("dir:"):
            prefix = kind[4:]
            if rel == prefix or rel.startswith(prefix + "/"):
                # user/libc 优先于 user；app/api 优先于 app
                if name == "uruntime" and (rel.startswith("user/libc/")
                                           or rel.startswith("user/progs/")):
                    continue
                if name == "app" and rel.startswith("app/api/"):
                    continue
                return name
    for name, kind, _ in LAYERS:
        if kind.startswith("glob:") and rel == kind[5:]:
            return name
    return "root"


INCLUDE_DIRS = [
    "", "core", "graphics", "shell", "tools", "gui", "user", "user/libc",
    "user/progs", "lib", "input", "crypto", "net", "boot", "app", "app/api",
    "ui", "libc",
]


def resolve_include(from_abs: str, target: str) -> Optional[str]:
    """把 #include "x.h" 解析成仓库相对路径；解析不到返回 None。"""
    if target.startswith("/"):
        return None
    from_dir = os.path.dirname(from_abs)
    cand = os.path.normpath(os.path.join(from_dir, target))
    if os.path.isfile(cand):
        return os.path.relpath(cand, REPO_ROOT)
    for d in INCLUDE_DIRS:
        cand = os.path.normpath(os.path.join(SRC_DIR, d, target))
        if os.path.isfile(cand):
            return os.path.relpath(cand, REPO_ROOT)
    return None


def collect_file_allowances(path: str) -> Dict[str, str]:
    """扫描整个文件里的 /* deps-allow: layer -- reason */ 段落级豁免。"""
    allow: Dict[str, str] = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            m = ALLOW_RE.search(raw)
            if not m:
                continue
            reason = (m.group(2) or "").strip()
            if not reason:
                # 段落级豁免也必须有原因
                continue
            for layer in m.group(1).replace(" ", ",").split(","):
                layer = layer.strip()
                if layer:
                    allow[layer] = reason
    return allow


def scan_file(abs_path: str, rel_path: str) -> List[Violation]:
    if os.path.basename(rel_path) in UMBRELLA_HEADERS:
        return []
    file_allow = collect_file_allowances(abs_path)
    src_layer = layer_of(rel_path)
    out: List[Violation] = []
    with open(abs_path, encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            m = INCLUDE_RE.match(raw)
            if not m:
                continue
            target_rel = resolve_include(abs_path, m.group(1))
            if target_rel is None:
                continue
            tgt_layer = layer_of(target_rel)
            if tgt_layer == src_layer:
                continue
            if tgt_layer in ALLOWED.get(src_layer, set()):
                continue
            # 行级豁免
            line_allow = ALLOW_RE.search(raw)
            reason = ""
            if line_allow:
                allowed_layers = {x.strip() for x in
                                  line_allow.group(1).replace(" ", ",").split(",")}
                if tgt_layer in allowed_layers:
                    reason = line_allow.group(2) or ""
                    if not reason:
                        out.append(Violation(
                            rel_path, lineno, src_layer, tgt_layer, m.group(1),
                            "豁免缺少原因，拒绝放行"))
                        continue
                    continue
            if tgt_layer in file_allow:
                continue
            out.append(Violation(rel_path, lineno, src_layer, tgt_layer,
                                 m.group(1)))
    return out


def iter_sources():
    for dirpath, dirnames, filenames in os.walk(SRC_DIR):
        dirnames[:] = sorted(d for d in dirnames if d not in (".git", "build"))
        for name in sorted(filenames):
            if not name.endswith((".c", ".h")):
                continue
            abs_path = os.path.join(dirpath, name)
            yield abs_path, os.path.relpath(abs_path, REPO_ROOT)


def main() -> int:
    ap = argparse.ArgumentParser(description="HBOS 分层依赖门禁")
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    ap.add_argument("--layer", default=None, help="只看某个层的出边违规")
    args = ap.parse_args()

    violations: List[Violation] = []
    for abs_path, rel_path in iter_sources():
        violations.extend(scan_file(abs_path, rel_path))

    if args.layer:
        violations = [v for v in violations if v.source_layer == args.layer]

    violations.sort(key=lambda v: (v.source_layer, v.source, v.line))

    if args.json:
        print(json.dumps({
            "violations": [v.as_dict() for v in violations],
            "count": len(violations),
        }, ensure_ascii=False, indent=2))
        return 1 if violations else 0

    print("HBOS 分层依赖门禁")
    print("=" * 72)
    print("层定义（低 -> 高）:")
    for name, kind, desc in LAYERS:
        marker = " *豁免" if name in ("base",) else ""
        print(f"  {name:<10} {desc}{marker}")
    print("-" * 72)
    if not violations:
        print("OK：没有发现越层依赖。")
        return 0
    print(f"发现 {len(violations)} 处越层依赖：")
    for v in violations:
        print("  " + str(v))
    print("-" * 72)
    print("修法：把共用契约下沉到更低的层（新增底层头文件），")
    print("或在 include 行写 /* deps-allow: <layer> -- <原因> */。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
