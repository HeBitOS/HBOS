#!/usr/bin/env python3
"""HBOS 磁盘镜像(带 MBR 的 ext2)的只读访问原语。

为什么不 mount:装机机上没有 root,而且镜像随时可能被 QEMU 用着(独占)。
debugfs 能直接读单个目录项/文件,不碰宿主挂载表;需要读文件内容时用
`debugfs -R dump`,需要读符号链接目标时直接读 inode 的数据块。

被这两个工具复用:
  · scripts/check_headless_deps.py  —— 静态完备性校验(只读)
  · scripts/fix_headless_layout.py  —— 幂等修复资源布局(debugfs -w)
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# 镜像带 MBR,ext2 从 1 MiB 开始(见 scripts/mk_bigpkg_ext2.sh)。
DEFAULT_OFFSET = 1048576

LS_RE = re.compile(
    r"^\s*(\d+)\s+([0-7]{5,7})\s+\(\d+\)\s+\d+\s+\d+\s+(\d+)\s+\S+\s+\S+\s+(.+?)\s*$")
STAT_INO_RE = re.compile(r"^Inode:\s+(\d+)\s+Type:\s+(\w+)", re.M)
# 老 inode 没有 Project 字段,所以只能从 User 行里取 Size。
STAT_SIZE_RE = re.compile(r"^User:.*Size:\s+(\d+)", re.M)
STAT_BLOCKCOUNT_RE = re.compile(r"Blockcount:\s+(\d+)")
STAT_FASTLINK_RE = re.compile(r'Fast link dest:\s+"(.*)"')
STAT_BLOCK_RE = re.compile(r"\((\d+|IND|DIND|TIND)\):(\d+)")
_INDIRECT_INDEX = {"IND": 12, "DIND": 13, "TIND": 14}
NOTFOUND_RE = re.compile(r"^(\S+): File not found by ext2_lookup", re.M)

MODE_DIR = 0o040000
MODE_FILE = 0o100000
MODE_SYMLINK = 0o120000


class ImageError(RuntimeError):
    """镜像不可读/不是本工具认识的 ext2。"""


@dataclass(frozen=True)
class Entry:
    ino: int
    kind: str          # "dir" / "file" / "symlink" / "other"
    size: int


@dataclass(frozen=True)
class Stat:
    ino: int
    kind: str
    size: int
    fast_link: str | None
    blockcount: int                 # i_blocks,0 = 没有数据块(fast symlink)
    words: tuple[int, ...]          # i_block[0..14],按索引补齐到 15 项


@dataclass
class Resolution:
    """resolve() 的结果。status 见 RESOLVE_* 常量。"""
    status: str
    path: str | None = None          # 展开后的真实路径(status=ok)
    entry: Entry | None = None
    target: str | None = None        # 断链/被展开的链接目标,便于报错
    message: str = ""

    @property
    def ok(self) -> bool:
        return self.status == "ok"


RESOLVE_OK = "ok"
RESOLVE_MISSING = "missing"
RESOLVE_DANGLING = "dangling"
RESOLVE_LOOP = "loop"
RESOLVE_NOTDIR = "notdir"
RESOLVE_NOTFILE = "notfile"

MAX_SYMLINK_DEPTH = 8  # 与内核 vfs_expand_symlinks() 的上限一致


class Image:
    """一块 ext2 镜像(可带 MBR 偏移)的只读视图 + 少量写操作。"""

    def __init__(self, path, offset: int = DEFAULT_OFFSET, block_size: int = 4096,
                 scratch: Path | None = None, writable: bool = False):
        self.path = Path(path)
        self.offset = offset
        self.block_size = block_size
        self.writable = writable
        self._tmp = Path(scratch) if scratch else Path(tempfile.mkdtemp(
            prefix="hbos-img-"))
        self._tmp.mkdir(parents=True, exist_ok=True)
        self._script = self._tmp / "debugfs.cmd"
        self._ls_cache: dict[str, dict[str, Entry] | None] = {}
        self._stat_cache: dict[str, Stat | None] = {}

        if shutil.which("debugfs") is None:
            raise ImageError("找不到 debugfs(binutils),无法读镜像")
        if not self.path.is_file():
            raise ImageError(f"镜像不存在:{self.path}")
        self.offset = self._detect_ext2_offset(self.offset)
        # 一次 stat /:验证镜像真的能打开。
        out, err = self._debugfs(["stat /"])
        if NOTFOUND_RE.search(err) or "Inode:" not in out:
            raise ImageError(
                f"{self.path} 打不开或不是 ext2(offset={self.offset}):{err.strip()[:200]}")

    def _detect_ext2_offset(self, offset: int) -> int:
        """确认超级块位置,并从 s_log_block_size 读出块大小。

        镜像带 MBR(默认 1 MiB 处);也接受裸 ext2(offset=0),免得换个
        mk_*.sh 生成的镜像就得改代码。
        """
        for candidate in dict.fromkeys([offset, 0, DEFAULT_OFFSET]):
            sb = self._read_at(candidate + 1024, 1024)  # 超级块在 fs 起点 + 1024
            if len(sb) < 64 or sb[56:58] != b"\x53\xef":  # 0xEF53 小端
                continue
            log_block = int.from_bytes(sb[24:28], "little")
            if 0 <= log_block <= 6:
                self.block_size = 1024 << log_block
            return candidate
        raise ImageError(
            f"{self.path} 在 offset={offset} 处找不到 ext2 超级块(不是 ext2 镜像?)")

    # ── 底层:debugfs ────────────────────────────────────────────────────
    @property
    def fs_arg(self) -> str:
        return f"{self.path}?offset={self.offset}"

    def _debugfs(self, commands, write: bool | None = None):
        """跑一批 debugfs 命令,返回 (stdout, stderr)。默认只读。"""
        if write is None:
            write = self.writable
        if write and not self.writable:
            raise ImageError("只读模式下的写操作被拒绝")
        self._script.write_text("\n".join(commands) + "\n", encoding="utf-8")
        # -w 必须和 -f <脚本> 一起传给 debugfs;否则它会把脚本当成镜像。
        args = ["debugfs", "-f", str(self._script), self.fs_arg]
        if write:
            args.insert(1, "-w")
        proc = subprocess.run(args, capture_output=True, text=True,
                              errors="replace")
        return proc.stdout, proc.stderr

    @staticmethod
    def _sections(stdout: str) -> dict[str, list[str]]:
        """按 debugfs 回显的 `debugfs: <cmd>` 行切分输出。"""
        sections: dict[str, list[str]] = {}
        cur: list[str] | None = None
        for line in stdout.splitlines():
            if line.startswith("debugfs: "):
                cur = []
                sections[line[len("debugfs: "):].strip()] = cur
            elif cur is not None:
                cur.append(line)
        return sections

    # ── 目录 ────────────────────────────────────────────────────────────
    def ls_many(self, dirs) -> None:
        """一次 debugfs 调用里列多个目录,填进缓存(性能用,不必须)。"""
        want = [d for d in dict.fromkeys(dirs) if d not in self._ls_cache]
        if not want:
            return
        out, err = self._debugfs([f"ls -l {d}" for d in want])
        missing = set(NOTFOUND_RE.findall(err))
        sections = self._sections(out)
        for d in want:
            if d in missing:
                self._ls_cache[d] = None
                continue
            entries: dict[str, Entry] = {}
            for line in sections.get(f"ls -l {d}", []):
                m = LS_RE.match(line)
                if not m:
                    continue
                ino, mode, size, name = int(m.group(1)), int(m.group(2), 8), \
                    int(m.group(3)), m.group(4)
                if name in (".", ".."):
                    continue
                entries[name] = Entry(ino, _kind_of(mode), size)
            self._ls_cache[d] = entries

    def ls(self, dirpath: str) -> dict[str, Entry] | None:
        """返回目录项(名字→Entry);目录不存在返回 None。"""
        key = _norm(dirpath)
        if key not in self._ls_cache:
            self.ls_many([key])
        return self._ls_cache[key]

    # ── 单个 inode ──────────────────────────────────────────────────────
    def stat_many(self, paths) -> None:
        want = [p for p in dict.fromkeys(paths) if p not in self._stat_cache]
        if not want:
            return
        out, err = self._debugfs([f"stat {p}" for p in want])
        missing = set(NOTFOUND_RE.findall(err))
        sections = self._sections(out)
        for p in want:
            body = sections.get(f"stat {p}")
            if p in missing or not body:
                self._stat_cache[p] = None
                continue
            text = "\n".join(body)
            m = STAT_INO_RE.search(text)
            if not m:
                self._stat_cache[p] = None
                continue
            size = STAT_SIZE_RE.search(text)
            fast = STAT_FASTLINK_RE.search(text)
            bcount = STAT_BLOCKCOUNT_RE.search(text)
            words = [0] * 15
            for label, value in STAT_BLOCK_RE.findall(text):
                idx = _INDIRECT_INDEX.get(label, int(label) if label.isdigit() else None)
                if idx is not None and idx < 15:
                    words[idx] = int(value)
            self._stat_cache[p] = Stat(
                int(m.group(1)), m.group(2), int(size.group(1)) if size else 0,
                fast.group(1) if fast else None,
                int(bcount.group(1)) if bcount else 0, tuple(words))

    def stat(self, path: str) -> Stat | None:
        key = _norm(path)
        if key not in self._stat_cache:
            self.stat_many([key])
        return self._stat_cache[key]

    def readlink(self, path: str) -> str | None:
        """读符号链接目标,与内核 src/ext2.c ext2_readlink() 同规则:

        i_blocks == 0 且目标 <= 60 字节 → 目标内联在 inode.i_block 里(fast);
        否则 i_block[0] 指向存放目标的数据块(slow)。debugfs 只在部分 fast
        symlink 上打印 "Fast link dest",所以两种都要自己还原。
        """
        st = self.stat(path)
        if not st or st.kind != "symlink" or st.size <= 0 or st.size > 4095:
            return None
        if st.fast_link is not None:
            return st.fast_link
        if st.blockcount == 0 and st.size <= 60:
            blob = b"".join(w.to_bytes(4, "little") for w in st.words)
            return blob[:st.size].decode("utf-8", "replace")
        if st.blockcount > 0 and st.words[0]:
            return self._read_at(self.offset + st.words[0] * self.block_size,
                                 st.size).decode("utf-8", "replace")
        return None

    def _read_at(self, abs_offset: int, size: int) -> bytes:
        """从镜像里裸读一段字节。

        每次都新开句柄:工作区挂在 fuseblk(NTFS-3G)上,长命句柄在别的进程
        写过同一个文件之后仍然会读到旧页(实测:debugfs 刚写完的符号链接
        数据块,老句柄读出全 0,新开句柄读到正确内容)。
        """
        with self.path.open("rb") as fh:
            fh.seek(abs_offset)
            return fh.read(size)

    # ── 路径解析(逐段展开符号链接,与内核 vfs_expand_symlinks 同语义)────
    def resolve(self, path: str) -> Resolution:
        parts = [p for p in _norm(path).split("/") if p]
        cur = "/"
        depth = 0
        saw_link = False
        i = 0
        while i < len(parts):
            nxt = cur.rstrip("/") + "/" + parts[i]
            entries = self.ls(cur)
            if entries is None:
                return Resolution(RESOLVE_MISSING, message=f"{cur} 不存在")
            ent = entries.get(parts[i])
            if ent is None:
                return Resolution(
                    RESOLVE_DANGLING if saw_link else RESOLVE_MISSING,
                    message=f"{nxt} 不存在" + ("(链接目标悬空)" if saw_link else ""))
            final = i == len(parts) - 1
            if ent.kind == "symlink":
                depth += 1
                if depth > MAX_SYMLINK_DEPTH:
                    return Resolution(RESOLVE_LOOP,
                                      message=f"{nxt} 符号链接超过 {MAX_SYMLINK_DEPTH} 层")
                target = self.readlink(nxt)
                if target is None:
                    return Resolution(RESOLVE_LOOP,
                                      message=f"{nxt} 的链接目标读不出来")
                saw_link = True
                if target.startswith("/"):
                    cur = "/"
                    new_parts = [p for p in target.split("/") if p]
                else:
                    new_parts = [p for p in target.split("/") if p]
                parts = new_parts + parts[i + 1:]
                i = 0
                continue
            if final:
                return Resolution(RESOLVE_OK, nxt, ent)
            if ent.kind != "dir":
                return Resolution(RESOLVE_NOTDIR,
                                  message=f"{nxt} 不是目录(不能作为路径中段)")
            cur = nxt
            i += 1
        # path 是 "/"
        return Resolution(RESOLVE_OK, cur, None)

    # ── 文件内容 ────────────────────────────────────────────────────────
    def dump(self, path: str, dest: Path) -> Path:
        """把镜像里的普通文件导出到宿主(dest 必须是文件路径)。"""
        dest = Path(dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        out, err = self._debugfs([f"dump {path} {dest}"])
        if not dest.is_file() or dest.stat().st_size == 0:
            raise ImageError(f"dump {path} 失败:{out.strip()} {err.strip()}")
        return dest

    # ── 写操作(仅 writable=True 时可用)───────────────────────────────
    def mkdirs(self, dirs) -> None:
        cmds = []
        for d in dict.fromkeys(_norm(x) for x in dirs):
            parts = [p for p in d.split("/") if p]
            cur = ""
            for p in parts:
                cur += "/" + p
                cmds.append(f"mkdir {cur}")
        if cmds:
            self._debugfs(cmds, write=True)

    def symlink(self, path: str, target: str, force: bool = True) -> None:
        path = _norm(path)
        cmds = [f"rm {path}"] if force else []
        cmds.append(f"symlink {path} {target}")
        self._debugfs(cmds, write=True)
        self._ls_cache.pop(_dirname(path), None)
        self._stat_cache.pop(path, None)

    def close(self) -> None:
        """关闭镜像句柄(现在每次裸读都新开句柄,这里只是接口保留)。"""


def _kind_of(mode: int) -> str:
    if mode & MODE_DIR == MODE_DIR:
        return "dir"
    if mode & MODE_SYMLINK == MODE_SYMLINK:
        return "symlink"
    if mode & MODE_FILE == MODE_FILE:
        return "file"
    return "other"


def _norm(path: str) -> str:
    path = "/" + "/".join(p for p in str(path).split("/") if p and p != ".")
    return path


def _dirname(path: str) -> str:
    parent = path.rsplit("/", 1)[0]
    return parent or "/"
