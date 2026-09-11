#!/usr/bin/env bash
# tests/boot_tables.sh — 固件表与 boot_params 的离线检查
#
# 不需要 /dev/kvm，也不需要 guest 内核：
#   1. 把 tests/unit/boot_tables_test.c 和 src/boot/{mptable,acpi,zeropage}.c
#      编译成一个 host 程序，逐字段检查生成的表（见该文件开头的说明）
#   2. 若找得到 iasl（ACPICA 的 ASL 编译/反汇编器，环境变量 IASL 或 PATH），
#      再把每张 ACPI 表反汇编一遍，要求没有任何 Warning/Error
#
# 用法：
#   ./tests/boot_tables.sh            全部
#   IASL=/path/to/iasl ./tests/boot_tables.sh
#
# 退出码：0 全过，1 有 FAIL。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

CC="${CC:-gcc}"
UNIT_BIN="$ROOT/build/tests/boot_tables_test"
IASL="${IASL:-$(command -v iasl || true)}"

auto_tests() {

head1 "boot tables — offline checks"

mkdir -p "$(dirname "$UNIT_BIN")"
run_case "build boot_tables_test" \
    "$CC" -Wall -Wextra -Wno-unused-parameter -O2 -g -std=gnu11 \
    -I"$ROOT/include" -o "$UNIT_BIN" \
    "$ROOT/tests/unit/boot_tables_test.c" \
    "$ROOT/src/boot/mptable.c" "$ROOT/src/boot/acpi.c" "$ROOT/src/boot/zeropage.c"
expect_rc 0
end_case
[ -x "$UNIT_BIN" ] || return

local dump
dump="$(mktemp -d)"
run_case "MP table / ACPI / boot_params field checks" "$UNIT_BIN" --dump "$dump"
expect_rc 0
expect_has "0 failed"
expect_not "FAIL ["
end_case

if [ -z "$IASL" ] || [ ! -x "$IASL" ]; then
    skip_case "iasl -d disassembly" "iasl not found (set IASL=/path/to/iasl)"
else
    # iasl 把发现的问题写进反汇编出来的 .dsl（例如
    # "/* Incorrect checksum, should be 48 */"、"Invalid zero length subtable"），
    # 标准输出里看不到，所以断言的对象是 .dsl 的内容。
    local t base
    for t in "$dump"/*.dat; do
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
fi
rm -rf "$dump"

}

main() {
    auto_tests
    summary
}

main "$@"
