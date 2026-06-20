#!/usr/bin/env python3
import subprocess
import time
import os
import sys

WORKSPACE = "/media/data/hbosv2"
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
        
        # Check drivers status
        print("[TEST] Sending 'drivers'...")
        proc.stdin.write("drivers\n")
        proc.stdin.flush()
        time.sleep(2)
        
        # Power off
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
    
    # Check if USB keyboard and mouse are active/ready
    if "USB HID devices:  2" in stdout and "ready" in stdout:
        print("[SUCCESS] USB HID devices detected and ready!")
    else:
        print("[FAIL] USB HID devices NOT ready!")
        sys.exit(1)

if __name__ == "__main__":
    run_qemu_test()
