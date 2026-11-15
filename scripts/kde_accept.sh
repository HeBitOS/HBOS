#!/usr/bin/env bash
# HBOS KDE 桌面自动验收：启动 VM，按时间采样，判定"面板 / 桌面壁纸 / 窗口"三件事。
#
# 背景（实测参数，务必保留）：
#   - 单核 + 软件渲染下，面板首次出现约需 400~600 秒；
#     观测窗口短于 500 秒会把"面板还在画"误判成"没有面板"。
#   - -smp 2 会让内核 All tasks terminated，只能 -smp 1。
#   - 4GB 内存不够（plasmashell ENOMEM → SIGSEGV），必须 6144M。
#
# 用法:
#   bash scripts/kde_accept.sh                 # 默认观测 700 秒
#   bash scripts/kde_accept.sh --secs 900
#   bash scripts/kde_accept.sh --img build/hbos-kde-chromium.img --iso build/hbos-bios.iso
set -uo pipefail
cd "$(dirname "$0")/.."

SECS=700
IMG=build/hbos-kde-chromium.img
ISO=build/hbos-bios.iso
OUT=build/tmp/accept
MEM=6144M

while [ $# -gt 0 ]; do
    case "$1" in
        --secs) SECS=$2; shift 2 ;;
        --img)  IMG=$2;  shift 2 ;;
        --iso)  ISO=$2;  shift 2 ;;
        --mem)  MEM=$2;  shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数 $1"; exit 2 ;;
    esac
done

mkdir -p "$OUT/shots"
SERIAL=$OUT/serial.log
MON=$OUT/mon.sock
rm -f "$MON" "$SERIAL"

# 同一时刻只允许一台 VM（宿主内存有限）
exec 9>/tmp/hbos-vm.lock
if ! flock -w 900 9; then
    echo "错误: 900 秒内没拿到 VM 锁（/tmp/hbos-vm.lock），可能别的验证还在跑"; exit 75
fi

echo "[accept] 启动 QEMU ($MEM, -smp 1)…"
qemu-system-x86_64 -m "$MEM" -smp 1 -enable-kvm -cpu host \
    -display none -no-reboot -device ich9-ahci,id=ahci \
    -drive id=disk0,file="$IMG",format=raw,if=none \
    -device ide-hd,drive=disk0,bus=ahci.0 \
    -netdev user,id=hbosnet -device virtio-net-pci,netdev=hbosnet,disable-modern=on \
    -cdrom "$ISO" -boot d \
    -monitor "unix:$MON,server,nowait" \
    -serial "file:$SERIAL" \
    > "$OUT/qemu.err" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null; wait $QPID 2>/dev/null' EXIT

python3 - "$MON" "$OUT" "$SECS" "$QPID" <<'PY'
import os, socket, sys, time

mon, out, secs, qpid = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
ppm = os.path.join(out, "shots", "cur.ppm")
png = os.path.join(out, "shots", "cur.png")

s = None
for _ in range(300):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(8)
        s.connect(mon)
        time.sleep(0.2)
        try: s.recv(65536)
        except Exception: pass
        break
    except OSError:
        s = None
        time.sleep(0.1)
if s is None:
    print("[accept] 无法连接 QEMU monitor"); sys.exit(2)

def grab():
    """抓一帧，等 PPM 写完再转 PNG。"""
    try:
        s.sendall(f"screendump {ppm}\n".encode())
        try:
            s.settimeout(2); s.recv(65536)
        except Exception:
            pass
        prev = -1
        for _ in range(100):
            try: cur = os.path.getsize(ppm)
            except OSError: cur = -1
            if cur > 1000 and cur == prev:
                from PIL import Image
                Image.open(ppm).save(png)
                return True
            prev = cur
            time.sleep(0.03)
    except Exception as e:
        print("[accept] 抓帧失败:", e)
    return False

def metrics():
    """面板：底部 45px 非黑比例；桌面：避开窗口与面板的上方区域非黑比例。"""
    from PIL import Image
    im = Image.open(png).convert("L")
    px = im.load(); w, h = im.size
    def frac(x0, y0, x1, y1, step=6):
        n = t = 0
        for y in range(y0, min(y1, h), step):
            for x in range(x0, min(x1, w), step * 2):
                t += 1
                if px[x, y] > 16: n += 1
        return (n * 100 // t) if t else 0
    panel = frac(0, h - 45, w, h)
    desk  = frac(int(w*0.05), int(h*0.06), int(w*0.95), int(h*0.18))
    whole = frac(0, 0, w, h)
    return panel, desk, whole, (w, h)

print(f"[accept] 观测 {secs} 秒，每 60 秒采样")
timeline = []
t0 = time.time()
nextmark = 60
first_panel = None
while time.time() - t0 < secs:
    if time.time() - t0 < nextmark:
        time.sleep(2)
        continue
    el = int(time.time() - t0)
    nextmark += 60
    if not grab():
        continue
    panel, desk, whole, size = metrics()
    timeline.append((el, panel, desk, whole))
    mark = ""
    if panel > 20 and first_panel is None:
        first_panel = el
        mark = "  <== 面板出现"
    print(f"  t={el:4d}s panel={panel:3d}% desktop={desk:3d}% screen={whole:3d}%{mark}")

# 结论
print()
if not grab():
    print("[accept] 结束前抓帧失败"); sys.exit(3)
panel, desk, whole, size = metrics()
print("=" * 56)
print(f"最终画面: {size[0]}x{size[1]}   panel={panel}%  desktop={desk}%  screen={whole}%")
if first_panel:
    print(f"面板首次出现: t={first_panel}s")
reach = []
if panel > 20: reach.append("面板")
else:          reach.append("面板(缺)")
if desk > 20:  reach.append("壁纸/桌面内容")
else:          reach.append("壁纸/桌面内容(缺)")
print("判定:", "、".join(reach))
print(f"最终截图: {png}")
print("=" * 56)
# 退出码约定：0 面板+壁纸都达成；10 只有面板；20 都没有
if panel > 20 and desk > 20: sys.exit(0)
if panel > 20:               sys.exit(10)
sys.exit(20)
PY
RC=$?
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null
trap - EXIT
echo "[accept] 退出码=$RC （0=面板+壁纸；10=只有面板；20=都没有；75=抢不到锁）"
exit $RC
