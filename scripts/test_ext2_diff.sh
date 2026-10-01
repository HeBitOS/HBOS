#!/usr/bin/env bash
# ext2 宿主差分回归：造真实 ext2 镜像（mkfs.ext2 + debugfs）→ 用内核的 ext2.c
# 在宿主机上读回 → 与生成时的内容逐字节比对，并打印底层块命令计数。
#
# 为什么放在宿主机而不是 QEMU：块设备换成文件替身（tests/host/host_fs_backend.c）
# 后，读路径的**命令数**可以直接数出来，这是"间接块缓存"这类优化的量化证据；
# 同时 e2fsprogs 造的镜像保证我们读的是真实 Linux 写的卷。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CC="${CC:-gcc}"
IMG="$TMP/img.ext2"

# 1 KiB 块 + 128 字节 inode：让 70000 字节的文件跨过一级间接边界，
# 从而覆盖 ext2_inode_block() 的间接块解析路径。
dd if=/dev/zero of="$IMG" bs=1M count=16 status=none
mkfs.ext2 -q -b 1024 -I 128 "$IMG"

python3 - "$TMP" <<'PY'
import os, subprocess, sys
tmp = sys.argv[1]
sizes = [1, 511, 1024, 4095, 6000, 70000]

def expect(n, seed):
    """与 tests/host/test_ext2_diff.c 里的 expect() 完全一致的生成规则。"""
    s = (seed * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
    out = bytearray()
    for _ in range(n):
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        out.append((s >> 33) & 0xFF)
    return bytes(out)

for n in sizes:
    with open(os.path.join(tmp, f"f{n}.bin"), "wb") as f:
        f.write(expect(n, n))
for n in sizes:
    subprocess.run(["debugfs", "-w", "-R", f"write {tmp}/f{n}.bin f{n}.bin",
                    os.path.join(tmp, "img.ext2")],
                   check=True, capture_output=True)
PY

# 2) 在宿主机上编译真实的 ext2.c + 块设备替身，跑差分
"$CC" -O2 -Wall -Wextra -I"$ROOT/src" -I"$ROOT/tests/host" \
    -include "$ROOT/tests/host/hbos_shim.h" \
    "$ROOT/tests/host/test_ext2_diff.c" "$ROOT/src/ext2.c" \
    "$ROOT/tests/host/host_fs_backend.c" "$ROOT/tests/host/hbos_string_wrap.c" \
    -o "$TMP/test_ext2"

"$TMP/test_ext2" "$IMG"
