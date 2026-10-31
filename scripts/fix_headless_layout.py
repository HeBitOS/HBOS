#!/usr/bin/env python3
"""把 headless shell 需要的资源布局固化进镜像(幂等,可重复执行)。

背景:chromium-headless-shell 只带自己那三个 .pak;icudtl.dat /
snapshot_blob.bin / v8_context_snapshot.bin / resources.pak / locales 都在
chromium-common 包里。Debian 在真实系统上靠 /usr/lib/chromium 是同一个目录
解决,HBOS 里两个包各自装到 /opt/<包名>/,于是必须在 headless 目录里补符号
链接 —— 否则运行时报的是 "Invalid file descriptor to ICU data received"、
"Error loading V8 startup snapshot file" 这种看不懂的错。

为什么是符号链接而不是复制(实测过内核代码,风险可控):
  · src/vfs.c vfs_expand_symlinks() 会展开路径中的每一段(含最终段),
    上限 8 层;这里链长最多 2 层(headless → chromium-common → 真文件)。
  · src/ext2.c ext2_readlink() 同时支持 fast(≤60 字节内联在 inode)与
    slow(目标放在一个数据块里)两种布局,本脚本生成的链接两种都有
    (v8_context_snapshot.bin 的目标 61 字节 → slow)。
  · debugfs symlink 写的目录项 file_type=7,正是 ext2.c 要求的值
    (否则重启后会被当成普通文件读)。
  · 代价对比:复制要多约 12 MB(icudtl.dat 10.4M + v8 snapshot 0.7M +
    snapshot_blob 0.36M),而链接几乎不占空间;升级 chromium-common 时
    链接还自动跟着走。

幂等:先读镜像现状,只补/改"缺的、指向不对的";已经正确的原样跳过。
用法:
  scripts/fix_headless_layout.sh                     # 修默认镜像
  scripts/fix_headless_layout.sh --dry-run           # 只看要改什么
  python3 scripts/fix_headless_layout.py --expect my.json
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from headless_img import DEFAULT_OFFSET, Image, ImageError  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXPECT = Path(__file__).resolve().parent / "headless_deps_expect.json"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--img")
    ap.add_argument("--offset", type=int)
    ap.add_argument("--expect", default=str(DEFAULT_EXPECT))
    ap.add_argument("--dry-run", action="store_true", help="只报告,不修改镜像")
    ap.add_argument("--include-optional", action="store_true",
                    help="连 dlopen/exec 的辅助文件(ANGLE/SwiftShader/crashpad)一起补")
    args = ap.parse_args(argv)

    exp = json.loads(Path(args.expect).read_text(encoding="utf-8"))
    img_path = Path(args.img or exp["image"])
    if not img_path.is_absolute():
        img_path = ROOT / img_path
    offset = args.offset if args.offset is not None else exp.get("offset", DEFAULT_OFFSET)
    res_dir = exp["resource_dir"].rstrip("/")
    src_dir = exp["resource_source_dir"].rstrip("/")

    try:
        img = Image(img_path, offset=offset, writable=not args.dry_run)
    except ImageError as exc:
        print(f"[fix-headless-layout] 环境错误:{exc}", file=sys.stderr)
        return 2

    changed = skipped = failed = 0
    try:
        print(f"[fix-headless-layout] {img_path} (offset={img.offset}) "
              f"{res_dir} → {src_dir}")
        plan: list[tuple[str, str, str, str, bool]] = []  # rel,dest,target,why,force
        wanted = list(exp.get("required_resources", []))
        if args.include_optional:
            wanted += list(exp.get("optional_resources", []))
        for rel in wanted:
            if rel == Path(exp["shell"]).name:
                continue  # 二进制自己,不算资源
            dest = f"{res_dir}/{rel}"
            target = f"{src_dir}/{rel}"
            cur = img.resolve(dest)
            if cur.ok and cur.path == target:
                skipped += 1
                print(f"[skip] {rel}:已是 {target}")
                continue
            if cur.ok and cur.path == dest:
                # 自身目录里的真实文件(deb 自带的 .pak):不动它。
                skipped += 1
                print(f"[skip] {rel}:自身目录里已有真实文件,不替换")
                continue
            # 源必须真实存在,否则宁可报错,也不造一条悬空链接。
            src = img.resolve(target)
            if not src.ok or src.entry is None or src.entry.kind != "file" \
                    or src.entry.size == 0:
                failed += 1
                print(f"[FAIL] {rel}:源 {target} 不存在/不是文件,拒绝创建链接")
                continue
            why = {"missing": "缺失", "dangling": "悬空链接"}.get(cur.status, cur.status)
            plan.append((rel, dest, target, why, cur.status != "missing"))

        if plan and not args.dry_run:
            img.mkdirs({dest.rsplit("/", 1)[0] for _r, dest, _t, _w, _f in plan})
        for rel, dest, target, why, force in plan:
            if args.dry_run:
                print(f"[plan] {rel}:{why} → 建符号链接 {target}")
                changed += 1
                continue
            img.symlink(dest, target, force=force)
            after = img.resolve(dest)
            if after.ok and after.path == target:
                changed += 1
                print(f"[fix ] {rel}:{why} → {target}")
            else:
                failed += 1
                print(f"[FAIL] {rel}:写入后仍不可解析({after.status} {after.message})")
        tail = "(dry-run,未写镜像)" if args.dry_run else ""
        print(f"[fix-headless-layout] 结论:修复 {changed} 项,"
              f"已就绪 {skipped} 项,失败 {failed} 项{tail}")
        return 1 if failed else 0
    finally:
        img.close()


if __name__ == "__main__":
    raise SystemExit(main())
