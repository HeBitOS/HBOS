#!/usr/bin/env python3
"""Build an HPT `file` repository (Packages + pool of HPK2 payloads) out of
Debian .deb files, preserving the dependency graph.

This is the test-fixture generator behind C1/C3: the repository lives on an
ext2 data disk, so packages are not limited by ramfs (128 KiB/slot, 128 slots)
or by the HPT network buffer (2 MiB).

usage:
    mk_deb_repo.py --out build/repo-c1 a.deb b.deb ...

Extra files (test binaries, data blobs) are copied in with --extra
`src:dst-under-image` pairs; the destination is relative to the image root
(the repo itself always lands in <out>/repo when --root is given).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def convert(deb: Path, out_dir: Path) -> dict:
    """Run the existing converter and return its manifest."""
    subprocess.run(
        [sys.executable, str(REPO_ROOT / "HPT/tools/pkg2hax.py"),
         str(deb), "-o", str(out_dir)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    manifest = out_dir / f"{deb.stem.split('_')[0]}.manifest.json"
    if not manifest.is_file():
        raise SystemExit(f"converter produced no manifest for {deb}")
    return json.loads(manifest.read_text(encoding="utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="repository directory to fill")
    ap.add_argument("--root", help="image root; repo goes to <root>/repo")
    ap.add_argument("--extra", action="append", default=[],
                    help="SRC:DEST pair, DEST relative to the image root")
    ap.add_argument("--with-libc6", action="store_true",
                    help="also publish the built-in libc6 record "
                         "(build/hpt-repo/libc6.hax) so Debian packages can "
                         "resolve their libc6 dependency")
    ap.add_argument("debs", nargs="+")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    records = []
    if args.with_libc6:
        builtin = REPO_ROOT / "build/hpt-repo"
        hax = builtin / "libc6.hax"
        packages = builtin / "Packages"
        if not hax.is_file() or not packages.is_file():
            raise SystemExit("build/hpt-repo/libc6.hax missing: run `make bios-iso`")
        shutil.copy2(hax, out / hax.name)
        for line in packages.read_text(encoding="utf-8").splitlines():
            if line.startswith("libc6|"):
                records.append(line + "\n")
                print(f"[repo] {'libc6':20s} {hax.stat().st_size:>9d} B  (built-in)")
                break
        else:
            raise SystemExit("no libc6 record in build/hpt-repo/Packages")
    for deb_name in args.debs:
        deb = Path(deb_name)
        manifest = convert(deb, out)
        name = manifest["name"]
        payload = out / f"{name}.hax"
        digest = hashlib.sha256(payload.read_bytes()).hexdigest()
        deps = manifest.get("depends") or []
        desc = (manifest.get("description") or f"Debian package {name}")
        desc = desc.replace("|", "/").replace("\n", " ")
        records.append("|".join([
            name,
            manifest.get("version", "0"),
            manifest.get("architecture", "x86_64"),
            str(payload.stat().st_size),
            digest,
            payload.name,
            ",".join(deps),
            desc,
        ]) + "\n")
        print(f"[repo] {name:20s} {payload.stat().st_size:>9d} B  deps={','.join(deps) or '-'}")

    (out / "Packages").write_text("".join(records), encoding="utf-8")

    # HPT 的 validate_index() 要求依赖闭包自洽：index 里任何一条记录的
    # Depends 指向不存在的包，整个索引都会被判为"格式错误"。测试仓库是
    # 有选择地收包的（例如跳过只提供资源的 chromium-common、只用于构建
    # 脚本的 perl），所以这里把无法满足的依赖剪掉并显式列出来——比让
    # 索引整体不可用要好，也比悄悄改 depends 更有据可查。
    names = {r.split("|", 1)[0] for r in records}
    pruned = []
    fixed = []
    for record in records:
        fields = record.rstrip("\n").split("|")
        deps = [d for d in fields[6].split(",") if d]
        kept = []
        for dep in deps:
            if dep in names:
                kept.append(dep)
            else:
                pruned.append((fields[0], dep))
        fields[6] = ",".join(kept)
        if len(fields[6]) > 2000:
            print(f"[repo] warn {fields[0]}: dependency list truncated")
            fields[6] = fields[6][:2000].rsplit(",", 1)[0]
        if len(fields[7]) > 180:
            fields[7] = fields[7][:180]
        if len(fields[1]) > 38:
            fields[1] = fields[1][:38]
        fixed.append("|".join(fields) + "\n")
    (out / "Packages").write_text("".join(fixed), encoding="utf-8")
    for pkg, dep in pruned:
        print(f"[repo] prune {pkg}: depends on absent {dep}")

    if args.root:
        root = Path(args.root)
        repo_dst = root / "repo"
        if repo_dst.exists():
            shutil.rmtree(repo_dst)
        repo_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(out, repo_dst)
        for pair in args.extra:
            src, dst = pair.split(":", 1)
            target = root / dst.lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)
            print(f"[repo] extra {src} -> {dst}")
    print(f"[repo] {len(records)} packages -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
