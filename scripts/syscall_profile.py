#!/usr/bin/env python3
"""syscall 热点排行：把串口日志里的 syscall 数据聚合成"按 nr 的次数/占比"。

为什么需要这个脚本
==================
在它之前，想知道"哪个系统调用最热"只有两条路：

  1. `setenv tracepid <pid>` 逐条打印 —— 串口是逐字符模拟的，逐条打印本身
     就会把被观测的程序拖慢一个数量级，而且打完是一大坨文本，得自己数。
  2. 读 `--- last linux syscalls ---` 的环形缓冲 dump —— 那是**快照**，
     一次崩溃会 dump 好几份，直接全文件求和会把同一段调用重复计 N 次。

本脚本解决第二类数据的聚合，同时也能读内核里新增的 `sysprof` 聚合表
（`setenv sysprof 1` 采集，`sysprof` 出表，含次数与累计 tsc）。

两种数据源的区别（重要）
========================
  * dump 快照（`  pid=N nr=M ret=R a0=.. a1=..`）：**只有次数，没有耗时**。
    所以这一路的排行只能是"次数 top"，不能声称耗时占比。
  * `sysprof` 表（`   nr      count     tsc     pct`）：有次数也有累计 tsc，
    这才是真正的"次数 + 累计耗时"双维度排行。

快照去重
========
每份 dump 是 512 项环形缓冲的完整内容，相邻 dump 之间重叠极多。
把整个文件的行直接求和 = 把同一批调用计多次。因此这里按
`--- last linux syscalls` 标记切段，**默认只统计最大的那一段**
（最大段 = 覆盖最多不同调用，重复最少）；`--sections all` 可以逐段看。

用法
====
    python3 scripts/syscall_profile.py build/tmp/r32-chromium2.log
    python3 scripts/syscall_profile.py build/tmp/*.log --top 20
    python3 scripts/syscall_profile.py log.txt --sections all
    python3 scripts/syscall_profile.py log.txt --json out.json
"""

from __future__ import annotations

import argparse
import glob
import json
import re
import sys
from pathlib import Path

# x86_64 系统调用号 → 名字。只收本仓库日志里实际出现过、以及排查性能时
# 一定会去看的那批；没收录的按 `nr=N` 原样显示，不猜。
SYSCALL_NAMES = {
    0: "read", 1: "write", 2: "open", 3: "close", 4: "stat", 5: "fstat",
    6: "lstat", 7: "poll", 8: "lseek", 9: "mmap", 10: "mprotect",
    11: "munmap", 12: "brk", 13: "rt_sigaction", 14: "rt_sigprocmask",
    16: "ioctl", 17: "pread64", 18: "pwrite64", 19: "readv", 20: "writev",
    21: "access", 22: "pipe", 23: "select", 24: "sched_yield",
    25: "mremap", 28: "madvise", 32: "dup", 33: "dup2", 35: "nanosleep",
    39: "getpid", 41: "socket", 42: "connect", 43: "accept",
    44: "sendto", 45: "recvfrom", 46: "sendmsg", 47: "recvmsg",
    48: "shutdown", 49: "bind", 50: "listen", 53: "socketpair",
    54: "setsockopt", 55: "getsockopt", 56: "clone", 57: "fork",
    58: "vfork", 59: "execve", 60: "exit", 61: "wait4", 62: "kill",
    63: "uname", 72: "fcntl", 73: "flock", 74: "fsync", 75: "fdatasync",
    77: "ftruncate", 78: "getdents", 79: "getcwd", 80: "chdir",
    82: "rename", 83: "mkdir", 84: "rmdir", 87: "unlink", 89: "readlink",
    90: "chmod", 96: "gettimeofday", 99: "sysinfo", 102: "getuid",
    104: "getgid", 107: "geteuid", 108: "getegid", 110: "getppid",
    112: "setsid", 131: "sigaltstack", 137: "statfs", 158: "arch_prctl",
    186: "gettid", 202: "futex", 217: "getdents64", 218: "set_tid_address",
    228: "clock_gettime", 229: "clock_getres", 230: "clock_nanosleep",
    231: "exit_group", 232: "epoll_wait", 233: "epoll_ctl",
    234: "tgkill", 254: "epoll_create1", 257: "openat", 258: "mkdirat",
    262: "newfstatat", 263: "unlinkat", 271: "ppoll", 273: "set_robust_list",
    285: "fallocate", 288: "accept4", 290: "eventfd2", 291: "epoll_create",
    293: "pipe2", 297: "rt_tgsigqueueinfo", 302: "prlimit64",
    318: "getrandom", 319: "memfd_create", 322: "execveat",
    332: "statx", 334: "rseq", 435: "clone3",
}

# 环形缓冲 dump 的行：`  pid=2 nr=9 ret=17592... a0=0x0 a1=0x2000`
DUMP_LINE = re.compile(r"^\s+pid=(\d+) nr=(\d+) ret=(-?\d+)")
DUMP_HEADER = "--- last linux syscalls"

# `sysprof` 表行：`    9  1848  123456  12.34%`
PROF_ROW = re.compile(r"^\s{2,}(>=512|\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)%")
PROF_HEADER = "--- syscall profile"
PROF_TOTAL = re.compile(r"total calls=(\d+) tsc=(\d+)")


def name_of(nr: int) -> str:
    return SYSCALL_NAMES.get(nr, f"nr={nr}")


def split_dump_sections(lines: list[str]) -> list[list[tuple[int, int]]]:
    """按 dump 头切段，返回每段的 (pid, nr) 列表。"""
    sections: list[list[tuple[int, int]]] = []
    current: list[tuple[int, int]] | None = None
    for line in lines:
        if line.startswith(DUMP_HEADER):
            current = []
            sections.append(current)
            continue
        if current is None:
            continue
        m = DUMP_LINE.match(line)
        if m:
            current.append((int(m.group(1)), int(m.group(2))))
    return [s for s in sections if s]


def parse_prof_tables(lines: list[str]) -> list[dict]:
    """解析内核 `sysprof` 输出的聚合表（有次数 + 累计 tsc）。"""
    tables: list[dict] = []
    current: dict | None = None
    for line in lines:
        if line.startswith(PROF_HEADER):
            current = {"rows": [], "total_calls": None, "total_tsc": None}
            tables.append(current)
            continue
        if current is None:
            continue
        m = PROF_TOTAL.search(line)
        if m:
            current["total_calls"] = int(m.group(1))
            current["total_tsc"] = int(m.group(2))
            continue
        m = PROF_ROW.match(line)
        if m:
            nr_text = m.group(1)
            current["rows"].append({
                "nr": None if nr_text == ">=512" else int(nr_text),
                "label": ">=512" if nr_text == ">=512" else name_of(int(nr_text)),
                "count": int(m.group(2)),
                "tsc": int(m.group(3)),
                "pct": float(m.group(4)),
            })
    return [t for t in tables if t["rows"]]


def aggregate_counts(entries: list[tuple[int, int]]) -> list[dict]:
    """按 nr 聚合次数。pid 分布一并带上——同一 nr 集中在某个进程，
    往往比"总数"更能说明问题（例如全是 Chromium 子进程在轮询）。"""
    counts: dict[int, int] = {}
    pids: dict[int, set[int]] = {}
    for pid, nr in entries:
        counts[nr] = counts.get(nr, 0) + 1
        pids.setdefault(nr, set()).add(pid)
    total = len(entries)
    rows = []
    for nr, count in counts.items():
        rows.append({
            "nr": nr,
            "label": name_of(nr),
            "count": count,
            "pct": round(100.0 * count / total, 2) if total else 0.0,
            "pids": sorted(pids[nr]),
        })
    rows.sort(key=lambda r: (-r["count"], r["nr"]))
    return rows


def print_count_table(rows: list[dict], total: int, top: int, source: str) -> None:
    print(f"# 数据源: {source}")
    print(f"# 样本: {total} 条调用（注意：dump 快照只有次数，没有耗时）")
    print(f"# {'nr':>4}  {'syscall':<18} {'count':>7}  {'pct':>6}  pids")
    for row in rows[:top]:
        pids = ",".join(str(p) for p in row["pids"][:6])
        if len(row["pids"]) > 6:
            pids += ",..."
        print(f"  {row['nr']:>4}  {row['label']:<18} {row['count']:>7}  "
              f"{row['pct']:>5.2f}%  {pids}")


def print_prof_table(table: dict, top: int, source: str) -> None:
    print(f"# 数据源: {source}（内核 sysprof 聚合表：次数 + 累计 tsc）")
    print(f"# total calls={table['total_calls']} tsc={table['total_tsc']}")
    print(f"# {'nr':>4}  {'syscall':<18} {'count':>7}  {'tsc':>12}  {'pct':>6}")
    for row in table["rows"][:top]:
        nr = ">=512" if row["nr"] is None else str(row["nr"])
        print(f"  {nr:>4}  {row['label']:<18} {row['count']:>7}  "
              f"{row['tsc']:>12}  {row['pct']:>5.2f}%")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="把串口日志里的 syscall 数据聚合成热点排行")
    ap.add_argument("logs", nargs="+", help="串口日志路径（支持通配符）")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--sections", choices=("largest", "all"), default="largest",
                    help="dump 快照有重叠，默认只统计最大的那一段")
    ap.add_argument("--json", default="", help="把结果写成 JSON")
    args = ap.parse_args()

    paths: list[Path] = []
    for pattern in args.logs:
        matched = [Path(p) for p in glob.glob(pattern)]
        paths.extend(matched if matched else [Path(pattern)])
    paths = [p for p in paths if p.is_file()]
    if not paths:
        print("[sysprof] 没有可读的日志", file=sys.stderr)
        return 1

    payload: list[dict] = []
    rc = 0
    for path in paths:
        text = path.read_text(errors="ignore")
        lines = text.splitlines()
        prof_tables = parse_prof_tables(lines)
        sections = split_dump_sections(lines)

        if not prof_tables and not sections:
            print(f"[sysprof] {path}: 没有 syscall 数据（没开 tracepid/sysprof？）")
            continue

        print(f"=== {path} ===")
        if prof_tables:
            # sysprof 表是内核自己聚合好的，直接信它，不再从 dump 里重算。
            table = prof_tables[-1]
            print_prof_table(table, args.top, str(path))
            payload.append({"log": str(path), "kind": "sysprof", **table})
        elif sections:
            if args.sections == "all":
                for i, section in enumerate(sections):
                    rows = aggregate_counts(section)
                    print(f"\n-- dump 段 {i + 1}/{len(sections)} --")
                    print_count_table(rows, len(section), args.top, str(path))
            else:
                biggest = max(sections, key=len)
                rows = aggregate_counts(biggest)
                note = (f"共 {len(sections)} 段 dump，取最大段 "
                        f"({len(biggest)} 条)；其余段与它重叠，未重复计入")
                print(f"# {note}")
                print_count_table(rows, len(biggest), args.top, str(path))
                payload.append({
                    "log": str(path), "kind": "dump", "sections": len(sections),
                    "sampled": len(biggest), "rows": rows,
                })
        print()

    if args.json:
        Path(args.json).write_text(
            json.dumps(payload, indent=2, ensure_ascii=False))
        print(f"[sysprof] 已写入 {args.json}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
