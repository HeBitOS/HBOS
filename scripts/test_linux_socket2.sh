#!/usr/bin/env bash
# G1 多连接 TCP / Linux AF_INET 双 socket + 内核回环验证。
# 在 guest 里启动内核 httpd，linux_socket2 开两个并发 AF_INET socket
# 连本机 IP（10.0.2.15）与 127.0.0.1，各发 GET 后先读第二个——第一个
# 连接的响应必须在内核分发器轮询第二个连接期间被缓冲进自己的槽。
# 用法：scripts/test_linux_socket2.sh [ISO]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${1:-$ROOT/build/hbos-bios.iso}"
WORK="${TMPDIR:-/tmp}/hbos-sock2"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1"; exit 1; }; }
need qemu-system-x86_64

rm -rf "$WORK"; mkdir -p "$WORK"
mkfifo "$WORK/qemu.in"
exec 9<>"$WORK/qemu.in"
timeout 120 qemu-system-x86_64 -m 512M \
    -cdrom "$ISO" -boot d \
    -netdev user,id=net0 -device e1000,netdev=net0 \
    -serial stdio -monitor none -display none -no-reboot \
    <"$WORK/qemu.in" >"$WORK/qemu.log" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

for _ in $(seq 1 60); do
    grep -q "Shell ready" "$WORK/qemu.log" && break
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
sleep 2
printf 't' >&9
sleep 2
printf "echo 'hello single-conn test' > /a.txt\n" >&9
sleep 1
printf 'httpd 18080\n' >&9
sleep 2
printf 'run linux_socket2 10.0.2.15 18080 18080\n' >&9
sleep 10
printf 'run linux_socket2 127.0.0.1 18080 18080\n' >&9
sleep 10
exec 9>&-
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true

P1=$(grep -c "LINUX_SOCKET2: PASS" "$WORK/qemu.log" || true)
if [ "$P1" -ge 2 ]; then
    echo "[sock2] PASS (own-IP + 127.0.0.1 loopback dual-socket)"
    exit 0
fi
echo "[sock2] FAIL (passes=$P1)"
grep -n "sock\|httpd\|LINUX_SOCKET2" "$WORK/qemu.log" | tail -30
exit 1
