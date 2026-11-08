#!/usr/bin/env bash
# 把 xkeyboard-config 的 xkb 数据装进测试镜像。
#
# Chromium 的 Ozone/Wayland 后端在初始化键盘时用 libxkbcommon 编译键映射，
# 而 libxkbcommon 的默认 include 路径是 /usr/share/X11/xkb。镜像里没有这个
# 目录时它打印
#   xkbcommon: ERROR: failed to add default include path /usr/share/X11/xkb
# 并拿到一个空键映射，随后键盘初始化路径就会解引用空指针（实测 SIGSEGV）。
#
# 用法：scripts/install_guest_xkb.sh [镜像路径] [宿主 xkb 源目录]
set -euo pipefail

IMG="${1:-build/c3-chromium.img}"
SRC="${2:-/usr/share/X11/xkb}"
OFF_IMG="${IMG}?offset=1048576"

[ -d "$SRC" ] || { echo "找不到 xkb 数据: $SRC" >&2; exit 1; }
SRC="$(readlink -f "$SRC")"

cmds="$(mktemp)"
cleanup() { rm -f "$cmds"; }
trap cleanup EXIT

# debugfs 不能递归建目录，按深度排序后逐级 mkdir，再逐个 write。
{
    echo "mkdir /usr/share/X11"
    echo "mkdir /usr/share/X11/xkb"
    ( cd "$SRC" && find . -mindepth 1 -type d | LC_ALL=C sort ) |
        while read -r d; do
            echo "mkdir /usr/share/X11/xkb/${d#./}"
        done
    ( cd "$SRC" && find . -type f | LC_ALL=C sort ) |
        while read -r f; do
            echo "rm /usr/share/X11/xkb/${f#./}"
            echo "write $SRC/${f#./} /usr/share/X11/xkb/${f#./}"
        done
} >"$cmds"

debugfs -w -f "$cmds" "$OFF_IMG" >/dev/null 2>&1 || {
    echo "debugfs 写入失败，命令表保留在 $cmds" >&2
    trap - EXIT
    exit 1
}

count=$( ( cd "$SRC" && find . -type f ) | wc -l )
echo "已安装 $count 个 xkb 数据文件到 $IMG:/usr/share/X11/xkb"
