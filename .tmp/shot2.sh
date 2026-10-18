#!/bin/bash
ISO=$1; DISK=$2; OUT=$3; SECS=${4:-20}
MON=$(mktemp -u /tmp/mon.XXXX)
rm -f "$OUT" "$MON"
qemu-system-x86_64 -m 512M -cdrom "$ISO" -boot d -drive file="$DISK",format=raw,if=ide,index=0 \
  -serial file:.tmp/serial.log -display none -monitor unix:"$MON",server,nowait &
QPID=$!
sleep "$SECS"
python3 - "$MON" "$OUT" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
def cmd(c, wait=0.6):
    s.send(c.encode()+b"\n"); time.sleep(wait)
    try: return s.recv(1 << 20).decode(errors="replace")
    except Exception: return ""
cmd("sendkey h", 2)
cmd("screendump %s" % sys.argv[2], 1.5)
cmd("quit", 0.5)
PY
wait $QPID 2>/dev/null
