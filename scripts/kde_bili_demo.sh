#!/usr/bin/env bash
# 一次跑完：启动 HBOS VM -> 等 Chromium 窗口 -> 在地址栏输入 bilibili -> 截图。
# 设计成可脱离终端独立运行（setsid + nohup），避免工具调用被中断时连带杀掉。
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=build/tmp/bili
URL=${URL:-www.bilibili.com}
WAIT_CHROME=${WAIT_CHROME:-240}
WAIT_LOAD=${WAIT_LOAD:-150}
mkdir -p "$OUT"
rm -f "$OUT/mon.sock" "$OUT/serial.log"
LOG="$OUT/demo.log"
: > "$LOG"

say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

# 同一时刻只允许一台 VM
exec 9>/tmp/hbos-vm.lock
if ! flock -w 600 9; then
    say "抢不到 VM 锁，退出"; exit 75
fi

say "启动 QEMU（6GB, -smp 1）"
qemu-system-x86_64 -m 6144M -smp 1 -enable-kvm -cpu host \
    -display none -no-reboot -device ich9-ahci,id=ahci \
    -drive id=disk0,file=build/hbos-kde-chromium.img,format=raw,if=none \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    -netdev user,id=hbosnet -device virtio-net-pci,netdev=hbosnet,disable-modern=on \
    -cdrom build/hbos-bios.iso -boot d \
    -monitor "unix:$OUT/mon.sock,server,nowait" \
    -serial "file:$OUT/serial.log" \
    >> "$LOG" 2>&1 &
QPID=$!
say "qemu pid=$QPID"

# 等 Chromium 被 exec
t=0
while [ $t -lt "$WAIT_CHROME" ]; do
    if grep -qE '\[EXEC\].*usr/lib/chromium/chromium' "$OUT/serial.log" 2>/dev/null; then
        say "Chromium 已启动（t≈${t}s）"; break
    fi
    sleep 3; t=$((t+3))
done
if [ "$t" -ge "$WAIT_CHROME" ]; then
    say "等待 Chromium 超时"
fi
sleep 45     # 给窗口一点时间真正绘制出来

python3 - "$OUT/mon.sock" "$OUT" "$URL" "$WAIT_LOAD" <<'PY'
import os, socket, sys, time
mon, out, url, wait_load = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])

KEYMAP = {".": "dot", "/": "slash", "-": "minus"}
for c in "abcdefghijklmnopqrstuvwxyz": KEYMAP[c] = c
for d in "0123456789": KEYMAP[d] = d

s = None
for _ in range(200):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(8)
        s.connect(mon); time.sleep(0.2)
        try: s.recv(65536)
        except Exception: pass
        break
    except OSError:
        s = None; time.sleep(0.1)
if s is None:
    print("[demo] 连不上 monitor"); sys.exit(2)

def cmd(c, w=0.08):
    s.sendall((c + "\n").encode()); time.sleep(w)
    try: s.recv(65536)
    except Exception: pass

def shot(name):
    p = os.path.join(out, name + ".ppm")
    cmd(f"screendump {p}", 0.8)
    prev = -1
    for _ in range(120):
        try: cur = os.path.getsize(p)
        except OSError: cur = -1
        if cur > 1000 and cur == prev: break
        prev = cur; time.sleep(0.03)
    from PIL import Image
    Image.open(p).save(os.path.join(out, name + ".png"))
    return os.path.join(out, name + ".png")

print("[demo] 截图（打开前）:", shot("before"))
cmd("sendkey ctrl-l", 0.6)
time.sleep(0.6)
for ch in url:
    k = KEYMAP.get(ch)
    if k: cmd(k, 0.10)
print("[demo] 已输入:", url)
time.sleep(0.6)
cmd("sendkey ret", 0.6)
print(f"[demo] 回车，等待 {wait_load}s 加载…")
time.sleep(wait_load)
png = shot("bilibili")
from PIL import Image
im = Image.open(png).convert("L"); px = im.load(); w, h = im.size
nb = sum(1 for y in range(0, h, 8) for x in range(0, w, 8) if px[x, y] > 16)
tot = len(range(0, h, 8)) * len(range(0, w, 8))
print(f"[demo] 最终截图: {png}")
print(f"[demo] 非黑像素: {nb*100//max(tot,1)}%")
s.close()
PY

say "完成。截图在 $OUT/bilibili.png，VM 保持运行（pid=$QPID）供你观察"
say "监控面板若指向 $OUT/mon.sock 可直接看画面"
