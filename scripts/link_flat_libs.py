#!/usr/bin/env python3
"""在镜像的系统库目录里补 <soname> -> /opt/<pkg>/... 的符号链接。

为什么需要这层链接：src/tools/app.c 的 build_linux_env() 用
APP_LIB_PATH_MAX(1024) 字节的静态缓冲拼 LD_LIBRARY_PATH，/opt 下有 147 个
包时只能塞进前 ~15 个，排在后面的包目录根本不会进入搜索路径。镜像里那
批能用的库（libnss3/libglib-2.0/...）都是靠在 /lib64、/lib/x86_64-linux-gnu、
/usr/lib/x86_64-linux-gnu 里放一条指向 /opt/<pkg>/usr/lib/x86_64-linux-gnu/
的符号链接绕过去的——这三个目录排在搜索路径最前面，不受截断影响。

本脚本只做这一件事，不碰 install_app.py 的既有行为。

用法：
  python3 scripts/link_flat_libs.py --img build/c3-chromium.img \
      --lib libopenh264.so.8=/opt/libopenh264-8/usr/lib/x86_64-linux-gnu/libopenh264.so.8
"""
import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OFFSET = 1048576

# 搜索路径里排在 /opt/* 之前的固定目录，见 src/tools/app.c system_dirs[]。
SYSTEM_DIRS = ["/lib64", "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"]


def fs_arg(img: Path) -> str:
    return f"{img}?offset={OFFSET}"


def debugfs(img: Path, commands, write=True):
    script = ROOT / "build/tmp/debugfs.cmd"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text("\n".join(commands) + "\n")
    args = ["debugfs", "-w", "-f", str(script), fs_arg(img)] if write else \
           ["debugfs", "-f", str(script), fs_arg(img)]
    return subprocess.run(args, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default="build/c3-chromium.img")
    ap.add_argument("--lib", action="append", required=True,
                    metavar="SONAME=TARGET",
                    help="重复给出，例如 libFLAC.so.14=/opt/libflac14/usr/lib/x86_64-linux-gnu/libFLAC.so.14")
    args = ap.parse_args()

    img = Path(args.img)
    cmds = []
    for spec in args.lib:
        soname, target = spec.split("=", 1)
        if not target.startswith("/opt/"):
            raise SystemExit(f"target 必须指向 /opt 下的实际文件：{target}")
        for d in SYSTEM_DIRS:
            dest = f"{d}/{soname}"
            cmds.append(f"rm {dest}")
            cmds.append(f"symlink {dest} {target}")
        print(f"[link] {soname} -> {target}  (x{len(SYSTEM_DIRS)})")

    r = debugfs(img, cmds)
    if "Filesystem not open" in r.stdout + r.stderr:
        raise SystemExit(f"debugfs 打不开镜像：{r.stdout[:200]}")
    err = [l for l in (r.stdout + r.stderr).splitlines()
           if "not found" in l or "error" in l.lower()]
    for l in err:
        print(f"  ! {l}")


if __name__ == "__main__":
    main()
