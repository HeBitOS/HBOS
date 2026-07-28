#!/usr/bin/env bash
set -euo pipefail

HBOS_REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HBOS_NOGUI_BUILD="${NOGUI_BUILD_DIR:-build-nogui}"
HBOS_QEMU="${QEMU:-qemu-system-x86_64}"
HBOS_OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
HBOS_OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[NOGUI] missing command: $1"
        exit 1
    }
}

run_guest() {
    local name="$1"
    local log="$2"
    shift 2

    echo "[NOGUI] boot $name"
    : >"$log"
    "$@" >"$log" 2>&1 &
    local pid=$!

    for _ in $(seq 1 30); do
        if grep -q "\[SELFTEST\] POSIX/ramfs: PASS" "$log" &&
           grep -q "\[KERN\] Shell ready" "$log" &&
           grep -q "no-GUI" "$log"; then
            kill "$pid" >/dev/null 2>&1 || true
            wait "$pid" >/dev/null 2>&1 || true
            echo "[NOGUI] $name: PASS"
            return 0
        fi
        if ! kill -0 "$pid" >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done

    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
    echo "[NOGUI] $name: FAIL"
    tail -80 "$log"
    return 1
}

need "$HBOS_QEMU"
need grep
need nm

if [[ ! -r "$HBOS_OVMF_CODE" || ! -r "$HBOS_OVMF_VARS" ]]; then
    echo "[NOGUI] missing OVMF firmware"
    exit 1
fi

make -C "$HBOS_REPO_DIR" NOGUI_BUILD_DIR="$HBOS_NOGUI_BUILD" nogui

HBOS_NOGUI_KERNEL="$HBOS_REPO_DIR/$HBOS_NOGUI_BUILD/hbos-bios.bin"
if nm "$HBOS_NOGUI_KERNEL" |
   grep -Eq '(^|[[:space:]])(wm_|winsrv_|gpu_|gui_app_calc|_binary_build_gui_|__hbos_app_desc_|app_hello_main|app_uwc_main)'; then
    echo "[NOGUI] forbidden desktop, resource, or bundled-app symbol found"
    exit 1
fi
if ! grep -q 'hax_app_table_count = 0' \
      "$HBOS_REPO_DIR/$HBOS_NOGUI_BUILD/hax_manifest.c"; then
    echo "[NOGUI] bundled HAX app manifest is not empty"
    exit 1
fi
echo "[NOGUI] component boundary: PASS"

HBOS_TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$HBOS_TMP_DIR"' EXIT

run_guest "bios-iso" "$HBOS_TMP_DIR/bios.log" \
    "$HBOS_QEMU" -m 512M \
    -cdrom "$HBOS_REPO_DIR/$HBOS_NOGUI_BUILD/hbos-bios.iso" -boot d \
    -serial stdio -monitor none -display none -no-reboot

cp "$HBOS_OVMF_VARS" "$HBOS_TMP_DIR/OVMF_VARS.fd"
run_guest "uefi-iso" "$HBOS_TMP_DIR/uefi.log" \
    "$HBOS_QEMU" -machine q35 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file="$HBOS_OVMF_CODE" \
    -drive if=pflash,format=raw,file="$HBOS_TMP_DIR/OVMF_VARS.fd" \
    -cdrom "$HBOS_REPO_DIR/$HBOS_NOGUI_BUILD/hbos-uefi.iso" -boot d \
    -serial stdio -monitor none -display none -no-reboot

echo "[NOGUI] PASS"
