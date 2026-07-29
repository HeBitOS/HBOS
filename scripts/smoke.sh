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

run_network_guest() {
    local name="$1"
    local log="$2"
    local expected_driver="$3"
    shift 3
    local input_fifo="$tmpdir/$name.in"

    echo "[SMOKE] network $name"
    : >"$log"
    mkfifo "$input_fifo"
    exec 9<>"$input_fifo"
    "$@" <"$input_fifo" >"$log" 2>&1 &
    local pid=$!

    for _ in $(seq 1 30); do
        grep -q "\[KERN\] Shell ready" "$log" && break
        if ! kill -0 "$pid" >/dev/null 2>&1; then break; fi
        sleep 1
    done
    printf 't' >&9
    sleep 1
    printf 'netinfo\ndhcp\n' >&9

    for _ in $(seq 1 15); do
        grep -q "dhcp: ok ip=10.0.2.15" "$log" && break
        if ! kill -0 "$pid" >/dev/null 2>&1; then break; fi
        sleep 1
    done
    printf 'ping 10.0.2.2\n' >&9

    local passed=0
    for _ in $(seq 1 15); do
        if grep -q "driver: $expected_driver" "$log" &&
           grep -q "dhcp: ok ip=10.0.2.15" "$log" &&
           grep -q "PING 10.0.2.2: reply" "$log"; then
            passed=1
            break
        fi
        if ! kill -0 "$pid" >/dev/null 2>&1; then break; fi
        sleep 1
    done

    exec 9>&-
    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
    if [[ "$passed" == 1 ]]; then
        echo "[SMOKE] $name: PASS"
        return 0
    fi
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

run_network_guest "rtl8139-dhcp" "$tmpdir/rtl8139-dhcp.log" "Realtek RTL8139" \
    "$QEMU" -m 512M \
    -cdrom "$BUILD/hbos-bios.iso" -boot d \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -serial stdio -monitor none -display none -no-reboot

run_network_guest "e1000-dhcp" "$tmpdir/e1000-dhcp.log" "Intel E1000" \
    "$QEMU" -m 512M \
    -cdrom "$BUILD/hbos-bios.iso" -boot d \
    -netdev user,id=net0 -device e1000,netdev=net0 \
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

run_guest "bios-vmdk" "$tmpdir/bios-vmdk.log" \
    "$QEMU" -m 512M \
    -device ich9-ahci,id=ahci \
    -drive file="$BUILD/hbos_vmware_bios.vmdk",format=vmdk,if=none,id=hd0 \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -boot c -serial stdio -monitor none -display none -no-reboot

run_guest "bios-vdi" "$tmpdir/bios-vdi.log" \
    "$QEMU" -m 512M \
    -device ich9-ahci,id=ahci \
    -drive file="$BUILD/hbos_virtualbox_bios.vdi",format=vdi,if=none,id=hd0 \
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
