#!/usr/bin/env bash
# tests/host/run_ext4_read.sh — ext4 只读支持的跨实现验证。
#
# mkfs.ext4 默认特性 = extents + 64bit + metadata_csum + flex_bg，正好是本
# 驱动"按 ext2 结构读会读错"的全部来源。这里用真实镜像 + Linux e2fsprogs
# 写内容，再由 HBOS 的 ext2.c 读回逐字节比对。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$BUILD/tmp"

CC="${CC:-gcc}"
IMG="$TMP/img.ext4"

dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mkfs.ext4 -q -b 4096 -I 256 -F "$IMG"

python3 - "$TMP" <<'PY'
import os, subprocess, sys
tmp = sys.argv[1]
sizes = [1, 511, 5000, 70000, 1600000]

def expect(n, seed):
    s = (seed * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
    out = bytearray()
    for _ in range(n):
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        out.append((s >> 33) & 0xFF)
    return bytes(out)

for n in sizes:
    with open(os.path.join(tmp, f"f{n}.bin"), "wb") as fh:
        fh.write(expect(n, n))
PY

for n in 1 511 5000 70000 1600000; do
    debugfs -w -R "write $TMP/f$n.bin f$n.bin" "$IMG" >/dev/null 2>&1
done

"$CC" -O2 -Wall -Wextra -I"$ROOT/src" -I"$ROOT/tests/host" \
    -include "$ROOT/tests/host/hbos_shim.h" \
    "$ROOT/tests/host/test_ext4_read.c" "$ROOT/src/ext2.c" \
    "$ROOT/src/lib/ext4_extent.c" \
    "$ROOT/tests/host/host_fs_backend.c" "$ROOT/tests/host/hbos_string_wrap.c" \
    -o "$TMP/test_ext4_read"

"$TMP/test_ext4_read" "$IMG"

# 独立裁判：只读挂载不得改动卷
e2fsck -fn "$IMG" >"$TMP/fsck.log" 2>&1 || {
    echo "[FAIL] e2fsck 报错："
    sed -n '1,40p' "$TMP/fsck.log"
    exit 1
}
echo "  [ OK ] e2fsck -fn: clean"
echo "ext4 read host tests: PASS"
