#!/bin/sh
# 编译宿主侧→客机的 Wayland 测试程序。
#
# 输出名**去掉 linux_ 前缀**：源文件叫 linux_wayland_foo.c，但客机里装
# 的是 /bin/wayland_foo，两边名字必须一致。之前这里直接取 basename，
# 生成的是 build/tests/linux_wayland_foo，而安装脚本一直在装
# build/tests/wayland_foo（上一次手工编的旧文件）—— 于是出现"代码改了
# 但行为没变"，还被误判成"文件系统覆盖失败"，白白绕了一大圈。
#
# 用法：sh scripts/build_wl_tests.sh [源目录] [输出目录]

set -e

CC=${CC:-gcc}
SRCDIR="${1:-tests}"
OUTDIR="${2:-build/tests}"

mkdir -p "$OUTDIR"

CFLAGS="-O2 -fPIE -pie"
LDFLAGS="-Wl,-z,max-page-size=0x1000 -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2"

# 源文件和客机路径的对应关系写死在这里，避免"编出来的名字"和"装进去的
# 名字"再次对不上。
for pair in \
    "linux_wayland_min:wayland_min" \
    "linux_wayland_align:wayland_align" \
    "linux_wayland_big:wayland_big" \
    "linux_wayland_client:wayland_client" \
    "linux_wayland_surface:wayland_surface"; do
    src_name=${pair%%:*}
    out_name=${pair##*:}
    src="$SRCDIR/$src_name.c"
    [ -f "$src" ] || continue
    echo "[wl-test] $out_name"
    rm -f "$OUTDIR/$out_name"
    $CC $CFLAGS $LDFLAGS -o "$OUTDIR/$out_name" "$src"
done

echo "[wl-test] done -> $OUTDIR"
ls -l "$OUTDIR" | grep 'wayland_' || true

