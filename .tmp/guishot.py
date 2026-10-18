import socket, subprocess, sys, time, os
iso = sys.argv[1] if len(sys.argv) > 1 else "build/hbos-bios.iso"
disk = sys.argv[2] if len(sys.argv) > 2 else ".tmp/gui.img"
out = sys.argv[3] if len(sys.argv) > 3 else ".tmp/shot.png"
steps = sys.argv[4:] or ["mouse_move -900 -400", "mouse_button 1", "mouse_button 0", "screendump %s" % out]
mon = "/tmp/qgui.sock"
try: os.unlink(mon)
except FileNotFoundError: pass
p = subprocess.Popen(["qemu-system-x86_64","-m","512M","-cdrom",iso,"-boot","d",
  "-drive","file=%s,format=raw,if=ide,index=0" % disk,"-serial","file:.tmp/serial.log",
  "-display","none","-monitor","unix:%s,server,nowait" % mon])
time.sleep(18)
s = socket.socket(socket.AF_UNIX); s.connect(mon)
def cmd(c, wait=0.8):
    s.send(c.encode()+b"\n"); time.sleep(wait)
cmd("sendkey h", 2)
for st in steps:
    cmd(st)
cmd("quit", 0.5)
try: p.wait(timeout=5)
except Exception: p.kill()
print("done", out)
