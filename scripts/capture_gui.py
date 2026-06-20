#!/usr/bin/env python3
import subprocess
import time
import os
import sys
import socket
from PIL import Image

WORKSPACE = "/media/data/hbosv2"
ISO_PATH = f"{WORKSPACE}/build/hbos-bios.iso"
MONITOR_SOCKET = f"{WORKSPACE}/build/qemu-monitor.sock"
PPM_PATH = f"{WORKSPACE}/build/screenshot.ppm"
PNG_PATH = f"{WORKSPACE}/build/screenshot.png"

def capture():
    print("[1] Building project...")
    subprocess.run(["make"], cwd=WORKSPACE, check=True)

    if os.path.exists(MONITOR_SOCKET):
        os.remove(MONITOR_SOCKET)
    if os.path.exists(PPM_PATH):
        os.remove(PPM_PATH)
    if os.path.exists(PNG_PATH):
        os.remove(PNG_PATH)

    print("[2] Booting QEMU in graphic mode...")
    qemu_cmd = [
        "qemu-system-x86_64",
        "-m", "512M",
        "-cdrom", ISO_PATH,
        "-boot", "d",
        "-serial", "stdio",
        "-vga", "std",
        "-monitor", f"unix:{MONITOR_SOCKET},server,nowait",
        "-no-reboot"
    ]

    proc = subprocess.Popen(qemu_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    try:
        print("[3] Waiting 6 seconds for shell ready...")
        time.sleep(6)

        print("[4] Starting GUI...")
        proc.stdin.write("gui\n")
        proc.stdin.flush()

        print("[5] Waiting 3 seconds for GUI render...")
        time.sleep(3)

        print("[6] Connecting to QEMU monitor to trigger screendump...")
        client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        client.connect(MONITOR_SOCKET)
        
        # Read greeting
        time.sleep(0.5)
        client.recv(1024)

        # Send screendump command
        cmd = f"screendump {PPM_PATH}\n"
        client.sendall(cmd.encode())
        time.sleep(1)
        client.close()

        print("[7] Terminating QEMU...")
        proc.terminate()
        proc.communicate(timeout=5)
    except Exception as e:
        proc.kill()
        proc.communicate()
        print(f"[FAIL] Capture failed: {e}")
        sys.exit(1)

    if not os.path.exists(PPM_PATH):
        print(f"[FAIL] Screenshot was not generated at {PPM_PATH}!")
        sys.exit(1)

    print("[8] Converting PPM to PNG...")
    try:
        with Image.open(PPM_PATH) as img:
            img.save(PNG_PATH)
        print(f"[SUCCESS] GUI screenshot saved at {PNG_PATH}")
    except Exception as e:
        print(f"[FAIL] Conversion failed: {e}")
        sys.exit(1)

if __name__ == "__main__":
    capture()
