#!/usr/bin/env bash
# tests/offline/boot_tables.sh — 固件表的外部 oracle 检查（不需要 /dev/kvm）
#
# 和 offline/unit.sh 的分工：
#   unit.sh        跑 boot_tables_test 自己写的字段检查（自研 oracle）
#   boot_tables.sh 把生成的 ACPI 表交给 iasl 反汇编（ACPICA，外部 oracle）
# 两者用的是同一个二进制，但判据来源完全不同，所以分成两个 suite。
#
# iasl 把发现的问题写进反汇编出来的 .dsl（例如
# "/* Incorrect checksum, should be 48 */"、"Invalid zero length subtable"），
# 标准输出里看不到，所以断言的对象是 .dsl 的内容。
#
# 用法：
#   ./tests/offline/boot_tables.sh
#   IASL=/path/to/iasl ./tests/offline/boot_tables.sh

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

UNIT_BIN="$ROOT/build/tests/boot_tables_test_noalign"
IASL="${IASL:-$(command -v iasl || true)}"

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:print_stacktrace=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"

auto_tests() {

head1 "固件表 — iasl 反汇编（外部 oracle）"

if [ ! -x "$UNIT_BIN" ]; then
    skip_case "boot_tables" "$UNIT_BIN 未构建，先跑 make test"
    return
fi

local dump
dump="$(mktemp -d)"

run_case "生成 ACPI 表并转储" "$UNIT_BIN" --dump "$dump"
expect_rc 0
expect_has "0 failed"
end_case

if [ -z "$IASL" ] || [ ! -x "$IASL" ]; then
    skip_case "iasl -d 反汇编" "找不到 iasl（可用 IASL=/path/to/iasl 指定）"
    rm -rf "$dump"
    return
fi

local t base
for t in "$dump"/*.dat; do
    [ -e "$t" ] || { skip_case "iasl -d" "没有转储出任何表"; break; }
    base="$dump/$(basename "$t" .dat)"
    run_case "iasl -d $(basename "$t")" \
        bash -c '"$0" -p "$1" -d "$2" >/dev/null && cat "$1.dsl"' \
        "$IASL" "$base" "$t"
    expect_rc 0
    expect_has "Signature"
    expect_not "Incorrect"
    expect_not "Invalid"
    expect_not "Error"
    expect_not "Warning"
    end_case
done

rm -rf "$dump"

}

main() {
    case "${1:-all}" in
    -m|--manual) return 0 ;;
    esac
    auto_tests
    summary
}

main "${1:-all}"
