#!/usr/bin/env bash
# 校验 src/hbos_all.h 的三支各自都能编译，且覆盖 src/ 下全部头文件。
#
# 这个聚合头是给编辑器做索引用的（Ctrl+Click 跳转）。它能编过 ≠ 它有用，
# 但它编不过时 **一定** 没用——编辑器会把满屏符号标红，跳转也全是错的。
# 所以每次改头文件之后跑一遍，比等到编辑器里发现更省事。
#
# 三支的编译参数必须各自独立：它们本来就是三个不同的编译环境，共用一套
# -I 会互相污染（典型症状：用户态支里冒出内核 sys/types.h 的 useconds_t）。
set -u
cd "$(dirname "$0")/.."

HDR=src/hbos_all.h
FAIL=0

if [ ! -f "$HDR" ]; then
    echo "✗ $HDR 不存在，先跑 make all-headers"
    exit 1
fi

run_case() {
    name=$1
    expect=$2
    shift 2
    if gcc -fsyntax-only -x c -I. "$@" "$HDR" 2>/tmp/hbos-allh-err.txt; then
        n=$(grep -cE '^#include "' "$HDR" || true)
        echo "✓ $name"
        return 0
    fi
    echo "✗ $name"
    grep -E 'error:' /tmp/hbos-allh-err.txt | head -8 | sed 's/^/    /'
    FAIL=$((FAIL + 1))
    return 1
}

# 内核侧（默认支）
run_case "内核侧" 134 \
    -Isrc -Isrc/graphics -Isrc/shell -Isrc/core -Isrc/tools \
    -Isrc/gui -Isrc/user -Isrc/net \
    -DHBOS_ENABLE_GUI=1

# 用户态 libc 支：绝不能带 -Isrc，否则会把内核 sys/ 拉进来
run_case "用户态 libc" 38 \
    -Isrc/user/libc -Isrc/user/libc/sys -Isrc/user \
    -DHBOS_ALL_WITH_USER=1

# 安全网络配置支：必须带 lwip/mbedtls 的第三方路径和配置宏
run_case "安全网络配置" 7 \
    -Isrc -Isrc/net \
    -Ithird_party/lwip/src/include \
    -Ithird_party/mbedtls/include \
    -Ithird_party/mbedtls/tf-psa-crypto/include \
    -Ithird_party/mbedtls/tf-psa-crypto/drivers/builtin/include \
    -Isrc/net/mbedtls_shim \
    -DMBEDTLS_CONFIG_FILE='"net/mbedtls_config.h"' \
    -DTF_PSA_CRYPTO_CONFIG_FILE='"net/psa_crypto_config.h"' \
    -DHBOS_ALL_WITH_NET=1

# 覆盖率：src/ 下每个 .h 都必须在聚合头里出现
total=$(find src -name '*.h' ! -name 'hbos_all.h' | wc -l)
covered=$(find src -name '*.h' ! -name 'hbos_all.h' \
    | while read -r f; do
          rel=${f#src/}
          grep -qF "\"$rel\"" "$HDR" && echo x
      done | wc -l)
if [ "$total" -eq "$covered" ]; then
    echo "✓ 覆盖 $covered/$total 个头文件"
else
    echo "✗ 覆盖 $covered/$total，缺："
    find src -name '*.h' ! -name 'hbos_all.h' | while read -r f; do
        rel=${f#src/}
        grep -qF "\"$rel\"" "$HDR" || echo "    $f"
    done | head -10
    FAIL=$((FAIL + 1))
fi

if [ "$FAIL" -eq 0 ]; then
    echo "✓ hbos_all.h 三支全部可用"
    exit 0
fi
echo
echo "共 $FAIL 项失败。重新生成： make all-headers"
exit 1
