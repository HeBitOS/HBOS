#!/usr/bin/env bash
set -euo pipefail

HBOS_REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HBOS_VERSION_FILE="$HBOS_REPO_DIR/browser/chromium/VERSION"
HBOS_DEFAULT_WORKDIR="/run/media/linpinf/D8663B3A663B18A8/hbos-browser-work"
HBOS_BROWSER_WORKDIR="${HBOS_CHROMIUM_WORKDIR:-$HBOS_DEFAULT_WORKDIR}"
HBOS_DEPOT_TOOLS="$HBOS_BROWSER_WORKDIR/depot_tools"
HBOS_CHECKOUT_ROOT="$HBOS_BROWSER_WORKDIR/chromium"
HBOS_CHROMIUM_SRC="$HBOS_CHECKOUT_ROOT/src"

value() {
    sed -n "s/^$1=//p" "$HBOS_VERSION_FILE"
}

HBOS_CHROMIUM_REVISION="$(value revision)"
HBOS_CHROMIUM_VERSION="$(value version)"

usage() {
    echo "Usage: $0 {status|bootstrap|pin|hooks|host-gen|host-build}"
}

need_depot_tools() {
    if [[ ! -x "$HBOS_DEPOT_TOOLS/gclient" ]]; then
        echo "missing depot_tools: $HBOS_DEPOT_TOOLS" >&2
        exit 1
    fi
    if [[ -x "$HBOS_BROWSER_WORKDIR/bin/vpython3" ]]; then
        export PATH="$HBOS_BROWSER_WORKDIR/bin:$HBOS_DEPOT_TOOLS:$PATH"
    else
        export PATH="$HBOS_DEPOT_TOOLS:$PATH"
    fi
    export DEPOT_TOOLS_UPDATE=0
}

gclient_run() {
    if [[ -x "$HBOS_BROWSER_WORKDIR/bin/vpython3" ]]; then
        "$HBOS_BROWSER_WORKDIR/bin/vpython3" \
            "$HBOS_DEPOT_TOOLS/gclient.py" "$@"
    else
        gclient "$@"
    fi
}

fetch_run() {
    if [[ -x "$HBOS_BROWSER_WORKDIR/bin/vpython3" ]]; then
        "$HBOS_BROWSER_WORKDIR/bin/vpython3" \
            "$HBOS_DEPOT_TOOLS/fetch.py" "$@"
    else
        fetch "$@"
    fi
}

case "${1:-status}" in
    status)
        echo "workdir=$HBOS_BROWSER_WORKDIR"
        echo "version=$HBOS_CHROMIUM_VERSION"
        echo "expected_revision=$HBOS_CHROMIUM_REVISION"
        if [[ -d "$HBOS_CHROMIUM_SRC/.git" ]]; then
            echo "actual_revision=$(git -C "$HBOS_CHROMIUM_SRC" rev-parse HEAD)"
            git -C "$HBOS_CHROMIUM_SRC" status --short --branch
        else
            echo "checkout=missing"
        fi
        ;;
    bootstrap)
        mkdir -p "$HBOS_BROWSER_WORKDIR"
        if [[ ! -d "$HBOS_DEPOT_TOOLS/.git" ]]; then
            git clone --depth=1 \
                https://chromium.googlesource.com/chromium/tools/depot_tools.git \
                "$HBOS_DEPOT_TOOLS"
        fi
        need_depot_tools
        mkdir -p "$HBOS_CHECKOUT_ROOT"
        if [[ ! -d "$HBOS_CHROMIUM_SRC/.git" ]]; then
            (cd "$HBOS_CHECKOUT_ROOT" &&
                fetch_run --nohooks --no-history chromium)
        fi
        ;;
    pin)
        need_depot_tools
        [[ -d "$HBOS_CHROMIUM_SRC/.git" ]] || {
            echo "run '$0 bootstrap' first" >&2
            exit 1
        }
        git -C "$HBOS_CHROMIUM_SRC" fetch --depth=1 origin \
            "$HBOS_CHROMIUM_REVISION"
        git -C "$HBOS_CHROMIUM_SRC" checkout --detach \
            "$HBOS_CHROMIUM_REVISION"
        (cd "$HBOS_CHECKOUT_ROOT" &&
            gclient_run sync --no-history \
                --revision "src@$HBOS_CHROMIUM_REVISION")
        ;;
    hooks)
        need_depot_tools
        (cd "$HBOS_CHECKOUT_ROOT" && gclient_run runhooks)
        ;;
    host-gen)
        need_depot_tools
        [[ -d "$HBOS_CHROMIUM_SRC" ]] || {
            echo "Chromium source is missing" >&2
            exit 1
        }
        HBOS_GN="$HBOS_CHROMIUM_SRC/buildtools/linux64/gn"
        [[ -x "$HBOS_GN" ]] || {
            echo "GN binary is missing; run '$0 hooks' first" >&2
            exit 1
        }
        mkdir -p "$HBOS_CHROMIUM_SRC/out/hbos-host"
        cp "$HBOS_REPO_DIR/browser/chromium/args.host.gn" \
            "$HBOS_CHROMIUM_SRC/out/hbos-host/args.gn"
        (cd "$HBOS_CHROMIUM_SRC" &&
            "$HBOS_GN" gen out/hbos-host)
        ;;
    host-build)
        HBOS_NINJA="$HBOS_CHROMIUM_SRC/third_party/ninja/ninja"
        [[ -x "$HBOS_NINJA" && -f "$HBOS_CHROMIUM_SRC/out/hbos-host/build.ninja" ]] || {
            echo "host output is missing; run '$0 host-gen' first" >&2
            exit 1
        }
        "$HBOS_NINJA" -C "$HBOS_CHROMIUM_SRC/out/hbos-host" \
            -j "${HBOS_CHROMIUM_JOBS:-4}" content_shell
        ;;
    *)
        usage
        exit 2
        ;;
esac
