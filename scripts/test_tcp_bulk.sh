#!/usr/bin/env bash
# TCP 大响应静默截断端到端回归（docs/PERFORMANCE.md §6 / §6.1 验收流程固化）。
#
# 背景：手写 IPv4 TCP 栈曾在约 113 KB 处静默截断大响应（guest `wget -O -`
# 无报错、少字节）。根因是 net_tcp_exchange 的固定 80 次迭代上限（见 §6.1.1）。
# 本脚本用 QEMU + 宿主 HTTP 服务端做端到端**逐字节**回归，锁定该缺陷：
#
#   1) 142000 字节（报告用例）—— 修复前交付 113761/142000
#   2) 115200 字节 —— 80 段边界上侧，修复前截断
#   3) 113760 字节 —— 80 段边界下侧，修复前恰好跑满
#   4) 42600 字节基准 —— 修复前后都应完整（无回归）
#   5) 100000 字节 —— 落盘 + 重启回读（RAMFS_MAX_FILE_SIZE=131072 内）
#   6) 分段无关性：服务端按 300/600/900/1440 字节切分，四个结果都要完整
#      （修复前交付 115011/114361/113221，消费段数恒为 80）
#   7) 错误路径：服务端声明 Content-Length 大于实发字节，guest 必须报
#      `tcp response truncated`（修复前为静默成功）
#
# 校验方式：串口日志里的响应正文与宿主源文件逐字节比对（cmp 语义），不是
# 只比长度。142000 超出 RAMFS_MAX_FILE_SIZE（131072），只能走串口校验；
# 落盘用例一律 <= 131072。
#
# 用法: make tcp-bulk-test   （或直接 bash scripts/test_tcp_bulk.sh）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_BIN="${QEMU:-qemu-system-x86_64}"
ISO="${ISO:-$ROOT/build/hbos-bios.iso}"
GUEST_HOST="10.0.2.2"        # QEMU user-mode 网络里的宿主地址
BOOT_WAIT="${TCP_BULK_BOOT_WAIT:-180}"
CMD_WAIT="${TCP_BULK_CMD_WAIT:-150}"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "[TCP-BULK] missing command: $1"
        exit 1
    }
}
need "$QEMU_BIN"
need python3
need cmp
need curl
need qemu-img

WORK="$(mktemp -d)"
HTTP_PID=""
HELPER_PID=""
QEMU_PID=""

cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null || true
    [ -n "$HELPER_PID" ] && kill "$HELPER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() {
    echo "[TCP-BULK] FAIL: $1"
    shift
    local f
    for f in "$@"; do
        [ -f "$f" ] && { echo "---- tail $f ----"; tail -30 "$f"; }
    done
    exit 1
}

# ── 0. 确保被测镜像存在 ────────────────────────────────────────────
if [ ! -f "$ISO" ]; then
    echo "[TCP-BULK] $ISO missing, building bios-iso"
    make -C "$ROOT" bios-iso
fi

mkdir -p "$WORK/www"

# ── 1. 确定性测试文件（纯 ASCII、LF 行尾、每行 100 字节；行号在行首与
#       行尾各出现一次，既便于定位差异，也便于做"最后一行已到达"判据）──
python3 - "$WORK/www" <<'PY'
import os, sys
outdir = sys.argv[1]

def gen(path, size):
    buf = bytearray()
    i = 0
    while len(buf) < size:
        n = b"%08d" % (i * 100)
        buf += n + b"x" * 83 + n + b"\n"   # 8+83+8+1 = 100 字节/行
        i += 1
    data = bytes(buf[:size])
    assert len(data) == size, (len(data), size)
    assert b"\r" not in data and b"\x00" not in data
    with open(path, "wb") as f:
        f.write(data)

for name, size in (("t.txt", 142000), ("t115200.txt", 115200),
                   ("t113760.txt", 113760), ("t42600.txt", 42600),
                   ("t100000.txt", 100000)):
    gen(os.path.join(outdir, name), size)
print("[TCP-BULK] generated deterministic fixtures in %s" % outdir)
PY

# ── 2. 空闲端口自检（不硬编码端口，避免与宿主既有服务冲突） ─────────
pick_port() {
    python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()'
}
PORT="$(pick_port)"

# ── 3. 宿主 HTTP 服务端：python3 -m http.server（与复现步骤一致） ───
( cd "$WORK/www" && exec python3 -m http.server "$PORT" --bind 0.0.0.0 ) \
    >"$WORK/http.log" 2>&1 &
HTTP_PID=$!

for _ in $(seq 1 40); do
    curl -sf --max-time 2 "http://127.0.0.1:$PORT/t42600.txt" -o /dev/null && break
    kill -0 "$HTTP_PID" 2>/dev/null || fail "python3 -m http.server died on port $PORT" "$WORK/http.log"
    sleep 0.5
done
curl -sf --max-time 2 "http://127.0.0.1:$PORT/t42600.txt" -o /dev/null ||
    fail "host http.server not reachable on port $PORT" "$WORK/http.log"
echo "[TCP-BULK] host http.server on port $PORT (pid $HTTP_PID)"

# ── 4. 分段无关性 + 截断错误路径服务端（单进程多端口；端口由内核分配后
#       写进 segports.txt，避免端口竞争） ────────────────────────────
cat >"$WORK/helper.py" <<'PY'
import os, socket, sys, threading

work = sys.argv[1]
body = open(os.path.join(work, "www", "t.txt"), "rb").read()   # 142000

def header(n):
    return ("HTTP/1.0 200 OK\r\nServer: hbos-tcp-bulk-test\r\n"
            "Content-Type: text/plain\r\nContent-Length: %d\r\n"
            "Connection: close\r\n\r\n" % n).encode()

def handle(c, seg, declared, actual):
    try:
        c.recv(4096)
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c.sendall(header(declared))
        off = 0
        while off < actual:
            n = min(seg, actual - off)
            c.sendall(body[off:off + n])
            off += n
    except Exception:
        pass
    finally:
        try:
            c.shutdown(socket.SHUT_WR)   # FIN：数据到此为止
        except Exception:
            pass
        c.close()

def listen(seg, declared, actual, label, outfile):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 0))
    s.listen(8)
    with open(os.path.join(work, outfile), "a") as f:
        f.write("%d %d %d %s\n" % (s.getsockname()[1], seg, declared, label))
        f.flush()
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c, seg, declared, actual),
                         daemon=True).start()

# 慢速用例专用较小的正文（42600B）：idle 误判与响应大小无关，只需暂停
# 超过旧阈值（~640ms）即可复现；用 142000 会让串口回显时间过长，把整个
# 会话拖过单次 QEMU 的时间预算。
body_slow = open(os.path.join(work, "www", "t42600.txt"), "rb").read()

def handle_slow(c, pause_ms):
    """慢速服务端：正文发到约 40% 处暂停再续发。回归 t4 发现的空闲误判——
    旧实现连续 16 轮零进展（约 640ms）即判对端停发并报 tcp response
    truncated，而 640ms 小于 RFC 6298 最小 RTO 1000ms，于是合法但稍慢的
    服务端（重传间隙 >640ms）被误判为截断。修复后空闲判定改为墙钟 5000ms，
    暂停 1000ms/3000ms 都必须逐字节完整交付。"""
    import time
    try:
        c.recv(4096)
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        c.sendall(header(len(body_slow)))
        cut = len(body_slow) * 2 // 5
        c.sendall(body_slow[:cut])
        time.sleep(pause_ms / 1000.0)
        c.sendall(body_slow[cut:])
    except Exception:
        pass
    finally:
        try:
            c.shutdown(socket.SHUT_WR)
        except Exception:
            pass
        c.close()

def listen_slow(pause_ms, label, outfile):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 0))
    s.listen(8)
    with open(os.path.join(work, outfile), "a") as f:
        f.write("%d 0 %d %s\n" % (s.getsockname()[1], pause_ms, label))
        f.flush()
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle_slow, args=(c, pause_ms), daemon=True).start()

for seg in (300, 600, 900, 1440):
    threading.Thread(target=listen,
                     args=(seg, len(body), len(body), "seg%d" % seg, "segports.txt"),
                     daemon=True).start()
# 声明 142000、实发 100000 后 FIN ⇒ 客户端必须报截断（修复前静默成功）
threading.Thread(target=listen,
                 args=(1440, len(body), 100000, "trunc", "segports.txt"),
                 daemon=True).start()
# 慢速服务端：正文中途暂停 1000ms / 3000ms（均 >= RFC 6298 最小 RTO 1000ms）
# ⇒ 必须逐字节完整交付。旧实现的约 640ms 空闲判定会在此报截断（回归防线）。
for _pause in (1000, 2000):
    threading.Thread(target=listen_slow,
                     args=(_pause, "slow%d" % _pause, "segports.txt"),
                     daemon=True).start()
print("ready", flush=True)
while True:
    threading.Event().wait(3600)
PY

: >"$WORK/segports.txt"
python3 "$WORK/helper.py" "$WORK" >"$WORK/helper.log" 2>&1 &
HELPER_PID=$!
for _ in $(seq 1 60); do
    [ "$(wc -l <"$WORK/segports.txt")" -ge 7 ] && break
    kill -0 "$HELPER_PID" 2>/dev/null || fail "helper server died" "$WORK/helper.log"
    sleep 0.25
done
[ "$(wc -l <"$WORK/segports.txt")" -ge 7 ] ||
    fail "helper server did not publish 7 ports" "$WORK/helper.log"
echo "[TCP-BULK] helper servers: $(tr '\n' ';' <"$WORK/segports.txt")"

# ── 5. 串口会话驱动（照搬 scripts/test_fat32_persist.sh 的 coproc 范式）──
LOG_FILE=""
PROMPT="hbos:/#"

prompt_count() {
    { grep -oF "$PROMPT" "$LOG_FILE" 2>/dev/null || true; } | wc -l
}

# 等提示符出现次数超过 $1（= 上一条命令已跑完、正文已全部落盘）
wait_prompt_gt() {
    local target="$1" timeout="$2" c
    for _ in $(seq 1 "$timeout"); do
        c="$(prompt_count)"
        [ "$c" -gt "$target" ] && return 0
        kill -0 "$QEMU_PID" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}

wait_log() { # $1=等待文本 $2=超时秒数
    for _ in $(seq 1 "$2"); do
        grep -qF "$1" "$LOG_FILE" 2>/dev/null && return 0
        kill -0 "$QEMU_PID" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}

# guest_session <log> <script> [extra qemu args...]
# script 每行一条 guest shell 命令；以 "poweroff" 结尾时等待 qemu 自行退出。
guest_session() {
    local log="$1" script="$2"
    shift 2
    LOG_FILE="$log"
    : >"$log"

    coproc QEMUIN { exec timeout "${TCP_BULK_QEMU_TIMEOUT:-1800}" "$QEMU_BIN" -m 512M \
        -cdrom "$ISO" -boot d \
        "$@" \
        -netdev user,id=net0 -device e1000,netdev=net0 \
        -display none -serial stdio -monitor none -no-reboot \
        > >(sed -u -e 's/\x1b\[[0-9;]*m//g' -e 's/\r//g' >"$log") 2>&1; }
    QEMU_PID="$QEMUIN_PID"
    eval "exec ${QEMUIN[0]}<&-"

    # ① shell 就绪前发的输入会丢 → 必须等 "Shell ready"；
    # ② 提示符不带换行、刷不出行缓冲 → 等待标记一律用带换行的输出。
    wait_log "Shell ready" "$BOOT_WAIT" || {
        kill "$QEMU_PID" 2>/dev/null || true
        return 1
    }
    sleep 1
    printf 't' >&"${QEMUIN[1]}"          # 引导菜单：切到 Shell（与复现步骤一致）
    sleep 2
    printf 'dhcp\r' >&"${QEMUIN[1]}"
    wait_log "dhcp: ok" 90 || {
        kill "$QEMU_PID" 2>/dev/null || true
        return 1
    }
    sleep 1

    local cmd
    while IFS= read -r cmd; do
        [ -z "$cmd" ] && continue
        case "$cmd" in
        poweroff*)
            printf '%s\r' "$cmd" >&"${QEMUIN[1]}"
            for _ in $(seq 1 60); do
                kill -0 "$QEMU_PID" 2>/dev/null || break
                sleep 1
            done
            ;;
        *)
            local before
            before="$(prompt_count)"
            printf '%s\r' "$cmd" >&"${QEMUIN[1]}"
            wait_prompt_gt "$before" "$CMD_WAIT" || {
                echo "[TCP-BULK] command did not complete: $cmd" >&2
                kill "$QEMU_PID" 2>/dev/null || true
                return 1
            }
            sleep 1
            ;;
        esac
    done <"$script"

    kill "$QEMU_PID" 2>/dev/null || true
    if [ -n "${QEMUIN[1]+x}" ]; then eval "exec ${QEMUIN[1]}>&-"; fi
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
    return 0
}

# ── 6. 逐字节比对器：从串口日志抽出某 URL 的响应正文，与宿主源文件 cmp ──
cat >"$WORK/assert_body.py" <<'PY'
#!/usr/bin/env python3
r"""从串口日志抽出 `wget -O - <url>` 之后的响应正文，与宿主源文件逐字节比对。

日志已经过 `sed -e 's/\x1b\[[0-9;]*m//g' -e 's/\r//g'`，行尾统一为 \n。
guest 终端在正文末尾补一个 \n（正文本身不以 \n 结尾时）属于终端行为，
不算内容损坏，单独接受为 truth+'\n'。
"""
import sys

log_path, url, truth_path = sys.argv[1], sys.argv[2], sys.argv[3]
expect_error = sys.argv[4] if len(sys.argv) > 4 else ""

raw = open(log_path, "rb").read()
truth = open(truth_path, "rb").read()

needle = b"wget -O - " + url.encode()
idx = raw.find(needle)
if idx < 0:
    print("  [x] command echo not found: %s" % url)
    sys.exit(1)
start = raw.find(b"\n", idx)
start = len(raw) if start < 0 else start + 1
end = raw.find(b"hbos:/#", start)
if end < 0:
    end = len(raw)
body = raw[start:end]

if expect_error:
    if expect_error.encode() in raw[start:start + 4096]:
        print("  [ok] guest reported expected error: %r" % expect_error)
        sys.exit(0)
    print("  [x] expected error %r not found after command" % expect_error)
    print("      first 300 bytes: %r" % body[:300])
    sys.exit(1)

if body == truth:
    print("  [ok] byte-exact: %d bytes (cmp clean)" % len(truth))
    sys.exit(0)
if body == truth + b"\n":
    print("  [ok] byte-exact: %d bytes + terminal trailing newline" % len(truth))
    sys.exit(0)

common = 0
for i in range(min(len(body), len(truth))):
    if body[i] != truth[i]:
        break
    common += 1
diffs = [i for i in range(min(len(body), len(truth))) if body[i] != truth[i]]
print("  [x] MISMATCH: delivered=%d expected=%d common-prefix=%d interior-mismatches=%d"
      % (len(body), len(truth), common, len(diffs)))
if diffs:
    i = diffs[0]
    print("      first mismatch at %d; truth=%r body=%r"
          % (i, truth[i:i + 24], body[i:i + 24]))
sys.exit(1)
PY
chmod +x "$WORK/assert_body.py"

# 比对器自检：断言必须是"非空断言"——对截断正文必须报失败，否则回归会
# 假阳性通过（这正是本缺陷"静默截断"最容易骗过测试的地方）。
if ! python3 - "$WORK" <<'PY'
import os, subprocess, sys
work = sys.argv[1]
truth = os.path.join(work, "www", "t.txt")
log = os.path.join(work, "selfcheck.log")
with open(truth, "rb") as f:
    data = f.read()
with open(log, "wb") as f:
    f.write(b"wget -O - http://h/t.txt\n" + data[:113761] + b"\nhbos:/# ")
r = subprocess.run([sys.executable, os.path.join(work, "assert_body.py"),
                    log, "http://h/t.txt", truth],
                   capture_output=True, text=True)
if r.returncode == 0:
    print("  [x] self-check FAILED: assert_body.py accepted a truncated body")
    sys.exit(1)
print("  [ok] self-check: assert_body.py rejects the 113761-byte truncation")
PY
then
    fail "assert_body.py self-check failed (assertion would be vacuous)"
fi

# 落盘回读校验：`cat big` 的正文与宿主 100000 字节 fixture 逐字节比对。
cat >"$WORK/assert_reboot.py" <<'PY'
#!/usr/bin/env python3
r"""校验重启回读：串口日志里 `cat big` 的正文 == 宿主 fixture（逐字节）。"""
import sys

log_path, truth_path = sys.argv[1], sys.argv[2]
raw = open(log_path, "rb").read()
truth = open(truth_path, "rb").read()
idx = raw.find(b"cat big\n")
if idx < 0:
    print("  [x] 'cat big' echo not found in reboot log")
    sys.exit(1)
start = idx + len(b"cat big\n")
end = raw.find(b"hbos:/#", start)
body = raw[start:end if end >= 0 else len(raw)]
ok = body == truth or body == truth + b"\n"
print("  [%s] reboot readback: delivered=%d expected=%d cmp=%s"
      % ("ok" if ok else "x", len(body), len(truth),
         "clean" if ok else "MISMATCH"))
if not ok:
    common = 0
    for i in range(min(len(body), len(truth))):
        if body[i] != truth[i]:
            break
        common += 1
    print("      common prefix %d; truth=%r body=%r"
          % (common, truth[common:common + 24], body[common:common + 24]))
sys.exit(0 if ok else 1)
PY
chmod +x "$WORK/assert_reboot.py"

# ── 7. 会话 A：串口校验的 5 个尺寸用例 + 4 个分段用例 + 截断错误用例 ──
echo "[TCP-BULK] session A: serial-verified cases (142000 / 115200 / 113760 / 42600 / 100000)"
{
    echo "wget -O - http://$GUEST_HOST:$PORT/t.txt"
    echo "wget -O - http://$GUEST_HOST:$PORT/t115200.txt"
    echo "wget -O - http://$GUEST_HOST:$PORT/t113760.txt"
    echo "wget -O - http://$GUEST_HOST:$PORT/t42600.txt"
    echo "wget -O - http://$GUEST_HOST:$PORT/t100000.txt"
    while read -r hport _hseg _hdecl hlabel; do
        case "$hlabel" in slow*) continue ;; esac
        echo "wget -O - http://$GUEST_HOST:$hport/t.txt"
    done <"$WORK/segports.txt"
    # 以 poweroff 收尾：guest_session 在脚本读完后立即 kill QEMU，若最后一条
    # 命令是 wget，串口正文可能还在流式回显就被切断，断言会读到半截正文
    # （表现为 delivered 很小、common-prefix=0）。poweroff 让驱动显式等到
    # guest 自行停机，最后一条传输一定已经完整落进日志。
    echo "poweroff"
} >"$WORK/sessionA.script"

guest_session "$WORK/sessionA.log" "$WORK/sessionA.script" ||
    fail "session A QEMU run failed (see log)" "$WORK/sessionA.log"

RC=0
check_case() { # label url truthfile
    echo "[TCP-BULK] $1"
    python3 "$WORK/assert_body.py" "$WORK/sessionA.log" "$2" "$3" || RC=1
}

check_case "142000 bytes (report case; old impl: truncated at 113761)" \
    "http://$GUEST_HOST:$PORT/t.txt" "$WORK/www/t.txt"
check_case "115200 bytes (80-segment boundary, upper side; old impl: truncated)" \
    "http://$GUEST_HOST:$PORT/t115200.txt" "$WORK/www/t115200.txt"
check_case "113760 bytes (80-segment boundary, lower side)" \
    "http://$GUEST_HOST:$PORT/t113760.txt" "$WORK/www/t113760.txt"
check_case "42600 bytes (baseline control)" \
    "http://$GUEST_HOST:$PORT/t42600.txt" "$WORK/www/t42600.txt"
check_case "100000 bytes" \
    "http://$GUEST_HOST:$PORT/t100000.txt" "$WORK/www/t100000.txt"

while read -r hport hseg _hdecl hlabel; do
    case "$hlabel" in trunc|slow*) continue ;; esac
    check_case "segmentation independence: server chunks of $hseg bytes" \
        "http://$GUEST_HOST:$hport/t.txt" "$WORK/www/t.txt"
done <"$WORK/segports.txt"

# 慢速服务端用例（t4 finding requiredFix）：正文中途暂停 1000ms/2000ms，
# 均 >= RFC 6298 最小 RTO 1000ms，必须逐字节完整交付。旧实现约 640ms 的
# 空闲判定会在此报 tcp response truncated —— 这条是本回归的防线。
# 独立会话：142000 字节走串口本身就慢，与主用例挤在一个 QEMU 里会超出
# 单次运行的时间预算（会话 A 已含 10 条 142KB 级传输）。
{
    while read -r hport _hseg _hdecl hlabel; do
        case "$hlabel" in slow*) ;; *) continue ;; esac
        echo "wget -O - http://$GUEST_HOST:$hport/t.txt"
    done <"$WORK/segports.txt"
    echo "poweroff"   # 同上：确保最后一条慢速传输完整回显后再收尾
} >"$WORK/sessionSlow.script"

if [ -s "$WORK/sessionSlow.script" ]; then
    echo "[TCP-BULK] session A2: slow-server (mid-body pause) cases"
    guest_session "$WORK/sessionSlow.log" "$WORK/sessionSlow.script" ||
        fail "session A2 (slow server) QEMU run failed" "$WORK/sessionSlow.log"
    while read -r hport _hseg hdecl hlabel; do
        case "$hlabel" in slow*) ;; *) continue ;; esac
        echo "[TCP-BULK] slow server: ${hdecl}ms pause mid-body (must not be misjudged as truncation)"
        python3 "$WORK/assert_body.py" "$WORK/sessionSlow.log" \
            "http://$GUEST_HOST:$hport/t.txt" "$WORK/www/t42600.txt" || RC=1
    done <"$WORK/segports.txt"
fi

TRUNC_PORT="$(awk '$4=="trunc"{print $1}' "$WORK/segports.txt")"
echo "[TCP-BULK] truncation error path (declared Content-Length 142000, sent 100000)"
python3 "$WORK/assert_body.py" "$WORK/sessionA.log" \
    "http://$GUEST_HOST:$TRUNC_PORT/t.txt" "$WORK/www/t.txt" \
    "tcp response truncated" || RC=1

[ "$RC" -eq 0 ] || fail "session A byte-comparison failures" "$WORK/sessionA.log"

# ── 8. 会话 B/C：100000 字节落盘 + 重启回读（FAT32 持久化） ──────────
#     RAMFS_MAX_FILE_SIZE=131072；142000 超限 10928 字节 ⇒ 落盘用例只用
#     <= 131072 的文件（这里 100000），否则必然失败。
echo "[TCP-BULK] session B/C: 100000-byte persist + reboot readback"
DISK="$WORK/data.img"
# 盘要放得下 install auto 写进去的内核镜像（当前约 15 MB，压缩后）加上
# 落盘用例的 100000 B 文件；64M 在压缩前（内核 33 MB）会挤爆、让写入中途
# 失败并留下孤儿 LFN，故用 256M 留足余量。
qemu-img create -f raw "$DISK" 256M >/dev/null
DISK_ARGS=(-device ich9-ahci,id=ahci
           -drive "file=$DISK,format=raw,if=none,id=hd0"
           -device ide-hd,drive=hd0,bus=ahci.0)

cat >"$WORK/sessionB.script" <<EOF
install auto
wget -O big http://$GUEST_HOST:$PORT/t100000.txt
wc big
poweroff
EOF
guest_session "$WORK/sessionB.log" "$WORK/sessionB.script" "${DISK_ARGS[@]}" ||
    fail "session B (persist write) failed" "$WORK/sessionB.log"
grep -q "install: FAT32 partition ready" "$WORK/sessionB.log" ||
    fail "install auto did not report FAT32 partition ready" "$WORK/sessionB.log"
grep -q "saved big" "$WORK/sessionB.log" ||
    fail "guest wget -O big did not save" "$WORK/sessionB.log"
grep -q "100000  big" "$WORK/sessionB.log" ||
    fail "guest wc big did not report 100000 bytes" "$WORK/sessionB.log"

cat >"$WORK/sessionC.script" <<'EOF'
wc big
cat big
poweroff
EOF
guest_session "$WORK/sessionC.log" "$WORK/sessionC.script" "${DISK_ARGS[@]}" ||
    fail "session C (reboot readback) failed" "$WORK/sessionC.log"
grep -q "100000  big" "$WORK/sessionC.log" ||
    fail "persisted big file lost its size after reboot" "$WORK/sessionC.log"
if grep -q "not found" "$WORK/sessionC.log"; then
    fail "unexpected 'not found' during reboot readback" "$WORK/sessionC.log"
fi

echo "[TCP-BULK] reboot readback: cat big (100000 bytes) vs host fixture"
python3 "$WORK/assert_reboot.py" "$WORK/sessionC.log" "$WORK/www/t100000.txt" ||
    fail "reboot readback content mismatch" "$WORK/sessionC.log"

echo "[TCP-BULK] PASS (all cases byte-exact)"
