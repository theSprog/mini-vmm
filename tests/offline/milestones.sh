#!/usr/bin/env bash
# tests/offline/milestones.sh — MILESTONES.md 依赖关系与层级表的一致性
#
# 第一条检查仓库里真正的 MILESTONES.md。后面几条是 negative control：
# 把它复制一份、各注入一种错误，要求 tools/check_milestones.py 必须报错。
# 只有这些对照都红了，第一条的“全绿”才说明检查真的在起作用。
#
# 用法：./tests/offline/milestones.sh

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

TOOL="$ROOT/tools/check_milestones.py"
SRC="$ROOT/MILESTONES.md"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# mutate <名字> <python 表达式>：对 MILESTONES.md 的副本做一次替换，输出副本路径。
# 表达式里 s 是全文，要求替换确实发生，免得原文改动后对照用例悄悄失效。
mutate() {
    local out="$WORK/$1.md"
    python3 -I - "$SRC" "$out" "$2" <<'EOF' || return 1
import sys
src, out, expr = sys.argv[1:4]
s = open(src, encoding="utf-8").read()
t = eval(expr, {"s": s})
if t == s:
    sys.exit(f"mutation had no effect: {expr}")
open(out, "w", encoding="utf-8").write(t)
EOF
    echo "$out"
}

auto_tests() {

head1 "仓库里的 MILESTONES.md"

run_case "dependencies are valid and the layer table is up to date" python3 "$TOOL" "$SRC"
expect_rc 0
expect_has "layer table is up to date"
end_case

head1 "对照：注入错误必须被发现"

f="$(mutate unknown 's.replace("依赖: emu-blk · 参考", "依赖: emu-blkk · 参考", 1)')" || exit 1
run_case "misspelled dependency" python3 "$TOOL" "$f"
expect_rc 1
expect_has "unknown dependency 'emu-blkk'"
end_case

f="$(mutate later 's.replace("### emu-blk — AHCI 磁盘控制器\n\n依赖: pci-bus", "### emu-blk — AHCI 磁盘控制器\n\n依赖: pci-bus, nvme", 1)')" || exit 1
run_case "dependency defined later (would allow a cycle)" python3 "$TOOL" "$f"
expect_rc 1
expect_has "dependency 'nvme' is defined later"
end_case

# 只改层级号，不碰里程碑名：名字后面的 ✓ 会随勾选变化，按名字匹配的替换会悄悄失效
f="$(mutate stale 's.replace("\n| 3 | ", "\n| 4 | ", 1)')" || exit 1
run_case "stale layer table" python3 "$TOOL" "$f"
expect_rc 1
expect_has "layer table"
expect_has "is stale"
end_case

f="$(mutate deplayer 's.replace("### guest-memory — 多 slot 内存布局与带检查的访问 API\n\n依赖: linux-boot", "### guest-memory — 多 slot 内存布局与带检查的访问 API\n\n依赖: linux-boot, legacy-devices", 1)')" || exit 1
run_case "a new dependency shifts downstream layers" python3 "$TOOL" "$f"
expect_rc 1
# 只匹配到 ID 为止：行尾是否带完成标记取决于 guest-memory 的勾选状态
expect_has "+| 3 | guest-memory"
end_case

run_case "--write regenerates the table, then the copy checks clean" \
    sh -c 'python3 "$1" --write "$2" && python3 "$1" "$2"' _ "$TOOL" "$f"
expect_rc 0
expect_has "updated layer table"
expect_has "layer table is up to date"
end_case

f="$(mutate undone 's.replace("- [x] `reboot -f` 经 i8042", "- [ ] `reboot -f` 经 i8042", 1)')" || exit 1
run_case "unticking a checkbox removes the done mark" python3 "$TOOL" --print "$f"
expect_rc 0
expect_has "legacy-devices · smp ✓"
expect_not "legacy-devices ✓"
end_case

}

main() {
    case "${1:-all}" in
    -m|--manual) return 0 ;;
    esac
    auto_tests
    summary
}

main "${1:-all}"
