#!/usr/bin/env python3
"""HBOS KDE/Chromium boot verifier — single VM lifecycle, timestamped evidence.

Invoked by scripts/kde_verify.sh (which holds /tmp/hbos-vm.lock around the
whole run). Does not take the lock itself.

Produces, under <out>/<run-id>/:
  serial.log        raw guest serial
  serial.stamped    same text, each line prefixed with host elapsed seconds
  shots/NNN_<tag>.png  timeline screenshots
  timeline.json     every marker with host-seconds AND in-guest kernel t=
  procs.json        EXEC/EXIT ledger -> which guest processes are still alive
  report.md         human summary + exit-code justification

Exit codes (stable contract for other agents):
   0  all requested criteria met
  10  desktop (plasmashell) up, but konsole/chromium missing
  20  kwin ready, plasmashell never exec'd
  30  boot/ISO failure (no autostart hook, VM died, or timeout)
  40  QEMU refused to start (KVM/image/args)
  75  could not take the VM lock within --lock-timeout
"""
import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------- utilities

HOST_T0 = time.time()


def log(msg):
    print(f"[kde_verify +{time.time() - HOST_T0:7.2f}s] {msg}", flush=True)


def eprint(msg):
    print(msg, file=sys.stderr, flush=True)


class Monitor:
    """QEMU human monitor over a unix socket."""

    def __init__(self, path, wait=120.0):
        deadline = time.time() + wait
        while True:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                time.sleep(0.3)
                s.setblocking(False)
                try:
                    s.recv(65536)
                except Exception:
                    pass
                s.setblocking(True)
                self.s = s
                return
            except OSError:
                if time.time() > deadline:
                    raise SystemExit("monitor connect failed: " + path)
                time.sleep(0.1)

    def cmd(self, c, settle=0.12):
        self.s.sendall((c + "\n").encode())
        time.sleep(settle)
        try:
            self.s.setblocking(False)
            self.s.recv(65536)
            self.s.setblocking(True)
        except Exception:
            self.s.setblocking(True)


# ---------------------------------------------------------------- screendump

def screendump(mon, ppm_path, timeout=8.0):
    """Issue screendump and wait for a complete, size-stable PPM."""
    if os.path.exists(ppm_path):
        os.unlink(ppm_path)
    mon.cmd(f"screendump {ppm_path}", settle=0.05)
    deadline = time.time() + timeout
    last = -1
    while time.time() < deadline:
        if os.path.exists(ppm_path):
            sz = os.path.getsize(ppm_path)
            if sz == last and sz > 1024:
                return sz
            last = sz
        time.sleep(0.1)
    raise RuntimeError("screendump produced no stable file: " + ppm_path)


def ppm_to_png(ppm_path, png_path):
    from PIL import Image

    with Image.open(ppm_path) as im:
        im.convert("RGB").save(png_path)
    return png_path


def content_bbox(px, w, h, threshold=18, step=3):
    """Bounding box of all non-black pixels.

    Distinguishes "one window on a black screen" from "a full desktop":
    a real wallpaper drives this box to nearly the whole frame, while a single
    centred window leaves large black margins.
    """
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(0, h, step):
        for x in range(0, w, step):
            if sum(px[x, y]) > threshold:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return {"empty": True}
    return {
        "empty": False,
        "x": [minx, maxx], "y": [miny, maxy],
        "size": [maxx - minx, maxy - miny],
        "coverage_pct": round(100.0 * (maxx - minx) * (maxy - miny) / (w * h), 2),
    }


def analyze_png(png_path, panel_band=70):
    """Return objective pixel statistics used for the pass/fail verdict.

    step=13 keeps a 1920x1080 frame at ~12k samples: enough to distinguish a
    flat black screen from a rendered desktop, cheap enough to run per shot.
    """
    from PIL import Image

    with Image.open(png_path) as im:
        im = im.convert("RGB")
        w, h = im.size
        px = im.load()
        step = 13
        total = dark = 0
        colors = {}
        for y in range(0, h, step):
            for x in range(0, w, step):
                p = px[x, y]
                total += 1
                colors[p] = colors.get(p, 0) + 1
                if sum(p) < 30:
                    dark += 1

        # bottom panel band (KDE default panel is bottom-anchored)
        band_top = max(0, h - panel_band)
        b_total = b_dark = 0
        b_colors = {}
        for y in range(band_top, h, 3):
            for x in range(0, w, step):
                p = px[x, y]
                b_total += 1
                b_colors[p] = b_colors.get(p, 0) + 1
                if sum(p) < 30:
                    b_dark += 1

        # Wallpaper test. A centred window covers the middle of the screen, so
        # sampling only the centre reports a false "black wallpaper". Probe the
        # four corners + edges instead: no sane window layout covers all of
        # them, and a real wallpaper always shows there.
        probes = {
            "top_left": (0, 0, w // 5, h // 5),
            "top_right": (w - w // 5, 0, w, h // 5),
            "bottom_left": (0, h - h // 5, w // 5, h),
            "bottom_right": (w - w // 5, h - h // 5, w, h),
            "left_edge": (0, h // 3, w // 12, 2 * h // 3),
            "right_edge": (w - w // 12, h // 3, w, 2 * h // 3),
        }
        def pct(a, b):
            return round(100.0 * a / b, 2) if b else 0.0

        probe_stats = {}
        c_total = c_dark = 0
        for name, (x0, y0, x1, y1) in probes.items():
            t = d = 0
            for y in range(y0, y1, step):
                for x in range(x0, x1, step):
                    t += 1
                    if sum(px[x, y]) < 30:
                        d += 1
            probe_stats[name] = pct(d, t)
            c_total += t
            c_dark += d

    top = sorted(colors.items(), key=lambda kv: -kv[1])[:6]
    return {
        "size": [w, h],
        "distinct_colors": len(colors),
        "dark_pct": pct(dark, total),
        "panel_band_px": panel_band,
        "panel_nonblack_pct": pct(b_total - b_dark, b_total),
        "panel_dominant_color": (
            list(max(b_colors.items(), key=lambda kv: kv[1])[0]) if b_colors else None),
        "wallpaper_dark_pct": pct(c_dark, c_total),
        "wallpaper_dark_by_probe": probe_stats,
        "content_bbox": content_bbox(px, w, h),
        "wallpaper_present": (pct(c_dark, c_total) < 50.0),
        "top_colors": [[list(c), round(100.0 * n / total, 2)] for c, n in top],
        # A panel exists only if its band carries real, non-black pixels.
        "panel_likely_present": pct(b_total - b_dark, b_total) >= 5.0,
        "screen_effectively_black": pct(dark, total) >= 99.0,
    }


def diff_pngs(a, b):
    """Mean absolute difference (0..255) + changed-pixel percentage."""
    from PIL import Image

    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return {"comparable": False, "reason": "size mismatch"}
    pa, pb = ia.load(), ib.load()
    w, h = ia.size
    step = 9
    n = 0
    acc = 0
    changed = 0
    for y in range(0, h, step):
        for x in range(0, w, step):
            q, r = pa[x, y], pb[x, y]
            d = abs(q[0] - r[0]) + abs(q[1] - r[1]) + abs(q[2] - r[2])
            acc += d
            n += 1
            if d > 24:
                changed += 1
    return {
        "comparable": True,
        "mean_absdiff": round(acc / n / 3.0, 3),
        "changed_pct": round(100.0 * changed / n, 3),
        "responded": changed / n >= 0.002 or acc / n / 3.0 >= 0.5,
    }


# ---------------------------------------------------------------- serial

MARKERS = [
    # (key, regex, note)
    ("bios_banner", r"██   ██|HBOS", "kernel/shell banner on serial"),
    ("autostart_hook", r"检测到 /etc/hbos-autostart", "shell found /etc/hbos-autostart"),
    ("start_script_exec", r"\[EXEC\][^\n]*path=/usr/bin/start_kde_chromium\.sh"),
    ("dbus_session", r"\[EXEC\][^\n]*path=/usr/bin/dbus-daemon"),
    ("wl_fb_open", r"HBOS_WL_FB_OPEN"),
    ("wl_fb_ready", r"HBOS_WL_FB_READY"),
    ("kwin_exec", r"\[EXEC\][^\n]*path=/usr/bin/kwin_wayland"),
    ("kwin_accepting", r"Accepting client connections on sockets"),
    ("kwin_socket", r"wayland-0"),
    ("session_script_exec", r"\[EXEC\][^\n]*path=/usr/bin/kde_session\.sh"),
    ("kded6", r"\[EXEC\][^\n]*path=/usr/bin/kded6"),
    ("kactivitymanagerd", r"\[EXEC\][^\n]*path=.*kactivitymanagerd"),
    ("plasmashell_exec", r"\[EXEC\][^\n]*path=/usr/bin/plasmashell"),
    ("first_frame", r"HBOS_WL_FB_FRAME count=1 "),
    ("konsole_exec", r"\[EXEC\][^\n]*path=/usr/bin/konsole"),
    ("chromium_exec", r"\[EXEC\][^\n]*path=.*/chromium"),
    ("session_exit", r"KDE_SESSION_EXIT="),
]

KERNEL_T = re.compile(r"\bt=(\d+)\b")
EXEC_LINE = re.compile(r"^\[EXEC\] pid=(\d+) t=(\d+) path=(\S+)")
FRAME_LINE = re.compile(r"^HBOS_WL_FB_FRAME count=(\d+)")
EXIT_LINE = re.compile(r"^\[EXIT\] pid=(\d+) code=(-?\d+)")


class SerialTail(threading.Thread):
    """Poll the serial file and stamp every line with host elapsed time."""

    def __init__(self, path, out_path, stop_event):
        super().__init__(daemon=True)
        self.path = path
        self.out_path = open(out_path, "w", buffering=1)
        self.stop_event = stop_event
        self.lines = []          # [(host_elapsed, text)]
        self.lock = threading.Lock()
        self.offset = 0
        self.partial = b""

    def run(self):
        while not self.stop_event.is_set():
            self.drain()
            time.sleep(0.05)
        self.drain()

    def drain(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.offset)
                data = f.read()
                self.offset = f.tell()
        except OSError:
            return
        if not data:
            return
        buf = self.partial + data
        *complete, self.partial = buf.split(b"\n")
        now = time.time() - HOST_T0
        with self.lock:
            for raw in complete:
                text = raw.decode("utf-8", "replace").replace("\r", "")
                self.lines.append((now, text))
                self.out_path.write(f"{now:9.3f} {text}\n")

    def snapshot(self):
        with self.lock:
            return list(self.lines)

    def close(self):
        try:
            self.out_path.close()
        except Exception:
            pass


def build_timeline(lines):
    tl = {}
    for marker in MARKERS:
        if len(marker) == 2:
            key, rx = marker
            note = ""
        else:
            key, rx, note = marker
        cre = re.compile(rx)
        for host_t, text in lines:
            if cre.search(text):
                m = KERNEL_T.search(text)
                tl[key] = {
                    "host_sec": round(host_t, 3),
                    "kernel_t_ms": int(m.group(1)) if m else None,
                    "note": note,
                    "line": text[:200],
                }
                break
    return tl


def build_procs(lines, serial_path):
    """Approximate the guest process table from EXEC/EXIT events."""
    procs = {}
    frames = []
    for _host_t, text in lines:
        m = EXEC_LINE.match(text)
        if m:
            procs[int(m.group(1))] = {
                "path": m.group(3),
                "kernel_t_ms": int(m.group(2)),
                "alive": True,
                "exit_code": None,
            }
            continue
        m = EXIT_LINE.match(text)
        if m:
            pid = int(m.group(1))
            if pid in procs:
                procs[pid]["alive"] = False
                procs[pid]["exit_code"] = int(m.group(2))
            continue
        m = FRAME_LINE.match(text)
        if m:
            frames.append(int(m.group(1)))

    # A killed-by-signal process gets no [EXIT] line; the kernel's own serial
    # tail is authoritative only up to the last line we pulled.
    live = {p: v for p, v in procs.items() if v["alive"]}
    return {
        "frames_committed": max(frames) if frames else 0,
        "exec_count": len(procs),
        "live": {str(k): v for k, v in sorted(live.items())},
        "live_count": len(live),
        "all": {str(k): v for k, v in sorted(procs.items())},
    }


def wanted_live(procs, needle):
    """Live guest processes whose *basename* is needle.

    Substring matching would wrongly count /usr/bin/start_kde_chromium.sh as
    chromium, so compare basenames only.
    """
    return [v for v in procs["live"].values()
            if v["path"].rsplit("/", 1)[-1] == needle]


def diagnose(procs, tl):
    """Name the concrete blocker from evidence, instead of a vague verdict.

    Each finding is (id, severity, summary, evidence_line). The caller prints
    them; they are what the other agents act on.
    """
    findings = []
    live_paths = {v["path"]: p for p, v in procs["live"].items()}

    # konsole missing: find the shell that was supposed to launch it
    if not wanted_live(procs, "konsole"):
        if "/usr/bin/dbus-monitor" in live_paths:
            findings.append((
                "dbus_monitor_stuck", "blocker",
                "dbus-monitor is still alive and kde_session.sh never reached "
                "the konsole line: the pipe reader died but the monitor did not "
                "exit, so bash blocks on wait.",
                f"live pid {live_paths['/usr/bin/dbus-monitor']} = /usr/bin/dbus-monitor"))
        findings.append((
            "konsole_missing", "blocker",
            "no konsole process was ever exec'd.",
            "no [EXEC] line with basename konsole in serial"))

    if not wanted_live(procs, "chromium"):
        findings.append((
            "chromium_missing", "blocker",
            "no chromium process was ever exec'd.",
            "no [EXEC] line with basename chromium in serial"))

    # a child that exited 127 means the binary could not be loaded at all
    for pid, v in procs["all"].items():
        if v.get("exit_code") == 127:
            findings.append((
                f"exec_enoent_{v['path'].rsplit('/', 1)[-1]}", "high",
                f"{v['path']} exited 127 (execve ENOENT) - the file exists in the "
                "image but its loader/libs are missing.",
                f"pid {pid} path {v['path']} exit=127"))

    if procs["frames_committed"] and procs["frames_committed"] <= 5:
        findings.append((
            "scene_frozen", "high",
            f"only {procs['frames_committed']} wayland frames were committed; "
            "the compositor stopped producing new content.",
            "max HBOS_WL_FB_FRAME count"))

    wl = tl.get("wallpaper_dark_pct")
    return findings


def fingerprint(path):
    """Cheap identity of an input artifact: size + mtime_ns.

    Other agents repack the ext2 image while a VM may already be running; a
    changed fingerprint across a run means the evidence describes a mixture of
    two builds and must not be trusted.
    """
    try:
        st = os.stat(path)
        return {"path": path, "size": st.st_size, "mtime_ns": st.st_mtime_ns}
    except OSError as e:
        return {"path": path, "error": str(e)}


# ---------------------------------------------------------------- main flow

def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        prog="kde_verify_run.py",
        description="Run one HBOS VM and collect timestamped KDE evidence.",
    )
    ap.add_argument("--iso", default="build/hbos-bios.iso")
    ap.add_argument("--img", default="build/hbos-kde-chromium.img")
    ap.add_argument("--no-image", action="store_true", help="boot ISO only, no AHCI disk")
    ap.add_argument("--timeout", type=int, default=420, help="total VM wall-clock budget (s)")
    ap.add_argument("--shots", default="20,45,80,120,180,240,300",
                    help="comma list of screendump times, seconds since QEMU launch")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--mem", default="4096M")
    ap.add_argument("--out", default="build/tmp/kde_verify_runs")
    ap.add_argument("--accel", default="kvm", choices=["kvm", "tcg"])
    ap.add_argument("--net", default="none", help="'none' or 'user'")
    ap.add_argument("--key-test-at", type=int, default=0,
                    help="seconds after launch to run the interactive key test (0=after last shot)")
    ap.add_argument("--keys", default="k,o,n,s,o,l,e",
                    help="comma list of QEMU key names typed by the interaction test")
    ap.add_argument("--click-first", action="store_true",
                    help="click into the centre window before sending keys "
                         "so the compositor has a focus target")
    ap.add_argument("--expect", default="full",
                    choices=["full", "desktop", "boot"],
                    help="full=konsole+chromium+keyfdback, desktop=plasmashell, boot=shell only")
    ap.add_argument("--run-id", default=None)
    ap.add_argument("--replay", default=None, metavar="DIR",
                    help="rebuild timeline/report from an existing run dir, no VM")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.replay:
        return replay(args.replay)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    run_id = args.run_id or time.strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(args.out, run_id)
    shots_dir = os.path.join(outdir, "shots")
    os.makedirs(shots_dir, exist_ok=True)
    serial_path = os.path.join(outdir, "serial.log")
    stamped_path = os.path.join(outdir, "serial.stamped")
    mon_path = f"/media/data/hbosv2/build/tmp/kde_verify_tmp/mon-{run_id}.sock"
    os.makedirs(os.path.dirname(mon_path), exist_ok=True)
    for p in (mon_path,):
        if os.path.exists(p):
            os.unlink(p)
    open(serial_path, "w").close()

    iso = os.path.abspath(args.iso)
    img = os.path.abspath(args.img)
    if not os.path.isfile(iso):
        eprint(f"FATAL: ISO not found: {iso}")
        return 40
    if not args.no_image and not os.path.isfile(img):
        eprint(f"FATAL: image not found: {img}")
        return 40

    qemu = ["qemu-system-x86_64", "-m", args.mem, "-smp", str(args.smp),
            "-display", "none", "-no-reboot",
            "-monitor", f"unix:{mon_path},server,nowait",
            "-serial", f"file:{serial_path}",
            "-cdrom", iso, "-boot", "d"]
    if args.accel == "kvm":
        qemu += ["-enable-kvm", "-cpu", "host"]
    else:
        qemu += ["-accel", "tcg", "-cpu", "max"]
    if args.net == "none":
        qemu += ["-net", "none"]
    else:
        qemu += ["-netdev", "user,id=hbosnet",
                 "-device", "virtio-net-pci,netdev=hbosnet,disable-modern=on"]
    if not args.no_image:
        qemu += ["-device", "ich9-ahci,id=ahci",
                 "-drive", f"id=disk0,file={img},format=raw,if=none",
                 "-device", "ide-hd,drive=disk0,bus=ahci.0"]

    artifacts_before = {k: fingerprint(v) for k, v in
                        (("iso", iso), ("img", img)) if not (k == "img" and args.no_image)}

    log("QEMU: " + " ".join(qemu))
    proc = None
    stop = threading.Event()
    tail = None
    exit_code = 30
    shots = []
    key_result = None

    def cleanup():
        nonlocal proc
        names = ["quit", "q"]
        if proc is not None and proc.poll() is None:
            try:
                m = Monitor(mon_path, wait=2)
                for n in names:
                    try:
                        m.cmd(n, settle=0.2)
                    except Exception:
                        pass
                m.s.close()
            except Exception:
                pass
            try:
                proc.terminate()
                proc.wait(timeout=8)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
            log(f"QEMU reaped, returncode={proc.returncode}")
        if os.path.exists(mon_path):
            try:
                os.unlink(mon_path)
            except OSError:
                pass

    try:
        try:
            proc = subprocess.Popen(qemu, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE, text=True)
        except OSError as e:
            eprint(f"FATAL: cannot exec qemu: {e}")
            return 40

        time.sleep(0.8)
        if proc.poll() is not None:
            err = (proc.stderr.read() or "")[-2000:]
            eprint(f"FATAL: QEMU exited immediately rc={proc.returncode}\n{err}")
            return 40

        tail = SerialTail(serial_path, stamped_path, stop)
        tail.start()

        try:
            mon = Monitor(mon_path, wait=30)
        except SystemExit as e:
            eprint(f"FATAL: {e}")
            return 40

        shot_times = sorted(float(x) for x in args.shots.split(",") if x.strip())
        key_at = args.key_test_at or int(max(shot_times) + 10 if shot_times else 60)
        key_at = min(key_at, args.timeout - 20)

        plan = [("shot", t) for t in shot_times] + [("keys", float(key_at))]
        plan.sort(key=lambda kv: kv[1])

        for kind, t in plan:
            while time.time() - HOST_T0 < t:
                if proc.poll() is not None:
                    log(f"QEMU exited early rc={proc.returncode} at "
                        f"{time.time() - HOST_T0:.1f}s")
                    break
                time.sleep(0.2)
            if proc.poll() is not None:
                break

            if kind == "shot":
                tag = f"shot_{int(t):03d}s"
                ppm = os.path.join(shots_dir, f"{int(t):03d}.ppm")
                png = os.path.join(shots_dir, f"{int(t):03d}_{tag}.png")
                try:
                    sz = screendump(mon, ppm)
                    ppm_to_png(ppm, png)
                    os.unlink(ppm)
                    info = analyze_png(png)
                    info.update({"at_sec": t, "png": png,
                                 "host_sec": round(time.time() - HOST_T0, 3)})
                    shots.append(info)
                    log(f"shot t={t:>5.0f}s {sz}B -> {os.path.basename(png)} "
                        f"dark={info['dark_pct']}% colors={info['distinct_colors']} "
                        f"panel_nonblack={info['panel_nonblack_pct']}% "
                        f"wallpaper_dark={info['wallpaper_dark_pct']}%")
                except Exception as e:
                    log(f"shot t={t}s FAILED: {e}")
                continue

            # interactive key test: prove the guest reacts to injected input
            before = os.path.join(shots_dir, f"keys_{int(t):03d}s_before.png")
            after = os.path.join(shots_dir, f"keys_{int(t):03d}s_after.png")
            ppm = os.path.join(shots_dir, "keys.ppm")
            try:
                screendump(mon, ppm)
                ppm_to_png(ppm, before)
                keys = [k.strip() for k in args.keys.split(",") if k.strip()]
                # Chromium's window sits centred; clicking inside it gives a
                # wl_keyboard focus target, without which sendkey is a no-op.
                if args.click_first:
                    for _ in range(4):
                        mon.cmd("mouse_move 60 40", settle=0.05)
                    mon.cmd("mouse_button 1", settle=0.05)
                    mon.cmd("mouse_button 0", settle=0.4)
                    time.sleep(0.5)
                    screendump(mon, ppm)
                    ppm_to_png(ppm, before)
                for k in keys:
                    mon.cmd(f"sendkey {k}", settle=0.06)
                    time.sleep(0.05)
                time.sleep(1.0)
                screendump(mon, ppm)
                ppm_to_png(ppm, after)
                os.unlink(ppm)
                d = diff_pngs(before, after)
                key_result = {"at_sec": t, "keys": keys,
                              "before": before, "after": after, **d}
                log(f"key-test at t={t:.0f}s keys={','.join(keys)} "
                    f"changed={d.get('changed_pct')}% "
                    f"meandiff={d.get('mean_absdiff')} responded={d.get('responded')}")
            except Exception as e:
                log(f"key-test FAILED: {e}")

        exit_code = 30
    finally:
        stop.set()
        if tail is not None:
            time.sleep(0.15)
            tail.close()
        cleanup()

    # ------------------------------------------------------------ reporting
    lines = tail.snapshot() if tail is not None else []
    return finalize(outdir, run_id, dict(iso=iso,
                                        img=None if args.no_image else img,
                                        smp=args.smp, mem=args.mem,
                                        accel=args.accel, expect=args.expect,
                                        no_image=args.no_image,
                                        key_requested=args.key_test_at != 0),
                    lines, shots, key_result, artifacts_before)


def read_stamped(path):
    """Reload a serial.stamped file into [(host_sec, text)]."""
    lines = []
    with open(path, "r", errors="replace") as f:
        for raw in f:
            raw = raw.lstrip()
            if not raw:
                continue
            parts = raw.split(" ", 1)
            try:
                t = float(parts[0])
            except (ValueError, IndexError):
                continue
            lines.append((t, parts[1] if len(parts) > 1 else ""))
    return lines


def finalize(outdir, run_id, meta, lines, shots, key_result, artifacts_before=None):
    """Compute verdict from evidence and write timeline.json/procs.json/report.md."""
    tl = build_timeline(lines)
    procs = build_procs(lines, os.path.join(outdir, "serial.log"))

    def has(k):
        return k in tl

    plasma_live = wanted_live(procs, "plasmashell")
    konsole_live = wanted_live(procs, "konsole")
    chromium_live = wanted_live(procs, "chromium")

    expect = meta["expect"]
    if expect == "boot":
        exit_code = 0 if (has("autostart_hook") or has("bios_banner")) else 30
    elif has("plasmashell_exec"):
        if expect == "desktop":
            exit_code = 0 if (plasma_live or has("first_frame")) else 20
        elif has("konsole_exec"):
            exit_code = 0 if (has("chromium_exec") or meta.get("no_image")) else 10
        else:
            exit_code = 10
    elif has("kwin_accepting") or has("kwin_exec"):
        exit_code = 20
    else:
        exit_code = 30

    if meta.get("key_requested") and key_result and not key_result.get("responded"):
        if exit_code == 0:
            exit_code = 10

    artifacts_after = {}
    drift = []
    if artifacts_before:
        for k, before in artifacts_before.items():
            after = fingerprint(before["path"])
            artifacts_after[k] = after
            if before.get("mtime_ns") != after.get("mtime_ns") or \
               before.get("size") != after.get("size"):
                drift.append(k)

    timeline = {
        "run_id": run_id,
        "artifacts_before": artifacts_before or {},
        "artifacts_after": artifacts_after,
        "input_drift": drift,
        "iso": meta["iso"],
        "img": meta["img"],
        "smp": meta["smp"],
        "mem": meta["mem"],
        "accel": meta["accel"],
        "expect": expect,
        "exit_code": exit_code,
        "markers": tl,
        "processes": {
            "plasmashell_live": len(plasma_live),
            "konsole_live": len(konsole_live),
            "chromium_live": len(chromium_live),
            "frames_committed": procs["frames_committed"],
            "exec_count": procs["exec_count"],
            "live_count": procs["live_count"],
        },
        "shots": shots,
        "key_test": key_result,
        "findings": diagnose(procs, tl),
    }
    with open(os.path.join(outdir, "timeline.json"), "w") as f:
        json.dump(timeline, f, indent=2, ensure_ascii=False)
    with open(os.path.join(outdir, "procs.json"), "w") as f:
        json.dump(procs, f, indent=2, ensure_ascii=False)

    if timeline.get("input_drift"):
        if timeline["exit_code"] == 0:
            timeline["exit_code"] = 10

    write_report(outdir, timeline, procs)
    log(f"exit_code={exit_code} report={outdir}/report.md")
    return exit_code


def replay(dirpath):
    """Rebuild report from an existing run directory; never starts a VM."""
    dirpath = os.path.abspath(dirpath)
    stamped = os.path.join(dirpath, "serial.stamped")
    if not os.path.isfile(stamped):
        eprint(f"replay: missing {stamped}")
        return 40
    lines = read_stamped(stamped)
    shots = []
    shots_dir = os.path.join(dirpath, "shots")
    if os.path.isdir(shots_dir):
        for name in sorted(os.listdir(shots_dir)):
            if not name.endswith(".png"):
                continue
            m = re.match(r"(\d+)_shot_(\d+)s\.png$", name)
            if not m:
                continue
            png = os.path.join(shots_dir, name)
            info = analyze_png(png)
            info.update({"at_sec": float(m.group(2)), "png": png})
            shots.append(info)
    key_result = None
    before = os.path.join(shots_dir, "keys_370s_before.png") if os.path.isdir(shots_dir) else None
    after = os.path.join(shots_dir, "keys_370s_after.png") if os.path.isdir(shots_dir) else None
    if before and after and os.path.isfile(before) and os.path.isfile(after):
        d = diff_pngs(before, after)
        key_result = {"at_sec": 370.0, "keys": ["(replay)"],
                      "before": before, "after": after, **d}
    meta = dict(iso="(replay)", img=None, smp=1, mem="4096M", accel="kvm",
                expect="full", no_image=False, key_requested=False)
    print(f"replay: {len(lines)} serial lines, {len(shots)} shots")
    return finalize(dirpath, os.path.basename(dirpath), meta, lines, shots, key_result)


SPANS = [
    ("boot->autostart hook", "bios_banner", "autostart_hook"),
    ("boot->dbus session", "bios_banner", "dbus_session"),
    ("boot->wl_fb ready", "bios_banner", "wl_fb_ready"),
    ("boot->kwin accepting clients", "bios_banner", "kwin_accepting"),
    ("boot->kded6", "bios_banner", "kded6"),
    ("boot->plasmashell exec", "bios_banner", "plasmashell_exec"),
    ("boot->first composited frame", "bios_banner", "first_frame"),
    ("boot->konsole exec", "bios_banner", "konsole_exec"),
    ("boot->chromium exec", "bios_banner", "chromium_exec"),
    ("kwin->plasmashell", "kwin_accepting", "plasmashell_exec"),
]


def write_report(outdir, tl, procs):
    m = tl["markers"]
    p = tl["processes"]
    out = []
    out.append(f"# HBOS KDE verify — run {tl['run_id']}\n")
    out.append(f"- ISO: `{tl['iso']}`")
    out.append(f"- img: `{tl['img']}`")
    out.append(f"- accel={tl['accel']} smp={tl['smp']} mem={tl['mem']} expect={tl['expect']}")
    drift = tl.get("input_drift") or []
    if drift:
        out.append(f"- **WARNING: input artifacts changed during this run: {drift}**")
        out.append("  the image/ISO was repacked while the VM was running; "
                   "this run mixes two builds and is NOT a valid baseline.\n")
    out.append(f"- **exit_code = {tl['exit_code']}**\n")

    out.append("## Startup milestones (host seconds since QEMU launch)\n")
    out.append("| marker | host s | guest kernel t (ms) |")
    out.append("|---|---|---|")
    for k, v in m.items():
        out.append(f"| {k} | {v['host_sec']} | {v['kernel_t_ms']} |")
    out.append("")

    out.append("## Segment durations\n")
    out.append("| segment | seconds |")
    out.append("|---|---|")
    for label, a, b in SPANS:
        if a in m and b in m:
            out.append(f"| {label} | {round(m[b]['host_sec'] - m[a]['host_sec'], 2)} |")
        else:
            missing = [x for x in (a, b) if x not in m]
            out.append(f"| {label} | n/a (missing: {', '.join(missing)}) |")
    out.append("")

    out.append("## Screenshots\n")
    out.append("| t (s) | file | dark% | colors | panel nonblack% | wallpaper dark% | panel? |")
    out.append("|---|---|---|---|---|---|---|")
    for s in tl["shots"]:
        out.append(
            f"| {s['at_sec']:.0f} | `{s['png']}` | {s['dark_pct']} | "
            f"{s['distinct_colors']} | {s['panel_nonblack_pct']} | "
            f"{s['wallpaper_dark_pct']} | {s['panel_likely_present']} |")
    out.append("")

    kt = tl["key_test"]
    out.append("## Interactive input test\n")
    if kt:
        out.append(f"- sent keys: `{' '.join(kt['keys'])}` at t={kt['at_sec']:.0f}s")
        out.append(f"- before: `{kt['before']}`")
        out.append(f"- after:  `{kt['after']}`")
        out.append(f"- changed pixels: {kt.get('changed_pct')}%  mean absdiff: {kt.get('mean_absdiff')}")
        out.append(f"- **responded: {kt.get('responded')}**\n")
    else:
        out.append("- not run\n")

    out.append("## Guest process evidence (from serial EXEC/EXIT ledger)\n")
    out.append(f"- EXEC events: {p['exec_count']}, still alive: {p['live_count']}")
    out.append(f"- plasmashell alive: {p['plasmashell_live']}")
    out.append(f"- konsole alive: {p['konsole_live']}")
    out.append(f"- chromium alive: {p['chromium_live']}")
    out.append(f"- wayland frames committed (max count): {p['frames_committed']}\n")
    live = procs["live"]
    if live:
        out.append("| pid | path | kernel t (ms) |")
        out.append("|---|---|---|")
        for pid, v in live.items():
            out.append(f"| {pid} | `{v['path']}` | {v['kernel_t_ms']} |")
        out.append("")

    fs = tl.get("findings") or []
    out.append("## Findings (what to fix)\n")
    if fs:
        out.append("| id | severity | problem | evidence |")
        out.append("|---|---|---|---|")
        for f in fs:
            out.append(f"| `{f[0]}` | {f[1]} | {f[2]} | {f[3]} |")
    else:
        out.append("none")
    out.append("")

    out.append("## Verdict\n")
    v = tl["exit_code"]
    verdicts = {
        0: "all requested criteria met",
        10: "desktop up but interactive/full-scene criteria unmet",
        20: "kwin running, plasmashell never started",
        30: "boot failed / autostart hook never reached / total timeout",
    }
    out.append(f"`{v}` — {verdicts.get(v, 'unknown')}\n")

    with open(os.path.join(outdir, "report.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out))


if __name__ == "__main__":
    sys.exit(main())
