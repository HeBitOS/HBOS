#!/usr/bin/env bash
# HBOS KDE/Chromium verification harness.
#
# Holds /tmp/hbos-vm.lock for the entire VM lifetime so the 4 agents on this
# box (4 GB free, one VM = -m 4096M) serialise instead of OOMing each other.
#
#   bash scripts/kde_verify.sh                       # full run, default paths
#   bash scripts/kde_verify.sh --no-image --timeout 120
#   bash scripts/kde_verify.sh --shots 30,90,150 --smp 2 --mem 3072M
#   bash scripts/kde_verify.sh --lock-timeout 900    # queue behind other agents
#
# Exit codes are the contract; see --help / kde_verify_run.py docstring.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="${HBOS_VM_LOCK:-/tmp/hbos-vm.lock}"
PY="${HBOS_VERIFY_PY:-$ROOT/scripts/kde_verify_run.py}"

usage() {
    cat <<'EOF'
HBOS KDE/Chromium verification harness.

Holds /tmp/hbos-vm.lock for the whole VM lifetime, so any number of agents can
call it concurrently and the VMs queue instead of fighting over 4 GB of RAM.

usage: bash scripts/kde_verify.sh [options]

  --iso PATH          BIOS ISO                (default build/hbos-bios.iso)
  --img PATH          KDE ext2 disk image     (default build/hbos-kde-chromium.img)
  --no-image          boot the ISO alone, do not attach the AHCI disk
  --timeout SECS      total VM wall-clock budget              (default 420)
  --shots LIST        comma list of screendump times in seconds
                      (default 20,45,80,120,180,240,300)
  --smp N             vCPU count                              (default 1)
  --mem SIZE          guest RAM, e.g. 4096M / 2G              (default 4096M)
  --accel kvm|tcg     hypervisor backend                      (default kvm)
  --net none|user     NIC mode; 'user' adds virtio-net         (default none)
  --keys LIST         QEMU key names for the input test
                      (default k,o,n,s,o,l,e)
  --key-test-at SECS  force the input test at this time (0 = after last shot)
  --click-first       click into the centre window before sending keys
                      (without a focus target sendkey does nothing)
  --expect MODE       full | desktop | boot                   (default full)
                      full    = plasmashell + konsole + chromium
                      desktop = plasmashell only
                      boot    = shell reached the autostart hook
  --run-id ID         override the output directory name
  --out DIR           parent output dir  (default build/tmp/kde_verify_runs)
  --replay DIR        rebuild timeline/report from an existing run dir,
                      never starts a VM (no lock needed)
  --lock-timeout SECS how long to wait for the VM lock        (default 3600)
  --no-lock           skip flock entirely (ONLY for a lock-holder's child)
  -h, --help          this text

outputs, under <out>/<run-id>/:
  serial.log          raw guest serial
  serial.stamped      every line prefixed with host elapsed seconds
  shots/*.png         timeline screenshots (+ key-test before/after)
  timeline.json       markers, segments, shot stats, key test
  procs.json          EXEC/EXIT ledger = guest process liveness
  report.md           human report, regenerated on every run

exit codes:
   0  all requested criteria met
  10  desktop up, konsole/chromium or input feedback missing
  20  kwin running, plasmashell never started
  30  boot failure / autostart hook never reached / timeout
  40  QEMU refused to start (KVM, image, arguments)
  75  VM lock not acquired within --lock-timeout
EOF
}

PASSTHROUGH=()
LOCK_TIMEOUT=3600
NO_LOCK=0
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)      usage; exit 0 ;;
        --lock-timeout) LOCK_TIMEOUT="${2:?--lock-timeout needs a value}"; shift 2 ;;
        --lock-timeout=*) LOCK_TIMEOUT="${1#*=}"; shift ;;
        --no-lock)      NO_LOCK=1; shift ;;
        *)              PASSTHROUGH+=("$1"); shift ;;
    esac
done

if [ ! -f "$PY" ]; then
    echo "kde_verify: missing helper $PY" >&2
    exit 40
fi
mkdir -p "$ROOT/build/tmp"

if [ "$NO_LOCK" = "1" ]; then
    echo "kde_verify: WARNING --no-lock, caller must already hold $LOCK" >&2
    exec python3 "$PY" "${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}"
fi

if ! command -v flock >/dev/null 2>&1; then
    echo "kde_verify: flock not found, refusing to start a VM unlocked" >&2
    exit 40
fi

# The lock is taken on fd 9 and inherited across exec into python, so it stays
# held for the whole VM lifetime and is released the instant the process dies.
exec 9>"$LOCK"
START=$(date +%s)
if ! flock -w "$LOCK_TIMEOUT" 9; then
    echo "kde_verify: could not acquire $LOCK within ${LOCK_TIMEOUT}s" \
         "(another agent's VM is running)" >&2
    exit 75
fi
WAITED=$(( $(date +%s) - START ))
[ "$WAITED" -gt 0 ] && echo "kde_verify: acquired VM lock after ${WAITED}s wait" >&2

python3 "$PY" "${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}"
rc=$?
echo "kde_verify: run finished rc=$rc after $(( $(date +%s) - START ))s" >&2
exit "$rc"
