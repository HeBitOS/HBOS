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
    local expected="$3"
    local timeout_seconds="${4:-45}"
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
    # Multi-line commands (linux_jspage) are sent line by line; the shell
    # mishandles ';' separators after a redirect, so keep them apart.
    printf '%b\n' "$command" | while IFS= read -r line; do
        printf '%s\n' "$line" >&9
        sleep 1
    done
    sleep 5
    exec 9>&-
    if grep -Fq "$expected" "$LOG"; then
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

if [[ -n "${HBOS_COMPAT_TESTS:-}" ]]; then
    read -r -a tests <<<"$HBOS_COMPAT_TESTS"
else
    tests=(linux_syscall linux_pie linux_dynamic linux_abi linux_signal linux_signal_siginfo linux_js linux_jspage linux_mprotect linux_mmap_reclaim linux_file_mmap linux_process_abi linux_dlopen linux_dlopen_deps linux_compat_thread linux_clone3 linux_epoll_et linux_inotify)
    if [[ "${HBOS_MUSL_SMOKE:-0}" == "1" ]]; then
        tests=(linux_musl_stream linux_musl_pthread "${tests[@]}")
    fi
    if [[ "${HBOS_GLIBC_SMOKE:-0}" == "1" ]]; then
        tests=(linux_glibc "${tests[@]}")
    fi
fi

result=0
for test in "${tests[@]}"; do
    case "$test" in
        linux_syscall|linux_pie) expected="LINUX_SYSCALL_ELF: PASS" ;;
        linux_dynamic) expected="LINUX_INTERP: PASS" ;;
        linux_musl|linux_musl_stream) expected="LINUX_MUSL: PASS" ;;
        linux_musl_pthread) expected="LINUX_MUSL_PTHREAD: PASS" ;;
        linux_glibc) expected="LINUX_GLIBC: PASS" ;;
        linux_abi) expected="LINUX_ABI: PASS" ;;
        linux_signal) expected="LINUX_SIGNAL: PASS" ;;
        linux_signal_siginfo) expected="LINUX_SIGNAL_SIGINFO: PASS" ;;
        linux_js) expected="LINUX_JS: PASS" ;;
        linux_jspage) expected="title: JT" ;;
        linux_mprotect) expected="LINUX_MPROTECT: PASS" ;;
        linux_mmap_reclaim) expected="LINUX_MMAP_RECLAIM: PASS" ;;
        linux_file_mmap) expected="LINUX_FILE_MMAP: PASS" ;;
        linux_process_abi) expected="LINUX_PROCESS_ABI: PASS" ;;
        linux_dlopen) expected="LINUX_DLOPEN: PASS" ;;
        linux_dlopen_deps) expected="LINUX_DLOPEN_DEPS: PASS" ;;
        linux_compat_thread) expected="LINUX_THREAD: PASS" ;;
        linux_clone3) expected="LINUX_CLONE3: PASS" ;;
        linux_epoll_et) expected="LINUX_EPOLL_ET: PASS" ;;
        linux_inotify) expected="LINUX_INOTIFY: PASS" ;;
        *) echo "Unknown compatibility test: $test" >&2; exit 2 ;;
    esac
    case "$test" in
        linux_js) command="run js -t" ;;
        linux_jspage) command="echo '<html><script>document.title=\"JT\";document.write(6*7)</script></html>' > /tmp/j.html\njspage /tmp/j.html" ;;
        *) command="run $test" ;;
    esac
    if run_guest "$test" "$command" "$expected"; then
        :
    else
        result=1
    fi
done
rm -f "$LOG"
exit "$result"
