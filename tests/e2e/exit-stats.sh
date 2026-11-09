#!/usr/bin/env bash
# tests/e2e/exit-stats.sh — 里程碑 exit-stats 的验收
#
#   --exit-stats 在 VM 停下后按 exit_reason、按设备统计回到用户态的 exit，
#   并附上 KVM 自己的计数器（KVM_GET_STATS_FD）；统计不改变 guest 的行为
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/e2e/exit-stats.sh       全部
#   VMLINUX=/path/to/vmlinux ./tests/e2e/exit-stats.sh -a    只跑自动用例
#   ./tests/e2e/exit-stats.sh -m                             只看人工指引
#
# 判据的来源：
#   裸 payload 的每一条 in/out/hlt 都写在源码里（tools/payload/*.S），
#   exit 次数可以逐条数出来，和报告逐项比对，不是“大于 0”这种弱判据。
#   KVM 的 io_exits 在 svm.c io_interception() 里每次 IOIO 拦截加一，
#   这两个 payload 访问的端口都不在内核里模拟，所以必须等于用户态的
#   IO exit 数；KVM 的 exits 还包括内核就地处理的 exit，只能更多。
#   Linux guest 的次数不可预知，改用内部一致性：各设备行的读写之和
#   必须等于 IO / MMIO 两行的总数。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

PAYLOAD16="$ROOT/build/payload16.bin"
SERIAL64="$ROOT/build/serial64.bin"
INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 断言一个条件，失败时打印说明。数值比较这类 expect_* 表达不了的判据用。
check() {
    local what="$1"; shift
    if ! "$@"; then
        printf '   %sFAIL%s %s\n' "$C_RED" "$C_RST" "$what"
        CUR_BAD=1
    fi
}

# 报告里第一列等于 $1 的行的第 $2 列（awk 字段号），没有这一行输出空串
stat_col() {
    printf '%s\n' "$CUR_OUT" | awk -v n="$1" -v c="$2" '$1 == n { print $c; exit }'
}

# ------------------------------------------------------------------
auto_tests() {

head1 "裸 payload：报告里的每个数都能从源码数出来"

# real_mode_stub.S：out 0x10、in 0x10、out 0x10、hlt
run_case "real16: 3 IO + 1 HLT, all on dbg-port" \
    "$VMM" --mem 2M --exit-stats - "$PAYLOAD16"
expect_rc 0
expect_re '^  IO +3 +3$'
expect_re '^  HLT +1 +1$'
expect_re '^  total +4 +4$'
expect_re '^  dbg-port +PIO +1 +2$'
expect_re '^  serial8250 +PIO +0 +0$'
expect_re '^  unhandled +PIO +0 +0$'
expect_re '^  io_exits +3 +3$'
expect_re '^  halt_exits +1 +1$'
end_case

# serial_stub.S：31 个字符，每个先读一次 LSR 再写 THR（LSR.THRE 恒为 1，
# 不会多轮询）；之后 SCR 写读各一次，MCR 写两次，MSR 读一次，最后
# dbg-port 写一次、hlt。serial 读 31+1+1=33，写 31+1+2=34，IO 共 68。
run_case "long64 serial payload: 68 IO, KVM io_exits agrees" \
    "$VMM" --mode long64 --exit-stats - "$SERIAL64"
expect_rc 0
expect_re '^  IO +68 +68$'
expect_re '^  HLT +1 +1$'
expect_re '^  serial8250 +PIO +33 +34$'
expect_re '^  dbg-port +PIO +0 +1$'
expect_re '^  io_exits +68 +68$'
kexits="$(stat_col exits 2)"
check "KVM exits ($kexits) >= user-space exits (69)" \
    test "${kexits:-0}" -ge 69
end_case

head1 "统计不改变 guest 的行为"

# guest 可见的输出全在 stdout（串口和 dbg-port），报告写到文件。
# 打开和不打开统计，stdout 必须逐字节相同、退出码相同。
run_case "same guest output with and without --exit-stats" sh -c '
    "$1" --mode long64 -q "$2" > "$3/off.out"; echo "rc-off=$?"
    "$1" --mode long64 -q --exit-stats "$3/stats.txt" "$2" > "$3/on.out"; echo "rc-on=$?"
    cmp "$3/off.out" "$3/on.out" && echo "stdout identical"
    head -1 "$3/stats.txt"' _ "$VMM" "$SERIAL64" "$WORK"
expect_rc 0
expect_has "rc-off=0"
expect_has "rc-on=0"
expect_has "stdout identical"
expect_has "exit-stats: 1 vCPU(s)"
end_case

run_case "'-' writes the report to stderr, not stdout" sh -c '
    "$1" --mode long64 -q --exit-stats - "$2" 2>/dev/null' _ "$VMM" "$SERIAL64"
expect_rc 0
expect_has "Hello from guest via 8250 UART"
expect_not "exit-stats:"
end_case

run_case "unwritable stats path fails before the guest starts" \
    "$VMM" --mode long64 --exit-stats "$WORK/no/such/dir/stats.txt" "$SERIAL64"
expect_rc 1
expect_has "--exit-stats $WORK/no/such/dir/stats.txt: No such file or directory"
expect_not "entering guest"
end_case

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "guest 内核用例" "set VMLINUX=/path/to/vmlinux to enable"
    return
fi

if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

head1 "Linux guest（2 vCPU）"

STATS="$WORK/linux-stats.txt"
CUR_NAME="boot to shell with --exit-stats FILE, poweroff"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(python3 "$GUEST_SHELL" --timeout 90 \
    --cmd 'echo STATS-$((6*7))' \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --smp 2 --mem 512M --exit-stats "$STATS" \
       --initrd "$INITRD" "$VMLINUX" 2>&1)"
CUR_RC=$?
expect_rc 0
expect_has "STATS-42"
expect_has "guest requested poweroff via ACPI S5"
# 报告进了文件，不混进控制台
expect_not "exit-stats:"
end_case

run_case "report: per-vCPU columns, device rows add up to IO/MMIO" cat "$STATS"
expect_rc 0
expect_has "exit-stats: 2 vCPU(s)"
expect_re '^  reason +total +vcpu0 +vcpu1$'
expect_re '^  serial8250 +PIO +[1-9][0-9]* +[1-9][0-9]*$'
# 还没有 PCI 总线，Linux 经 0xcf8/0xcfc 探测 PCI 配置空间全部落空
expect_re '^  unhandled +PIO +[1-9][0-9]* +[0-9]+$'
io="$(stat_col IO 2)"
mmio="$(stat_col MMIO 2)"
pio_sum="$(printf '%s\n' "$CUR_OUT" | awk '$2 == "PIO" { s += $3 + $4 } END { print s + 0 }')"
mmio_sum="$(printf '%s\n' "$CUR_OUT" | awk '$2 == "MMIO" { s += $3 + $4 } END { print s + 0 }')"
check "IO exits (${io:-none}) == sum of PIO device rows ($pio_sum)" \
    test "${io:-0}" -eq "$pio_sum"
check "MMIO exits (${mmio:-0}) == sum of MMIO device rows ($mmio_sum)" \
    test "${mmio:-0}" -eq "$mmio_sum"
user_total="$(stat_col total 2)"
kexits="$(stat_col exits 2)"
check "KVM exits (${kexits:-none}) >= user-space exits (${user_total:-none})" \
    test "${kexits:-0}" -ge "${user_total:-1}"
end_case

}

manual_guide() {
guide "用 KVM tracepoint 交叉核对 KVM 的 exits 计数" \
"sudo perf stat -e kvm:kvm_exit,kvm:kvm_userspace_exit -- \\
    ./build/vmm --mode long64 --exit-stats - build/serial64.bin" \
"kvm:kvm_exit 的次数等于报告里 KVM vCPU stats 的 exits，
kvm:kvm_userspace_exit 的次数等于报告里 user-space exits 的 total"
}

main() {
    local mode="${1:-all}"

    case "$mode" in
    -m|--manual) manual_guide; return 0 ;;
    -a|--auto)   ;;
    all|"")      ;;
    *) echo "用法: $0 [-a 只跑自动 | -m 只看人工指引]"; return 2 ;;
    esac

    need_kvm
    need_build
    auto_tests

    local rc=0
    summary || rc=1

    if [ "$mode" != "-a" ] && [ "$mode" != "--auto" ]; then
        manual_guide
    fi
    return $rc
}

main "${1:-all}"
