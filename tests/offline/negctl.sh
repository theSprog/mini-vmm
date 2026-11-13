#!/usr/bin/env bash
# tests/offline/negctl.sh — negative control：证明单元测试有能力报警
#
# 一套断言全绿，只说明它在当前实现上没报警，不说明它有能力报警。
# 这里把被测代码换成故意写错的版本，再跑同一套用例，**要求每一个 mutant 都让测试非零退出**。
#
# 判据反过来：退出码为 0 才是 FAIL —— 那意味着这类错误能溜过去。
#
# 两类 mutant：
#   接口层  tests/unit/{mem,io_bus}_mutants.c 整份替换对外函数，只依赖函数
#           签名，src/ 内部怎么重构都不影响，详见 tests/unit/mutants.h。
#   源码层  tests/mutants/<里程碑>.py，由 tests/mutate.py 在源码副本里替换
#           一段文字后重新构建单元测试。用于错误点在 static 函数或一条条件
#           表达式里、换不掉整个对外函数的情形。按文字定位而不按行号，源码
#           改动让那段文字不再唯一出现时报 STALE，同样算 FAIL。
#
# 用法：./tests/offline/negctl.sh

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

BIN_DIR="$ROOT/build/tests"
TARGETS="mem_test io_bus_test"

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:print_stacktrace=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"

auto_tests() {

head1 "negative control — 每个 mutant 都必须被抓住"

local t n i line name breaks out rc
for t in $TARGETS; do
    if [ ! -x "$BIN_DIR/$t" ]; then
        skip_case "$t mutants" "未构建，先跑 make test"
        continue
    fi

    n="$("$BIN_DIR/$t" --list-mutants | head -1)"
    log ""
    log "${C_BLD}$t：$n 个 mutant${C_RST}"

    i=0
    while [ "$i" -lt "$n" ]; do
        line="$("$BIN_DIR/$t" --list-mutants | sed -n "$((i + 2))p")"
        name="$(printf '%s' "$line" | cut -f1 | cut -d' ' -f2-)"
        breaks="$(printf '%s' "$line" | cut -f2)"

        out="$("$BIN_DIR/$t" --mutant "$i" 2>&1)"
        rc=$?

        CUR_NAME="$t/$name（$breaks）"
        printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
        CUR_BAD=0
        CUR_OUT="$out"
        CUR_RC=$rc
        if [ "$rc" -eq 0 ]; then
            printf '   %sFAIL%s mutant 没有被抓住：测试仍然退出 0\n' \
                "$C_RED" "$C_RST"
            CUR_BAD=1
        else
            # 还要确认它是因为断言失败而红的，不是因为编译/桩/无关崩溃
            case "$out" in
            *"FAIL"*) ;;
            *) printf '   %sFAIL%s 退出码非 0，但输出里没有断言失败，可能红错了地方\n' \
                   "$C_RED" "$C_RST"
               CUR_BAD=1 ;;
            esac
        fi
        end_case
        i=$((i + 1))
    done
done

head1 "源码层 mutant（tests/mutate.py）"

# mutate.py 自己先在没打补丁的副本上跑一遍基线，基线不绿时它只打印
# 原因、不输出逐条结果，这里把它记成一条失败
local status id rest detail
out="$(python3 "$ROOT/tests/mutate.py" 2>&1)"
rc=$?
if ! printf '%s\n' "$out" | grep -qP '^(CAUGHT|ESCAPED|STALE|BUILD-FAIL|BAD-RED)\t'; then
    CUR_NAME="tests/mutate.py"
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=1
    CUR_OUT="$out"
    CUR_RC=$rc
    printf '   %sFAIL%s 没有逐条结果（退出码 %s）\n' "$C_RED" "$C_RST" "$rc"
    end_case
    return
fi
while IFS=$'\t' read -r status id rest detail; do
    CUR_NAME="$id（$rest）"
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=0
    CUR_OUT="$detail"
    CUR_RC=0
    if [ "$status" != CAUGHT ]; then
        printf '   %sFAIL%s %s\n' "$C_RED" "$C_RST" "$status"
        CUR_BAD=1
    fi
    end_case
done < <(printf '%s\n' "$out" | grep -P '^(CAUGHT|ESCAPED|STALE|BUILD-FAIL|BAD-RED)\t')

}

main() {
    case "${1:-all}" in
    -m|--manual) return 0 ;;
    esac
    auto_tests
    summary
}

main "${1:-all}"
