#!/usr/bin/env bash
# 禁止在主题层之外硬编码颜色。
#
# 背景：重构前有 472 处硬编码颜色，导致加一个主题要改几百处、深浅色不一致。
# 收敛到 src/ui/theme.c 的令牌表后，这条断言防止有人（或未来的我）再退化回去。
#
# 例外（有意豁免）：
#   - src/ui/theme.c     令牌定义本身
#   - src/lib/json.c     颜色解析（只做数值转换）
#   - 注释行             举例说明时提到的颜色不算硬编码
#   - 字符串字面量内     cc.c 里的示例源码（教用户画图时用 rgb()）
set -u
cd "$(dirname "$0")/.."

fail=0

# 1) rgb(a,b,c) / gui_rgb(a,b,c) 字面量调用
#    排除：注释行、字符串里的示例代码、主题与解析器自身
hits=$(grep -rnE '\b(gui_rgb|rgb)\([[:space:]]*[0-9]+,[[:space:]]*[0-9]+,[[:space:]]*[0-9]+[[:space:]]*\)' \
    src/ --include='*.c' --include='*.h' 2>/dev/null \
    | grep -vE '^src/ui/theme\.c:|^src/lib/json\.c:' \
    | grep -vE ':[[:space:]]*(\*|//|/\*)' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*"' || true)
if [ -n "$hits" ]; then
    echo "✗ 发现硬编码颜色（请改用 ui_tokens.h 的令牌）："
    echo "$hits" | head -20
    fail=1
fi

# 2) 字面量颜色常量（如 0xFF3DAEE9）。
#    难点：8 位十六进制同样用于位掩码与寄存器值（0x00000005 之类），直接匹配
#    会大量误报。因此只在"明确属于调色"的范围内检查——GUI 与图形层——并按
#    "非全 0/全 F 且不是位移组合"过滤。渲染后端（fb.c/graphics.c）与效果层
#    的 0xFFFFFFFF 是"默认填充色"这类通用值，属于合理豁免。
hits2=$(grep -rnE '0x[0-9A-Fa-f]{8}' src/tools src/gui src/ui --include='*.c' 2>/dev/null \
    | grep -vE '^src/ui/theme\.c:|^src/lib/json\.c:' \
    | grep -vE '0x(00000000|FFFFFFFF|ffffffff|FF000000|00FFFFFF)' \
    | grep -vE '<<|>>|\||MASK|MAGIC|mask|magic|KEY|TYPE|CMD|SIG|ID|cpuid|CPUID|leaf|MSR|msr' \
    | grep -E '0x(F[0-9A-Fa-f]|E[0-9A-Fa-f]|D[0-9A-Fa-f]|C[0-9A-Fa-f]|B[0-9A-Fa-f]|A[0-9A-Fa-f]|[0-9][0-9A-Fa-f])[0-9A-Fa-f]{6}' \
    | grep -vE '[<>]=?|[=!]=|\(0x|, *0x' || true)
if [ -n "$hits2" ]; then
    echo "✗ 发现疑似硬编码颜色常量："
    echo "$hits2" | head -20
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "✓ 未发现主题层之外的硬编码颜色"
fi
exit $fail
