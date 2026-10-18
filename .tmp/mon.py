import socket, subprocess, sys, time, os, signal
mon = "/tmp/qmon.sock"
try: os.unlink(mon)
except FileNotFoundError: pass
p = subprocess.Popen(["qemu-system-x86_64","-m","512M","-cdrom","build/hbos-bios.iso","-boot","d",
  "-drive","file=.tmp/gui.img,format=raw,if=ide,index=0","-serial","file:.tmp/serial.log",
  "-display","none","-monitor","unix:%s,server,nowait" % mon])
time.sleep(18)
s = socket.socket(socket.AF_UNIX); s.connect(mon)
def cmd(c, wait=0.5):
    s.send(c.encode()+b"\n"); time.sleep(wait)
    out=b""
    s.setblocking(False)
    try:
        while True:
            d=s.recv(65536)
            if not d: break
            out+=d
    except Exception: pass
    s.setblocking(True)
    return out.decode(errors="replace")
cmd("sendkey h", 2)
if len(sys.argv)>1:
    print(cmd(sys.argv[1]))
cmd("quit",0.5)
try: p.wait(timeout=5)
except Exception: p.kill()
