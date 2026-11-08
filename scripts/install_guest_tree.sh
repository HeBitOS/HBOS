#!/usr/bin/env bash
# 把宿主上一棵目录树整体写进 HBOS 测试镜像（debugfs 一次 -f 批量执行）。
#
# 为什么需要：镜像里的资源文件必须和二进制**同一版本**。实测
# /opt/chromium/usr/lib/chromium/resources.pak 只有 13 046 519 字节，而
# chromium 156 包里那份是 20 368 637 字节 —— pak 头（DataPack v5）看得过去、
# 加载也"成功"，但消息 id 表对不上，于是 ui::ResourceBundle::
# GetLocalizedStringImpl 里 GetRawDataResourceForScale 返回空 span，
# 立刻命中冷块里的 int3。这种"文件版本不匹配"靠读代码永远看不出来，
# 必须逐字节比大小/哈希。
#
# 用法：scripts/install_guest_tree.sh <宿主导入目录> <镜像内目标目录> [镜像]
#   例：scripts/install_guest_tree.sh build/tmp/app-tree/usr/lib/chromium \
#           /opt/chromium/usr/lib/chromium
set -euo pipefail

SRC="${1:?用法: install_guest_tree.sh <src-dir> <guest-dest> [img]}"
DEST="${2:?用法: install_guest_tree.sh <src-dir> <guest-dest> [img]}"
IMG="${3:-build/c3-chromium.img}"
OFF_IMG="${IMG}?offset=1048576"

[ -d "$SRC" ] || { echo "找不到源目录: $SRC" >&2; exit 1; }
SRC="$(readlink -f "$SRC")"
DEST="${DEST%/}"

cmds="$(mktemp)"
trap 'rm -f "$cmds"' EXIT

{
    # 逐级创建目标目录（debugfs 不能递归 mkdir）。
    parts="${DEST#/}"
    accum=""
    IFS='/' read -ra segs <<<"$parts"
    for seg in "${segs[@]}"; do
        accum="$accum/$seg"
        echo "mkdir $accum"
    done
    ( cd "$SRC" && find . -mindepth 1 -type d | LC_ALL=C sort ) |
        while read -r d; do
            echo "mkdir $DEST/${d#./}"
        done
    ( cd "$SRC" && find . -type f | LC_ALL=C sort ) |
        while read -r f; do
            rel="${f#./}"
            echo "rm $DEST/$rel"
            echo "write $SRC/$rel $DEST/$rel"
            # debugfs write 建出来的是 0644：可执行文件必须补上 x 位，
            # 否则 guest 里 `run` 直接报 "app not found"（实测 bilibili
            # 的 electron 就是这样）。
            mode=$(stat -c '%a' "$SRC/$rel")
            echo "sif $DEST/$rel mode 0100$mode"
        done
} >"$cmds"

debugfs -w -f "$cmds" "$OFF_IMG" >/dev/null 2>&1

count=$( ( cd "$SRC" && find . -type f ) | wc -l )
bytes=$( ( cd "$SRC" && find . -type f -printf '%s\n' ) | awk '{s+=$1} END {print s+0}' )
echo "已写入 $count 个文件（$bytes 字节）到 $IMG:$DEST"
