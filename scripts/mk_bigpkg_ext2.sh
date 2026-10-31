#!/usr/bin/env bash
# Build an ext2 data disk holding a local HPT repo with a LARGE multi-file
# package, to exercise streaming install (> RAMFS_MAX_FILE_SIZE, > 2 MiB buffer).
#
# The payload binary is a host PIE that needs only libc.so.6 (default:
# /usr/bin/cstool, ~7 MB).  It is NOT a Debian binary; it is only a size /
# streaming test vehicle.  Real Debian packages go through the same path.
#
# usage: scripts/mk_bigpkg_ext2.sh [out.img] [host-binary]
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=${1:-build/bigpkg-ext2.img}
BIN=${2:-/usr/bin/cstool}
NAME=$(basename "$BIN")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

# 1. wrap the binary into a minimal .deb (control.tar.gz + data.tar.xz)
mkdir -p "$W/data/usr/bin" "$W/ctl"
cp "$BIN" "$W/data/usr/bin/$NAME"
chmod 755 "$W/data/usr/bin/$NAME"
cat > "$W/ctl/control" <<EOF
Package: $NAME
Version: 1.0-1
Architecture: amd64
Depends: libc6
Description: large streaming-install test package ($NAME)
EOF
( cd "$W/data" && tar --owner=0 --group=0 -cJf "$W/data.tar.xz" . )
( cd "$W/ctl"  && tar --owner=0 --group=0 -czf "$W/control.tar.gz" ./control )
echo 2.0 > "$W/debian-binary"
( cd "$W" && ar rc "$NAME.deb" debian-binary control.tar.gz data.tar.xz )

# 2. deb -> HPK2 (uncompressed USTAR; the HPT client only unpacks flags==0)
python3 HPT/tools/pkg2hax.py "$W/$NAME.deb" -o "$W/out" >/dev/null

# 3. repo dir = built-in libc6 + the big package
REPO="$W/repo"
mkdir -p "$REPO"
cp build/hpt-repo/libc6.hax "$REPO/"
cp "$W/out/$NAME.hax" "$REPO/"
python3 - "$REPO" "$NAME" "$W/out/$NAME.manifest.json" <<'PY'
import hashlib, json, sys
repo, name, manifest = sys.argv[1:4]
lines = [l for l in open("build/hpt-repo/Packages", encoding="utf-8") if l.startswith("libc6|")]
assert lines, "libc6 record missing: run make bios-iso first"
payload = open(f"{repo}/{name}.hax", "rb").read()
m = json.load(open(manifest))
lines.append("|".join([name, m["version"], "x86_64", str(len(payload)),
    hashlib.sha256(payload).hexdigest(), f"{name}.hax",
    ",".join(m.get("depends", [])), m["description"]]) + "\n")
open(f"{repo}/Packages", "w", encoding="utf-8").write("".join(lines))
print(lines[-1], end="")
PY

# 4. MBR disk with one type-0x83 partition at LBA 2048 (HBOS ext2_try_mount only
#    scans partitions), ext2 inside (1 KiB blocks like scripts/test_ext2_diff.sh),
#    populated from $REPO.
mkdir -p "$W/root"
cp -r "$REPO" "$W/root/repo"
rm -f "$IMG"
truncate -s 96M "$IMG"
echo 'label: dos
start=2048, type=83' | sfdisk -q "$IMG"
mkfs.ext2 -q -b 4096 -I 128 -E offset=$((2048 * 512)) -d "$W/root" "$IMG" 24064
echo "ext2 image: $IMG ($(du -h "$IMG" | cut -f1) used)"
