#!/usr/bin/env bash
# tests/offline/unit.sh — C 单元测试（不需要 /dev/kvm，不需要 guest 内核）
#
# 二进制由 Makefile 构建（make build/tests/xxx_test），带
# AddressSanitizer + UndefinedBehaviorSanitizer，且 -fno-sanitize-recover=all，
# 所以任何越界或未定义行为都会让进程直接非零退出，不会只打一行警告。
#
# 每个二进制自己打印 "<name>: N checks in C cases, M failed"，
# 这里断言退出码为 0 且摘要里是 "0 failed"。
#
# 用法：./tests/offline/unit.sh [-a]      （-a 只是为了和其它 suite 对齐）

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

BIN_DIR="$ROOT/build/tests"
UNITS="mem_test io_bus_test boot_tables_test"

# 让 sanitizer 的报告尽量好读，并保证一出问题就退出
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0:print_stacktrace=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"

auto_tests() {

head1 "C 单元测试（ASan + UBSan）"

local u
for u in $UNITS; do
    if [ ! -x "$BIN_DIR/$u" ]; then
        skip_case "$u" "未构建，先跑 make test 或 make $BIN_DIR/$u"
        continue
    fi
    run_case "$u" "$BIN_DIR/$u"
    expect_rc 0
    expect_has "0 failed"
    expect_not "FAIL "
    # sanitizer 触发时进程会打这两行之一，退出码可能仍是 1，单独断言更醒目
    expect_not "AddressSanitizer"
    expect_not "runtime error:"
    end_case
done


# 曾经崩过的输入，每次都重放一遍。这些是 fuzz 找到的，但重放只要毫秒，
# 没理由让它们只在按分钟计费的 fuzz suite 里才跑到。
head1 "回归：曾经让加载器崩掉的输入"

local f bin
for f in "$ROOT"/tests/fuzz/regress/*.bin; do
    [ -e "$f" ] || { skip_case "regress" "tests/fuzz/regress 里没有输入"; break; }
    case "$(basename "$f")" in
    elf-*) bin="$ROOT/build/fuzz/elf_loader_fuzz" ;;
    bz-*)  bin="$ROOT/build/fuzz/bzimage_fuzz" ;;
    *)     continue ;;
    esac
    if [ ! -x "$bin" ]; then
        skip_case "regress $(basename "$f")" "$(basename "$bin") 未构建（需要 clang）"
        continue
    fi
    run_case "regress $(basename "$f")" "$bin" "$f"
    expect_rc 0
    expect_not "runtime error:"
    expect_not "AddressSanitizer"
    expect_not "ERROR: libFuzzer"
    end_case
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
