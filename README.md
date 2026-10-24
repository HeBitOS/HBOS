<p align="center">
  <img src="docs/logo/new-logo.png" alt="HBOS Logo" width="200">
</p>

# HBOS

**English** · [简体中文](README.zh-CN.md)

> He Bit OS — a small experimental operating system for x86_64

Version: `v0.1-beta5-pre18` · BIOS / UEFI · early development
GUI: `HIVE 0.1-beta5-gui.4` (Toolkit API 1.4 / TUI Kit 1.0)

![HBOS desktop](docs/screenshots/hive-desktop.jpg)

HBOS is a from-scratch OS: bootloader glue, kernel, drivers, filesystems, network
stack and desktop all live in this tree. It runs on bare metal or in a VM and does
not depend on Linux as a runtime host — although it ships a Linux syscall
compatibility layer so that unmodified Debian binaries can be loaded and run.

> [!WARNING]
> HBOS is early-stage: do not store important data on it and do not use it in
> production. Secure Boot must be disabled for UEFI boots.

## Build and run

```bash
export TMPDIR=$PWD/build/tmp        # the system /tmp is tiny; always override it
make bios-iso -j16                  # -> build/hbos-bios.iso
python3 scripts/qemu_session.py build/hbos-bios.iso \
    --mem 4096M --log build/tmp/run.log -- "help"
```

* There is no KVM in most containers, so QEMU runs under TCG: **one VM at a time**,
  each boot costs 1–2 minutes.
* `scripts/qemu_session.py --disk <img>` boots with a persistent AHCI disk.

## Installing Debian packages (`.deb`)

HBOS does not run `dpkg`. A `.deb` is converted into an HPT package, published in
an HPT repository, and installed inside HBOS with `hpt`.

### 1. Convert and pack (host side)

```bash
python3 HPT/tools/pkg2hax.py pkg.deb -o out/        # deb -> payload + manifest
python3 scripts/mk_deb_repo.py --out build/repo build/debs/*.deb   # repo + deps
```

### 2. Deliver the repository to HBOS

* **file repository (recommended, no 2 MiB network limit)** — bake `build/repo`
  into a disk image (`scripts/mk_bigpkg_ext2.sh`, `scripts/mk_test_img.sh`) and
  point HPT at `/packages`;
* **HTTP repository** — `hpt-server --root build/repo --port 8080`, then use
  `10.0.2.2` from inside QEMU.

### 3. Install inside HBOS

```text
run hpt init
run hpt source file /packages
run hpt update
run hpt search <name>
run hpt install <name>
run hpt list --installed
run hpt remove <name>
```

There is also a graphical installer in HIVE (search / details / progress bar).

### One-shot shortcut: `make install-app`

```bash
make install-app APP_DEB=./some-package.deb
```

This converts the `.deb`, injects the whole tree into the disk image under the
manifest's `install_prefix` (`/opt/<pkg>` by default) and boots QEMU with the
result visible. Example with `cowsay-off`:

```
[install-app] cowsay-off 3.03+dfsg2-8 -> /opt/cowsay-off  (4 files, 2 links)
hbos:/# ls /opt/cowsay-off
usr/  <DIR>
```

## Host-side access to the guest filesystem

The ext2 filesystem starts at a 1 MiB offset (MBR partition table), so the host can
read and write it directly with `debugfs` — no VM required:

```bash
make img-ls  P=/opt/cowsay-off/usr/share/cowsay/cows
make img-get P=/opt/.../bong.cow O=/tmp/bong.cow
make img-put F=./a.txt D=/opt/cowsay-off/usr/share/cowsay/cows/a.txt
```

Use `APP_IMG=build/some.img` to target a specific disk image.

## Attaching files to the ISO

```bash
make bios-iso ISO_PAYLOAD=./payload-dir     # copied to payload/ inside the ISO
```

## Compatibility status

| Area | State |
|---|---|
| ELF loading, `ld.so`, shared-library closure | works (Chromium + 41 dependencies) |
| File I/O: open/read/write/pread/pwrite/ftruncate/fallocate/rename/unlink/symlink | works |
| sqlite (locking, WAL, page-aligned truncation) | works |
| Threads (`clone`/`clone3`, TLS switch), futex, epoll/poll/eventfd | works |
| Unix sockets, TCP/IP stack | protocol layer works, link configuration still rough |
| `statx`, `rseq` | `ENOSYS` |
| `unlink()` of an open file | not yet (deferred inode free missing) |
| multi-process shared memory (`MAP_SHARED`) | private snapshot only |
| **tty / termios / pty** | **missing** — every interactive program fails here |

Concrete example — `sl` installs fine but cannot run:

```
hbos:/# run /opt/sl/usr/games/sl
Error opening te…                       ← ncurses: "Error opening terminal: …"
kernel: ioctl(1, TCGETS) = -EBADF
```

Anything curses/readline based (`sl`, `top`, `vim`, `bash`, `zsh`) needs termios
first; shells additionally need pty and job control.

## Chromium notes

* The Debian `chromium` package is **not built with the headless shell**, so
  `--headless --dump-dom` only produces "no window → clean exit".
* `--ozone-platform=headless` (without `--headless`) keeps the browser alive, which
  is the useful starting point for CDP / Wayland work.
* Symbolize any guest RIP with `python3 scripts/symbolize_rip.py 0x110xxxxxxx`
  (Chromium PIE base `0x1100000000`).

## Troubleshooting

* `ls -l` is not supported by the HBOS shell — use plain `ls`.
* Read Chromium's full logs without flooding the serial port:
  `--enable-logging --v=1 --log-file=/tmp/vlog.txt`, then on the host
  `debugfs -R "cat /tmp/vlog.txt" "img?offset=1048576"`.
* Only one QEMU may run at a time; a stale one locks the disk image.

## Documentation

* `docs/jindu.md` — day-by-day engineering log with evidence for every change
* `docs/ROADMAP-C1-C3-RESULT.md` — Linux compatibility milestones
* `CHROMIUM_COMPAT_ROADMAP.md` — Chromium bring-up plan
* `HPT/docs/HPT.md` — package manager reference

## Translations

Default language is English. Add a new language by creating `README.<lang>.md`
(e.g. `README.ja.md`) and adding it to the language line at the top of every
README variant.
