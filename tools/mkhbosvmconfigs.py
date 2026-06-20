#!/usr/bin/env python3
import argparse
import os
import uuid


def write_text(path: str, data: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(data)


def vmx_template(name: str, disk: str, firmware: str) -> str:
    lines = [
        '.encoding = "UTF-8"',
        'config.version = "8"',
        'virtualHW.version = "20"',
        f'displayName = "{name}"',
        'guestOS = "other-64"',
        'memsize = "512"',
        'numvcpus = "1"',
        'bios.bootOrder = "hdd"',
        'firmware = "efi"' if firmware == "efi" else 'firmware = "bios"',
        'efi.secureBoot.enabled = "FALSE"',
        'ide0.present = "TRUE"',
        'ide0:0.present = "TRUE"',
        f'ide0:0.fileName = "{disk}"',
        'ide0:0.deviceType = "disk"',
        'ethernet0.present = "TRUE"',
        'ethernet0.connectionType = "nat"',
        'ethernet0.virtualDev = "e1000"',
        'ethernet0.addressType = "generated"',
        'usb.present = "FALSE"',
        'usb_xhci.present = "FALSE"',
        'sound.present = "FALSE"',
        'serial0.present = "FALSE"',
        'parallel0.present = "FALSE"',
        'svga.autodetect = "TRUE"',
        'mks.enable3d = "FALSE"',
        'mouse.vusb.enable = "FALSE"',
    ]
    return "\n".join(lines) + "\n"


def vbox_template(name: str, disk: str, firmware: str) -> str:
    machine_uuid = uuid.uuid4()
    disk_uuid = uuid.uuid4()
    firmware_xml = '<Firmware type="EFI"/>' if firmware == "efi" else ""
    return f'''<?xml version="1.0"?>
<VirtualBox xmlns="http://www.virtualbox.org/" version="1.19-linux">
  <Machine uuid="{{{machine_uuid}}}" name="{name}" OSType="Other_64" snapshotFolder="Snapshots">
    <MediaRegistry>
      <HardDisks>
        <HardDisk uuid="{{{disk_uuid}}}" location="{disk}" format="VDI" type="Normal"/>
      </HardDisks>
    </MediaRegistry>
    <Hardware>
      <CPU count="1">
        <PAE enabled="true"/>
      </CPU>
      <Memory RAMSize="512"/>
      <HID Pointing="PS2Mouse" Keyboard="PS2Keyboard"/>
      {firmware_xml}
      <Boot>
        <Order position="1" device="HardDisk"/>
        <Order position="2" device="DVD"/>
        <Order position="3" device="None"/>
        <Order position="4" device="None"/>
      </Boot>
      <Display controller="VBoxVGA" VRAMSize="16"/>
      <BIOS>
        <IOAPIC enabled="true"/>
      </BIOS>
      <Network>
        <Adapter slot="0" enabled="true" MACAddress="080027{uuid.uuid4().hex[:6].upper()}" cable="true" type="82540EM">
          <NAT/>
        </Adapter>
      </Network>
      <AudioAdapter enabled="false"/>
      <USB/>
      <StorageControllers>
        <StorageController name="IDE" type="PIIX4" PortCount="2" useHostIOCache="true" Bootable="true">
          <AttachedDevice type="HardDisk" hotpluggable="false" port="0" device="0">
            <Image uuid="{{{disk_uuid}}}"/>
          </AttachedDevice>
        </StorageController>
      </StorageControllers>
    </Hardware>
  </Machine>
</VirtualBox>
'''


def vboxmanage_script(name: str, disk: str, firmware: str, shell: str) -> str:
    efi = " on" if firmware == "efi" else " off"
    if shell == "cmd":
        return f'''@echo off
setlocal
set VM={name}
VBoxManage unregistervm "%VM%" --delete 2>nul
VBoxManage createvm --name "%VM%" --ostype Other_64 --register
VBoxManage modifyvm "%VM%" --memory 512 --cpus 1 --ioapic on --firmware{efi} --graphicscontroller vboxvga --vram 16 --mouse ps2 --keyboard ps2 --audio none --usb off --boot1 disk --boot2 dvd --nictype1 82540EM --nic1 nat --cableconnected1 on
VBoxManage storagectl "%VM%" --name "IDE" --add ide --controller PIIX4 --hostiocache on --bootable on
VBoxManage storageattach "%VM%" --storagectl "IDE" --port 0 --device 0 --type hdd --medium "%~dp0{disk}"
VBoxManage startvm "%VM%"
'''
    return f'''#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
VM="{name}"
VBoxManage unregistervm "$VM" --delete >/dev/null 2>&1 || true
VBoxManage createvm --name "$VM" --ostype Other_64 --register
VBoxManage modifyvm "$VM" --memory 512 --cpus 1 --ioapic on --firmware{efi} --graphicscontroller vboxvga --vram 16 --mouse ps2 --keyboard ps2 --audio none --usb off --boot1 disk --boot2 dvd --nictype1 82540EM --nic1 nat --cableconnected1 on
VBoxManage storagectl "$VM" --name "IDE" --add ide --controller PIIX4 --hostiocache on --bootable on
VBoxManage storageattach "$VM" --storagectl "IDE" --port 0 --device 0 --type hdd --medium "{disk}"
VBoxManage startvm "$VM"
'''


def main() -> int:
    parser = argparse.ArgumentParser(description="Create HBOS VMware and VirtualBox launch configs")
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()

    build = args.build_dir
    configs = [
        ("HBOS VMware BIOS", "hbos_vmware_bios.vmdk", "hbos_vmware_bios.vmx", "bios"),
        ("HBOS VMware UEFI", "hbos_vmware_uefi.vmdk", "hbos_vmware_uefi.vmx", "efi"),
    ]
    for name, disk, out, firmware in configs:
        write_text(os.path.join(build, out), vmx_template(name, disk, firmware))

    vbox_configs = [
        ("HBOS VirtualBox BIOS", "hbos_virtualbox_bios.vdi", "hbos_virtualbox_bios.vbox", "bios"),
        ("HBOS VirtualBox UEFI", "hbos_virtualbox_uefi.vdi", "hbos_virtualbox_uefi.vbox", "efi"),
    ]
    for name, disk, out, firmware in vbox_configs:
        write_text(os.path.join(build, out), vbox_template(name, disk, firmware))
        stem = out.rsplit(".", 1)[0]
        write_text(os.path.join(build, f"{stem}.sh"), vboxmanage_script(name, disk, firmware, "sh"))
        write_text(os.path.join(build, f"{stem}.cmd"), vboxmanage_script(name, disk, firmware, "cmd"))
        os.chmod(os.path.join(build, f"{stem}.sh"), 0o755)

    print("HBOS VM configs:")
    for out in [
        "hbos_vmware_bios.vmx",
        "hbos_vmware_uefi.vmx",
        "hbos_virtualbox_bios.vbox",
        "hbos_virtualbox_uefi.vbox",
        "hbos_virtualbox_bios.sh",
        "hbos_virtualbox_uefi.sh",
        "hbos_virtualbox_bios.cmd",
        "hbos_virtualbox_uefi.cmd",
    ]:
        print(f"  {os.path.join(build, out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
