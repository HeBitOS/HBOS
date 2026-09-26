#!/usr/bin/env bash
# FAT32 持久化全链路回归测试（issue #1 验收流程固化）。
#
# 在 QEMU 里对空白硬盘跑 `install auto`（真 FAT32）→ 写根目录/子目录文件 →
# 宿主机用 fsck.fat / mtools 校验卷（规范合规 + 双向互认）→ mcopy 注入宿主
# 文件 → 重启回读全部文件。
#
# 用法: make fat32-smoke   （或直接 bash scripts/test_fat32_persist.sh）
set -euo pipefail

HBOS_REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HBOS_NOGUI_BUILD="${NOGUI_BUILD_DIR:-build-nogui}"
HBOS_QEMU="${QEMU:-qemu-system-x86_64}"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[FAT32] missing command: $1"
        exit 1
    }
}

need "$HBOS_QEMU"
need qemu-img
need mdir
need mtype
need mcopy
need fsck.fat
need dd
need grep
need sed

# ── 构建被测镜像（no-GUI：串口直达 shell） ──────────────────────────
make -C "$HBOS_REPO_DIR" NOGUI_BUILD_DIR="$HBOS_NOGUI_BUILD" nogui
HBOS_ISO="$HBOS_REPO_DIR/$HBOS_NOGUI_BUILD/hbos-bios.iso"

HBOS_TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$HBOS_TMP_DIR"' EXIT

DISK="$HBOS_TMP_DIR/data.img"
qemu-img create -f raw "$DISK" 64M >/dev/null

# 默认安装范围 = LBA 2048（src/fs.c 的 HBFS_DEFAULT_START_LBA，与 Makefile
# fat32-copy 目标的 @@1048576 一致）
PART_OFFSET_LBA=2048
DISK_BYTES="$(qemu-img info -f raw "$DISK" | sed -n 's/^virtual size:.*(\([0-9]*\) bytes)/\1/p' | head -1)"
TOTAL_SECTORS=$((DISK_BYTES / 512))

# ── 串口驱动：coproc 建立到 qemu 的常驻管道，发一条命令、等它的回显/
# 输出标记（都是换行结尾、能穿过 sed 行缓冲）落进日志再发下一条。
# 注意不能等 shell 提示符本身：提示符不带换行，行缓冲过滤器永远刷不出它。 ──
LOG_FILE=""
QEMU_PID=""

wait_log() { # $1=等待的文本 $2=超时秒数
    for _ in $(seq 1 "$2"); do
        grep -q "$1" "$LOG_FILE" 2>/dev/null && return 0
        kill -0 "$QEMU_PID" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}

guest_session() { # $1=日志 $2=命令/标记对照文件（每行: 命令|发完后等待的标记）
    local log="$1" script="$2"
    LOG_FILE="$log"
    : >"$log"

    coproc QEMUIN { exec timeout 240 "$HBOS_QEMU" -m 512M \
        -device ich9-ahci,id=ahci \
        -drive file="$DISK",format=raw,if=none,id=hd0 \
        -device ide-hd,drive=hd0,bus=ahci.0 \
        -cdrom "$HBOS_ISO" -boot d \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        -display none -serial stdio -monitor none -no-reboot \
        > >(sed -u -e 's/\x1b\[[0-9;]*m//g' -e 's/\r//g' >"$log") 2>&1; }
    QEMU_PID="$QEMUIN_PID"
    eval "exec ${QEMUIN[0]}<&-" # child 的 stdout 由 procsub 收走，主进程关掉读端

    # shell 起来之前发的输入会丢（串口输入环形缓冲还没接上），必须先等
    # 到 "Shell ready" 再发第一条命令；再留 1s 让提示符就绪。
    wait_log "Shell ready" 120 || {
        kill "$QEMU_PID" 2>/dev/null || true
        return 1
    }
    sleep 1

    local line cmd marker
    while IFS='|' read -r cmd marker; do
        [ -z "$cmd" ] && continue
        printf '%s\r' "$cmd" >&"${QEMUIN[1]}"
        if [ -n "$marker" ] && [ "$marker" != "-" ]; then
            wait_log "$marker" 90 || {
                kill "$QEMU_PID" 2>/dev/null || true
                return 1
            }
        fi
    done <"$script"

    # 最后一条命令（poweroff）：等 qemu 自己退出
    for _ in $(seq 1 60); do
        kill -0 "$QEMU_PID" 2>/dev/null || break
        sleep 1
    done
    kill "$QEMU_PID" 2>/dev/null || true
    # 先关写端再 wait：coproc 被 wait 回收后 bash 会撤销 QEMUIN 变量
    if [ -n "${QEMUIN[1]+x}" ]; then eval "exec ${QEMUIN[1]}>&-"; fi
    wait "$QEMU_PID" 2>/dev/null || true
    return 0
}

die_fail() {
    echo "[FAT32] FAIL: $1"
    shift
    local f
    for f in "$@"; do
        [ -f "$f" ] && { echo "---- tail $f ----"; tail -40 "$f"; }
    done
    exit 1
}

# ── 阶段 1：装卷 + 写文件（根目录 + 子目录） ────────────────────────
cat >"$HBOS_TMP_DIR/p1.script" <<'EOF'
install auto|install: FAT32 partition ready
writefile persist.txt hello-issue1-persistence|writefile persist.txt
mkdir testdir|mkdir testdir
writefile testdir/nested.txt nested-write-ok|testdir/nested.txt
poweroff|-
EOF
echo "[FAT32] boot 1: install auto + write root/subdir files"
guest_session "$HBOS_TMP_DIR/p1.log" "$HBOS_TMP_DIR/p1.script" ||
    die_fail "boot 1 session failed" "$HBOS_TMP_DIR/p1.log"
grep -q "install: FAT32 partition ready" "$HBOS_TMP_DIR/p1.log" ||
    die_fail "install auto did not report FAT32 partition ready" "$HBOS_TMP_DIR/p1.log"

# ── 宿主机校验：规范合规 + HBOS 写入的内容用标准工具可读 ───────────
dd if="$DISK" of="$HBOS_TMP_DIR/part.img" bs=512 \
    skip=$PART_OFFSET_LBA count=$((TOTAL_SECTORS - PART_OFFSET_LBA)) status=none

fsck.fat -vn "$HBOS_TMP_DIR/part.img" >"$HBOS_TMP_DIR/fsck.log" 2>&1 ||
    die_fail "fsck.fat rejected the volume" "$HBOS_TMP_DIR/fsck.log"
if grep -Eq "Auto-correcting|less than the required minimum|summary wrong" \
    "$HBOS_TMP_DIR/fsck.log"; then
    die_fail "fsck.fat reported fixable FAT32 compliance problems" "$HBOS_TMP_DIR/fsck.log"
fi

mdir -i "$HBOS_TMP_DIR/part.img" :: >"$HBOS_TMP_DIR/mdir.log" 2>&1 ||
    die_fail "mtools could not list the volume" "$HBOS_TMP_DIR/mdir.log"
grep -qi "persist" "$HBOS_TMP_DIR/mdir.log" ||
    die_fail "PERSIST.TXT not visible to mtools" "$HBOS_TMP_DIR/mdir.log"
grep -qi "testdir" "$HBOS_TMP_DIR/mdir.log" ||
    die_fail "TESTDIR not visible to mtools" "$HBOS_TMP_DIR/mdir.log"

[ "$(mtype -i "$HBOS_TMP_DIR/part.img" ::PERSIST.TXT)" = "hello-issue1-persistence" ] ||
    die_fail "mtools read back wrong PERSIST.TXT content" "$HBOS_TMP_DIR/mdir.log"
[ "$(mtype -i "$HBOS_TMP_DIR/part.img" ::TESTDIR/NESTED.TXT)" = "nested-write-ok" ] ||
    die_fail "mtools read back wrong TESTDIR/NESTED.TXT content" "$HBOS_TMP_DIR/mdir.log"

# 宿主 → HBOS 方向：mtools 注入，重启后 guest 必须能读
echo "written-by-linux-host-mtools" >"$HBOS_TMP_DIR/fromhost.txt"
mcopy -i "$DISK@@$((PART_OFFSET_LBA * 512))" "$HBOS_TMP_DIR/fromhost.txt" ::FROMHOST.TXT ||
    die_fail "mcopy could not inject a file into the volume"

# ── 阶段 2：重启回读（持久化） ─────────────────────────────────────
cat >"$HBOS_TMP_DIR/p2.script" <<'EOF'
cat persist.txt|hello-issue1-persistence
cat testdir/nested.txt|nested-write-ok
cat FROMHOST.TXT|written-by-linux-host-mtools
poweroff|-
EOF
echo "[FAT32] boot 2: reboot and read everything back"
guest_session "$HBOS_TMP_DIR/p2.log" "$HBOS_TMP_DIR/p2.script" ||
    die_fail "boot 2 session failed" "$HBOS_TMP_DIR/p2.log"

P2="$HBOS_TMP_DIR/p2.log"
grep -A1 "cat persist.txt" "$P2" | grep -q "hello-issue1-persistence" ||
    die_fail "persist.txt did not survive reboot" "$P2"
grep -A1 "cat testdir/nested.txt" "$P2" | grep -q "nested-write-ok" ||
    die_fail "testdir/nested.txt did not survive reboot (subdir rebuild)" "$P2"
grep -A1 "cat FROMHOST.TXT" "$P2" | grep -q "written-by-linux-host-mtools" ||
    die_fail "host-injected FROMHOST.TXT not readable in guest" "$P2"
if grep -q "not found" "$P2"; then
    die_fail "unexpected 'not found' during reboot readback" "$P2"
fi

echo "[FAT32] PASS"
