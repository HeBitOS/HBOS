#!/bin/sh
# HBOS 构建依赖安装脚本。
#
#   sudo ./install.sh          安装构建依赖（自动识别 apt / pacman / dnf）
#   ./install.sh --check       只检查缺什么，不装任何东西
#   ./install.sh --yes         不询问，直接装
#
# 装完之后再跑一次 `make deps`，把子模块（含 mbedtls 嵌套子模块）拉齐。
#
# 注意：本脚本只装构建 HBOS 需要的主机工具；Windows 请用 WSL2 + Ubuntu 再执行。
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
YES=0
CHECK=0
for arg in "$@"; do
    case "$arg" in
        --check|-c) CHECK=1 ;;
        --yes|-y)   YES=1 ;;
        -h|--help)  sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

if [ "$CHECK" -eq 1 ]; then
    exec bash "$ROOT/scripts/fetch-deps.sh" --check
fi

if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_ID=${ID:-unknown}
    DISTRO_LIKE=${ID_LIKE:-}
else
    DISTRO_ID=unknown
    DISTRO_LIKE=
fi

case " $DISTRO_ID $DISTRO_LIKE " in
    *debian*|*ubuntu*) PM=apt ;;
    *arch*|*manjaro*)  PM=pacman ;;
    *fedora*|*rhel*|*centos*) PM=dnf ;;
    *) PM=unknown ;;
esac

APT_PKGS="build-essential binutils nasm grub-pc-bin grub-efi-amd64-bin grub-common \
xorriso mtools dosfstools qemu-system-x86 qemu-utils ovmf \
python3 python3-pil python3-jsonschema python3-jinja2 python3-yaml \
cmake llvm autoconf automake pkg-config git curl"

PACMAN_PKGS="base-devel nasm grub xorriso mtools dosfstools \
qemu-system-x86 qemu-img edk2-ovmf \
python python-pillow python-jsonschema python-jinja python-yaml \
cmake llvm autoconf automake pkgconf git curl"

DNF_PKGS="gcc gcc-c++ make binutils nasm grub2-tools grub2-pc-modules \
xorriso mtools dosfstools qemu-system-x86 qemu-img edk2-ovmf \
python3 python3-pillow python3-jsonschema python3-jinja2 python3-pyyaml \
cmake llvm autoconf automake pkgconf git curl"

confirm() {
    [ "$YES" -eq 1 ] && return 0
    printf '继续？[y/N] '
    read -r ans
    case "$ans" in y|Y|yes|YES) return 0 ;; *) echo "已取消"; return 1 ;; esac
}

case "$PM" in
    apt)
        echo "==> 发行版：${PRETTY_NAME:-Debian/Ubuntu}（apt）"
        echo "    将要安装：$APT_PKGS"
        confirm || exit 1
        SUDO=""
        [ "$(id -u)" -eq 0 ] || SUDO="sudo"
        $SUDO apt-get update
        # shellcheck disable=SC2086
        $SUDO apt-get install -y $APT_PKGS
        ;;
    pacman)
        echo "==> 发行版：${PRETTY_NAME:-Arch}（pacman）"
        echo "    将要安装：$PACMAN_PKGS"
        confirm || exit 1
        SUDO=""
        [ "$(id -u)" -eq 0 ] || SUDO="sudo"
        # shellcheck disable=SC2086
        $SUDO pacman -Sy --needed --noconfirm $PACMAN_PKGS
        ;;
    dnf)
        echo "==> 发行版：${PRETTY_NAME:-Fedora}（dnf）"
        echo "    将要安装：$DNF_PKGS"
        confirm || exit 1
        SUDO=""
        [ "$(id -u)" -eq 0 ] || SUDO="sudo"
        # shellcheck disable=SC2086
        $SUDO dnf install -y $DNF_PKGS
        ;;
    *)
        echo "未识别的发行版（${DISTRO_ID:-unknown}）。请手动安装：" >&2
        echo "  gcc binutils make nasm grub xorriso mtools dosfstools" >&2
        echo "  qemu-system-x86 ovmf python3 python3-pil python3-jsonschema" >&2
        echo "  python3-jinja2 python3-yaml cmake llvm autoconf automake pkg-config git" >&2
        exit 1
        ;;
esac

echo
echo "==> 依赖安装完成。接下来："
echo "    make deps    # 拉子模块并复核工具链"
echo "    make         # 构建 BIOS + UEFI ISO"
