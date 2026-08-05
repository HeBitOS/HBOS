#!/usr/bin/env python3
import subprocess
import time
import os
import sys

WORKSPACE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO_PATH = f"{WORKSPACE}/build/hbos-bios.iso"
USB_IMG_PATH = f"{WORKSPACE}/build/usb_disk.img"

def run_command(cmd, check=True):
    print(f"[RUN] {' '.join(cmd)}")
    return subprocess.run(cmd, capture_output=True, text=True, check=check)

def setup_usb_img():
    print("[1/4] Creating blank USB disk image (64MB)...")
    if os.path.exists(USB_IMG_PATH):
        os.remove(USB_IMG_PATH)
    run_command(["qemu-img", "create", "-f", "raw", USB_IMG_PATH, "64M"])

def run_qemu_test():
    print("[2/4] Booting QEMU with USB xHCI storage attached...")
    qemu_cmd = [
        "qemu-system-x86_64",
        "-m", "512M",
        "-cdrom", ISO_PATH,
        "-boot", "d",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-storage,bus=xhci.0,drive=usb0",
        "-drive", f"file={USB_IMG_PATH},if=none,id=usb0,format=raw",
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot"
    ]
    
    # Launch QEMU process
    proc = subprocess.Popen(qemu_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    
    try:
        # Wait for boot
        print("[TEST] Waiting 6 seconds for shell ready...")
        time.sleep(6)
        
        # Shell input is keyboard-driven. This test verifies enumeration,
        # endpoint configuration and READ CAPACITY from serial boot markers.
        time.sleep(2)
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=5)
    except Exception as e:
        proc.kill()
        stdout, stderr = proc.communicate()
        print(f"[FAIL] Integration test crashed: {e}")
        sys.exit(1)
        
    print("\n[QEMU OUTPUT]")
    print("----------------------------------------")
    print(stdout)
    print("----------------------------------------")
    
    required = [
        "[XHCI] Device addressed successfully!",
        "[MSC] storage ready:",
        "blocks=131072 block_size=512",
        "[KERN] Shell ready",
    ]
    missing = [marker for marker in required if marker not in stdout]
    if missing:
        print("[FAIL] Missing xHCI storage markers: " + ", ".join(missing))
        sys.exit(1)
    print("[SUCCESS] xHCI USB storage capacity detected!")

if __name__ == "__main__":
    setup_usb_img()
    run_qemu_test()
