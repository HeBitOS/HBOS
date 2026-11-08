#!/usr/bin/env python3
"""chromium-headless-shell 在 HBOS 镜像里的静态完备性校验(不启动 QEMU)。

回答一个问题:把镜像丢给 QEMU 之前,该在的文件/符号是不是都在?
检查四件事:
  1. ELF 依赖闭包  —— shell 的 NEEDED 逐层展开,每个 soname 在镜像的库搜索
     路径里找得到(ld-linux/libc/libm 由 HBOS 自带,不算缺失)。
  2. 固定依赖清单  —— docs/wiki/HEADLESS-DEPS.md 里钉死的那些包必须都在,
     同时报告"二进制新引入但清单没记"的漂移。
  3. 资源文件      —— shell 自己目录里必须有的数据文件;符号链接必须能解析
     到真实存在的非空文件(悬空链接单独报)。
  4. 版本符号      —— 已知踩坑的库必须真的导出 shell 需要的符号
     (hb_face_set_get_table_tags_func 那类 undefined symbol 的根因)。

缺失即退出码非 0(1=检查失败,2=工具/环境错误),可以当安装收尾的闸门。

用法:
  python3 scripts/check_headless_deps.py                    # 查默认镜像
  python3 scripts/check_headless_deps.py --img build/x.img --expect my.json
  python3 scripts/check_headless_deps.py --depth 0           # 只看直接 NEEDED
  python3 scripts/check_headless_deps.py --json              # 机器可读
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from headless_img import (DEFAULT_OFFSET, Image, ImageError,  # noqa: E402
                          RESOLVE_OK, RESOLVE_DANGLING, RESOLVE_LOOP,
                          RESOLVE_MISSING, RESOLVE_NOTDIR)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXPECT = Path(__file__).resolve().parent / "headless_deps_expect.json"

# HBOS 的库搜索顺序,见 src/tools/app.c build_linux_env()。
SYSTEM_DIRS = ["/lib64", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
               "/usr/lib", "/lib"]
OPT_SUBDIRS = ["/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib"]

# nm -P 的输出里,大写字母 = 全局符号(能被子进程解析的只有这些)。
DEFINED_TYPES = set("TDBRWVSiG")

NEEDED_RE = re.compile(r"\(NEEDED\).*\[(.*?)\]")
VERSION_SUFFIX_RE = re.compile(r"@.*$")


class Report:
    """收集 [ok]/[FAIL]/[WARN] 行,最后统一打印。"""

    def __init__(self):
        self.rows: list[tuple[str, str, str]] = []

    def add(self, status: str, section: str, text: str):
        self.rows.append((status, section, text))

    def ok(self, section, text): self.add("ok", section, text)
    def fail(self, section, text): self.add("fail", section, text)
    def warn(self, section, text): self.add("warn", section, text)
    def info(self, section, text): self.add("info", section, text)

    @property
    def failures(self):
        return [r for r in self.rows if r[0] == "fail"]

    def print(self, stream=sys.stdout):
        tag = {"ok": "[ ok ]", "fail": "[FAIL]", "warn": "[WARN]",
               "info": "[info]"}
        section = None
        for status, sec, text in self.rows:
            if status == "info":
                if sec != section:
                    stream.write(f"\n=== {sec} ===\n")
                    section = sec
                stream.write(f"[info] {text}\n")
            else:
                stream.write(f"{tag[status]} {text}\n")


def run(cmd) -> str:
    env = dict(os.environ, LC_ALL="C", LANG="C")
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                          env=env)
    if proc.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} 失败:{proc.stderr.strip()[:200]}")
    return proc.stdout


def elf_needed(path: Path) -> list[str]:
    out = run(["readelf", "-d", "-W", str(path)])
    return [m.group(1) for m in (NEEDED_RE.search(l) for l in out.splitlines())
            if m]


def nm_symbols(path: Path) -> tuple[set[str], set[str]]:
    """返回 (未定义符号, 已定义符号)。"""
    out = run(["nm", "-D", "-P", "--", str(path)])
    undef, defined = set(), set()
    for line in out.splitlines():
        fields = line.split()
        if len(fields) < 2:
            continue
        name, kind = fields[0], fields[1]
        name = VERSION_SUFFIX_RE.sub("", name)
        if kind == "U" or (kind in ("w", "v") and len(fields) < 3):
            undef.add(name)
        elif kind in DEFINED_TYPES:
            defined.add(name)
    return undef, defined


class Checker:
    def __init__(self, img: Image, expect: dict, maxdepth: int, workdir: Path,
                 with_symbols=True):
        self.img = img
        self.exp = expect
        self.maxdepth = maxdepth
        self.workdir = workdir
        self.with_symbols = with_symbols
        self.dumps: dict[str, Path] = {}
        self.search_dirs: list[str] = []
        self.report = Report()

    # ── 基础设施 ────────────────────────────────────────────────────────
    def build_search_dirs(self):
        dirs = list(SYSTEM_DIRS)
        opt = self.img.ls("/opt") or {}
        for name in sorted(opt):
            for sub in OPT_SUBDIRS:
                dirs.append(f"/opt/{name}{sub}")
        self.img.ls_many(dirs)  # 一次 debugfs 调用列完全部搜索目录
        self.search_dirs = [d for d in dirs if self.img.ls(d) is not None]
        self.report.info("环境", f"库搜索路径 {len(self.search_dirs)} 个目录"
                                 f"(HBOS 顺序:系统目录 → /opt/<pkg>/*)")

    def find_lib(self, soname: str):
        """按 HBOS 的搜索顺序找 soname,返回 (镜像路径, Resolution) 或 None。"""
        for d in self.search_dirs:
            entries = self.img.ls(d) or {}
            if soname in entries:
                res = self.img.resolve(f"{d}/{soname}")
                return (f"{d}/{soname}", res)
        return None

    def find_lib_all(self, soname: str) -> list[str]:
        """所有命中这个 soname 的搜索目录(顺序即搜索顺序)。"""
        return [f"{d}/{soname}" for d in self.search_dirs
                if soname in (self.img.ls(d) or {})]

    def local_copy(self, resolved_path: str) -> Path:
        if resolved_path in self.dumps:
            return self.dumps[resolved_path]
        dest = self.workdir / "files" / resolved_path.lstrip("/").replace("/", "_")
        self.img.dump(resolved_path, dest)
        self.dumps[resolved_path] = dest
        return dest

    # ── 1. ELF 依赖闭包 ─────────────────────────────────────────────────
    def check_closure(self, shell_local: Path):
        provided = set(self.exp.get("hbos_provided_sonames", []))
        seen: dict[str, int] = {}
        found: dict[str, str] = {}
        missing: list[tuple[str, int]] = []
        broken: list[tuple[str, str]] = []
        hbos: set[str] = set()

        queue = [(s, 1) for s in elf_needed(shell_local)]
        direct = [s for s, _d in queue]
        while queue:
            soname, depth = queue.pop(0)
            if soname in seen:
                continue
            seen[soname] = depth
            hit = self.find_lib(soname)
            if hit is None:
                # 镜像里没有的才可能是 HBOS 自带;镜像里有的必须用镜像那份,
                # 否则会把"包被删了"误判成"系统自带"。
                if soname in provided:
                    hbos.add(soname)
                else:
                    missing.append((soname, depth))
                continue
            path, res = hit
            if not res.ok or res.entry is None or res.entry.kind != "file" \
                    or res.entry.size == 0:
                broken.append((soname, f"{path}: {res.status} {res.message}"))
                continue
            found[soname] = res.path
            if depth < self.maxdepth:
                for nxt in elf_needed(self.local_copy(res.path)):
                    if nxt not in seen:
                        queue.append((nxt, depth + 1))

        self.report.info("1. ELF 依赖闭包",
                         f"直接 NEEDED {len(direct)} 个,闭包(深度≤{self.maxdepth})"
                         f"共 {len(seen)} 个")
        for soname in direct:
            if soname in found:
                self.report.ok("1. ELF 依赖闭包",
                               f"{soname} → {self.describe(found[soname], soname)}")
            elif soname in hbos:
                self.report.ok("1. ELF 依赖闭包",
                               f"{soname} (HBOS 自带,镜像里不需要)")
            elif any(s == soname for s, _d in missing):
                self.report.fail("1. ELF 依赖闭包", f"{soname} 缺失(直接 NEEDED)")
            else:
                self.report.fail("1. ELF 依赖闭包",
                                 f"{soname} 命中但不可用:{broken}")
        for soname, depth in missing:
            if soname in direct:
                continue
            self.report.fail("1. ELF 依赖闭包",
                             f"{soname} 缺失(深度 {depth} 的传递依赖)")
        for soname, text in broken:
            self.report.fail("1. ELF 依赖闭包", f"{soname} 命中但不可用:{text}")
        return direct, found, missing, hbos

    def describe(self, resolved: str, soname: str) -> str:
        if Path(resolved).name == soname:
            return resolved
        return f"{resolved}(经 {soname} 符号链接)"

    # ── 2. 固定依赖清单 ─────────────────────────────────────────────────
    def check_pinned(self, direct: list[str]):
        pinned = self.exp.get("required_sonames", [])
        provided = set(self.exp.get("hbos_provided_sonames", []))
        missing = []
        for soname in pinned:
            if self.find_lib(soname) is None and soname not in provided:
                missing.append(soname)
        self.report.info("2. 固定依赖清单",
                         f"docs/wiki/HEADLESS-DEPS.md 钉住 {len(pinned)} 个 soname")
        if missing:
            for soname in missing:
                self.report.fail("2. 固定依赖清单", f"{soname} 不在镜像里")
        else:
            self.report.ok("2. 固定依赖清单",
                           f"{len(pinned)} 个 soname 全部命中")
        drift = [s for s in direct if s not in pinned and s not in provided]
        if drift:
            self.report.warn("2. 固定依赖清单",
                             "二进制了新依赖但清单没记(请更新 expect+deps 文档):"
                             + " ".join(sorted(drift)))

        # 同一个 soname 被多个包提供时,实际用哪一份取决于 /opt 的 readdir 顺序
        # (内核 lib_path_scan_opt 就是按 readdir 拼路径的),属于随机性来源。
        for soname in pinned:
            hits = self.find_lib_all(soname)
            resolved = []
            for h in hits:
                r = self.img.resolve(h)
                if r.ok and r.path not in resolved:
                    resolved.append(r.path)
            if len(resolved) > 1:
                self.report.warn("2. 固定依赖清单",
                                 f"{soname} 有 {len(resolved)} 份不同实现,"
                                 f"实际加载取决于 /opt 顺序:" + " | ".join(resolved))

    # ── 3. 资源文件 ─────────────────────────────────────────────────────
    def check_resources(self):
        res_dir = self.exp["resource_dir"].rstrip("/")
        required = self.exp.get("required_resources", [])
        self.report.info("3. 资源文件", f"{res_dir} 下必须存在 {len(required)} 项")
        for rel in required:
            path = f"{res_dir}/{rel}"
            res = self.img.resolve(path)
            if res.status == RESOLVE_DANGLING:
                self.report.fail("3. 资源文件", f"{rel} 悬空符号链接:{res.message}")
                continue
            if res.status == RESOLVE_LOOP:
                self.report.fail("3. 资源文件", f"{rel} 符号链接成环:{res.message}")
                continue
            if not res.ok or res.entry is None:
                self.report.fail("3. 资源文件", f"{rel} 缺失")
                continue
            if res.entry.kind != "file":
                self.report.fail("3. 资源文件",
                                 f"{rel} 是 {res.entry.kind},不是普通文件")
                continue
            if res.entry.size == 0:
                self.report.fail("3. 资源文件", f"{rel} 是 0 字节空文件")
                continue
            how = "自身目录内" if res.path == path else f"符号链接 → {res.path}"
            self.report.ok("3. 资源文件",
                           f"{rel} ({res.entry.size} 字节,{how})")

        optional = self.exp.get("optional_resources", [])
        helper_dirs = self.exp.get("helper_search_dirs", [])
        if optional:
            self.report.info("3. 资源文件",
                             f"另有 {len(optional)} 个 dlopen/exec 辅助文件(只报 WARN)")
        for rel in optional:
            path = f"{res_dir}/{rel}"
            res = self.img.resolve(path)
            if res.ok and res.entry is not None and res.entry.size > 0:
                self.report.ok("3. 资源文件(可选)",
                               f"{rel} ({res.entry.size} 字节,"
                               + ("自身目录内" if res.path == path
                                  else f"符号链接 → {res.path}") + ")")
                continue
            found = [f"{d}/{rel}" for d in helper_dirs
                     if rel in (self.img.ls(d) or {})]
            hint = (";在别的包目录里有:" + " ".join(found[:2])) if found else ""
            self.report.warn("3. 资源文件(可选)",
                             f"{rel} 不在 shell 目录({res.status}){hint}")

    # ── 4. 版本符号 ─────────────────────────────────────────────────────
    def check_symbols(self, shell_local: Path):
        if not self.with_symbols:
            self.report.warn("4. 版本符号", "已用 --no-symbols 跳过")
            return
        explicit = self.exp.get("required_symbols", {})
        undef_shell, _def = nm_symbols(shell_local)
        self.report.info("4. 版本符号",
                         f"shell 未定义动态符号 {len(undef_shell)} 个"
                         f"(其中 hb_* {sum(1 for s in undef_shell if s.startswith('hb_'))} 个)")

        for soname, wanted in explicit.items():
            defined = self.lib_symbols(soname)
            if defined is None:
                continue  # 库本身缺失,第 1/2 节已经报过
            missing = [s for s in wanted if s not in defined]
            if missing:
                self.report.fail("4. 版本符号",
                                 f"{soname} 缺 {len(missing)} 个必检符号:"
                                 + " ".join(missing[:6]))
            else:
                self.report.ok("4. 版本符号",
                               f"{soname} 必检符号 {len(wanted)}/{len(wanted)} 全部导出")

        for fam in self.exp.get("symbol_families", []):
            flags = fam.get("prefixes", [])
            excludes = tuple(fam.get("exclude_prefixes", []))
            libs = fam.get("libs") or [fam["owner"]]
            wanted = {s for s in undef_shell
                      if any(s.startswith(p) for p in flags)
                      and not any(s.startswith(e) for e in excludes)}
            if not wanted:
                continue
            defined: set[str] = set()
            resolved_all = True
            for lib in libs:
                syms = self.lib_symbols(lib)
                if syms is None:
                    resolved_all = False
                    continue
                defined |= syms
            if not resolved_all:
                continue
            missing = sorted(wanted - defined)
            label = f"{'/'.join(libs)}"
            if missing:
                self.report.fail("4. 版本符号",
                                 f"符号族 {flags}({label})需要 {len(wanted)} 个,"
                                 f"缺 {len(missing)} 个:" + " ".join(missing[:6]))
            else:
                self.report.ok("4. 版本符号",
                               f"符号族 {flags}({label})需要 {len(wanted)} 个,全部导出")

    def lib_symbols(self, soname: str) -> set[str] | None:
        hit = self.find_lib(soname)
        if hit is None:
            self.report.fail("4. 版本符号", f"{soname} 不在镜像里,无法查符号")
            return None
        path, res = hit
        if not res.ok or res.entry is None or res.entry.kind != "file":
            self.report.fail("4. 版本符号", f"{soname} 命中但不可用:{res.message}")
            return None
        _undef, defined = nm_symbols(self.local_copy(res.path))
        return defined


def load_expect(path: Path) -> dict:
    if not path.is_file():
        raise SystemExit(f"[check-headless-deps] 找不到期望清单:{path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"[check-headless-deps] 期望清单不是合法 JSON:{exc}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--img", help="镜像路径(默认取 expect 里的 image)")
    ap.add_argument("--offset", type=int, help="ext2 在镜像内的字节偏移")
    ap.add_argument("--expect", default=str(DEFAULT_EXPECT),
                    help="期望清单 JSON(可替换成篡改版做自检)")
    ap.add_argument("--shell", help="覆盖 shell 在镜像里的路径")
    ap.add_argument("--depth", type=int, default=3,
                    help="依赖闭包深度,0=只看直接 NEEDED(默认 3)")
    ap.add_argument("--no-symbols", action="store_true", help="跳过版本符号检查")
    ap.add_argument("--json", action="store_true", help="输出机器可读 JSON")
    ap.add_argument("--workdir", help="解包临时目录(默认 build/tmp/headless-check)")
    args = ap.parse_args(argv)

    exp = load_expect(Path(args.expect))
    img_path = Path(args.img or exp.get("image", "build/c3-chromium.img"))
    if not img_path.is_absolute():
        img_path = ROOT / img_path
    offset = args.offset if args.offset is not None else exp.get("offset", DEFAULT_OFFSET)
    shell = args.shell or exp["shell"]
    workdir = Path(args.workdir) if args.workdir else ROOT / "build/tmp/headless-check"
    workdir.mkdir(parents=True, exist_ok=True)

    tmpdir = Path(tempfile.mkdtemp(prefix="check-", dir=workdir))
    try:
        img = Image(img_path, offset=offset, scratch=tmpdir)
    except ImageError as exc:
        print(f"[check-headless-deps] 环境错误:{exc}", file=sys.stderr)
        return 2

    try:
        chk = Checker(img, exp, args.depth, tmpdir,
                      with_symbols=not args.no_symbols)
        chk.build_search_dirs()
        res = img.resolve(shell)
        if not res.ok or res.entry is None:
            chk.report.add("fail", "0. 镜像",
                           f"headless shell {shell} 不存在({res.status})")
            chk.report.print()
            return 2
        chk.report.info("0. 镜像",
                        f"{img_path} (offset={offset}) shell={res.path} "
                        f"{res.entry.size} 字节")
        shell_local = chk.local_copy(res.path)
        direct, found, missing, hbos = chk.check_closure(shell_local)
        chk.check_pinned(direct)
        chk.check_resources()
        chk.check_symbols(shell_local)

        failures = chk.report.failures
        if args.json:
            print(json.dumps({
                "image": str(img_path),
                "shell": res.path,
                "direct_needed": direct,
                "resolved": {k: v for k, v in found.items()},
                "missing_libs": [s for s, _d in missing],
                "hbos_provided": sorted(hbos),
                "warnings": [t for _s, _sec, t in chk.report.rows if _s == "warn"],
                "failures": [t for _s, _sec, t in failures],
                "ok": not failures,
            }, ensure_ascii=False, indent=2))
        else:
            chk.report.print()
            print(f"\n[check-headless-deps] 结论:"
                  f"{'PASS 无缺失' if not failures else f'FAIL 共 {len(failures)} 项缺失/异常'}"
                  f"(直接 NEEDED {len(direct)} 个,解析到 {len(found)} 个,"
                  f"HBOS 自带 {len(hbos)} 个)")
        return 1 if failures else 0
    finally:
        img.close()
        if not os.environ.get("CHECK_HEADLESS_KEEP"):
            shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
