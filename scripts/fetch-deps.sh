#!/usr/bin/env bash
# 构建前置检查 / 补齐（issue #7：全新 clone 直接 make 失败）。
#
#   bash scripts/fetch-deps.sh          检查并补齐子模块，缺工具时给出安装命令
#   bash scripts/fetch-deps.sh --check  只检查，不拉取任何东西
#
# 做三件事：
#   1. 递归初始化子模块（limine-bin / HPT / mbedtls / lwip / HIVE / HAX-Compile），
#      包含 mbedtls 的嵌套子模块（framework / tf-psa-crypto / mldsa-native）；
#   2. 确认 HAX-Compile 里真的有 mk/hax-apps.mk 与 tools/genhax.py；
#   3. 检查宿主机工具链，缺什么就打印对应发行版的安装命令。
#
# 不联网、不装包、不 sudo —— 只读检查 + git 拉取，安装动作交给用户。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CHECK_ONLY=0
case "${1:-}" in
    --check|-c) CHECK_ONLY=1 ;;
    -h|--help)  sed -n '2,14p' "$0"; exit 0 ;;
    "") ;;
    *) echo "未知参数：$1（用 --check 只检查）" >&2; exit 2 ;;
esac

# 部分发行版的 git 子脚本靠 PATH 找 git-sh-setup，exec-path 不一定在里面。
if GIT_EXEC_PATH="$(git --exec-path 2>/dev/null)"; then
    case ":$PATH:" in
        *":$GIT_EXEC_PATH:"*) ;;
        *) PATH="$GIT_EXEC_PATH:$PATH"; export PATH ;;
    esac
fi

info() { printf '\033[36m==>\033[0m %s\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }

MISSING_TOOLS=()
MISSING_PKGS=()
MISSING_PY=()

need_tool() {  # need_tool <可执行文件> <apt 包名>
    if command -v "$1" >/dev/null 2>&1; then
        return 0
    fi
    MISSING_TOOLS+=("$1")
    MISSING_PKGS+=("$2")
    return 1
}

# ── 1. 子模块 ────────────────────────────────────────────────────────────
info "检查子模块"
if [[ $CHECK_ONLY -eq 1 ]]; then
    for d in limine-bin HPT HIVE HAX-Compile third_party/mbedtls third_party/lwip; do
        if [[ -e "$d/.git" ]]; then ok "$d"; else warn "$d 未初始化（运行本脚本不加参数即可补齐）"; fi
    done
else
    git submodule update --init --recursive ||
        { echo "子模块拉取失败：先把网络/代理配好，再重试。" >&2; exit 1; }
    ok "git submodule update --init --recursive"
fi

# ── 2. HAX-Compile（.hax 应用打包器；缺了 make 会硬失败）──────────────────
info "检查 HAX-Compile"
HAX_DIR=""
for candidate in HAX-Compile ../HAX-Compile; do
    if [[ -f "$candidate/mk/hax-apps.mk" && -f "$candidate/tools/genhax.py" ]]; then
        HAX_DIR="$candidate"
        break
    fi
done
if [[ -n "$HAX_DIR" ]]; then
    ok "$HAX_DIR/mk/hax-apps.mk + tools/genhax.py"
elif [[ $CHECK_ONLY -eq 1 ]]; then
    warn "没找到 HAX-Compile（make 会失败；不加参数运行本脚本可自动补齐）"
else
    warn "HAX-Compile 不完整，从 GitHub 克隆到 HAX-Compile/"
    rm -rf HAX-Compile.tmp
    git clone --depth 1 https://github.com/HeBitOS/HAX-Compile.git HAX-Compile.tmp
    mv HAX-Compile.tmp HAX-Compile
    [[ -f HAX-Compile/mk/hax-apps.mk ]] || {
        echo "克隆下来的 HAX-Compile 仍缺 mk/hax-apps.mk —— 仓库内容不对，请报 issue。" >&2
        exit 1
    }
    ok "HAX-Compile 已就位（$HAX_DIR）"
fi

# ── 3. mbedtls 的嵌套子模块 ──────────────────────────────────────────────
info "检查 mbedtls 嵌套子模块"
MBEDED=1
for f in third_party/mbedtls/include/mbedtls/ssl.h \
         third_party/mbedtls/framework/data_files \
         third_party/mbedtls/tf-psa-crypto/core \
         third_party/mbedtls/tf-psa-crypto/drivers/pqcp/mldsa-native; do
    [[ -e "$f" ]] || { MBEDED=0; break; }
done
if [[ $MBEDED -eq 1 ]]; then
    ok "framework / tf-psa-crypto / mldsa-native"
elif [[ $CHECK_ONLY -eq 1 ]]; then
    warn "mbedtls 嵌套子模块不全（SSL 头会找不到）"
else
    git -C third_party/mbedtls submodule update --init --recursive
    ok "已递归拉取 mbedtls 子模块"
fi

# ── 4. 宿主机工具链 ──────────────────────────────────────────────────────
info "检查宿主机工具链"
need_tool gcc        build-essential   && ok "gcc"
need_tool ld         binutils          && ok "ld"
need_tool make       make              && ok "make"
need_tool nasm       nasm              && ok "nasm"
need_tool xorriso    xorriso           && ok "xorriso"
need_tool mformat    mtools            && ok "mtools"
need_tool mkfs.fat   dosfstools        && ok "dosfstools"
need_tool grub-mkrescue grub-pc-bin    && ok "grub-mkrescue"
need_tool cmake      cmake             && ok "cmake"
need_tool autoconf   autoconf          && ok "autoconf"
need_tool automake   automake          && ok "automake"
need_tool pkg-config pkg-config        && ok "pkg-config"
need_tool python3    python3           && ok "python3"
need_tool qemu-system-x86_64 qemu-system-x86 && ok "qemu-system-x86_64"

# limine 需要 llvm-objcopy（版本后缀也算）
if command -v llvm-objcopy >/dev/null 2>&1 || ls /usr/bin/llvm-objcopy-* >/dev/null 2>&1; then
    ok "llvm-objcopy"
else
    MISSING_TOOLS+=("llvm-objcopy")
    MISSING_PKGS+=("llvm")
fi

# python 侧（mbedtls 的 CMake 生成脚本要用）
for mod in PIL jsonschema jinja2 yaml; do
    if python3 -c "import $mod" >/dev/null 2>&1; then
        ok "python3 -m $mod"
    else
        MISSING_PY+=("$mod")
    fi
done

if [[ ${#MISSING_PY[@]} -gt 0 ]]; then
    case " ${MISSING_PY[*]} " in
        *" PIL "*) MISSING_PKGS+=("python3-pil") ;;
    esac
    case " ${MISSING_PY[*]} " in
        *" jsonschema "*) MISSING_PKGS+=("python3-jsonschema") ;;
    esac
    case " ${MISSING_PY[*]} " in
        *" jinja2 "*) MISSING_PKGS+=("python3-jinja2") ;;
    esac
    case " ${MISSING_PY[*]} " in
        *" yaml "*) MISSING_PKGS+=("python3-yaml") ;;
    esac
fi

# ── 5. 结论 ──────────────────────────────────────────────────────────────
if [[ ${#MISSING_TOOLS[@]} -eq 0 && ${#MISSING_PY[@]} -eq 0 ]]; then
    info "依赖齐全，可以 make"
    exit 0
fi

echo
warn "缺少：${MISSING_TOOLS[*]:-}${MISSING_TOOLS[*]:+ }${MISSING_PY[*]:-}"
# 去重后给出安装命令
PKGS="$(printf '%s\n' "${MISSING_PKGS[@]}" | awk '!seen[$0]++' | tr '\n' ' ')"
cat <<EOF

安装（Ubuntu / Debian）：
  sudo apt update && sudo apt install -y ${PKGS}

安装（Arch）：
  sudo pacman -S --needed $(printf '%s\n' "$PKGS" | sed 's/python3-pil/python-pillow/;s/python3-jsonschema/python-jsonschema/;s/python3-jinja2/python-jinja/;s/python3-yaml/python-yaml/;s/build-essential/base-devel/;s/grub-pc-bin/grub/;s/python3/python/;s/qemu-system-x86/qemu-system-x86/')

也可以直接跑 sudo ./install.sh（自动识别发行版）。
EOF
exit 1
