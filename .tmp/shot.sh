#!/bin/bash
# 用法: shot.sh <iso> <disk.img> <out.png> [seconds]
ISO=$1; DISK=$2; OUT=$3; SECS=${4:-25}
MON=$(mktemp -u /tmp/mon.XXXX)
rm -f "$OUT" "$MON"
qemu-system-x86_64 -m 512M -cdrom "$ISO" -boot d -drive file="$DISK",format=raw,if=ide,index=0 \
  -serial file:.tmp/serial.log -display none -monitor unix:"$MON",server,nowait &
QPID=$!
sleep "$SECS"
python3 - "$MON" "$OUT" <<'PY'
import socket, sys, time
sock = socket.socket(socket.AF_UNIX)
sock.connect(sys.argv[1])
time.sleep(0.3)
sock.send(b"info status\n"); time.sleep(0.3)
sock.recv(65536)
sock.send(("screendump %s\n" % sys.argv[2]).encode()); time.sleep(1.0)
try: sock.recv(65536)
except Exception: pass
sock.send(b"quit\n"); time.sleep(0.3)
PY
wait $QPID 2>/dev/null
ls -l "$OUT" 2>/dev/null | awk '{print $5, $9}'
