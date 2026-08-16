#!/usr/bin/env python3
"""GUI 级浏览器验证：在 QEMU 图形模式中打开本地 Vue 页面，截图并做像素
分析，确认 js.hax DOM 渲染管线（Vue 挂载 → 迷你 HTML → 内核重渲染）在
真实 GUI 里工作。串口驱动 shell 写页面 + 启动 gui <url>；QEMU monitor
unix socket 触发 screendump。
"""
import os
import socket
import subprocess
import sys
import time
import threading

WORKSPACE = "/media/data/hbosv2"
ISO = f"{WORKSPACE}/build/hbos-bios.iso"
SOCK = f"{WORKSPACE}/build/gui-monitor.sock"
PPM = f"{WORKSPACE}/build/gui-shot.ppm"

VUE_PAGE = """<html><head><title>Vue GUI 演示</title></head><body>
<div id="app"><p>加载中...</p></div>
<script>
new Vue({el:"#app",data(){return{msg:"VUE_GUI_OK"}},template:"<div><h1>{{msg}}</h1></div>"});
</script>
</body></html>
"""


def send_cmd(stdin, line, wait=0.8):
    stdin.write(line + "\n")
    stdin.flush()
    time.sleep(wait)


def analyze_ppm(path):
    """检查截图：应包含大片白色页面区 + 深色文字像素。"""
    with open(path, "rb") as f:
        data = f.read()
    # P6 header: P6\nW H\n255\n
    header_end = 0
    for i in range(3):
        header_end = data.index(b"\n", header_end) + 1
    header = data[:header_end].decode().split()
    w, h = int(header[1]), int(header[2])
    body = data[header_end:]
    white = 0
    dark = 0
    total = 0
    # 浏览器窗口由 gui <url> 打开在左上区域；只分析该区域，避免桌面
    # 壁纸/其他窗口让白色和深色占比测试误判。
    x0, x1 = 120, min(w, 980)
    y0, y1 = 50, min(h, 560)
    for y in range(y0, y1, 2):
        row = y * w * 3
        for x in range(x0, x1, 2):
            off = row + x * 3
            if off + 2 >= len(body):
                continue
            r, g, b = body[off], body[off + 1], body[off + 2]
            total += 1
            if r > 240 and g > 240 and b > 240:
                white += 1
            elif r < 120 and g < 120 and b < 120:
                dark += 1
    frac_w = white / max(total, 1)
    frac_d = dark / max(total, 1)
    print(f"[gui-vue] {w}x{h} white={frac_w:.3f} dark={frac_d:.3f}")
    # 浏览器页面区是白底；文字（含标题/列表）应产生深色像素
    return frac_w > 0.05 and frac_d > 0.0005


def main():
    if not os.path.exists(ISO):
        print(f"ISO not found: {ISO} (run: make bios-iso)")
        return 1
    for p in (SOCK, PPM):
        if os.path.exists(p):
            os.remove(p)

    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-m", "512M", "-cdrom", ISO, "-boot", "d",
         "-serial", "stdio", "-vga", "std",
         "-monitor", f"unix:{SOCK},server,nowait", "-no-reboot"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1)

    serial_lines = []
    serial_text = [""]
    def read_serial():
        for line in qemu.stdout:
            serial_lines.append(line)
            serial_text[0] += line
    reader = threading.Thread(target=read_serial, daemon=True)
    reader.start()

    deadline = time.time() + 45
    while "[KERN] Shell ready" not in serial_text[0] and time.time() < deadline:
        if qemu.poll() is not None:
            print("[gui-vue] QEMU exited before shell")
            return 1
        time.sleep(0.2)
    if "[KERN] Shell ready" not in serial_text[0]:
        print("[gui-vue] shell timeout; serial tail:")
        print("".join(serial_lines[-20:]))
        qemu.terminate()
        return 1

    send_cmd(qemu.stdin, "T")  # 启动菜单选 Shell
    time.sleep(2)
    send_cmd(qemu.stdin, "rm /tmp/vue.html")
    # 写入本地 Vue 演示页；页面里的 JS 不含 shell 单引号。
    for line in VUE_PAGE.strip("\n").split("\n"):
        escaped = line.replace("'", "'\\''")
        send_cmd(qemu.stdin, f"echo '{escaped}' >> /tmp/vue.html")
    send_cmd(qemu.stdin, "jspage /tmp/vue.html")
    deadline = time.time() + 20
    while "VUE_GUI_OK" not in serial_text[0] and time.time() < deadline:
        time.sleep(0.2)
    if "VUE_GUI_OK" not in serial_text[0]:
        print("[gui-vue] jspage Vue preflight failed; serial tail:")
        print("".join(serial_lines[-20:]))
        qemu.terminate()
        return 1
    # 启动 GUI 浏览器并加载。加载任务完成后主循环会触发最终重绘；本地页
    # 无网络子资源，给 QuickJS/Vue 和一帧合成留出足够时间。
    send_cmd(qemu.stdin, "gui file:///tmp/vue.html", wait=2)
    time.sleep(10)

    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(SOCK)
    time.sleep(0.5)
    client.recv(4096)
    client.sendall(f"screendump {PPM}\n".encode())
    time.sleep(2)
    client.recv(4096)
    client.sendall(b"quit\n")
    client.close()
    time.sleep(1)
    qemu.terminate()
    qemu.wait(timeout=5)

    if not os.path.exists(PPM):
        print("[gui-vue] screendump failed")
        return 1
    ok = analyze_ppm(PPM)
    print("[gui-vue] " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
