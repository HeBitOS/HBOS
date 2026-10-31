#!/usr/bin/env python3
"""真实鼠标移动测试：QEMU 注入 → PS/2 → 内核 → HIVE 指针。

为什么用截图而不是读坐标：HIVE 的指针位置在主循环的局部变量里，
在 GUI 模式下没有可用的串口 shell 去执行 `mouse` 命令。截图是唯一
不依赖人工观察的验证手段。

判定思路：
  1. 先连续截两张（不动鼠标）当作基线 —— 桌面上的时钟、动画会让
     每次截图都不同，不先量一下基线的差异范围，就会把"时钟跳了一秒"
     误判成"鼠标动了"。
  2. 注入几次 mouse_move，再截一张。
  3. 比较"移动前后的差异范围"是否明显大于基线差异。
     指针移动会在画面上留下一条从旧位置到新位置的变化带。

用法：python3 scripts/mouse_move_test.py [--iso PATH] [--keep]
退出码 0 = PASS，1 = FAIL。
"""

from __future__ import annotations

import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def start_qemu(iso: Path, mon: Path, log: Path) -> subprocess.Popen:
    if mon.exists():
        mon.unlink()
    cmd = [
        "qemu-system-x86_64",
        "-cdrom", str(iso),
        "-m", "1024M",
        "-boot", "d",
        "-display", "none",
        "-vga", "std",
        "-serial", f"file:{log}",
        "-monitor", f"unix:{mon},server,nowait",
        "-no-reboot",
    ]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def mon_send(sock: socket.socket, line: str, wait: float = 0.3) -> None:
    sock.sendall(line.encode() + b"\n")
    time.sleep(wait)


def screendump(sock: socket.socket, path: Path, wait: float = 1.5) -> bool:
    if path.exists():
        path.unlink()
    mon_send(sock, f"screendump {path}", wait)
    return path.exists()


def image_bbox_diff(a: Path, b: Path, skip_top: int = 0):
    """返回两张 PPM 的差异包围盒 (x0, y0, x1, y1)，完全相同返回 None。

    skip_top：裁掉顶部若干像素。HIVE 顶栏有时钟，秒数每帧都在跳，
    不裁掉的话"时钟跳了一秒"会被当成"鼠标动了"。
    """
    from PIL import Image, ImageChops
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return (0, 0, max(ia.size[0], ib.size[0]),
                max(ia.size[1], ib.size[1]))
    if skip_top > 0:
        box = (0, skip_top, ia.size[0], ia.size[1])
        ia = ia.crop(box)
        ib = ib.crop(box)
    return ImageChops.difference(ia, ib).getbbox()


def bbox_area(box) -> int:
    if not box:
        return 0
    return max(0, box[2] - box[0]) * max(0, box[3] - box[1])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default=str(ROOT / "build/hbos-bios.iso"))
    ap.add_argument("--boot", type=float, default=30.0,
                    help="等桌面起来的时间（TCG 下 25-40s）")
    ap.add_argument("--keep", action="store_true", help="保留中间截图")
    args = ap.parse_args()

    iso = Path(args.iso)
    if not iso.exists():
        print(f"[mouse-test] 找不到 ISO: {iso}")
        return 1

    out_dir = ROOT / "build/tmp"
    out_dir.mkdir(parents=True, exist_ok=True)
    mon = out_dir / "mouse-test.sock"
    log = out_dir / "mouse-test.log"
    shot_a = out_dir / "mouse-a.ppm"
    shot_b = out_dir / "mouse-b.ppm"
    shot_c = out_dir / "mouse-c.ppm"

    print(f"[mouse-test] 启动 QEMU（等待 {args.boot:.0f}s 到桌面）…")
    qemu = start_qemu(iso, mon, log)
    try:
        # 等 monitor socket 出现
        for _ in range(100):
            if mon.exists():
                break
            time.sleep(0.1)
        else:
            print("[mouse-test] monitor socket 没起来")
            return 1

        # 先等引导菜单出现，再回车选默认项（GUI）。
        # 少了这一步会一直停在菜单上 —— 截图只有十几种颜色、鼠标当然不动，
        # 看起来像"指针坏了"，其实是桌面根本没启动。
        time.sleep(8)
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(str(mon))
        time.sleep(0.3)
        mon_send(sock, "sendkey ret", 1.0)
        print(f"[mouse-test] 已回车进入 GUI，等待 {args.boot:.0f}s 桌面就绪…")
        time.sleep(args.boot)

        if not screendump(sock, shot_a):
            print("[mouse-test] 第一次 screendump 失败")
            return 1

        # 基线：不动鼠标再截一张，量出"本来就有的差异"（时钟/动画）
        time.sleep(1.2)
        if not screendump(sock, shot_c):
            print("[mouse-test] 基线 screendump 失败")
            return 1
        base_box = image_bbox_diff(shot_a, shot_c, skip_top=40)
        base_area = bbox_area(base_box)
        print(f"[mouse-test] 基线差异: {base_box} 面积={base_area}")

        # 注入移动：每步 <=120px、间隔 >=0.4s —— HMP 的 clamp_delta 会把
        # 单帧累计位移钳到 127，连发会被合并后截断，落点会飘。
        print("[mouse-test] 注入鼠标移动…")
        for _ in range(3):
            mon_send(sock, "mouse_move 90 0", 0.45)
        mon_send(sock, "mouse_move 0 70", 0.45)

        time.sleep(0.6)
        if not screendump(sock, shot_b):
            print("[mouse-test] 移动后 screendump 失败")
            return 1

        move_box = image_bbox_diff(shot_a, shot_b, skip_top=40)
        move_area = bbox_area(move_box)
        print(f"[mouse-test] 移动后差异: {move_box} 面积={move_area}")

        # 判定：移动带来的变化必须显著大于基线噪声。
        # 阈值取基线的 3 倍且至少 2000 像素 —— 指针是一个约 12x18 的
        # 箭头，移动后旧位置和新位置两块加起来通常上千像素。
        threshold = max(base_area * 3, 2000)
        if move_area < threshold:
            print(f"[mouse-test] FAIL: 变化面积 {move_area} < 阈值 {threshold}")
            print("[mouse-test] 指针没动，或者 HIVE 主循环没在消费鼠标事件")
            return 1

        print(f"[mouse-test] PASS: 变化面积 {move_area} >= 阈值 {threshold}")
        sock.close()
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        if not args.keep:
            for p in (shot_a, shot_b, shot_c):
                if p.exists():
                    p.unlink()
    return 0


if __name__ == "__main__":
    sys.exit(main())
