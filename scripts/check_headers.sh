#!/usr/bin/env bash
# 头文件注释模板检查（docs/wiki/CODE_STYLE.md §3.1）。
#
# 检查三件事，各自独立计错：
#   1) 缺模板头     —— 前 40 行内找不到 @file / @layer / @owner / @stable
#   2) 缺索引       —— 有 @stable 但没有 "索引头文件" 段
#   3) 索引对不上   —— 索引里列的名字，文件里根本不存在（拼写错误/已删除）
#
# 只报告，不做"是否列全"的反向检查：那需要 C 语法分析，而且短头文件允许把
# 索引并进 @brief（§3.1.1 最后一条），硬性比对会误报。这一项留给 review。
#
# 豁免分两级：
#   · 完全不查 —— third_party 上游头（改了会和上游 merge 冲突）
#   · 只查索引不查模板头（LIBCOMPAT 白名单）—— ln / wd / init / dlfcn 这些
#     是 Linux ABI 兼容头，是**给别人看的契约**（与 glibc 的对应关系），套
#     内核的 @layer 模板反而误导：它们的层次由 ABI 决定，不由我们分层决定。
#     但这不意味着可以不写索引——它们恰恰最需要"导出什么"的清单。
set -u
cd "$(dirname "$0")/.."

# Linux ABI 兼容头：只查索引。
LIBCOMPAT=' src/user/libc/sys/ln.h src/user/libc/sys/wd.h src/user/libc/init.h
            src/user/libc/dlfcn.h src/user/libc/link.h '

missing_header=0
missing_index=0
bad_index=0

for f in $(find src -name '*.h' | sort); do
    case "$f" in
        *third_party*|*lwip*|*mbedtls*|*/arch/*) continue ;;
    esac

    head40=$(head -40 "$f")
    is_libcompat=0
    case "$LIBCOMPAT" in
        *" $f "*) is_libcompat=1 ;;
    esac

    # 1) 模板头
    if [ "$is_libcompat" -eq 0 ] &&
       { ! printf '%s' "$head40" | grep -q '@layer' ||
         ! printf '%s' "$head40" | grep -q '@owner' ||
         ! printf '%s' "$head40" | grep -q '@stable'; }; then
        echo "✗ 缺模板头      $f"
        missing_header=$((missing_header + 1))
        continue
    fi

    # 2) 索引段（"索引头文件" 出现在注释里即可，位置限定在前 40 行）
    if ! printf '%s' "$head40" | grep -q '索引头文件'; then
        echo "✗ 缺索引        $f"
        missing_index=$((missing_index + 1))
        continue
    fi

    # 3) 索引里点名的标识符必须真的存在。索引行形如：
    #      *   类型  foo_t   一句话职责
    #    取每行前两个字段里的第二个（类型/宏/函数 标签之后那个名字）。
    names=$(head -40 "$f" \
        | sed -n '/索引头文件/,/\*\//p' \
        | grep -E '^\s*\*\s+(类型|宏|函数|常量|枚举)\s' \
        | awk '{print $3}' \
        | grep -E '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u)
    for n in $names; do
        if ! grep -q "\b$n\b" "$f"; then
            echo "✗ 索引对不上    $f: 索引里有 '$n'，文件里找不到"
            bad_index=$((bad_index + 1))
        fi
    done
done

total=$((missing_header + missing_index + bad_index))
if [ "$total" -eq 0 ]; then
    echo "✓ 头文件注释模板与索引全部合规"
    exit 0
fi

echo
echo "共 $total 项：缺模板头 $missing_header / 缺索引 $missing_index / 索引对不上 $bad_index"
echo "规范见 docs/wiki/CODE_STYLE.md §3.1；只查合规不查是否列全（列全留给 review）。"
exit 1
