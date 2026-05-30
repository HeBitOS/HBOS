# HBOS 0.1 beta2

HBOS 0.1 beta2 is a developer-focused release that turns the project from a console-first kernel demo into a more testable OS workspace: BIOS and UEFI boot artifacts are built side by side, the filesystem path now has both an in-memory POSIX workflow and an early HBFS disk backend, and the shell has enough file, disk, and app commands for contributors to start testing real workflows instead of only boot output.

## Highlights

- BIOS and UEFI ISO outputs are now explicit: `build/hbos-bios.iso` and `build/hbos-uefi.iso`.
- UEFI boot support has been improved for virtual machines, including GPT/ESP installed-disk image generation.
- HBFS now has a disk-backed path and can be discovered from MBR/GPT partitions.
- The POSIX-style fd layer is usable for simple programs: `open`, `read`, `write`, `close`, `lseek`, `fstat`, `stat`, `unlink`, `isatty`, `getpid`, `sbrk`, `malloc`, and `free`.
- Shell file workflows are available through commands such as `ls`, `cat`, `touch`, `rm`, `writefile`, and `appendfile`.
- User-app scaffolding is available under `src/apps`, with syscall demos such as `hello` and `uwc`.
- Startup selftests now validate the POSIX/ramfs path and print `[SELFTEST] POSIX/ramfs: PASS` on success.
- CJK rendering is more stable during terminal scrolling, including fixes for misplaced Chinese glyphs and cursor remnants.
- `poweroff` and `shutdown` now try ACPI poweroff first, with virtual-machine fallback ports retained.

## Developer Notes

This release is intended for kernel, shell, filesystem, and userspace contributors. The best next work items are syscall ABI cleanup, richer user applications, ATA/AHCI persistence testing, and turning the current installer and disk manager into a smoother end-user workflow.

## Build

```bash
make clean all
```

Expected primary artifacts:

- `build/hbos-bios.iso`
- `build/hbos-uefi.iso`

## Known Limits

- HBFS and the installer are still early and should be treated as development features.
- POSIX compatibility is intentionally partial; only the currently implemented fd and memory APIs should be considered available.
- Disk and poweroff behavior can still vary by emulator or virtual machine firmware.
