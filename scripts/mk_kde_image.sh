#!/usr/bin/env bash
# Pack a KDE + Chromium rootfs tree into the MBR/ext2 disk image that
# `make run-kde` boots.  Idempotent: every run rebuilds the image from
# whatever the rootfs currently contains.
#
# Layout (matches the delivered build/hbos-kde-chromium.img):
#   MBR, one type-0x83 partition at LBA 2048, ext2 inside it.
#   block_size 4096, inode_size 256, 131072 inodes (61843 free in the
#   shipped image, so the rootfs fits with headroom).
#
# Why the image is assembled in two pieces instead of `mke2fs -E offset`:
# e2fsck and debugfs do not understand the `offset` extended option, so a
# filesystem written at an offset can never be verified before it is handed
# to the guest.  Building the partition standalone keeps `e2fsck -fn` real.
#
# usage: scripts/mk_kde_image.sh [rootfs] [out.img]
#        scripts/mk_kde_image.sh --reproducible      (byte-identical runs)
# env:   KDE_IMG_SIZE  disk image size (default 2048M)
set -euo pipefail

ROOTFS=build/tmp/kwin_root
OUT=build/hbos-kde-chromium.img
IMG_SIZE=${KDE_IMG_SIZE:-2048M}   # disk image size; KDE_IMG_SIZE overrides for tests
FS_BLOCK_SIZE=4096
FS_INODE_SIZE=256
FS_INODES=131072
PART_START_LBA=2048
REPRODUCIBLE=0

# Files the guest autostart chain and the KDE session depend on.  A missing
# entry here produces a bootable image that never reaches the desktop, so it
# is a hard error rather than a warning.
REQUIRED=(
    usr/bin/kde_session.sh
    usr/bin/start_kde_chromium.sh
    usr/bin/plasmashell
    usr/bin/konsole
    usr/lib/chromium/chromium
    etc/hbos-autostart
)

usage() { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; exit "${1:-0}"; }
die()  { printf '错误: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)     usage 0 ;;
        --reproducible) REPRODUCIBLE=1; shift ;;
        --)            shift; break ;;
        -*)            die "未知选项 $1" ;;
        *)             break ;;
    esac
done
[ $# -ge 1 ] && ROOTFS=$1
[ $# -ge 2 ] && OUT=$2
[ $# -gt 2 ] && die "参数过多: $*"

cd "$(dirname "$0")/.."

# mke2fs -d copies the tree as the invoking user; the shipped image also
# carries uid 1000 for the session scripts, so no root is needed.  Running as
# root would silently rewrite every owner to 0.
[ "$(id -u)" -eq 0 ] && echo "警告: 以 root 运行会把 rootfs 全部属主写成 0（现有镜像为 1000）" >&2

# ---- 1. rootfs sanity ------------------------------------------------------
[ -d "$ROOTFS" ] || die "rootfs 目录不存在: $ROOTFS"
ROOTFS_ABS=$(readlink -f "$ROOTFS")
OUT_ABS=$(readlink -f "$(dirname "$OUT")")/$(basename "$OUT")
case "$OUT_ABS/" in
    "$ROOTFS_ABS"/*) die "输出路径位于 rootfs 内部: $OUT_ABS" ;;
esac

MISSING=()
for f in "${REQUIRED[@]}"; do
    [ -e "$ROOTFS/$f" ] || MISSING+=("$f")
done
if [ "${#MISSING[@]}" -gt 0 ]; then
    printf '错误: rootfs 缺少 %d 个关键文件，拒绝打包（否则镜像能启动但进不了桌面）:\n' \
        "${#MISSING[@]}" >&2
    printf '  - %s\n' "${MISSING[@]}" >&2
    printf 'rootfs: %s\n' "$ROOTFS_ABS" >&2
    exit 1
fi

NFILES=$(find "$ROOTFS" -xdev -type f | wc -l)
NDIRS=$(find "$ROOTFS" -xdev -type d | wc -l)
[ "$NFILES" -gt 100 ] || die "rootfs 看起来是空的（仅 $NFILES 个文件）: $ROOTFS_ABS"

mkdir -p "$(dirname "$OUT")"

# ---- 2. free space precheck ------------------------------------------------
# Peak usage is the whole disk image plus the standalone partition image; the
# previous image stays in place until the atomic rename, so count it too.
IMG_BYTES=$(numfmt --from=iec "$IMG_SIZE")
PART_BYTES=$((IMG_BYTES - PART_START_LBA * 512))
NEED_BYTES=$((IMG_BYTES + PART_BYTES + 512 * 1024 * 1024))
AVAIL_BYTES=$(df -B1 --output=avail "$(dirname "$OUT")" | tail -1 | tr -d ' ')
if [ "$AVAIL_BYTES" -lt "$NEED_BYTES" ]; then
    die "$(printf '磁盘空间不足: 需要 %.2f GiB（镜像 %.2f GiB + 分区 %.2f GiB + 512 MiB 余量），%s 仅剩 %.2f GiB' \
        "$(awk -v b="$NEED_BYTES" 'BEGIN{printf "%.2f", b/1073741824}')" \
        "$(awk -v b="$IMG_BYTES"  'BEGIN{printf "%.2f", b/1073741824}')" \
        "$(awk -v b="$PART_BYTES" 'BEGIN{printf "%.2f", b/1073741824}')" \
        "$(dirname "$OUT")" \
        "$(awk -v b="$AVAIL_BYTES" 'BEGIN{printf "%.2f", b/1073741824}')")"
fi
printf '空间   : 需要 %s（镜像+分区+余量），%s 可用 %s\n' \
    "$(numfmt --to=iec "$NEED_BYTES")" "$(dirname "$OUT")" "$(numfmt --to=iec "$AVAIL_BYTES")"

WORK=$(mktemp -d "$(dirname "$OUT")/.tmp-kde-img.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# ---- 3. rootfs snapshot stamp ---------------------------------------------
NEWEST=$(find "$ROOTFS" -xdev -type f -newermt '-30 minutes' 2>/dev/null | wc -l)
NEWEST_MTIME=$(find "$ROOTFS" -xdev -type f -printf '%T@\n' 2>/dev/null | awk 'BEGIN{m=0} {if ($1+0>m) m=$1+0} END{printf "%.0f", m}')
NEWEST_MTIME=$(date -d "@${NEWEST_MTIME%%.*}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo unknown)

printf '=== HBOS KDE image ===\n'
printf 'rootfs : %s (%s 个文件, %s 个目录, %s)\n' \
    "$ROOTFS_ABS" "$NFILES" "$NDIRS" "$(du -sh "$ROOTFS" | cut -f1)"
printf '快照   : 最新 mtime %s，近 30 分钟内有改动 %s 个文件\n' "$NEWEST_MTIME" "$NEWEST"
printf '输出   : %s (%s)\n\n' "$OUT_ABS" "$IMG_SIZE"
START=$SECONDS

# ---- 4. standalone ext2 partition -----------------------------------------
PART="$WORK/part.img"
FS_BLOCKS=$((PART_BYTES / FS_BLOCK_SIZE))
truncate -s "$PART_BYTES" "$PART"

MKE2FS_ARGS=(-t ext2 -q -b "$FS_BLOCK_SIZE" -I "$FS_INODE_SIZE" -N "$FS_INODES")
if [ "$REPRODUCIBLE" -eq 1 ]; then
    # Fixed UUID/hash seed plus a fixed timestamp inside every inode makes two
    # runs over an unchanged rootfs byte-identical.
    MKE2FS_ARGS+=(-U 6b4f3c1e-8d2a-4f7b-9c5e-0a1b2c3d4e5f
                  -E "hash_seed=9f8e7d6c-5b4a-4938-8271-60513f2e1d0c")
    export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1735689600}
fi

echo "[1/4] 构建 ext2 分区（${FS_BLOCKS} 个 ${FS_BLOCK_SIZE}B 块, ${FS_INODES} inodes）..."
mke2fs "${MKE2FS_ARGS[@]}" -d "$ROOTFS" "$PART" "$FS_BLOCKS"

# ---- 5. verify the filesystem before it can reach the guest ----------------
echo "[2/4] 校验 ext2 分区 (e2fsck -fn)..."
if ! FSCK_OUT=$(e2fsck -fn "$PART" 2>&1); then
    printf '%s\n' "$FSCK_OUT" >&2
    die "e2fsck 校验失败，拒绝产出镜像"
fi
case "$FSCK_OUT" in
    *"FILE SYSTEM WAS MODIFIED"*) die "e2fsck 报告文件系统不一致：$FSCK_OUT" ;;
esac

SB=$(dumpe2fs -h "$PART" 2>/dev/null)
FS_INO_TOTAL=$(awk -F': *' '/^Inode count/{print $2}' <<<"$SB")
FS_INO_FREE=$(awk -F': *' '/^Free inodes/{print $2}' <<<"$SB")
FS_BLK_TOTAL=$(awk -F': *' '/^Block count/{print $2}' <<<"$SB")
FS_BLK_FREE=$(awk -F': *' '/^Free blocks/{print $2}' <<<"$SB")
FS_BS=$(awk -F': *' '/^Block size/{print $2}' <<<"$SB")
[ "$FS_BS" = "$FS_BLOCK_SIZE" ] || die "块大小不符: 期望 $FS_BLOCK_SIZE, 实际 $FS_BS"
[ "${FS_INO_FREE:-0}" -gt 0 ] || die "inode 已耗尽（$FS_INO_TOTAL 个全用光），rootfs 超出几何上限"
# <1% free blocks means the next rootfs edit cannot be packed; fail loudly
# instead of shipping an image that is one commit away from breaking.
[ $((FS_BLK_FREE * 100 / FS_BLK_TOTAL)) -ge 1 ] || \
    die "空闲块仅 $FS_BLK_FREE/$FS_BLK_TOTAL (<1%)，rootfs 已接近镜像容量上限"

# ---- 6. embed into MBR disk image -----------------------------------------
echo "[3/4] 组装 MBR 磁盘镜像..."
DISK="$WORK/disk.img"
truncate -s "$IMG_BYTES" "$DISK"
printf 'label: dos\nstart=%s, type=83\n' "$PART_START_LBA" | sfdisk -q "$DISK"
dd if="$PART" of="$DISK" bs=1M seek=$((PART_START_LBA * 512 / 1048576)) conv=notrunc status=none

# Read the partition back out of the finished disk and compare digests: this
# catches a truncated or misaligned dd, the one way the assembly can silently
# corrupt an otherwise verified filesystem.
PART_SHA=$(sha256sum "$PART" | cut -d' ' -f1)
DISK_SHA=$(dd if="$DISK" bs=1M skip=$((PART_START_LBA * 512 / 1048576)) status=none | sha256sum | cut -d' ' -f1)
[ "$PART_SHA" = "$DISK_SHA" ] || \
    die "分区回读校验失败：嵌入后的数据 ($DISK_SHA) 与分区镜像 ($PART_SHA) 不一致"
printf '      分区回读 sha256 %s… 一致\n' "${PART_SHA:0:16}"

# ---- 7. atomic publish ----------------------------------------------------
echo "[4/4] 原子替换 $OUT_ABS ..."
chmod 644 "$DISK"
mv -f "$DISK" "$OUT_ABS"

ELAPSED=$((SECONDS - START))
printf '\n=== 完成 ===\n'
printf '镜像   : %s\n' "$OUT_ABS"
printf '大小   : %s 字节 (%s)\n' "$(stat -c %s "$OUT_ABS")" "$(du -h "$OUT_ABS" | cut -f1)"
printf 'inode  : %s/%s 已用, 空闲 %s\n' \
    "$((FS_INO_TOTAL - FS_INO_FREE))" "$FS_INO_TOTAL" "$FS_INO_FREE"
printf '数据块 : %s/%s 已用 (%s%%), 块大小 %s\n' \
    "$((FS_BLK_TOTAL - FS_BLK_FREE))" "$FS_BLK_TOTAL" \
    "$(( (FS_BLK_TOTAL - FS_BLK_FREE) * 100 / FS_BLK_TOTAL ))" "$FS_BS"
printf '耗时   : %s 秒\n' "$ELAPSED"
printf '快照   : rootfs 最新 mtime %s\n' "$NEWEST_MTIME"
[ "$NEWEST" -gt 0 ] && printf '提示   : 打包期间有 %s 个文件在近 30 分钟内被改过，本镜像只反映上述时刻\n' "$NEWEST"
exit 0
