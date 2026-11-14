#!/usr/bin/env bash
# tests/host/run_preempt.sh — 每核抢占关断计数器的纯逻辑单测。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
mkdir -p "$BUILD/tmp"
CC="${CC:-gcc}"

"$CC" -O2 -Wall -Wextra -I"$ROOT/src" -I"$ROOT/tests/host" \
    "$ROOT/tests/host/test_preempt.c" \
    -o "$BUILD/tmp/test_preempt"

"$BUILD/tmp/test_preempt"
