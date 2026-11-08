#!/usr/bin/env bash
# 一次 QEMU 会话里跑多种 Chromium flag 组合，省掉多轮 4~6 分钟的等待。
#
# 为什么需要：每改一处内核代码，都要重新起一次 QEMU（TCG 无 KVM，冷启动
# 就要几十秒）。而"哪种 flag 组合能让 --dump-dom 出 HTML"本身就是一个
# 多维问题。逐个试 = 每轮几分钟；串在一个会话里 = 一轮几分钟试完全部。
#
# 用法：
#   bash scripts/headless_matrix.sh [--iso build/hbos-bios.iso] [--disk build/c3-chromium.img]
#
# 输出：build/tmp/matrix-<时间戳>.log，以及每个组合的判定摘要。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="${ROOT}/build/hbos-bios.iso"
DISK="${ROOT}/build/c3-chromium.img"
TMP="${ROOT}/build/tmp"
SHELL_BIN="/opt/chromium-headless-shell/usr/lib/chromium/chromium-headless-shell"

while [ $# -gt 0 ]; do
    case "$1" in
        --iso)  ISO="$2"; shift 2 ;;
        --disk) DISK="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$TMP"
STAMP="$(date +%H%M%S)"
LOG="${TMP}/matrix-${STAMP}.log"

# 每个组合一行：标签|额外的 chromium 参数
# 顺序有意义：越靠前的越可能是"正常路径"，越靠后越是"强制降级"。
COMBOS=(
  "baseline|--disable-gpu --disable-gpu-sandbox --disable-dev-shm-usage"
  "nogpu-compositing|--disable-gpu --disable-gpu-compositing --disable-software-rasterizer --disable-dev-shm-usage"
  "in-process-gpu|--in-process-gpu --ozone-platform=headless --disable-dev-shm-usage"
  "single-process|--single-process --disable-gpu --disable-dev-shm-usage"
  "swiftshader|--use-gl=swiftshader --ozone-platform=headless --disable-dev-shm-usage"
)

echo "[matrix] ISO=$ISO DISK=$DISK"
echo "[matrix] log=$LOG"
echo "[matrix] 组合数=${#COMBOS[@]}"

# 先把命令序列拼出来：每个组合前打一行分隔标记，便于事后切分日志。
CMDS=()
CMDS+=("setenv stderr 1")
CMDS+=("setenv syscdump 0")   # 关掉 syscall 环形 dump，省串口带宽
for entry in "${COMBOS[@]}"; do
    label="${entry%%|*}"
    flags="${entry#*|}"
    CMDS+=("echo ===MATRIX-BEGIN:${label}===")
    CMDS+=("run ${SHELL_BIN} --no-sandbox --headless ${flags} --dump-dom about:blank")
    CMDS+=("echo ===MATRIX-END:${label}===")
done

python3 "${ROOT}/scripts/qemu_session.py" "$ISO" \
    --disk "$DISK" --mem 2048M \
    --boot-timeout 120 --timeout 150 \
    --log "$LOG" \
    -- "${CMDS[@]}" >/dev/null 2>&1

echo
echo "[matrix] ===== 结果摘要 ====="
for entry in "${COMBOS[@]}"; do
    label="${entry%%|*}"
    # 截取该组合的日志段
    seg="$(awk -v s="===MATRIX-BEGIN:${label}===" -v e="===MATRIX-END:${label}===" \
             'index($0,s){on=1;next} index($0,e){on=0} on' "$LOG" 2>/dev/null)"
    html=$(printf '%s' "$seg" | grep -ciE '<html|<!DOCTYPE' || true)
    gp=$(printf '%s' "$seg" | grep -c 'General Protection' || true)
    segv=$(printf '%s' "$seg" | grep -c 'SIGSEGV' || true)
    b127=$(printf '%s' "$seg" | grep -c 'code=127' || true)
    noch=$(printf '%s' "$seg" | grep -c 'no connection' || true)
    printf '  %-20s HTML=%-3s GP=%-3s SEGV=%-3s 127=%-3s noconn=%s\n' \
           "$label" "$html" "$gp" "$segv" "$b127" "$noch"
done

echo
echo "[matrix] 完整日志: $LOG"
echo "[matrix] 提示: 任何一行 HTML>0 即说明该组合打通了 --dump-dom"
