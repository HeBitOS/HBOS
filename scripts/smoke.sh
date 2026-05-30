#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
QEMU="${QEMU:-qemu-system-x86_64}"
OVMF_CODE="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[SMOKE] missing command: $1"
        exit 1
    }
}

run_guest() {
    local name="$1"
    local log="$2"
    shift 2

    echo "[SMOKE] boot $name"
    : >"$log"
    "$@" >"$log" 2>&1 &
    local pid=$!

    for _ in $(seq 1 30); do
        if grep -q "\[SELFTEST\] POSIX/ramfs: PASS" "$log" &&
           grep -q "\[KERN\] Shell ready" "$log"; then
            kill "$pid" >/dev/null 2>&1 || true
            wait "$pid" >/dev/null 2>&1 || true
            echo "[SMOKE] $name: PASS"
            return 0
        fi
        if ! kill -0 "$pid" >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done

    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
    echo "[SMOKE] $name: FAIL"
    tail -80 "$log"
    return 1
}

need "$QEMU"
need qemu-img
need grep

if [[ ! -r "$OVMF_CODE" || ! -r "$OVMF_VARS" ]]; then
    echo "[SMOKE] missing OVMF firmware:"
    echo "  $OVMF_CODE"
    echo "  $OVMF_VARS"
    exit 1
fi

make -C "$ROOT" release install-img

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cp "$OVMF_VARS" "$BUILD/OVMF_VARS_SMOKE_ISO.fd"
run_guest "bios-iso" "$tmpdir/bios-iso.log" \
    "$QEMU" -m 512M \
    -cdrom "$BUILD/hbos-bios.iso" -boot d \
    -serial stdio -monitor none -display none -no-reboot

run_guest "uefi-iso" "$tmpdir/uefi-iso.log" \
    "$QEMU" -machine q35 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS_SMOKE_ISO.fd" \
    -cdrom "$BUILD/hbos-uefi.iso" -boot d \
    -serial stdio -monitor none -display none -no-reboot

run_guest "bios-hdd" "$tmpdir/bios-hdd.log" \
    "$QEMU" -m 512M \
    -device ich9-ahci,id=ahci \
    -drive file="$BUILD/hbos_installed_bios.img",format=raw,if=none,id=hd0 \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -boot c -serial stdio -monitor none -display none -no-reboot

cp "$OVMF_VARS" "$BUILD/OVMF_VARS_SMOKE_HDD.fd"
run_guest "uefi-hdd" "$tmpdir/uefi-hdd.log" \
    "$QEMU" -machine q35 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS_SMOKE_HDD.fd" \
    -device ich9-ahci,id=ahci \
    -drive file="$BUILD/hbos_installed_uefi.img",format=raw,if=none,id=hd0 \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -boot c -serial stdio -monitor none -display none -no-reboot

cp "$OVMF_VARS" "$BUILD/OVMF_VARS_SMOKE_VMDK.fd"
run_guest "vmware-vmdk" "$tmpdir/vmware-vmdk.log" \
    "$QEMU" -machine q35 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS_SMOKE_VMDK.fd" \
    -device ich9-ahci,id=ahci \
    -drive file="$BUILD/hbos_vmware_uefi.vmdk",format=vmdk,if=none,id=hd0 \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -boot c -serial stdio -monitor none -display none -no-reboot

echo "[SMOKE] PASS"
