#!/usr/bin/env bash
set -euo pipefail

HBOS_REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HBOS_MODE="${1:---static}"
HBOS_FAILURES=0

usage() {
    echo "Usage: $0 [--static|--build|--boot]"
    echo "  --static  Check source/API baseline only (default)"
    echo "  --build   Run static checks and build BIOS/UEFI ISOs"
    echo "  --boot    Run static checks and the full existing smoke matrix"
}

pass() {
    echo "[BASELINE] PASS: $1"
}

fail() {
    echo "[BASELINE] FAIL: $1"
    HBOS_FAILURES=$((HBOS_FAILURES + 1))
}

need_command() {
    if command -v "$1" >/dev/null 2>&1; then
        pass "command '$1' is available"
    else
        fail "missing command '$1'"
    fi
}

need_file() {
    if [[ -f "$HBOS_REPO_DIR/$1" ]]; then
        pass "required file '$1'"
    else
        fail "missing required file '$1'"
    fi
}

need_pattern() {
    local file="$1"
    local pattern="$2"
    local label="$3"
    if rg -q "$pattern" "$HBOS_REPO_DIR/$file"; then
        pass "$label"
    else
        fail "$label"
    fi
}

extract_syscalls() {
    local file="$1"
    sed -n '/^[[:space:]]*typedef enum {\|^[[:space:]]*enum {/,/HBOS_SYS_MAX\|^};/p' "$file" |
        sed -n 's/^[[:space:]]*\(HBOS_SYS_[A-Z0-9_]*\).*/\1/p' |
        awk '$0 != "HBOS_SYS_MAX"'
}

case "$HBOS_MODE" in
    --static|--build|--boot)
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        usage
        exit 2
        ;;
esac

echo "[BASELINE] HBOS Chromium compatibility phase 1"
echo "[BASELINE] repository: $HBOS_REPO_DIR"
echo "[BASELINE] mode: $HBOS_MODE"

need_command rg
need_command sed
need_command awk
need_command wc

need_file "CHROMIUM_COMPAT_ROADMAP.md"
need_file "docs/CHROMIUM_COMPAT_BASELINE.md"
need_file "src/syscall.h"
need_file "src/user/libc/syscall.h"
need_file "src/gui/gui_app.h"
need_file "src/gui/gui_apps.c"
need_file "src/gui/gui_state.h"
need_file "src/gui/winsrv.h"
need_file "src/core/task.h"

need_pattern "src/syscall.h" "HBOS_SYS_WIN_OPEN" "kernel window syscall ABI is present"
need_pattern "src/user/libc/syscall.h" "HBOS_SYS_WIN_OPEN" "user window syscall ABI is present"
need_pattern "src/gui/gui_app.h" "gui_app_module_t" "GUI module ABI v1 is present"
need_pattern "src/core/task.c" "void task_schedule\\(void\\)" "preemptive scheduler entry is present"
need_pattern "src/core/heap.c" "void kfree\\(void \\*ptr\\)" \
    "kernel heap exposes kfree"
need_pattern "src/core/heap.c" "block->free = 1" \
    "kernel heap releases blocks instead of retaining the phase-1 no-op"
need_pattern "src/core/heap.c" "merge_next\\(block\\)" \
    "kernel heap coalesces adjacent free blocks"

HBOS_TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$HBOS_TMP_DIR"' EXIT

extract_syscalls "$HBOS_REPO_DIR/src/syscall.h" >"$HBOS_TMP_DIR/kernel-syscalls"
extract_syscalls "$HBOS_REPO_DIR/src/user/libc/syscall.h" >"$HBOS_TMP_DIR/libc-syscalls"

if diff -u "$HBOS_TMP_DIR/kernel-syscalls" "$HBOS_TMP_DIR/libc-syscalls" >/dev/null; then
    pass "kernel and user libc syscall sequences match"
else
    fail "kernel and user libc syscall sequences differ"
    diff -u "$HBOS_TMP_DIR/kernel-syscalls" "$HBOS_TMP_DIR/libc-syscalls" || true
fi

HBOS_SYSCALL_COUNT="$(wc -l <"$HBOS_TMP_DIR/kernel-syscalls" | tr -d ' ')"
HBOS_APP_ID_COUNT="$(rg -c '^#define GUI_APP_[A-Z0-9_]+[[:space:]]+-?[0-9]+' \
    "$HBOS_REPO_DIR/src/gui/gui_state.h")"
HBOS_MODULE_COUNT="$(rg -c '^[[:space:]]*&gui_app_' "$HBOS_REPO_DIR/src/gui/gui_apps.c")"
HBOS_GUI_LINES="$(wc -l <"$HBOS_REPO_DIR/src/tools/gui.c" | tr -d ' ')"
HBOS_EMBEDDED_APP_COUNT="$(rg -c '^static void draw_(notes|uwc|snake|browser|code|diag)_app' \
    "$HBOS_REPO_DIR/src/tools/gui.c")"

echo "[BASELINE] METRIC: syscalls=$HBOS_SYSCALL_COUNT"
echo "[BASELINE] METRIC: gui_app_ids=$HBOS_APP_ID_COUNT"
echo "[BASELINE] METRIC: registered_modules=$HBOS_MODULE_COUNT"
echo "[BASELINE] METRIC: tools_gui_lines=$HBOS_GUI_LINES"
echo "[BASELINE] METRIC: embedded_app_drawers=$HBOS_EMBEDDED_APP_COUNT"

if [[ "$HBOS_SYSCALL_COUNT" -ge 96 ]]; then
    pass "system call inventory contains the 96-call v1 baseline"
else
    fail "system call inventory fell below the 96-call v1 baseline"
fi

if [[ "$HBOS_MODULE_COUNT" -ge 8 ]]; then
    pass "module registry contains the 8-module phase 1 baseline"
else
    fail "module registry fell below the 8-module phase 1 baseline"
fi

if [[ "$HBOS_APP_ID_COUNT" -ge 15 ]]; then
    pass "GUI app ID inventory contains the 15-ID phase 1 baseline"
else
    fail "GUI app ID inventory fell below the 15-ID phase 1 baseline"
fi

if [[ "$HBOS_FAILURES" -ne 0 ]]; then
    echo "[BASELINE] static checks failed: $HBOS_FAILURES"
    exit 1
fi

if [[ "$HBOS_MODE" == "--build" ]]; then
    echo "[BASELINE] building BIOS and UEFI ISOs"
    make -C "$HBOS_REPO_DIR" bios-iso uefi-iso
fi

if [[ "$HBOS_MODE" == "--boot" ]]; then
    echo "[BASELINE] running existing full smoke matrix"
    bash "$HBOS_REPO_DIR/scripts/smoke.sh"
fi

echo "[BASELINE] PASS"
