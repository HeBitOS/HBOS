#!/usr/bin/env python3
"""HBOS 串口日志用户态地址符号化(可靠版)。

设计目标只有一个:**绝不给出假符号**。给不出可信符号时明确拒绝并说明原因。

三条硬规则
----------
1. 基址不硬编码。HBOS 的 ELF 加载器(src/elf.c)把 PIE 装到
   ``load_bias = PIE_BASE - image_min``(PIE_BASE 是内核常量,image_min 是
   所有 PT_LOAD 里最小的 ``p_vaddr & PAGE_MASK``)。本工具从 src/elf.c
   读 PIE_BASE、从 ELF 程序头读 image_min,把推导过程打印出来。
   日志里 ``same-perm span=`` 不是映射基址:它是内核 ±16MB 页表走查的结果
   (src/core/gdt_idt.c),所以 span 长度恰好等于 0x2001000 时两端都被截断,
   span 起点**不是**基址。工具会做这个一致性检查。

2. 地址必须落在该二进制的某个 PT_LOAD 里才给符号。落在可执行段外的
   地址(数据段、共享库区间 0x1000_0000_0000、栈、堆)一律拒绝,不会硬套
   主程序符号表 —— 假符号比没有符号更危险。

3. 符号表必须和二进制的 build-id 对得上。每个 profile 记录 build-id,
   运行时用真实二进制的 build-id 校验;对不上就拒绝(这正是历史上
   "把 headless-shell 的地址套到 chromium 的符号表上" 的事故根因)。

用法
----
    # 单个/多个地址(--profile 可用名字或 build-id 前缀)
    python3 scripts/symbolize_rip.py --profile hs156 0x11077CD4B7 0x11099565FA

    # 直接给 ELF + 符号表
    python3 scripts/symbolize_rip.py --elf build/tmp/headless-check/chromium-headless-shell \\
        --syms build/tmp/headless-shell156.syms 0x11077CD4B7

    # 扫日志:先按日志里的可执行文件路径/ud2 指纹自动认出是哪个二进制,再符号化
    python3 scripts/symbolize_rip.py --log build/tmp/hs-dom10.log

    # 自检(PASS/FAIL;含"用错基址必须被检出"的负向对照)
    python3 scripts/symbolize_rip.py --selfcheck

    # profile 登记 / 生成符号表
    python3 scripts/symbolize_rip.py --profiles
    python3 scripts/symbolize_rip.py --register --name v156-chromium --elf <binary> [--syms <table>]
    python3 scripts/symbolize_rip.py --mksyms <dbgsym.debug> -o build/tmp/xxx.syms

profile 清单存在 build/tmp/symbol-profiles.json(不进 git)。
"""
from __future__ import annotations

import argparse
import bisect
import glob
import json
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(ROOT, "build", "tmp", "symbol-profiles.json")
LOADER_SRC = os.path.join(ROOT, "src", "elf.c")
PAGE = 0x1000
# src/core/gdt_idt.c 的 same-perm 走查上限:4096 页 = 16MB(单侧)
SPAN_WALK_LIMIT = 4096 * PAGE
PT_LOAD, PT_NOTE = 1, 4
PF_X, PF_W = 1, 2
# 内核把共享库 / ld.so / 用户堆放在 0x1000_0000_0000 起(PIE 主程序在 0x1100_0000_0000)
SHLIB_LO = 0x100000000000
STACK_LO = 0x600000000000
PIE_REGION_LO = 0x1100000000
PIE_REGION_HI = 0x1200000000
CODE_TYPES = ("t", "T", "W", "w")
SKIP_TYPES = ("a", "N", "n", "U", "u")


# --------------------------------------------------------------------------- ELF
class Elf:
    """极简 ELF64 读取:程序头 + build-id + PT_LOAD 字节读取。"""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        hdr = self.f.read(64)
        if hdr[:4] != b"\x7fELF" or hdr[4] != 2:
            raise ValueError(f"不是 ELF64 文件: {path}")
        self.e_type = struct.unpack_from("<H", hdr, 16)[0]
        e_entry = struct.unpack_from("<Q", hdr, 24)[0]
        e_phoff = struct.unpack_from("<Q", hdr, 32)[0]
        e_phentsize, e_phnum = struct.unpack_from("<HH", hdr, 54)
        self.entry = e_entry
        self.loads = []          # (vaddr, memsz, filesz, offset, flags)
        self.build_id = None
        for i in range(e_phnum):
            self.f.seek(e_phoff + i * e_phentsize)
            p = self.f.read(56)
            if len(p) < 56:
                break
            p_type, p_flags, p_off, p_vaddr = struct.unpack_from("<IIQQ", p, 0)
            p_filesz, p_memsz = struct.unpack_from("<QQ", p, 32)
            if p_type == PT_LOAD:
                # filesz=0 也要保留(dbgsym 的 LOAD 只留 memsz):范围/权限判断要靠它,
                # 只是取不到字节。字节级校验另有 read_vaddr 把关。
                self.loads.append((p_vaddr, p_memsz, p_filesz, p_off, p_flags))
            elif p_type == PT_NOTE:
                self._read_notes(p_off, p_filesz)

    def _read_notes(self, off, size):
        self.f.seek(off)
        blob = self.f.read(size)
        pos = 0
        while pos + 12 <= len(blob):
            namesz, descsz, ntype = struct.unpack_from("<III", blob, pos)
            pos += 12
            name = blob[pos:pos + namesz].rstrip(b"\0")
            pos += (namesz + 3) & ~3
            desc = blob[pos:pos + descsz]
            pos += (descsz + 3) & ~3
            if name == b"GNU" and ntype == 3 and self.build_id is None:
                self.build_id = desc.hex()

    @property
    def image_min(self):
        return min((v & ~(PAGE - 1) for v, *_ in self.loads), default=None)

    @property
    def exec_load(self):
        for v, memsz, _fs, _off, fl in sorted(self.loads):
            if fl & PF_X:
                return v & ~(PAGE - 1), (v & ~(PAGE - 1)) + memsz
        return None

    @property
    def has_bytes(self):
        return any(filesz > 0 for _v, _m, filesz, _o, _f in self.loads)

    def seg_of(self, vaddr):
        for v, memsz, filesz, off, fl in self.loads:
            if v <= vaddr < v + memsz:
                return (v, memsz, filesz, off, fl)
        return None

    def read_vaddr(self, vaddr, n=2):
        for v, _memsz, filesz, off, _fl in self.loads:
            if v <= vaddr < v + filesz:
                self.f.seek(off + (vaddr - v))
                return self.f.read(n)
        return None

    def describe(self):
        out = [f"ELF {self.path}"]
        if self.build_id:
            out.append(f"  build-id {self.build_id}")
        out.append(f"  entry  0x{self.entry:x}  image_min 0x{self.image_min:x}")
        for v, memsz, filesz, off, fl in sorted(self.loads):
            perm = "".join(c for c, bit in (("R", 4), ("W", 2), ("X", 1)) if fl & bit)
            out.append(f"  LOAD vaddr=0x{v:x} memsz=0x{memsz:x} filesz=0x{filesz:x} "
                       f"off=0x{off:x} {perm}")
        return "\n".join(out)


def loader_pie_base():
    """从 src/elf.c 读内核的 PIE 常量(不硬编码,证据可追溯)。"""
    try:
        text = open(LOADER_SRC, encoding="utf-8", errors="replace").read()
    except OSError:
        return None, None
    m = re.search(r"pie_base\s*=\s*(0x[0-9a-fA-F]+)", text)
    if not m:
        return None, None
    line = text[:m.start()].count("\n") + 1
    return int(m.group(1), 16), f"{os.path.relpath(LOADER_SRC, ROOT)}:{line}"


def derive_base(elf, override=None):
    """返回 (base, [证据行])。base 使 vaddr = base + 符号偏移。"""
    ev = []
    if override is not None:
        ev.append(f"基址由 --base 显式指定:0x{override:x}(跳过推导,风险自负)")
        return override, ev
    pie, src = loader_pie_base()
    if pie is None:
        raise SystemExit(f"无法从 {os.path.relpath(LOADER_SRC, ROOT)} 读到 pie_base,"
                         "请用 --base 显式给出基址")
    imin = elf.image_min
    ev.append(f"内核 PIE 常量 {src}: pie_base=0x{pie:x}")
    ev.append(f"ELF 程序头最小 PT_LOAD p_vaddr(页对齐 image_min)=0x{imin:x}")
    ev.append("加载器 load_bias = pie_base - image_min(见该处代码),"
              f"故本例 bias=0x{pie - imin:x} = 主程序 vaddr 基址")
    return pie - imin, ev


# ---------------------------------------------------------------------- 符号表
class Symbols:
    def __init__(self, path):
        self.path = path
        self.code_a, self.code_n = [], []
        self.data_a, self.data_n = [], []
        with open(path, "r", errors="replace") as fh:
            for line in fh:
                parts = line.rstrip("\n").split(" ", 2)
                if len(parts) < 3 or parts[1] in SKIP_TYPES or len(parts[1]) != 1:
                    continue
                try:
                    a = int(parts[0], 16)
                except ValueError:
                    continue
                if parts[1] in CODE_TYPES:
                    self.code_a.append(a)
                    self.code_n.append(parts[2])
                else:
                    self.data_a.append(a)
                    self.data_n.append(parts[2])

    def _lookup(self, addrs, names, off):
        i = bisect.bisect_right(addrs, off) - 1
        if i < 0:
            return None
        return names[i], off - addrs[i]

    def code(self, off):
        return self._lookup(self.code_a, self.code_n, off)

    def data(self, off):
        return self._lookup(self.data_a, self.data_n, off)

    def nearest_any(self, off):
        # 极少量场景(地址落在数据段)给个参考,仅用于"非代码段"提示
        best = None
        for addrs, names in ((self.code_a, self.code_n), (self.data_a, self.data_n)):
            r = self._lookup(addrs, names, off)
            if r and (best is None or r[1] < best[1]):
                best = r
        return best


# ------------------------------------------------------------------- profile
def load_registry():
    if not os.path.exists(REGISTRY):
        return {"profiles": []}
    with open(REGISTRY, encoding="utf-8") as fh:
        return json.load(fh)


def save_registry(reg):
    os.makedirs(os.path.dirname(REGISTRY), exist_ok=True)
    with open(REGISTRY, "w", encoding="utf-8") as fh:
        json.dump(reg, fh, indent=2, ensure_ascii=False)
        fh.write("\n")


def meta_of(syms_path):
    path = syms_path + ".meta.json"
    if not os.path.exists(path):
        return None
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return None


def verify_provenance(elf, syms_path, debug_path=None):
    """符号表/调试文件必须与二进制同源(build-id 一致),否则拒绝出符号。

    这是历史上那次事故的守门人:把 headless-shell 的地址套到 chromium 的
    符号表上,得到的全是假符号。
    """
    problems = []
    meta = meta_of(syms_path) if syms_path else None
    if meta and meta.get("build_id") and elf.build_id and meta["build_id"] != elf.build_id:
        problems.append(f"符号表来源 dbgsym build-id={meta['build_id'][:16]}"
                        f"({meta.get('source_debug')}),二进制 build-id={elf.build_id[:16]}")
    if debug_path and os.path.exists(debug_path):
        dbg = Elf(debug_path)
        if dbg.build_id and elf.build_id and dbg.build_id != elf.build_id:
            problems.append(f"dbgsym {os.path.relpath(debug_path, ROOT)} build-id="
                            f"{dbg.build_id[:16]} != 二进制 {elf.build_id[:16]}")
        elif dbg.image_min != elf.image_min or dbg.exec_load != elf.exec_load:
            problems.append("dbgsym 与二进制的 PT_LOAD 布局不一致")
    elif debug_path:
        # dbgsym 没了不算错:符号表侧车文件里的 build-id 仍是可校验的凭据。
        print(f"提示:登记的 dbgsym 已不在磁盘上({os.path.relpath(debug_path, ROOT)}),"
              "跳过交叉校验(符号表 build-id 校验仍然生效)", file=sys.stderr)
    if problems:
        raise SystemExit("拒绝:符号表与二进制不同源,给不出可信符号 ->\n  "
                         + "\n  ".join(problems)
                         + "\n对策:找到 build-id 匹配的 dbgsym(debuginfod / debian-debug"
                           " 对应的 -dbgsym 包),用 --mksyms 重新生成符号表。")
    return True


def upsert_profile(name, elf_path, syms_path=None, guest_paths=None, version=None,
                   debug_path=None):
    elf = Elf(elf_path)
    if not elf.build_id:
        raise SystemExit(f"{elf_path} 里没有 GNU build-id,无法用于校验")
    reg = load_registry()
    if syms_path:
        verify_provenance(elf, syms_path, debug_path)
    entry = {
        "name": name,
        "build_id": elf.build_id,
        "version": version,
        "elf": os.path.relpath(elf_path, ROOT),
        "syms": os.path.relpath(syms_path, ROOT) if syms_path else None,
        "debug": os.path.relpath(debug_path, ROOT) if debug_path else None,
        "guest_paths": guest_paths or [],
    }
    reg["profiles"] = [p for p in reg["profiles"] if p.get("name") != name]
    reg["profiles"].append(entry)
    save_registry(reg)
    meta = {"source_debug": os.path.abspath(debug_path) if debug_path else None,
            "build_id": elf.build_id, "binary": entry["elf"]}
    if syms_path:
        with open(syms_path + ".meta.json", "w", encoding="utf-8") as fh:
            json.dump(meta, fh, indent=2)
            fh.write("\n")
    return entry


def resolve_profile(token):
    """按名字或 build-id 前缀选 profile;返回 (entry, Elf)。"""
    reg = load_registry()
    cands = [p for p in reg["profiles"]
             if p["name"] == token or p["build_id"].startswith(token)]
    if not cands:
        raise SystemExit(f"没有匹配的 profile: {token}\n可用: " + profiles_summary())
    if len(cands) > 1:
        raise SystemExit(f"profile 名字不唯一: {token} -> " +
                         ", ".join(p["name"] for p in cands))
    return _open_profile(cands[0])


def _open_profile(entry):
    elf_path = os.path.join(ROOT, entry["elf"])
    if not os.path.exists(elf_path):
        raise SystemExit(f"profile {entry['name']} 的二进制不存在: {entry['elf']}")
    elf = Elf(elf_path)
    if elf.build_id != entry["build_id"]:
        raise SystemExit(
            f"拒绝:{entry['name']} 登记的 build-id {entry['build_id']} 与磁盘上的二进制 "
            f"{elf.build_id} 不一致(二进制被换过)")
    return entry, elf


def profiles_summary():
    reg = load_registry()
    if not reg["profiles"]:
        return "(空:先 --register 登记,或用 --elf/--syms)"
    lines = []
    for p in reg["profiles"]:
        syms = p.get("syms") or "-"
        ok = "ok" if p.get("syms") and os.path.exists(os.path.join(ROOT, p["syms"])) else "缺符号表"
        lines.append(f"  {p['name']:<44} {p['build_id'][:12]}  syms={syms} [{ok}]")
    return "\n".join(lines)


# --------------------------------------------------------------- 地址分类
def classify(addr, base, elf):
    """返回 (kind, text)。kind: code / nonexec / outside / shlib / other

    注意地址空间布局:PIE 主程序在 0x1_1000_0000 起(73GB 量级),
    共享库 / ld.so / 用户堆在 0x1000_0000_0000 起(17.6TB 量级),
    两者相差三个数量级,不能互相套符号。
    """
    if not (PIE_REGION_LO <= addr < PIE_REGION_HI):
        if addr >= STACK_LO:
            return "other", f"用户栈区间(>= 0x{STACK_LO:x}),拒绝猜测"
        if addr >= SHLIB_LO:
            return "shlib", ("共享库 / ld.so / 用户堆区间(>= 0x1000_0000_0000),"
                             "本符号表不覆盖 —— 需要该库自己的 dbgsym,拒绝套主程序符号")
        return "other", "不在主程序 PIE 区间 [0x1100000000,0x1200000000),拒绝猜测"
    off = addr - base
    seg = elf.seg_of(off)
    if seg is None:
        return "outside", (f"偏移 0x{off:x} 不在该二进制的任何 PT_LOAD 内 -> "
                           "要么基址不对,要么这不是该二进制的地址;拒绝给符号")
    if seg[4] & PF_X:
        return "code", f"可执行段 [0x{seg[0]:x},0x{seg[0]+seg[1]:x}) 内"
    perm = "".join(c for c, bit in (("R", 4), ("W", 2), ("X", 1)) if seg[4] & bit)
    return "nonexec", (f"拒绝:偏移 0x{off:x} 落在非可执行段({perm})"
                       f"[0x{seg[0]:x},0x{seg[0]+seg[1]:x})内,不是指令地址"
                       "(基址错?或这是数据指针)")


def symbolize(addr, base, elf, syms):
    kind, why = classify(addr, base, elf)
    off = addr - base
    if kind == "code":
        hit = syms.code(off)
        if hit is None:
            return kind, f"0x{addr:x}  {why};符号表里没有更低的符号"
        name, delta = hit
        return kind, f"0x{addr:x}  +0x{off:x}  {name}+0x{delta:x}"
    if kind == "nonexec":
        hit = syms.nearest_any(off)
        ref = (f";最近符号(仅参考,不可当函数名) {hit[0]}+0x{hit[1]:x}" if hit else "")
        return kind, f"0x{addr:x}  {why}{ref}"
    return kind, f"0x{addr:x}  {why}"


# ------------------------------------------------------------- 日志解析
TRAP_RE = re.compile(r"^\[TRAP\] user (.*?) rip=(0x[0-9a-fA-F]+)")
BT_RE = re.compile(r"^\s*bt\[(\d+)\] (?:rip|ret)=(0x[0-9a-fA-F]+)")
SPAN_RE = re.compile(r"same-perm span=([0-9a-fA-F]+)-([0-9a-fA-F]+) \(rip at \+0x([0-9a-fA-F]+)\)")
CMDLINE_RE = re.compile(r"/[\w./-]*(?:chromium[\w.-]*|ld-linux[\w.-]*|libc[\w.-]*)")


def parse_log(path):
    """按崩溃块解析日志:每个 [TRAP] 一行开启一块,块内带 span / bt / 前置 FATAL 行。"""
    blocks, cur, last_fatal, paths = [], None, "", []
    for line in open(path, errors="replace"):
        m = TRAP_RE.match(line)
        if m:
            kind = m.group(1).strip()
            cur = {"kind": kind, "rip": int(m.group(2), 16), "trap": "Breakpoint" in kind,
                   "bt": [], "span": None, "fatal": last_fatal}
            blocks.append(cur)
            continue
        if "FATAL" in line or "CHECK failed" in line:
            last_fatal = line.strip()[:160]
        if cur is None:
            continue
        m = BT_RE.match(line)
        if m:
            cur["bt"].append((int(m.group(1)), int(m.group(2), 16)))
            continue
        m = SPAN_RE.search(line)
        if m:
            if cur["span"] is None:
                cur["span"] = {"lo": int(m.group(1), 16), "hi": int(m.group(2), 16),
                               "rip": int(m.group(1), 16) + int(m.group(3), 16)}
            continue
        for p in CMDLINE_RE.findall(line):
            if "/" in p and p not in paths:
                paths.append(p)
    traps = [{"kind": b["kind"], "rip": b["rip"], "trap": b["trap"]} for b in blocks]
    bt = [x for b in blocks for x in b["bt"]]
    spans = [b["span"] for b in blocks if b["span"]]
    return {"blocks": blocks, "traps": traps, "bt": bt, "spans": spans, "paths": paths}


def span_check(addr, span, base, elf):
    """用日志的 same-perm span 检查基址假设是否自洽(打印证据)。"""
    out = []
    size = span["hi"] - span["lo"]
    page = addr & ~(PAGE - 1)
    low_trunc = (page - span["lo"]) == SPAN_WALK_LIMIT
    high_trunc = (span["hi"] - (page + PAGE)) == SPAN_WALK_LIMIT
    ev = elf.exec_load
    if low_trunc or high_trunc:
        which = ("两端" if low_trunc and high_trunc else
                 ("下端" if low_trunc else "上端"))
        out.append(f"  span=0x{span['lo']:x}-0x{span['hi']:x} 长度 0x{size:x}:"
                   f"{which}恰好命中 16MB 走查上限 -> 该端点不是映射基址,"
                   "不能当基址用")
    else:
        out.append(f"  span=0x{span['lo']:x}-0x{span['hi']:x} 长度 0x{size:x}:"
                   "端点未触上限(是真边界)")
    if ev:
        out.append(f"  基址假设 0x{base:x} -> 可执行段 [0x{base + ev[0]:x},0x{base + ev[1]:x}):"
                   f" rip{'在段内' if ev[0] <= addr - base < ev[1] else '不在段内'};"
                   f" 向下还剩 0x{addr - (base + ev[0]):x} 才会撞到段首"
                   f"(>=16MB 才能解释下端被截断: {'一致' if addr - (base + ev[0]) >= SPAN_WALK_LIMIT else '不一致'})")
    return size, out


def pick_profile(log_data, base=None):
    """按日志自身证据选 profile:先看命令行里的路径,再用 ud2 指纹确认。"""
    reg = load_registry()
    traps = [t["rip"] for t in log_data["traps"] if t["trap"]]
    scored = []
    for entry in reg["profiles"]:
        try:
            _e, elf = _open_profile(entry)
        except SystemExit:
            continue
        syms_ok = bool(entry.get("syms")) and os.path.exists(os.path.join(ROOT, entry["syms"]))
        guest = any(any(g == p or p.endswith(g.split("/usr/")[-1]) for g in entry.get("guest_paths", []))
                    for p in log_data["paths"])
        hits = tested = 0
        if traps and elf.has_bytes:
            for t in traps:
                b = elf.read_vaddr(t - (base if base is not None else
                                        (loader_pie_base()[0] - elf.image_min)), 2)
                if b is None:
                    continue
                tested += 1
                hits += (b == b"\x0f\x0b" or b[:1] == b"\xcc")
        scored.append((entry, syms_ok, guest, hits, tested))
    # 指纹优先:命中全部被检验的 trap
    perfect = [s for s in scored if s[4] > 0 and s[3] == s[4]]
    if len(perfect) == 1:
        return perfect[0][0], f"ud2 指纹命中 {perfect[0][3]}/{perfect[0][4]} 个 trap"
    if len(perfect) > 1:
        g = [s for s in perfect if s[2]]
        if len(g) == 1:
            return g[0][0], "ud2 指纹命中且命令行路径吻合"
        return None, ("多个 profile 都通过 ud2 指纹测试(" +
                      ", ".join(s[0]["name"] for s in perfect) + "),无法唯一确定")
    byguest = [s for s in scored if s[2] and s[4] > 0]
    if len(byguest) == 1:
        return byguest[0][0], "命令行路径唯一吻合(但无 trap 可做指纹校验)"
    if len(byguest) == 1 and not traps:
        return byguest[0][0], "命令行路径吻合"
    detail = "; ".join(f"{s[0]['name']}:{s[3]}/{s[4]}" for s in scored if s[4])
    return None, f"没有任何 profile 通过 ud2 指纹测试({detail or '无可用 trap'})"


# ------------------------------------------------------------------- 自检
# 已知答案:hb60000/headless-shell 156.0.8078.73-1 上,hs-dom10.log 的
# FATAL(gin/v8_initializer.cc:640 "Error loading V8 startup snapshot file")
# 之后的 ud2 崩点及其调用链。语义必须与那条 FATAL 对得上。
KNOWN = [
    ("0x11077CD4B7", "logging::LogMessage::Flush", "ud2 崩点:FATAL 打印路径"),
    ("0x11077CD6FD", "logging::LogMessageFatal::~LogMessageFatal", "FATAL 日志对象析构"),
    ("0x11099565FA", "gin::V8Initializer::LoadV8SnapshotFromFile", "日志点 gin/v8_initializer.cc:640"),
    ("0x1109956153", "gin::V8Initializer::LoadV8Snapshot", "同一 FATAL 调用者"),
    ("0x11071A4D4E", "content::ContentMainRunnerImpl::Initialize", "启动主流程"),
]


def selfcheck(verbose=True):
    results = []

    def add(name, ok, detail):
        results.append((name, ok, detail))

    hs_log = os.path.join(ROOT, "build/tmp/hs-dom10.log")
    log_data = parse_log(hs_log) if os.path.exists(hs_log) else None
    hs_entry = None
    hs_elf = hs_syms = None
    if log_data:
        hs_entry, why = pick_profile(log_data)
        if hs_entry:
            hs_elf = Elf(os.path.join(ROOT, hs_entry["elf"]))
            if hs_entry.get("syms"):
                hs_syms = Symbols(os.path.join(ROOT, hs_entry["syms"]))
    # 1 基址推导
    if hs_elf:
        base, ev = derive_base(hs_elf)
        add("1 基址推导(不硬编码)", base == PIE_REGION_LO and hs_elf.image_min == 0,
            f"bias=0x{base:x};" + "; ".join(ev[:2]))
    else:
        add("1 基址推导(不硬编码)", False, "缺少 headless-shell profile/二进制")
    # 2 用日志 span 反证"span 起点不是基址"
    if log_data and log_data["spans"] and hs_elf:
        size, lines = span_check(log_data["spans"][0]["rip"], log_data["spans"][0], base, hs_elf)
        add("2 span 不是基址(截断证据)", size == 2 * SPAN_WALK_LIMIT + PAGE,
            f"span 长度 0x{size:x} == 2*16MB+4K,两端都撞上限;" + lines[1])
    else:
        add("2 span 不是基址(截断证据)", False, "日志里没有 same-perm span 行")
    # 3 ud2 指纹:崩点必须真是陷阱指令
    if log_data and hs_elf:
        traps = [t["rip"] for t in log_data["traps"] if t["trap"]]
        hit = sum(1 for t in traps if hs_elf.read_vaddr(t - base, 2) == b"\x0f\x0b")
        add("3 ud2 指纹校验", traps and hit == len(traps),
            f"{hit}/{len(traps)} 个 Breakpoint rip 处字节是 0f 0b(ud2=IMMEDIATE_CRASH)")
    else:
        add("3 ud2 指纹校验", False, "无 trap 或二进制缺失")
    # 4 负向对照:故意用错基址必须被检出(崩点字节不再是指令 + 分类拒绝)
    if log_data and hs_elf:
        wrong = 0x11067CD000          # 日志 span 起点(被误当成映射基址的那个值)
        traps = [t["rip"] for t in log_data["traps"] if t["trap"]]
        bad = sum(1 for t in traps if hs_elf.read_vaddr(t - wrong, 2) == b"\x0f\x0b")
        kinds = [classify(t, wrong, hs_elf)[0] for t in traps]
        lines = [symbolize(t, wrong, hs_elf, hs_syms)[1] for t in traps] if hs_syms else []
        refused = all(l.startswith("0x") and ("拒绝" in l or "不在" in l) for l in lines) if lines else True
        detected = bool(traps) and bad < len(traps) and all(k != "code" for k in kinds) and refused
        add("4 负向对照:错基址必须被检出", detected,
            f"base=0x{wrong:x} 时 ud2 命中 {bad}/{len(traps)}、分类={kinds}、"
            f"且不产出函数名 -> 错误基址被检出")
    else:
        add("4 负向对照:错基址必须被检出", False, "无 trap 或二进制缺失")
    # 5 build-id 守卫:符号表与二进制不匹配必须拒绝(真跑一遍守卫)
    reg = load_registry()
    mism = None
    for p in reg["profiles"]:
        if p.get("syms") and p["build_id"] != (hs_elf.build_id if hs_elf else None) \
                and os.path.exists(os.path.join(ROOT, p["syms"])):
            mism = p
            break
    if mism and hs_elf:
        try:
            verify_provenance(hs_elf, os.path.join(ROOT, mism["syms"]))
            ok, detail = False, "守卫没拦住不匹配的符号表"
        except SystemExit as exc:
            ok = True
            detail = (f"拿 {mism['name']}({mism['build_id'][:12]}) 的表配 "
                      f"{hs_elf.build_id[:12]} 的二进制 -> {str(exc).splitlines()[0]}")
    else:
        ok, detail = False, "找不到不匹配的 profile 来做负向测试"
    add("5 build-id 守卫", ok, detail)
    # 6 共享库区间拒绝
    if hs_elf and hs_syms:
        kind, msg = classify(0x1000015A4137, base, hs_elf)
        _k2, line = symbolize(0x1000015A4137, base, hs_elf, hs_syms)
        add("6 共享库地址拒绝", kind == "shlib" and "拒绝" in line,
            f"0x1000015A4137 -> [{kind}] {line[:120]}")
    else:
        add("6 共享库地址拒绝", False, "缺少 profile")
    # 7 已知答案(语义)
    if hs_elf and hs_syms:
        bad = []
        for hexaddr, want, _note in KNOWN:
            kind, line = symbolize(int(hexaddr, 16), base, hs_elf, hs_syms)
            if want not in line:
                bad.append(f"{hexaddr}!={want}")
        add("7 已知答案语义校验", not bad,
            (f"{len(KNOWN)} 个已知地址全部命中" if not bad else "失败:" + ", ".join(bad)))
    else:
        add("7 已知答案语义校验", False, "缺少 profile")
    # 8 与独立 DWARF 读取器(eu-addr2line)交叉校验
    ea = shutil_which("eu-addr2line")
    if ea and hs_entry and hs_entry.get("syms"):
        debug_file = hs_entry.get("debug")
        if debug_file and os.path.exists(os.path.join(ROOT, debug_file)):
            # eu-addr2line 要的是未重定位的 vaddr(即 VA - base)
            out = subprocess.run([ea, "-f", "-C", "-e", os.path.join(ROOT, debug_file)] +
                                 [hex(int(h, 16) - base) for h, _w, _n in KNOWN],
                                 capture_output=True, text=True).stdout.splitlines()
            got = [strip_args(g) for g in out[0::2]]
            want = [strip_args(w) for _h, w, _n in KNOWN]
            agree = all(g == w or w.startswith(g) for g, w in zip(got, want)) and len(got) == len(want)
            add("8 eu-addr2line 交叉校验", agree,
                "两个独立 DWARF 读取器结论一致" if agree
                else f"不一致: {got[:2]} vs {want[:2]}")
        else:
            add("8 eu-addr2line 交叉校验", False, "profile 里没登记 dbgsym 路径")
    else:
        add("8 eu-addr2line 交叉校验", False, "无 eu-addr2line 或 profile")

    if verbose:
        print("=== symbolize_rip 自检 ===")
        for name, ok, detail in results:
            print(f"[{'PASS' if ok else 'FAIL'}] {name}\n        {detail}")
        print(f"--- {sum(1 for _n, o, _d in results if o)}/{len(results)} PASS ---")
    return all(ok for _n, ok, _d in results)


def strip_args(name):
    """用于跨工具比对:去掉名字末尾的参数列表(nm 与 eu-addr2line 写法不同)。"""
    return re.sub(r"\(.*\)$", "", name).strip()


def shutil_which(name):
    for d in os.environ.get("PATH", "/usr/bin").split(os.pathsep):
        p = os.path.join(d, name)
        if os.path.exists(p) and os.access(p, os.X_OK):
            return p
    return None


# --------------------------------------------------------------- mksyms
def mksyms(debug, out):
    if not os.path.exists(debug):
        raise SystemExit(f"dbgsym 不存在: {debug}")
    with open(out, "w", encoding="utf-8") as fh:
        rc = subprocess.run(["nm", "-n", "-C", "--defined-only", debug],
                            stdout=fh, text=True).returncode
    if rc != 0:
        raise SystemExit(f"nm 失败(rc={rc}),{out} 可能不完整")
    elf = Elf(debug)
    meta = {"source_debug": os.path.abspath(debug), "build_id": elf.build_id,
            "binary": None}
    with open(out + ".meta.json", "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2)
    print(f"写出 {out} (build-id {elf.build_id})")
    print(f"      {out}.meta.json")
    print("提示:把该 dbgsym 路径登记到 profile(debug 字段)才能做交叉校验")


# ------------------------------------------------------------------- main
def cmd_log(path, verbose):
    log_data = parse_log(path)
    entry, why = pick_profile(log_data)
    print(f"日志 {path}")
    print(f"  识别:{why}")
    if entry is None:
        print("  拒绝符号化:无法唯一确定这些地址属于哪个二进制。")
        print(profiles_summary())
        return 2
    elf = Elf(os.path.join(ROOT, entry["elf"]))
    base, ev = derive_base(elf)
    print(f"  profile:{entry['name']}  build-id {entry['build_id'][:12]}")
    print(f"  二进制:{entry['elf']}")
    print("  基址依据:")
    for line in ev:
        print(f"    - {line}")
    if not entry.get("syms"):
        print(f"  拒绝符号化:{entry['name']} 没有匹配的符号表(dbgsym 不可得)")
        return 2
    syms = Symbols(os.path.join(ROOT, entry["syms"]))
    print(f"  符号表:{entry['syms']}(代码符号 {len(syms.code_a)} 条)")
    for span in log_data["spans"][:2]:
        _size, lines = span_check(span["rip"], span, base, elf)
        for line in lines:
            print(line)
    traps = [t["rip"] for t in log_data["traps"] if t["trap"]]
    if traps:
        hit = sum(1 for t in traps if elf.read_vaddr(t - base, 2) == b"\x0f\x0b")
        print(f"  陷阱字节校验:{hit}/{len(traps)} 个 Breakpoint rip 处是 0f 0b(ud2)")
    for each in log_data["paths"][:3]:
        print(f"  (日志中的可执行文件路径: {each})")
    for n, blk in enumerate(log_data["blocks"], 1):
        tag = f"[{n}] TRAP {blk['kind']}"
        rip = blk["rip"]
        kind, line = symbolize(rip, base, elf, syms)
        note = ""
        if kind == "code":
            b = elf.read_vaddr(rip - base, 2)
            if b == b"\x0f\x0b":
                note = "  [ud2 陷阱字节 ✓]"
            elif b and b[:1] == b"\xcc":
                note = "  [int3 ✓]"
        print(f"  {tag} rip=0x{rip:x}")
        if blk.get("fatal"):
            print(f"      前置日志: {blk['fatal']}")
        print(f"      [{kind}] {line}{note}")
        if blk.get("span"):
            for l in span_check(rip, blk["span"], base, elf)[1]:
                print(f"      {l.strip()}")
        for i, a in sorted(blk["bt"]):
            k2, l2 = symbolize(a, base, elf, syms)
            print(f"      bt[{i}] [{k2}] {l2}")
    return 0


def cmd_addrs(addrs, entry, elf, syms, base, verbose):
    base, ev = derive_base(elf, base)
    print(f"profile: {entry['name'] if entry else '(--elf 直接指定)'}"
          f"  build-id {elf.build_id[:12] if elf.build_id else '?'}")
    print(f"二进制 : {elf.path}")
    print(f"符号表 : {syms.path if syms else '(无)'}")
    for line in ev:
        print(f"基址   : {line}")
    print("说明   : [ud2]/[int3] 表示该地址处的字节是陷阱指令;"
          "只有 trap 的 rip 才有这个含义,bt[] 的返回地址上出现属正常字节巧合")
    print("提示   : 地址取自日志时优先用 --log:它会用 ud2 指纹自动认二进制,"
          "避免把 A 的地址套到 B 的符号表上(本工具历史上最贵的一次事故)")
    rc = 0
    for a in addrs:
        kind, line = symbolize(a, base, elf, syms)
        if kind == "code" and syms:
            b = elf.read_vaddr(a - base, 2)
            note = "  [ud2]" if b == b"\x0f\x0b" else ("  [int3]" if b and b[:1] == b"\xcc" else "")
            print(f"  {line}{note}")
        else:
            print(f"  拒绝 {line}")
            rc = 2
    return rc


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addrs", nargs="*", help="地址(0x...)")
    ap.add_argument("--profile", help="profile 名或 build-id 前缀")
    ap.add_argument("--elf", help="真实二进制(校验 PT_LOAD / build-id / 陷阱字节)")
    ap.add_argument("--syms", help="nm -n -C --defined-only 表")
    ap.add_argument("--base", help="显式基址(默认按 src/elf.c 推导)")
    ap.add_argument("--log", help="从日志抓地址并自动识别二进制")
    ap.add_argument("--selfcheck", action="store_true", help="跑自检并返回 PASS/FAIL")
    ap.add_argument("--profiles", action="store_true", help="列出已登记 profile")
    ap.add_argument("--register", action="store_true", help="登记/更新 profile")
    ap.add_argument("--mksyms", help="由 dbgsym 生成符号表")
    ap.add_argument("-o", "--out", default=None, help="--mksyms 的输出路径")
    ap.add_argument("--name", help="--register 的 profile 名")
    ap.add_argument("--version", help="--register 的版本备注")
    ap.add_argument("--guest-path", action="append", default=[],
                    help="--register 的镜像内路径(可重复)")
    ap.add_argument("--debug", help="--register 的 dbgsym 路径(交叉校验用)")
    args = ap.parse_args()

    if args.selfcheck:
        return 0 if selfcheck() else 1
    if args.profiles:
        print(profiles_summary())
        return 0
    if args.mksyms:
        out = args.out or os.path.join(ROOT, "build/tmp", os.path.basename(args.mksyms) + ".syms")
        mksyms(args.mksyms, out)
        return 0
    if args.register:
        if not (args.name and args.elf):
            raise SystemExit("--register 需要 --name 与 --elf")
        e = upsert_profile(args.name, args.elf, args.syms, args.guest_path, args.version,
                           args.debug)
        print(f"已登记 {e['name']} build-id {e['build_id']} "
              f"(syms={'有' if e['syms'] else '无'})")
        return 0
    if args.log:
        return cmd_log(args.log, True)

    # 直接地址
    if not args.addrs:
        ap.print_help()
        return 1
    addrs = []
    for tok in args.addrs:
        try:
            addrs.append(int(tok, 16))
        except ValueError:
            raise SystemExit(f"不是合法十六进制地址: {tok}")
    base = int(args.base, 16) if args.base else None
    entry = None
    if args.elf:
        elf = Elf(args.elf)
        syms = Symbols(args.syms) if args.syms else None
        syms_path, debug_path = args.syms, args.debug
        if not args.syms:
            reg = load_registry()
            for p in reg["profiles"]:
                if p["build_id"] == elf.build_id and p.get("syms"):
                    entry = p
                    syms_path = os.path.join(ROOT, p["syms"])
                    debug_path = os.path.join(ROOT, p["debug"]) if p.get("debug") else None
                    syms = Symbols(syms_path)
                    break
        if syms is None:
            raise SystemExit("没有符号表:请用 --syms,或 --register 登记该 build-id")
        verify_provenance(elf, syms_path, debug_path)
        return cmd_addrs(addrs, entry, elf, syms, base, False)
    if args.profile:
        entry, elf = resolve_profile(args.profile)
        if not entry.get("syms"):
            raise SystemExit(
                f"拒绝符号化:{entry['name']} 没有匹配的符号表(build-id "
                f"{entry['build_id']})。该版本的 dbgsym 拿不到,给不出可信符号。")
        syms_path = os.path.join(ROOT, entry["syms"])
        debug_path = os.path.join(ROOT, entry["debug"]) if entry.get("debug") else None
        verify_provenance(elf, syms_path, debug_path)
        return cmd_addrs(addrs, entry, elf, Symbols(syms_path), base, False)
    print("需要 --profile 或 --elf(或 --log/--selfcheck)。可用 profile:")
    print(profiles_summary())
    return 1


if __name__ == "__main__":
    sys.exit(main())
