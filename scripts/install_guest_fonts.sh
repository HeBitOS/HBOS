#!/usr/bin/env bash
# 把 fontconfig 配置和一份 TTF 装进测试镜像。
#
# Chromium 的文字光栅化依赖系统字体：没有 /etc/fonts/fonts.conf 时它会打印
# "Fontconfig error: Cannot load default config file"，canvas 的 fillText
# 也就画不出任何字形（实测深色像素为 0）。装上之后同一个页面能画出 3355 个
# 字形像素。
#
# 用法：scripts/install_guest_fonts.sh [镜像路径] [字体路径]
set -euo pipefail

IMG="${1:-build/c3-chromium.img}"
FONT="${2:-/usr/share/fonts/TTF/DejaVuSans.ttf}"
OFF_IMG="${IMG}?offset=1048576"

[ -f "$FONT" ] || { echo "找不到字体: $FONT" >&2; exit 1; }

tmp_conf="$(mktemp)"
trap 'rm -f "$tmp_conf"' EXIT
cat > "$tmp_conf" <<'CONF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir>/usr/share/fonts</dir>
  <dir>/usr/local/share/fonts</dir>
  <cachedir>/tmp/fontconfig-cache</cachedir>
</fontconfig>
CONF

for dir in /usr/share /usr/share/fonts /usr/share/fonts/truetype \
           /usr/share/fonts/truetype/dejavu /etc/fonts; do
    debugfs -w -R "mkdir $dir" "$OFF_IMG" >/dev/null 2>&1 || true
done
debugfs -w -R "rm /etc/fonts/fonts.conf" "$OFF_IMG" >/dev/null 2>&1 || true
debugfs -w -R "write $tmp_conf /etc/fonts/fonts.conf" "$OFF_IMG" >/dev/null
debugfs -w -R "rm /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" "$OFF_IMG" >/dev/null 2>&1 || true
debugfs -w -R "write $FONT /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" "$OFF_IMG" >/dev/null
echo "已安装 fontconfig 配置与 $(basename "$FONT") 到 $IMG"
