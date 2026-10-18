import socket, subprocess, sys, time, os
iso, disk, out = sys.argv[1], sys.argv[2], sys.argv[3]
steps = sys.argv[4:]
mon = "/tmp/qgui2.sock"
try: os.unlink(mon)
except FileNotFoundError: pass
p = subprocess.Popen(["qemu-system-x86_64","-m","512M","-cdrom",iso,"-boot","d",
  "-drive","file=%s,format=raw,if=ide,index=0" % disk,"-serial","file:.tmp/serial.log",
  "-display","none","-monitor","unix:%s,server,nowait" % mon])
time.sleep(18)
s = socket.socket(socket.AF_UNIX); s.connect(mon)
def cmd(c, wait=1.2):
    s.send(c.encode()+b"\n"); time.sleep(wait)
cmd("sendkey h", 2)
for st in steps:
    if st.startswith("mouse_move"):
        # 每帧位移有上限（clamp_delta）：把一次跳转拆成 20 个小步
        parts = st.split()
        dx, dy = int(parts[1]), int(parts[2])
        for _ in range(20):
            cmd("mouse_move %d %d" % (dx // 20, dy // 20), 0.12)
    else:
        cmd(st)
cmd("quit", 0.5)
try: p.wait(timeout=5)
except Exception: p.kill()
print("done")
