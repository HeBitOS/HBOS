#!/usr/bin/env python3
import subprocess
import time
import os
import sys

WORKSPACE = "/media/data/hbosv2"
ISO_PATH = f"{WORKSPACE}/build/hbos-bios.iso"
VBOX_LOG_PATH = f"{WORKSPACE}/build/vbox_a_serial.log"

def run_command(cmd, shell=False, check=True):
    print(f"[RUN] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    return subprocess.run(cmd, shell=shell, capture_output=True, text=True, check=check)

def build_iso():
    print("[1/3] Building ISO...")
    res = run_command(["make"], check=False)
    if res.returncode != 0:
        print("[FAIL] Compilation failed!")
        print(res.stderr)
        sys.exit(1)
    print("[PASS] ISO built successfully.")

def test_qemu():
    print("\n[2/3] Running QEMU test...")
    qemu_cmd = [
        "qemu-system-x86_64",
        "-m", "512M",
        "-cdrom", ISO_PATH,
        "-boot", "d",
        "-serial", "stdio",
        "-display", "none",
        "-no-reboot"
    ]
    
    # Start QEMU process
    proc = subprocess.Popen(qemu_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    
    try:
        # Wait a bit for boot
        time.sleep(3)
        # Send help and enter
        print("[QEMU] Sending 'help' command to serial port...")
        proc.stdin.write("help\n")
        proc.stdin.flush()
        
        time.sleep(2)
        # Send echo test
        print("[QEMU] Sending 'echo test_success' to serial port...")
        proc.stdin.write("echo test_success\n")
        proc.stdin.flush()
        
        time.sleep(2)
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=5)
    except Exception as e:
        proc.kill()
        stdout, stderr = proc.communicate()
        print(f"[FAIL] QEMU test crashed: {e}")
        sys.exit(1)
        
    print("[QEMU] Output captured:")
    print("----------------------------------------")
    print(stdout)
    print("----------------------------------------")
    
    if "[KERN] Shell ready" not in stdout:
        print("[FAIL] QEMU did not reach shell ready state!")
        sys.exit(1)
        
    if "test_success" not in stdout:
        print("[FAIL] QEMU failed to process inputs!")
        sys.exit(1)
        
    print("[PASS] QEMU test passed.")

def test_vbox():
    print("\n[3/3] Running VirtualBox 'a' test...")
    
    # Ensure VM "a" is powered off
    print("[VBOX] Powering off VM 'a' if running...")
    subprocess.run(["VBoxManage", "controlvm", "a", "poweroff"], capture_output=True)
    
    # Configure serial port
    print("[VBOX] Configuring UART1 to output to file...")
    run_command(["VBoxManage", "modifyvm", "a", "--uart1", "0x3F8", "4"])
    run_command(["VBoxManage", "modifyvm", "a", "--uartmode1", "file", VBOX_LOG_PATH])
    
    # Remove old log
    if os.path.exists(VBOX_LOG_PATH):
        os.remove(VBOX_LOG_PATH)
        
    # Start VM "a" headless
    print("[VBOX] Starting VM 'a' in headless mode...")
    run_command(["VBoxManage", "startvm", "a", "--type", "headless"])
    
    # Wait for GRUB menu to load
    print("[VBOX] Waiting 4 seconds for GRUB menu...")
    time.sleep(4)
    
    # Send Down Arrow + Enter (Select text fallback and boot immediately)
    print("[VBOX] Sending Down Arrow + Enter to GRUB to select text fallback...")
    run_command(["VBoxManage", "controlvm", "a", "keyboardputscancode", "e0", "50", "e0", "d0", "1c", "9c"])
    
    # Wait for HBOS shell to load
    print("[VBOX] Waiting 3 seconds for HBOS shell...")
    time.sleep(3)
    
    # Send keys: "help\n"
    # Scancodes (Set 1): h=23 a3, e=12 92, l=26 a6, p=19 99, Enter=1c 9c
    print("[VBOX] Sending 'help' keystrokes via VBoxManage...")
    run_command(["VBoxManage", "controlvm", "a", "keyboardputscancode", 
                 "23", "a3", "12", "92", "26", "a6", "19", "99", "1c", "9c"])
    
    time.sleep(2)
    
    # Send keys: "echo vboxsuccess\n"
    # Scancodes: e=12 92, c=2e ae, h=23 a3, o=18 98, Space=39 b9, v=2f af, b=30 b0, o=18 98, x=2d ad, s=1f 9f, u=16 96, c=2e ae, c=2e ae, e=12 92, s=1f 9f, s=1f 9f, Enter=1c 9c
    print("[VBOX] Sending 'echo vboxsuccess' keystrokes via VBoxManage...")
    run_command(["VBoxManage", "controlvm", "a", "keyboardputscancode",
                 "12", "92", "2e", "ae", "23", "a3", "18", "98", "39", "b9", 
                 "2f", "af", "30", "b0", "18", "98", "2d", "ad", "1f", "9f",
                 "16", "96", "2e", "ae", "2e", "ae", "12", "92", "1f", "9f", "1f", "9f", "1c", "9c"])
                 
    time.sleep(3)
    
    # Poweroff
    print("[VBOX] Powering off VM 'a'...")
    run_command(["VBoxManage", "controlvm", "a", "poweroff"])
    
    # Verify log
    if not os.path.exists(VBOX_LOG_PATH):
        print(f"[FAIL] VBOX log file not found at {VBOX_LOG_PATH}")
        sys.exit(1)
        
    with open(VBOX_LOG_PATH, "r", errors="ignore") as f:
        log_content = f.read()
        
    print("[VBOX] Output captured:")
    print("----------------------------------------")
    print(log_content)
    print("----------------------------------------")
    
    if "[KERN] Shell ready" not in log_content:
        print("[FAIL] VirtualBox VM 'a' did not reach shell ready state!")
        sys.exit(1)
        
    if "vboxsuccess" not in log_content:
        print("[FAIL] VirtualBox VM 'a' failed to process keyboard input!")
        sys.exit(1)
        
    print("[PASS] VirtualBox 'a' test passed.")

if __name__ == "__main__":
    build_iso()
    test_qemu()
    test_vbox()
    print("\n[ALL PASS] Every test succeeded without errors!")
