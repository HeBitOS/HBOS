#!/usr/bin/env bash
# G1 多连接 TCP / Linux AF_INET 双 socket 验证。
# 宿主起 python http.server；QEMU 用两个 guestfwd 端口（各自独立
# chardev）分别指到宿主；guest 里 linux_socket2 开两个并发 AF_INET
# socket 各发一个 GET，先读第二个——第一个连接的响应必须在内核分发器
# 轮询第二个连接期间被缓冲进自己的槽。
# 用法：scripts/test_linux_socket2.sh [ISO]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${1:-$ROOT/build/hbos-bios.iso}"
WORK="${TMPDIR:-/tmp}/hbos-sock2"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1"; exit 1; }; }
need qemu-system-x86_64
need python3
need curl

pkill -f 'http.server 8080' 2>/dev/null || true
sleep 0.5
mkdir -p "$WORK/www"
printf 'hello single-conn test\n' > "$WORK/www/a.txt"
python3 -m http.server 8080 --bind 127.0.0.1 -d "$WORK/www" \
    >"$WORK/http.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1
curl -sS -m 2 http://127.0.0.1:8080/a.txt >/dev/null || {
    echo "[sock2] host server failed"; cat "$WORK/http.log"; exit 1; }

rm -f "$WORK/qemu.in" "$WORK/qemu.log"
mkfifo "$WORK/qemu.in"
exec 9<>"$WORK/qemu.in"
timeout 120 qemu-system-x86_64 -m 512M \
    -cdrom "$ISO" -boot d \
    -chardev socket,id=ch0,host=127.0.0.1,port=8080,server=off \
    -chardev socket,id=ch1,host=127.0.0.1,port=8080,server=off \
    -netdev user,id=net0,\
      guestfwd=tcp:10.0.2.99:18080-chardev:ch0,\
      guestfwd=tcp:10.0.2.99:18081-chardev:ch1 \
    -device e1000,netdev=net0 \
    -serial stdio -monitor none -display none -no-reboot \
    <"$WORK/qemu.in" >"$WORK/qemu.log" 2>&1 &
QPID=$!

for _ in $(seq 1 60); do
    grep -q "Shell ready" "$WORK/qemu.log" && break
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
sleep 2
printf 't' >&9
sleep 2
printf 'dhcp\n' >&9
sleep 6
printf 'run linux_socket2 10.0.2.99 18080 18081\n' >&9
for _ in $(seq 1 20); do
    grep -q "LINUX_SOCKET2: PASS\|LINUX_SOCKET2: FAIL" "$WORK/qemu.log" && break
    sleep 1
done
exec 9>&-
kill $QPID 2>/dev/null || true
wait $QPID 2>/dev/null || true

if grep -q "LINUX_SOCKET2: PASS" "$WORK/qemu.log"; then
    echo "[sock2] PASS"
    exit 0
fi
echo "[sock2] FAIL"
tail -30 "$WORK/qemu.log"
exit 1
