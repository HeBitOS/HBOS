#!/usr/bin/env bash
# tests/host/run_ext2_write.sh — ext2 写路径的宿主回归（跨实现验收）。
#
# 流程：mkfs.ext2 造**真实**镜像 → 用内核 src/ext2.c 在上面做全套写操作
# → 用 Linux e2fsprogs 的 e2fsck -fn 判卷是否干净 → debugfs 复核残留内容。
#
# 为什么必须用 e2fsck：驱动"自己读回来一致"只能证明自洽，证明不了磁盘布局
# 符合 ext2 规范（位图/计数/i_blocks/目录链接计数任意一处错了，Linux 挂载
# 时才会炸）。e2fsck 是唯一独立裁判。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BUILD/tmp"

CC="${CC:-gcc}"

"$CC" -O2 -Wall -Wextra -I"$ROOT/src" -I"$ROOT/tests/host" \
    -include "$ROOT/tests/host/hbos_shim.h" \
    "$ROOT/tests/host/test_ext2_write.c" "$ROOT/src/ext2.c" \
    "$ROOT/src/lib/ext4_extent.c" \
    "$ROOT/tests/host/host_fs_backend.c" "$ROOT/tests/host/hbos_string_wrap.c" \
    -o "$TMP/test_ext2_write"

status=0
for bs in 1024 4096; do
    IMG="$TMP/img-$bs.ext2"
    echo "── block_size=$bs ─────────────────────────────────────"
    dd if=/dev/zero of="$IMG" bs=1M count=8 status=none
    mkfs.ext2 -q -b "$bs" -I 128 -F "$IMG"

    # 1) 驱动自洽
    if ! "$TMP/test_ext2_write" "$IMG"; then
        echo "  [FAIL] 驱动写路径自洽检查未通过"
        status=1
        continue
    fi

    # 2) 独立裁判：e2fsck 必须判干净（-f 强制全检，-n 不改镜像）
    if e2fsck -fn "$IMG" >"$TMP/e2fsck-$bs.log" 2>&1; then
        echo "  [ OK ] e2fsck -fn: clean"
    else
        echo "  [FAIL] e2fsck -fn 报错："
        sed -n '1,40p' "$TMP/e2fsck-$bs.log"
        status=1
    fi

    # 3) debugfs 复核：卷上应只剩 mkfs 的 lost+found，且它的属性正常
    if debugfs -R "ls -l /" "$IMG" 2>/dev/null | tee "$TMP/ls-$bs.log" | grep -q "lost+found"; then
        :
    else
        echo "  [FAIL] 根目录里找不到 lost+found："
        cat "$TMP/ls-$bs.log"
        status=1
    fi
    if debugfs -R "stat /lost+found" "$IMG" 2>/dev/null | grep -q "Links: 2"; then
        :
    else
        echo "  [FAIL] lost+found 链接计数异常："
        debugfs -R "stat /lost+found" "$IMG" 2>/dev/null
        status=1
    fi
done

echo "─────────────────────────────────────────────────────"
if [ "$status" -eq 0 ]; then
    echo "ext2 write host tests: PASS"
else
    echo "ext2 write host tests: FAIL"
fi
exit "$status"
