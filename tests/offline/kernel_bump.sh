#!/usr/bin/env bash
# tests/offline/kernel_bump.sh — tools/kernel_bump.py 的自测
#
# 换内核时这个工具决定哪些文档引用被自动改写，改错了的引用读起来照样通顺，
# 没人会发现，所以它的判断要有一份不依赖真实内核树的对照。这里现场造两棵
# 很小的“内核树”：新树在文件开头插了 100 行（大于工具的 40 行上下文窗口，
# 否则旧行号在新树里也会被当成成立），改了一行原文，把一个函数挪进了另一个
# 文件，再用一份写好的文档逐项核对每种输出。
#
# 用法：./tests/offline/kernel_bump.sh

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

KB="$ROOT/tools/kernel_bump.py"
W="$(mktemp -d)"
trap 'rm -rf -- "$W"' EXIT

# expect-not 把在仓库源码（含 tests/）里出现过的字符串当成 mini-vmm 自己打印的，
# 所以假内核里的消息在本文件中不能整段写出来，运行时再拼
HELLO="kernel says hello"" world"
WIDGET="failed to"" probe"
GONE="vanished kernel"" message"
MARKER="MARK-""0-END"
# mini-vmm 打印 "memslot %d: ..."：文字段只有 memslot 一个词，只能按 printf 的形状认出来
MEMSLOT="memslot ""7:"

mktree() {   # mktree DIR PATCHLEVEL
    mkdir -p "$1/arch/x86/kvm" "$1/drivers/foo" "$1/kernel"
    printf 'VERSION = 1\nPATCHLEVEL = %s\nSUBLEVEL = 0\nEXTRAVERSION =\n' "$2" > "$1/Makefile"
    printf 'int kvm_thing(void)\n{\n\treturn 0;\n}\n' > "$1/arch/x86/kvm/x86.c"
    printf '// same\nint same_fn(void)\n{\n\treturn same_value_in_both_trees();\n}\n' > "$1/drivers/foo/same.c"
    printf 'void say(void)\n{\n\tprintk("%s\\n");\n\tprintk("widget %%d %s\\n", 3);\n}\n' \
        "$HELLO" "$WIDGET" > "$1/kernel/msg.c"
}

# 旧树 bar.c：alpha 定义在第 2 行，第 4 行是乘法，beta 第 10 行，gamma_fn 第 15 行
mktree "$W/old" 0
cat > "$W/old/drivers/foo/bar.c" <<'EOF'
// bar
int alpha(int x)
{
	int unique_alpha_marker = x * 3;
	return unique_alpha_marker;
}

static int beta(void)
{
	return compute_beta_value(42);
}

static void gamma_fn(void)
{
	do_gamma_thing(1, 2, 3);
}
EOF

# 新树：开头多 100 行，beta 的返回值变了，gamma_fn 挪进 moved.c
mktree "$W/new" 1
{ for _ in $(seq 100); do echo "// padding"; done
  sed -e 's/compute_beta_value(42)/compute_beta_value(43)/' -e '/^static void gamma_fn/,$d' \
      "$W/old/drivers/foo/bar.c"; } > "$W/new/drivers/foo/bar.c"
printf '// moved\nstatic void gamma_fn(void)\n{\n\tdo_gamma_thing(1, 2, 3);\n}\n' \
    > "$W/new/drivers/foo/moved.c"

write_remap_doc() {
    cat > "$W/remap.md" <<'EOF'
赋值在 `drivers/foo/bar.c:4` 的 `alpha()` 里。
`drivers/foo/bar.c:4` 是一次乘法。
`drivers/foo/bar.c:10` 的 `beta()` 返回固定值。
`drivers/foo/bar.c:15` 的 `gamma_fn()` 做事。
host 侧 `arch/x86/kvm/x86.c:3` 的 `kvm_thing()` 不归 guest 管。
两棵树里都成立：`drivers/foo/same.c:4` 的 `same_fn()`。
EOF
}

auto_tests() {

head1 "remap"

write_remap_doc
cp "$W/remap.md" "$W/remap.orig"
run_case "dry run lists each kind and leaves the file alone" \
    sh -c '"$1" remap --old "$2/old" --new "$2/new" "$2/remap.md"; echo "rc=$?"; cmp "$2/remap.md" "$2/remap.orig" && echo unchanged' \
    _ "$KB" "$W"
expect_has "remap.md:1: drivers/foo/bar.c:4 -> drivers/foo/bar.c:104 (exact)"
expect_has "remap.md:2: SUGGEST drivers/foo/bar.c:4 -> drivers/foo/bar.c:104 (no identifier on this doc line to verify it)"
expect_has "remap.md:3: SUGGEST drivers/foo/bar.c:10 -> drivers/foo/bar.c:110 (fuzzy"
expect_has "remap.md:4: SUGGEST drivers/foo/bar.c:15 -> drivers/foo/moved.c:4 (gamma_fn() moved)"
expect_not "x86.c"
expect_not "same.c"
expect_has "rc=1"
expect_has "unchanged"
end_case

# 只改写上下文能验证的那一条；第二次运行时它在新树里已经成立，不能再被换算一次
run_case "--apply rewrites only verified references, and a second run changes nothing" \
    sh -c '"$1" remap --old "$2/old" --new "$2/new" --apply "$2/remap.md" "$2/remap.md" >/dev/null 2>&1
           head -2 "$2/remap.md"; cp "$2/remap.md" "$2/once.md"
           "$1" remap --old "$2/old" --new "$2/new" --apply "$2/remap.md" | grep -v SUGGEST; cmp "$2/remap.md" "$2/once.md" && echo stable' \
    _ "$KB" "$W"
expect_has "赋值在 \`drivers/foo/bar.c:104\` 的 \`alpha()\` 里。"
expect_has "\`drivers/foo/bar.c:4\` 是一次乘法。"
expect_not "remap.md:1:"
expect_has "stable"
end_case

head1 "refs"

cat > "$W/refs.md" <<'EOF'
这一条成立：`drivers/foo/bar.c:104` 的 `alpha()`。
这一条漂了：`drivers/foo/bar.c:4` 的 `alpha()`。
这一条越界：`drivers/foo/bar.c:9999` 的 `alpha()`。
这个文件不存在：`drivers/foo/gone.c`。
仓库自己的文件：`vm.c:1`。
host 侧：`arch/x86/kvm/x86.c:3`。

**Linux 1.0（guest）**

- `drivers/foo/bar.c`：`alpha()`、`no_such_function()`
EOF
run_case "refs reports drift, missing files and identifiers, stale version headers" \
    "$KB" refs --tree "$W/new" "$W/refs.md"
expect_rc 1
expect_has "refs.md:2: DRIFT drivers/foo/bar.c:4"
expect_has "refs.md:3: PASTEOF drivers/foo/bar.c:9999"
expect_has "refs.md:4: MISSING drivers/foo/gone.c"
expect_has "refs.md:8: VERSION header names Linux 1.0, tree is 1.1"
expect_has "refs.md:10: IDENT no_such_function"
expect_not "refs.md:1:"
expect_not "vm.c"
expect_not "x86.c"
expect_not "IDENT alpha"
end_case

run_case "host side checks only the KVM paths" \
    "$KB" refs --side host --tree "$W/new" "$W/refs.md"
expect_rc 0
expect_not "bar.c"
end_case

head1 "logs"

printf '[    0.000000] Linux version 1.1-test (builder@host) #1 SMP\n[    0.100000] foo: probed 3 devices\n' > "$W/boot.log"
cat > "$W/logs.md" <<'EOF'
```
[    0.000000] Linux version 1.1-test (...) #1 SMP
[    0.200000] foo: probed 4 devices
```
行内引用 `bar: this message never appears` 应该被报出来，命令 `perf kvm stat live` 不算日志。
EOF
run_case "logs ignores numbers and ... and reports only missing log text" \
    "$KB" logs --log "$W/boot.log" "$W/logs.md"
expect_rc 1
expect_has "logs.md:5: NOTINLOG bar: this message never appears"
expect_not "logs.md:2:"
expect_not "logs.md:3:"
expect_not "perf kvm"
end_case

head1 "expect-not"

{ echo "run_case x sh -c 'echo MARK-\$n-END'"
  echo "expect_not \"$HELLO\""
  echo "expect_not \"$MARKER\""
  echo "expect_not \"$MEMSLOT\""
  echo "expect_not \"widget 3 $WIDGET\""
  echo "expect_not \"$GONE\""; } > "$W/sample.sh"
run_case "expect-not separates kernel messages from VMM and test-script strings" \
    "$KB" expect-not --tree "$W/new" "$W/sample.sh"
expect_rc 1
expect_has "sample.sh:5: FRAGMENT \"widget 3 $WIDGET\" only \"$WIDGET\" found, at kernel/msg.c:4"
expect_has "sample.sh:6: NOTFOUND \"$GONE\""
expect_not "$HELLO"
expect_not "$MARKER"
expect_not "$MEMSLOT"
end_case

head1 "用法错误"

run_case "no subcommand" "$KB"
expect_rc 2
expect_has "(see --help)"
end_case

run_case "a tree that is not a Linux source tree" "$KB" refs --tree "$W" "$W/refs.md"
expect_rc 2
expect_has "does not look like a Linux source tree"
end_case

run_case "a missing input file is a usage error and nothing is written" \
    sh -c '"$1" remap --old "$2/old" --new "$2/new" --apply "$2/remap.md" "$2/nope.md"; echo "rc=$?"; cmp "$2/remap.md" "$2/once.md" && echo untouched' \
    _ "$KB" "$W"
expect_has "nope.md: no such file (see --help)"
expect_has "rc=2"
expect_has "untouched"
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
