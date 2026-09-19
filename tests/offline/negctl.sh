#!/usr/bin/env bash
# tests/offline/negctl.sh — negative control：证明单元测试有能力报警
#
# 一套断言全绿，只说明它在当前实现上没报警，不说明它有能力报警。
# 这里把被测函数换成 tests/unit/{mem,io_bus}_mutants.c 里故意写错的实现，
# 再跑同一套用例，**要求每一个 mutant 都让测试非零退出**。
#
# 判据反过来：退出码为 0 才是 FAIL —— 那意味着这类错误能溜过去。
#
# 为什么不是“改一行源码再撤销”：那是一次性的，行号会漂，重构一次就失效。
# 这里的 mutant 只依赖函数签名，能一直留在仓库里。详见 tests/unit/mutants.h。
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

}

main() {
    case "${1:-all}" in
    -m|--manual) return 0 ;;
    esac
    auto_tests
    summary
}

main "${1:-all}"
