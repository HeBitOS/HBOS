#!/usr/bin/env python3
"""把 .deb 装进 HBOS 磁盘镜像，并（可选）立刻开 QEMU 看效果。

流程：
  1) HPT/tools/pkg2hax.py 生成 manifest（拿包名 / install_prefix / entrypoint）
  2) dpkg-deb -x 解出目录树
  3) 用 debugfs -f 一次性写进镜像（镜像带 MBR，ext2 从 1 MiB 开始）
  4) 可选：直接调 qemu_session.py 启动，进去就能看到文件

用法：
  python3 scripts/install_app.py --deb ./foo.deb
  python3 scripts/install_app.py --deb ./foo.deb --no-launch
  python3 scripts/install_app.py --img-get /opt/foo/usr/bin/foo -o /tmp/foo
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OFFSET = 1048576  # MBR 之后，见 scripts/mk_bigpkg_ext2.sh


def fs_arg(img: Path) -> str:
    return f"{img}?offset={OFFSET}"


def debugfs(img: Path, commands, write=True):
    script = ROOT / "build/tmp/debugfs.cmd"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("\n".join(commands) + "\n")
    args = ["debugfs", "-w", "-f", str(script), fs_arg(img)] if write else \
           ["debugfs", "-f", str(script), fs_arg(img)]
    return subprocess.run(args, capture_output=True, text=True)


def ensure_dirs(img: Path, dirs):
    cmds = []
    for d in sorted(dirs):
        parts = [p for p in d.strip("/").split("/") if p]
        cur = ""
        for part in parts:
            cur += "/" + part
            cmds.append(f"mkdir {cur}")
    if cmds:
        debugfs(img, cmds)


def install(deb: Path, img: Path):
    out = ROOT / "build/tmp/app-out"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    subprocess.run([sys.executable, str(ROOT / "HPT/tools/pkg2hax.py"),
                    str(deb), "-o", str(out)], check=True, stdout=subprocess.DEVNULL)
    manifest = next(out.glob("*.manifest.json"))
    meta = json.loads(manifest.read_text(encoding="utf-8"))
    prefix = meta.get("install_prefix") or f"/opt/{meta['name']}"
    tree = ROOT / "build/tmp/app-tree"
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    subprocess.run(["dpkg-deb", "-x", str(deb), str(tree)], check=True)

    dirs, files, links = set(), [], []
    for path in sorted(tree.rglob("*")):
        rel = path.relative_to(tree).as_posix()
        dest = f"{prefix.rstrip('/')}/{rel}"
        dirs.add(dest.rsplit("/", 1)[0] if path.is_file() else dest)
        if path.is_symlink():
            links.append((dest, os.readlink(path)))
        elif path.is_file():
            files.append((path, dest))

    ensure_dirs(img, dirs)
    cmds = []
    for src, dest in files:
        cmds.append(f"rm {dest}")
        cmds.append(f"write {src} {dest}")
        cmds.append(f"sif {dest} mode 0100{oct(src.stat().st_mode & 0o777)[2:]}")
    for dest, target in links:
        cmds.append(f"rm {dest}")
        cmds.append(f"symlink {dest} {target}")
    result = debugfs(img, cmds)
    if "Filesystem not open" in result.stdout + result.stderr:
        raise SystemExit(f"debugfs 打不开镜像：{result.stdout[:200]}")

    entry = meta.get("entrypoint") or ""
    print(f"[install-app] {meta['name']} {meta.get('version','')} -> {prefix}"
          f"  ({len(files)} files, {len(links)} links)")
    hint = f"{prefix.rstrip('/')}/{entry.lstrip('/')}" if entry else ""
    if hint:
        print(f"[install-app] entry: {hint}")
        print(f"[install-app] 进入 HBOS 后执行： run {hint}")
    else:
        print("[install-app] 这是库包（manifest 无 entrypoint），只能用 ls/cat 查看或用 LD_LIBRARY_PATH 引用")

    # 把"进去怎么跑"写进镜像，用户开机就能看到
    note = ROOT / "build/tmp/last-install.txt"
    note.parent.mkdir(parents=True, exist_ok=True)
    note.write_text(f"{meta['name']} {meta.get('version', '')} -> {prefix}\n"
                    + (f"run {hint}\n" if hint else "library package (no entrypoint)\n"))
    ensure_dirs(img, {"/etc/hpt"})
    debugfs(img, ["rm /etc/hpt/last-install.txt",
                  f"write {note} /etc/hpt/last-install.txt",
                  "sif /etc/hpt/last-install.txt mode 0100644"])
    return meta, prefix, entry


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deb")
    ap.add_argument("--img", default="build/c3-chromium.img")
    ap.add_argument("--iso", default="build/hbos-bios.iso")
    ap.add_argument("--mem", default="4096M")
    ap.add_argument("--no-launch", action="store_true")
    ap.add_argument("--img-ls")
    ap.add_argument("--img-get")
    ap.add_argument("--img-put")
    ap.add_argument("--img-dest")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()

    img = Path(args.img)
    if args.img_ls:
        r = debugfs(img, [f"ls -l {args.img_ls}"], write=False)
        print(r.stdout)
        return
    if args.img_put:
        dest = args.img_dest
        ensure_dirs(img, {dest.rsplit("/", 1)[0]})
        r = debugfs(img, [f"rm {dest}", f"write {args.img_put} {dest}",
                          f"sif {dest} mode 0100644"])
        print(r.stdout or r.stderr)
        return
    if args.img_get:
        r = debugfs(img, [f"dump {args.img_get} {args.output or '/tmp/out.bin'}"],
                    write=False)
        print(r.stdout or r.stderr)
        return
    if not args.deb:
        raise SystemExit("需要 --deb")

    meta, prefix, entry = install(Path(args.deb), img)
    if args.no_launch:
        return

    name = meta["name"]
    inner = f"ls {prefix}"
    if entry:
        inner += f"; run {prefix.rstrip('/')}/{entry.lstrip('/')}"
    cmd = ["timeout", "420", sys.executable, str(ROOT / "scripts/qemu_session.py"),
           args.iso, "--disk", str(img), "--mem", args.mem,
           "--timeout", "60", "--boot-timeout", "90",
           "--log", "build/tmp/install-app.log", "--", f"ls {prefix}"]
    print("[install-app] 启动 QEMU …（串口日志 build/tmp/install-app.log）")
    subprocess.run(cmd)


if __name__ == "__main__":
    main()
