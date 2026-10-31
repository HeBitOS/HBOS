#!/bin/sh
# 幂等修复 chromium-headless-shell 的资源布局(见 fix_headless_layout.py 的说明)。
# 用脚本名调用,是为了让"该在的文件装完之后"这一步有一个固定的入口:
#   scripts/fix_headless_layout.sh [--img build/c3-chromium.img] [--dry-run]
set -e
exec python3 "$(dirname "$0")/fix_headless_layout.py" "$@"
