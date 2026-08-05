#!/usr/bin/env bash
# Boot the HBOS BIOS ISO in QEMU and run the Linux compatibility smoke
# tests from the shell prompt, verifying each expected PASS marker.
#
# Prerequisites:
#   make bios-iso HBOS_COMPAT_SMOKE=1   (ISO with compat smoke apps)
#   qemu-system-x86_64
#
# Usage:
#   scripts/test_linux_compat_smoke.sh [iso_path]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${1:-$ROOT/build/hbos-bios.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="$(mktemp)"

if [[ ! -f "$ISO" ]]; then
    echo "ISO not found: $ISO (run: make bios-iso HBOS_COMPAT_SMOKE=1)" >&2
    exit 2
fi

run_guest() {
    local name="$1"
    local command="$2"
    local timeout_seconds="${3:-45}"
    echo "[COMPAT-SMOKE] $name: boot"
    : >"$LOG"

    # stdin fifo for serial commands
    local fifo="$ROOT/build/.compat-smoke.fifo"
    rm -f "$fifo"
    mkfifo "$fifo"
    exec 9<>"$fifo"
    "$QEMU" -m 512M -cdrom "$ISO" -boot d -serial stdio \
        -display none -no-reboot -no-shutdown \
        <"$fifo" >>"$LOG" 2>&1 &
    local pid=$!

    local elapsed=0
    while (( elapsed < timeout_seconds )); do
        if grep -q "\[KERN\] Shell ready" "$LOG"; then
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "[COMPAT-SMOKE] $name: QEMU exited early"
            tail -40 "$LOG"
            rm -f "$fifo"
            return 1
        fi
        sleep 1
        ((elapsed++))
    done

    if ! grep -q "\[KERN\] Shell ready" "$LOG"; then
        echo "[COMPAT-SMOKE] $name: shell not ready in ${timeout_seconds}s"
        tail -40 "$LOG"
        rm -f "$fifo"
        return 1
    fi

    # The boot menu offers a launch-mode prompt; pick the command-line Shell.
    printf 'T\n' >&9
    sleep 2
    printf '%s\n' "$command" >&9
    sleep 5
    exec 9>&-
    if grep -q ": PASS" "$LOG"; then
        echo "[COMPAT-SMOKE] $name: PASS"
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        rm -f "$fifo"
        return 0
    fi

    echo "[COMPAT-SMOKE] $name: FAIL"
    tail -60 "$LOG"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$fifo"
    return 1
}

result=0
for test in linux_syscall linux_pie linux_abi linux_compat_thread linux_epoll_et; do
    if run_guest "$test" "run $test"; then
        :
    else
        result=1
    fi
done
rm -f "$LOG"
exit "$result"
