#!/usr/bin/env bash
# tests/host/run_ext2_write.sh — ext2 写路径的宿主回归（跨实现验收）。
#
# 流程：mkfs.ext2 造**真实**镜像 → 用内核 src/ext2.c 在上面做全套写操作
# → 用 Linux e2fsprogs 的 e2fsck 判卷是否干净、卸载状态是否正确。
#
# 为什么必须用 e2fsck：驱动"自己读回来一致"只能证明自洽，证明不了磁盘布局
# 符合 ext2 规范（位图/计数/i_blocks/目录链接计数任意一处错了，Linux 挂载
# 时才会炸）。e2fsck 是唯一独立裁判。
#
# 两个镜像各跑一次：
#   1) 正常路径：测试结束时 ext2_sync() → s_state=VALID → e2fsck 应判 clean
#      且**跳过**全检（"cleanly unmounted"）。
#   2) --no-sync：模拟"挂载中断电" → s_state 必须不含 VALID，e2fsck -n 必须
#      报 "was not cleanly unmounted, check forced" 并做全检；因为驱动写完即落盘，
#      全检本身仍应 0 错误。
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

# 读超级块 s_state（偏移 1024 + 58，little-endian u16）
sb_state() {
    python3 - "$1" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
state = int.from_bytes(data[1024 + 58:1024 + 60], "little")
print(state)
PY
}

status=0
for bs in 1024 4096; do
    IMG="$TMP/img-$bs.ext2"
    echo "── block_size=$bs ─────────────────────────────────────"
    dd if=/dev/zero of="$IMG" bs=1M count=8 status=none
    mkfs.ext2 -q -b "$bs" -I 128 -F "$IMG"

    # 1) 驱动自洽 + 卸载标记
    if ! "$TMP/test_ext2_write" "$IMG"; then
        echo "  [FAIL] 驱动写路径自洽检查未通过"
        status=1
        continue
    fi
    state=$(sb_state "$IMG")
    if [ $((state & 1)) -ne 1 ]; then
        echo "  [FAIL] 同步后 s_state=0x$state，缺少 EXT2_VALID_FS"
        status=1
    fi

    # 2) 独立裁判：干净卸载 → 不带 -f 的 e2fsck 必须直接判 clean（跳过全检）
    if LC_ALL=C e2fsck -n "$IMG" >"$TMP/e2fsck-$bs.log" 2>&1; then
        if grep -q "clean," "$TMP/e2fsck-$bs.log" &&            ! grep -q "Pass 1:" "$TMP/e2fsck-$bs.log"; then
            echo "  [ OK ] e2fsck: cleanly unmounted, skipped full check"
        else
            echo "  [FAIL] e2fsck 没有把卷当成干净卸载："
            sed -n '1,20p' "$TMP/e2fsck-$bs.log"
            status=1
        fi
    else
        echo "  [FAIL] e2fsck -n 报错："
        sed -n '1,40p' "$TMP/e2fsck-$bs.log"
        status=1
    fi
    # 全检也必须 0 错误（clean 之外的独立结论）
    if ! LC_ALL=C e2fsck -fn "$IMG" >"$TMP/e2fsck-full-$bs.log" 2>&1; then
        echo "  [FAIL] e2fsck -fn 全检报错："
        sed -n '1,40p' "$TMP/e2fsck-full-$bs.log"
        status=1
    fi

    # 3) 模拟断电：不 sync，s_state 必须脏；e2fsck 必须强制全检且无错误
    DIRTY="$TMP/dirty-$bs.ext2"
    dd if=/dev/zero of="$DIRTY" bs=1M count=8 status=none
    mkfs.ext2 -q -b "$bs" -I 128 -F "$DIRTY"
    if ! "$TMP/test_ext2_write" "$DIRTY" 99 --no-sync >"$TMP/dirty-$bs.run.log" 2>&1; then
        echo "  [FAIL] --no-sync 运行失败："
        tail -10 "$TMP/dirty-$bs.run.log"
        status=1
    fi
    state=$(sb_state "$DIRTY")
    if [ $((state & 1)) -ne 0 ]; then
        echo "  [FAIL] 未同步的卷 s_state=0x$state 仍是 clean"
        status=1
    fi
    LC_ALL=C e2fsck -n "$DIRTY" >"$TMP/dirty-$bs.fsck.log" 2>&1 || true
    if grep -q "not cleanly unmounted" "$TMP/dirty-$bs.fsck.log"; then
        echo "  [ OK ] e2fsck: 脏卷被强制全检"
    else
        echo "  [FAIL] e2fsck 没有识别出脏卷："
        sed -n '1,10p' "$TMP/dirty-$bs.fsck.log"
        status=1
    fi
    if LC_ALL=C e2fsck -fn "$DIRTY" >"$TMP/dirty-$bs.full.log" 2>&1; then
        echo "  [ OK ] 脏卷全检 0 错误"
    else
        echo "  [FAIL] 脏卷全检有错误："
        sed -n '1,40p' "$TMP/dirty-$bs.full.log"
        status=1
    fi

    # 4) debugfs 复核卷上只剩 lost+found
    if debugfs -R "ls -l /" "$IMG" 2>/dev/null | grep -q "lost+found"; then
        :
    else
        echo "  [FAIL] 根目录里找不到 lost+found"
        debugfs -R "ls -l /" "$IMG" 2>/dev/null
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
