#!/usr/bin/env python3
import subprocess
import time
import os
import sys

WORKSPACE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO_PATH = f"{WORKSPACE}/build/hbos-bios.iso"

def run_qemu_test():
    print("[1/2] Booting QEMU with USB xHCI Keyboard and Mouse...")
    qemu_cmd = [
        "qemu-system-x86_64",
        "-m", "512M",
        "-cdrom", ISO_PATH,
        "-boot", "d",
        "-device", "qemu-xhci,id=xhci",
        "-device", "usb-kbd,bus=xhci.0",
        "-device", "usb-mouse,bus=xhci.0",
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
        
        # Shell input comes from the emulated keyboard, not the serial port.
        # Serial is intentionally output-only in this test.
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
        "[HID] registered as keyboard",
        "[HID] registered as mouse",
        "[KERN] Shell ready",
    ]
    missing = [marker for marker in required if marker not in stdout]
    if missing:
        print("[FAIL] Missing xHCI/HID boot markers: " + ", ".join(missing))
        sys.exit(1)
    print("[SUCCESS] xHCI keyboard and mouse initialized!")

if __name__ == "__main__":
    run_qemu_test()
