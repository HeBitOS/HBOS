#!/usr/bin/env python3
import subprocess
import time
import os
import sys

WORKSPACE = "/media/data/hbosv2"
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
        
        # Check drivers status
        print("[TEST] Sending 'drivers'...")
        proc.stdin.write("drivers\n")
        proc.stdin.flush()
        time.sleep(2)
        
        # Check initial diskmgr status
        print("[TEST] Sending 'diskmgr'...")
        proc.stdin.write("diskmgr\n")
        proc.stdin.flush()
        time.sleep(2)
        
        # Install auto to partition and format USB drive
        print("[TEST] Sending 'install auto'...")
        proc.stdin.write("install auto\n")
        proc.stdin.flush()
        time.sleep(3)
        
        # Write file to persistent storage
        print("[TEST] Writing file to persistent USB storage...")
        proc.stdin.write("writefile /test_usb.txt Hello_from_USB_Storage_integrated_in_HBOS\n")
        proc.stdin.flush()
        time.sleep(1)
        
        # Read file back
        print("[TEST] Reading file back from persistent USB storage...")
        proc.stdin.write("cat /test_usb.txt\n")
        proc.stdin.flush()
        time.sleep(2)
        
        # Final status check
        print("[TEST] Sending final 'diskmgr'...")
        proc.stdin.write("diskmgr\n")
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
    
    # Temporarily print pass to inspect output
    print("[DIAGNOSTIC DONE]")

if __name__ == "__main__":
    setup_usb_img()
    run_qemu_test()
