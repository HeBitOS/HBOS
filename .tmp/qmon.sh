#!/bin/bash
# qmon.sh <cmd...> : 起 QEMU、执行命令列表、退出
MON=$(mktemp -u /tmp/mon.XXXX); rm -f "$MON"
qemu-system-x86_64 -m 512M -cdrom build/hbos-bios.iso -boot d -drive file=.tmp/gui.img,format=raw,if=ide,index=0 \
  -serial file:.tmp/serial.log -display none -monitor unix:"$MON",server,nowait &
QPID=$!
sleep 18
python3 - "$MON" "$@" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
def cmd(c, wait=0.5):
    s.send(c.encode()+b"\n"); time.sleep(wait)
    try: return s.recv(1<<20).decode(errors="replace")
    except Exception: return ""
cmd("sendkey h", 2)
for c in sys.argv[2:]:
    print(cmd(c))
cmd("quit", 0.5)
PY
wait $QPID 2>/dev/null
