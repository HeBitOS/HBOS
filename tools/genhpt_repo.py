#!/usr/bin/env python3
"""Build the built-in HPT repository from the HAX apps bundled in the kernel
image (build/app/*.hax), with a FLAT layout that fits HBOS's ramfs naming
limit (full normalized path <= MAX_FILENAME-1 == 31 chars):

    /packages/Packages
    /packages/<name>.hax        (payload, short name instead of pool/x_y_z.hax)

Only apps with a valid .haxmeta whose payload fits RAMFS_MAX_FILE_SIZE
(64 KiB) are included — larger resident tools (tcc.hax) and hpt.hax itself
don't need to be (re)installable.

Usage: genhpt_repo.py <apps-dir> <out-repo-dir>
"""

import hashlib
import shutil
import struct
import sys
from pathlib import Path

HAX_META_MAGIC = 0x4D584148
RAMFS_MAX_FILE_SIZE = 131072
VERSION = "0.1.0"
NAME = "x86_64"


def read_hax_meta(path: Path):
    """读取 .haxmeta：返回 (name, description, kind) 或 None。

    kind 沿用 hax.h：1=TUI 2=GUI 4=GUI_WIN（可 OR）。安装器要靠它区分
    "这是能点开的图形应用" 还是 "这是命令行工具"，所以必须带出来。
    """
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        return None
    section_offset = struct.unpack_from("<Q", data, 0x28)[0]
    section_size = struct.unpack_from("<H", data, 0x3A)[0]
    section_count = struct.unpack_from("<H", data, 0x3C)[0]
    names_index = struct.unpack_from("<H", data, 0x3E)[0]
    if (
        section_size < 64
        or names_index >= section_count
        or section_offset + section_size * section_count > len(data)
    ):
        return None

    def section(index):
        offset = section_offset + section_size * index
        return (
            struct.unpack_from("<I", data, offset)[0],
            struct.unpack_from("<Q", data, offset + 24)[0],
            struct.unpack_from("<Q", data, offset + 32)[0],
        )

    _, names_offset, names_size = section(names_index)
    names = data[names_offset : names_offset + names_size]
    for index in range(section_count):
        name_offset, payload_offset, payload_size = section(index)
        if name_offset >= len(names):
            continue
        end = names.find(b"\0", name_offset)
        if end < 0 or names[name_offset:end] != b".haxmeta":
            continue
        if payload_size < 104 or payload_offset + payload_size > len(data):
            return None
        metadata = data[payload_offset : payload_offset + 104]
        magic, kind = struct.unpack_from("<II", metadata)
        if magic != HAX_META_MAGIC or kind == 0:
            return None
        name = metadata[8:40].split(b"\0", 1)[0].decode("utf-8", "replace")
        description = metadata[40:104].split(b"\0", 1)[0].decode("utf-8", "replace")
        return name, description, kind
    return None


# 应用名（.haxmeta 里的中文描述）→ (图标名, 分类)。索引只带 ASCII 短名，
# 由内核侧 de_icon_id() 前缀匹配到内置图标。
APP_ICONS = {
    "calc": ("calc", "Utility;"),
    "clock": ("clock", "Utility;"),
    "files": ("file", "System;"),
    "hexview": ("code", "Development;"),
    "imgview": ("image", "Graphics;"),
    "settings": ("settings", "System;"),
    "shortcuts": ("shortcut", "Utility;"),
    "taskmgr": ("system", "System;"),
    "tui": ("code", "Development;"),
    "widgets": ("code", "Development;"),
    "wdemo": ("code", "Development;"),
    "hpt": ("apps", "System;"),
    "tcc": ("code", "Development;"),
    "browser": ("browser", "Network;"),
    "hivepaint": ("paint", "Graphics;"),
    "ninvaders": ("game", "Game;"),
}


def guess_app_meta(name: str, description: str) -> tuple[str, str, str]:
    """由包名推断 (kind, 显示名, 图标名, 分类)。

    真正的 .desktop 由 pkg2hax 从 deb 里提取；HIVE 的 .hax 应用没有包内
    desktop 文件，所以这里按命名补一份等价的元数据——否则装完在开始菜单
    里只能看到 `calc` 而不是「计算器」。
    """
    icon, cats = APP_ICONS.get(name, ("apps", "Utility;"))
    # 显示名覆盖表：HAX 的 .haxmeta 描述往往是英文名，给商店用中文名。
    label = {"ninvaders": "太空侵略者"}.get(name) or description.strip() or name
    # 显示名里不该出现分隔符（索引是 | 分隔的）
    label = label.replace("|", "/")[:40]
    return label, icon, cats


def build_multifile_test_package(out_dir: Path) -> str:
    """zlib-tree：多文件 HAX v2 目录树包的**基准样本**。

    这个包不是"随便放两个文件"的占位——它是安装器的回归样本，故意覆盖
    真实 zlib 开发包会遇到的每一种条目：

      * 多级目录（usr/include、usr/lib/pkgconfig、usr/share/doc/…）
      * 普通文件（头文件、静态库、pkgconfig、文档）
      * 实体共享库 + 两条相对符号链接（libz.so → libz.so.1 → libz.so.1.3.1）
      * manifest 里同时描述 dir/file/symlink 三类条目与 sha256

    以前只有 libz.so.1.3.1 一个文件和一条链接，安装器把"建目录/建链接/
    写文件"三条路径里任何一条写错都测不出来。补齐后，只要这个包装完
    `ls /opt/zlib-tree/usr/lib` 有 4 个条目、`cat pkgconfig/zlib.pc` 内容
    正确，就说明多文件树是通的。
    """
    import io
    import json
    import tarfile

    pkg_name = "zlib-tree"
    version = "1.3.1"

    zlib_h = b"""/* zlib.h -- interface of the 'zlib' general purpose compression library
   version 1.3.1 (HBOS HPT multi-file tree sample) */
#ifndef ZLIB_H
#define ZLIB_H
#define ZLIB_VERSION "1.3.1"
#define ZLIB_VERNUM 0x1310
#define Z_NO_COMPRESSION         0
#define Z_BEST_SPEED             1
#define Z_BEST_COMPRESSION       9
#define Z_DEFAULT_COMPRESSION   -1
#define Z_OK                     0
#define Z_STREAM_END             1
#define Z_STREAM_ERROR          -2
int  zlibVersion(void);
int  compress2(unsigned char *dest, unsigned long *destLen,
               const unsigned char *source, unsigned long sourceLen, int level);
int  uncompress(unsigned char *dest, unsigned long *destLen,
                const unsigned char *source, unsigned long sourceLen);
#endif /* ZLIB_H */
"""
    zconf_h = b"""/* zconf.h -- configuration of the zlib compression library */
#ifndef ZCONF_H
#define ZCONF_H
#define MAX_MEM_LEVEL 9
#define MAX_WBITS   15
#endif /* ZCONF_H */
"""
    pc_file = (
        "prefix=/opt/zlib-tree/usr\n"
        "exec_prefix=${prefix}\n"
        "libdir=${exec_prefix}/lib\n"
        "sharedlibdir=${libdir}\n"
        "includedir=${prefix}/include\n\n"
        "Name: zlib\n"
        "Description: zlib compression library (HBOS HPT sample)\n"
        "Version: 1.3.1\n"
        "Libs: -L${libdir} -lz\n"
        "Cflags: -I${includedir}\n"
    ).encode()
    lib_bytes = (b"\x7fELF\x02\x01\x01\x00" + b"libz.so.1.3.1 (HBOS test payload)\n"
                 + bytes(range(256)) * 4)
    lib_a = b"!<arch>\n" + b"libz.a (HBOS test archive)\n" + bytes(range(128)) * 2
    readme = ("zlib-tree 1.3.1\n"
              "多文件 HAX v2 目录树示例包：目录 / 普通文件 / 符号链接 / pkgconfig。\n"
              "安装后位于 /opt/zlib-tree，可用 hpt remove zlib-tree 完整卸载。\n").encode("utf-8")

    entries = [
        ("usr", "dir", 0o755, b""),
        ("usr/include", "dir", 0o755, b""),
        ("usr/include/zlib.h", "file", 0o644, zlib_h),
        ("usr/include/zconf.h", "file", 0o644, zconf_h),
        ("usr/lib", "dir", 0o755, b""),
        ("usr/lib/libz.so.1.3.1", "file", 0o755, lib_bytes),
        ("usr/lib/libz.a", "file", 0o644, lib_a),
        ("usr/lib/pkgconfig", "dir", 0o755, b""),
        ("usr/lib/pkgconfig/zlib.pc", "file", 0o644, pc_file),
        ("usr/share", "dir", 0o755, b""),
        ("usr/share/doc", "dir", 0o755, b""),
        ("usr/share/doc/zlib-tree", "dir", 0o755, b""),
        ("usr/share/doc/zlib-tree/README", "file", 0o644, readme),
    ]
    links = [
        ("usr/lib/libz.so.1", "libz.so.1.3.1"),
        ("usr/lib/libz.so", "libz.so.1"),
    ]

    files_meta = []
    installed = 0
    for path, kind, mode, data in entries:
        item = {"path": path, "type": kind, "mode": "%04o" % mode, "size": len(data)}
        if kind == "file":
            item["sha256"] = hashlib.sha256(data).hexdigest()
            installed += len(data)
        files_meta.append(item)
    for path, target in links:
        files_meta.append({"path": path, "type": "symlink", "mode": "0777",
                           "size": 0, "target": target})

    manifest = {
        "schema": 2,
        "format": 2,
        "name": pkg_name,
        "version": version,
        "architecture": NAME,
        "install_prefix": f"/opt/{pkg_name}",
        "installed_size": installed,
        "description": "zlib 1.3.1 多文件目录树（头文件/静态库/共享库/链接/pkgconfig）",
        "depends": [],
        "files": files_meta,
    }
    manifest_bytes = json.dumps(manifest, indent=2, ensure_ascii=False).encode("utf-8")

    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w:", format=tarfile.USTAR_FORMAT) as tf:
        minfo = tarfile.TarInfo(name=".HPT/manifest.json")
        minfo.type = tarfile.REGTYPE
        minfo.mode = 0o644
        minfo.size = len(manifest_bytes)
        tf.addfile(minfo, io.BytesIO(manifest_bytes))

        for path, kind, mode, data in entries:
            info = tarfile.TarInfo(name=f"root/{path}")
            info.mode = mode
            if kind == "dir":
                info.type = tarfile.DIRTYPE
                tf.addfile(info)
            else:
                info.type = tarfile.REGTYPE
                info.size = len(data)
                tf.addfile(info, io.BytesIO(data))

        for path, target in links:
            info = tarfile.TarInfo(name=f"root/{path}")
            info.type = tarfile.SYMTYPE
            info.mode = 0o777
            info.linkname = target
            tf.addfile(info)

    raw_tar = tar_buf.getvalue()
    payload = b"HPK2\x00\x00\x00\x00" + struct.pack("<Q", len(raw_tar)) + raw_tar
    payload_name = f"{pkg_name}.hax"
    (out_dir / payload_name).write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    return (
        f"{pkg_name}|{version}|{NAME}|{len(payload)}|{digest}|"
        f"{payload_name}||zlib 1.3.1 多文件目录树（include/lib/pkgconfig/doc + 2 符号链接）"
        f"|lib||file|Development;\n"
    )


def build_libc6_package(out_dir: Path) -> str:
    import io
    import json
    import tarfile

    pkg_name = "libc6"
    version = "2.40-1"
    manifest = {
        "schema": 2,
        "format": 2,
        "name": pkg_name,
        "version": version,
        "architecture": NAME,
        "install_prefix": "/lib64",
        "installed_size": 0,
        "description": "GNU C Library: Shared libraries (installed to /lib64)",
        "depends": [],
        "files": [
            {"path": "lib64", "type": "dir", "mode": "0755", "size": 0},
            {
                "path": "lib64/ld-lsb-x86-64.so.3",
                "type": "symlink",
                "mode": "0777",
                "size": 0,
                "target": "/lib64/ld-linux-x86-64.so.2",
            },
        ],
    }
    manifest_bytes = json.dumps(manifest, indent=2).encode("utf-8")

    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w:", format=tarfile.USTAR_FORMAT) as tf:
        minfo = tarfile.TarInfo(name=".HPT/manifest.json")
        minfo.type = tarfile.REGTYPE
        minfo.mode = 0o644
        minfo.size = len(manifest_bytes)
        tf.addfile(minfo, io.BytesIO(manifest_bytes))

        dinfo = tarfile.TarInfo(name="root/lib64")
        dinfo.type = tarfile.DIRTYPE
        dinfo.mode = 0o755
        tf.addfile(dinfo)

        sinfo = tarfile.TarInfo(name="root/lib64/ld-lsb-x86-64.so.3")
        sinfo.type = tarfile.SYMTYPE
        sinfo.mode = 0o777
        sinfo.linkname = "/lib64/ld-linux-x86-64.so.2"
        tf.addfile(sinfo)

    raw_tar = tar_buf.getvalue()
    payload = b"HPK2\x00\x00\x00\x00" + struct.pack("<Q", len(raw_tar)) + raw_tar
    payload_name = f"{pkg_name}.hax"
    (out_dir / payload_name).write_bytes(payload)
    digest = hashlib.sha256(payload).hexdigest()
    return (
        f"{pkg_name}|{version}|{NAME}|{len(payload)}|{digest}|"
        f"{payload_name}||GNU C Library: Shared libraries (/lib64)\n"
    )


def include_debian_hpk2_packages(out_dir: Path) -> list[str]:
    import json

    repo_root = Path(__file__).resolve().parent.parent
    conv_dir = repo_root / "out"
    records = []
    for name in ("hello", "tree", "zlib1g"):
        hax_path = conv_dir / f"{name}.hax"
        manifest_path = conv_dir / f"{name}.manifest.json"
        if not hax_path.is_file() or not manifest_path.is_file():
            continue
        payload = hax_path.read_bytes()
        if payload[:5] != b"HPK2\x00":
            continue
        meta = json.loads(manifest_path.read_text(encoding="utf-8"))
        version = meta.get("version", VERSION)
        arch = meta.get("architecture", NAME)
        depends = ",".join(meta.get("depends", []))
        desc = meta.get("description", f"Debian package {name}").replace("|", "/")
        kind = meta.get("kind", "cli")
        apps = meta.get("apps") or []
        app_name = apps[0].get("name", name).replace("|", "/") if apps else ""
        icon = apps[0].get("icon", "apps") if apps else "apps"
        cats = ";".join(apps[0].get("categories", [])) + ";" if apps else "Utility;"
        payload_name = f"{name}.hax"
        (out_dir / payload_name).write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        for rec_name in (name,):
            records.append(
                f"{rec_name}|{version}|{arch}|{len(payload)}|{digest}|"
                f"{payload_name}|{depends}|{desc}|{kind}|{app_name}|{icon}|{cats}\n"
            )
    return records


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: genhpt_repo.py <apps-dir> <out-repo-dir> [extra-apps-dir]",
              file=sys.stderr)
        return 2
    apps_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    # 额外的应用目录：商店演示应用放这里——它只以软件包形式存在，
    # 不参与内核的 HAX 内置清单（装之前开始菜单里看不到）。
    app_dirs = [apps_dir]
    if len(sys.argv) == 4:
        extra = Path(sys.argv[3])
        if extra.is_dir():
            app_dirs.append(extra)

    records = []
    hax_files: list[Path] = []
    for d in app_dirs:
        hax_files.extend(sorted(d.glob("*.hax")))
    for hax in hax_files:
        size = hax.stat().st_size
        if size > RAMFS_MAX_FILE_SIZE:
            print(f"[genhpt_repo] skip {hax.name}: {size} B > {RAMFS_MAX_FILE_SIZE} B")
            continue
        meta = read_hax_meta(hax)
        if meta is None:
            print(f"[genhpt_repo] skip {hax.name}: no .haxmeta")
            continue
        name, description, kind = meta
        # flat payload name must keep the full path under MAX_FILENAME-1 (31)
        payload_name = f"{name}.hax"
        if len(f"packages/{payload_name}") > 31:
            print(f"[genhpt_repo] skip {name}: payload path too long")
            continue
        payload = hax.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        dst = out_dir / payload_name
        shutil.copyfile(hax, dst)
        is_gui = bool(kind & 2)          # HAX_KIND_GUI
        app_kind = "gui" if is_gui else "tui"
        app_name, icon, cats = guess_app_meta(name, description)
        desc = description.replace("|", "/")
        if is_gui:
            records.append(
                f"{name}|{VERSION}|{NAME}|{len(payload)}|{digest}|"
                f"{payload_name}||{desc}|{app_kind}|{app_name}|{icon}|{cats}\n"
            )
        else:
            records.append(
                f"{name}|{VERSION}|{NAME}|{len(payload)}|{digest}|"
                f"{payload_name}||{desc}|cli||{icon}|{cats}\n"
            )

    records.insert(0, build_multifile_test_package(out_dir))
    for rec in reversed(include_debian_hpk2_packages(out_dir)):
        records.insert(0, rec)
    records.insert(0, build_libc6_package(out_dir))

    if not records:
        print("[genhpt_repo] no bundled packages (core-only build)")
        (out_dir / "Packages").write_text("", encoding="utf-8")
        return 0
    (out_dir / "Packages").write_text("".join(records), encoding="utf-8")
    print(f"[genhpt_repo] {len(records)} packages -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
