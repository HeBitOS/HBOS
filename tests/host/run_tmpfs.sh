#!/usr/bin/env bash
# tests/host/run_tmpfs.sh — tmpfs 独立单测（不启动内核、不碰块设备）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
mkdir -p "$BUILD/tmp"
CC="${CC:-gcc}"

"$CC" -O2 -Wall -Wextra -I"$ROOT/src" -I"$ROOT/tests/host" \
    -include "$ROOT/tests/host/hbos_shim.h" \
    "$ROOT/tests/host/test_tmpfs.c" "$ROOT/src/fs/tmpfs.c" \
    "$ROOT/tests/host/hbos_string_wrap.c" \
    -o "$BUILD/tmp/test_tmpfs"

"$BUILD/tmp/test_tmpfs"
