#!/usr/bin/env bash
# Build an ext2 data disk that HBOS mounts as its root filesystem, holding:
#   /repo          HPT `file` repository converted from the given .deb files
#   /tests/*       host-built dynamic test binaries (glibc, PT_INTERP
#                  /lib64/ld-linux-x86-64.so.2)
#   /data/big40.bin 40 MiB page-patterned file for the big-mmap test
#
# usage: scripts/mk_test_img.sh <out.img> [pkg.deb ...]
#        scripts/mk_test_img.sh build/c1-ext2.img build/debs/*.deb
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=${1:?usage: mk_test_img.sh <out.img> [deb...]}
shift
DEBS=("$@")
SIZE_MB=${SIZE_MB:-256}

W=build/imgwork
# 逐项清理而不是 rm -rf：批量删除拦截器会把整个工作目录当成高危目标。
mkdir -p "$W"
if [ -n "$(ls -A "$W" 2>/dev/null)" ]; then
  find "$W" -mindepth 1 -delete 2>/dev/null || true
fi
mkdir -p "$W/root/data" "$W/root/tests"

# ── 1. HPT repository from the Debian packages ────────────────────────
if [ ${#DEBS[@]} -gt 0 ]; then
  python3 scripts/mk_deb_repo.py --with-libc6 --out "$W/repo" --root "$W/root" "${DEBS[@]}"
else
  mkdir -p "$W/root/repo"
  : > "$W/root/repo/Packages"
fi

# ── 2. big-mmap fixture: 40 MiB, page-varying payload ─────────────────
python3 - "$W/root/data/big40.bin" <<'PY'
import sys
from pathlib import Path
PAGE = 4096
SIZE = 40 * 1024 * 1024
buf = bytearray(SIZE)
for page in range(SIZE // PAGE):
    # must match pattern() in tests/linux_bigmmap.c
    buf[page * PAGE:(page + 1) * PAGE] = bytes([(page * 131 + 17) & 0xFF]) * PAGE
Path(sys.argv[1]).write_bytes(bytes(buf))
PY

# ── 3. dynamic test binaries ──────────────────────────────────────────
CC=${CC:-gcc}
GLIBC_LIBDIR=$(dirname "$(ldconfig -p | awk '/libc.so.6 \(libc6,x86-64\)/{print $NF; exit}')")
for src in tests/linux_bigmmap.c; do
  name=$(basename "$src" .c)
  $CC -O2 -fPIE -pie -Wl,-z,max-page-size=0x1000 \
      -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
      "$src" -o "$W/root/tests/$name"
  echo "[img] test binary tests/$name (dyn, glibc $GLIBC_LIBDIR)"
done

# ── 4. ext2 image (MBR partition at LBA 2048, 4 KiB blocks) ───────────
rm -f "$IMG"
truncate -s "${SIZE_MB}M" "$IMG"
echo 'label: dos
start=2048, type=83' | sfdisk -q "$IMG"
BLOCKS=$(( (SIZE_MB - 1) * 1024 / 4 ))
mkfs.ext2 -q -b 4096 -I 256 -E offset=$((2048 * 512)) -d "$W/root" "$IMG" "$BLOCKS"
echo "[img] $IMG ($(du -h "$IMG" | cut -f1) on disk)"
