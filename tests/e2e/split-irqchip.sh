#!/usr/bin/env bash
# tests/e2e/split-irqchip.sh — 里程碑 split-irqchip 的验收
#
#   --kernel-irqchip split 时 PIC、IOAPIC、PIT 在用户态，LAPIC 留在 KVM；
#   level 中断的 EOI 经 KVM_EXIT_IOAPIC_EOI 回到用户态 IOAPIC；PIT 的补发
#   策略在两种模式下都由 --pit-reinject 选择
#
# 用法：
#   ./tests/e2e/split-irqchip.sh       全部
#   ./tests/e2e/split-irqchip.sh -a    只跑自动用例
#   ./tests/e2e/split-irqchip.sh -m    只看人工指引
#
# 两种模式下现有 e2e 全部通过由 IRQCHIP=split ./tests/run.sh 证明（见
# tests/vmm_irqchip.sh），这里只验证 split 特有的机制，并且每个用例在
# 两种模式下各跑一遍，内核态 irqchip 就是对照组。
#
# 判据的来源：
#   level EOI：AHCI 在 pci=nomsi 下走 INTx，IOAPIC 17 号脚是 level。用户态
#     IOAPIC 投递一次之后 remote IRR 置位，在 EOI 清掉它之前这根线不会再
#     投递（82093AA datasheet 3.2.4 RTE bit 14）。所以 guest 收到不止一次
#     ahci 中断、libata 没有超时，本身就证明 EOI 回来了；exit-stats 里的
#     IOAPIC_EOI 次数不少于 guest 数到的 ahci 中断数，是同一件事在 host 侧的
#     计数。内核态 irqchip 下这条 exit 不会出现。
#   XT-PIC：noapic 时 Linux 只用 8259A，timer 和串口的中断都经 PIC 的 INT
#     输出、KVM_INTERRUPT 注入（ExtINT），/proc/interrupts 的芯片名是 XT-PIC。
#   PIT 计时：pitprobe（tools/guest/pitprobe.c）让 PIT 通道 2 数 10ms，用 TSC
#     量窗口长度。kvmclock 打开时 guest 的 "tsc: Detected" 来自 KVM 报的
#     TSC 频率，与 PIT 无关，所以两者相符说明 PIT 的时间基准是准的。
#   补发策略：guest 以 noapictimer nohz=off highres=off 启动，tick 只来自
#     PIT 且保持周期模式，每个 IRQ0 让 jiffies 加一（kernel/time/tick-common.c
#     tick_periodic 的 do_timer(1)），而 CLOCK_MONOTONIC 来自 TSC/kvmclock，
#     不受丢 tick 影响。host 上 SIGSTOP 整个 VMM 1 秒模拟“完全得不到 CPU”的
#     过载：补发打开时积压的 tick 在恢复后逐个补上，jiffies 与单调时钟之差
#     回到 0；关闭时这 1 秒的 tick 合并成一个，jiffies 落后约 1000。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
CLIENT="python3 $ROOT/tests/monitor_client.py --timeout 10"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
SOCK="$WORK/mon.sock"

check() {
    local what="$1"; shift
    if ! "$@"; then
        printf '   %sFAIL%s %s\n' "$C_RED" "$C_RST" "$what"
        CUR_BAD=1
    fi
}

# 启动 guest，输出放进 CUR_OUT，并开始一个用例
boot_case() {
    CUR_NAME="$1"; shift
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=0
    CUR_OUT="$(python3 "$GUEST_SHELL" --timeout 120 "$@" 2>&1 </dev/null)"
    CUR_RC=$?
}

# CUR_OUT 里第一行匹配 $1 的第 $2 个字段（awk 字段号）
field() {
    printf '%s\n' "$CUR_OUT" | tr -d '\r' | awk -v re="$1" -v c="$2" '$0 ~ re { print $c; exit }'
}

# exit-stats 报告文件里第一列等于 $2 的行的第 $3 列，没有这一行输出空串
stat_col() {
    awk -v n="$2" -v c="$3" '$1 == n { print $c; exit }' "$1"
}

# ------------------------------------------------------------------
cmdline_tests() {
head1 "命令行约束"

run_case "--kernel-irqchip off 指向 user-lapic 里程碑" \
    "$VMM_BIN" --mode linux --kernel-irqchip off --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--kernel-irqchip off (LAPIC in user space) is not implemented yet; it belongs to the user-lapic milestone"
expect_not "KVM API"
end_case

run_case "--kernel-irqchip 只认 on 和 split" \
    "$VMM_BIN" --mode linux --kernel-irqchip userspace --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--kernel-irqchip must be on or split: userspace"
end_case

run_case "--pit-reinject 只认 on 和 off" \
    "$VMM_BIN" --mode linux --pit-reinject yes --initrd "$ROOT/Makefile" "$ROOT/Makefile"
expect_rc 1
expect_has "--pit-reinject must be on or off: yes"
end_case

run_case "裸 payload 没有中断控制器，两个选项都要求 --mode linux" sh -c '
    "$1" --mode long64 --kernel-irqchip split "$2"; echo "rc=$?"
    "$1" --mode long64 --pit-reinject off "$2"; echo "rc=$?"' _ "$VMM_BIN" "$ROOT/build/serial64.bin"
expect_has "--kernel-irqchip requires --mode linux"
expect_has "--pit-reinject requires --mode linux"
check "both rejected with rc 1" test "$(printf '%s\n' "$CUR_OUT" | grep -c '^rc=1$')" -eq 2
end_case
}

# ------------------------------------------------------------------
# 每种模式一次主启动：AHCI INTx（level）、pitprobe、monitor 的 info irqchip、
# exit-stats 报告
main_boot_tests() {
local mode="$1" stats="$WORK/stats-$1.txt" disk="$WORK/disk.img"

head1 "kernel-irqchip=$mode：level 中断、PIT 计时、exit 分布"
truncate -s 16M "$disk"
boot_case "[$mode] AHCI INTx、pitprobe、info irqchip" \
    --cmd "echo AHCI-IRQ \$(awk '/ahci/ {print \$2, \$3, \$4}' /proc/interrupts)" \
    --cmd 'pitprobe 20 | tail -3' \
    --host "$CLIENT $SOCK 'info irqchip'" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 256M --kernel-irqchip "$mode" \
          --drive id=d0,file="$disk" --device ahci,drive=d0 --append pci=nomsi \
          --monitor "$SOCK" --exit-stats "$stats" \
          --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
# libata 等不到命令完成时打印 "qc timeout"：level 中断一次之后再也不来就是这样
expect_not "qc timeout"
expect_has "ata1: SATA link up"

local ahci chip eoi mhz med
ahci="$(field '^AHCI-IRQ ' 2)"
chip="$(field '^AHCI-IRQ ' 3)"
check "guest counted more than one ahci interrupt (got '${ahci:-none}')" \
    test "${ahci:-0}" -ge 2
check "ahci interrupt is level on the IO-APIC (got '${chip:-none}')" \
    test "$chip" = "IO-APIC"
expect_re '^AHCI-IRQ [0-9]+ IO-APIC 17-fasteoi'

# pitprobe：OUT2 每轮都起来了，窗口折算的 MHz 与 kvmclock 报的 TSC 频率相差 1% 以内
expect_re '^pitprobe: [0-9]+/20 accepted'
mhz="$(printf '%s\n' "$CUR_OUT" | tr -d '\r' | sed -n 's/.*tsc: Detected \([0-9]*\)\..* MHz.*/\1/p' | head -1)"
med="$(field '^pitprobe: ' 14)"
check "PIT 10ms window = ${med:-none} MHz of TSC, guest reports ${mhz:-none} MHz (within 1%)" \
    awk -v a="${med:-0}" -v b="${mhz:-0}" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(b > 0 && d * 100 <= b) }'

if [ "$mode" = split ]; then
    expect_re '^  pit: reinject on, ticks [0-9]+'
    eoi="$(printf '%s\n' "$CUR_OUT" | sed -n 's/.*ioapic eoi exits \([0-9]*\).*/\1/p' | head -1)"
    check "info irqchip reports IOAPIC EOI exits (${eoi:-none}) >= ahci interrupts (${ahci:-none})" \
        test "${eoi:-0}" -ge "${ahci:-1}"
    local p17
    # 行首是 "ioapic delivered:" 两个字段，pin N 在第 N + 3 个字段
    p17="$(printf '%s\n' "$CUR_OUT" | awk '/ioapic delivered:/ { print $(17 + 3); exit }')"
    check "IOAPIC pin 17 deliveries (${p17:-none}) >= ahci interrupts (${ahci:-none})" \
        test "${p17:-0}" -ge "${ahci:-1}"
else
    expect_re '^  kernel irqchip: PIC \+ IOAPIC \+ PIT \+ LAPIC in KVM$'
    expect_re '^  pit: reinject on$'
fi
end_case

# exit-stats：芯片的 exit 只在 split 下出现
CUR_NAME="[$mode] exit-stats：PIC/PIT/IOAPIC 的 exit 和 IOAPIC_EOI"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(cat "$stats" 2>&1)"
CUR_RC=0
local eoi_exits pit_w nmisc_r io_w total
eoi_exits="$(stat_col "$stats" IOAPIC_EOI 2)"
pit_w="$(stat_col "$stats" pit 4)"
nmisc_r="$(stat_col "$stats" nmisc 3)"
io_w="$(stat_col "$stats" ioapic 4)"
total="$(stat_col "$stats" total 2)"
if [ "$mode" = split ]; then
    check "IOAPIC_EOI exits (${eoi_exits:-none}) >= ahci interrupts (${ahci:-none})" \
        test "${eoi_exits:-0}" -ge "${ahci:-1}"
    # kvmclock 启动不读 PIT 计数，但 Linux 会给通道 0 装数（clockevent）
    check "PIT writes exit to user space (${pit_w:-none})" test "${pit_w:-0}" -gt 0
    check "port 0x61 reads exit to user space (${nmisc_r:-none})" test "${nmisc_r:-0}" -gt 0
    check "IOAPIC MMIO writes exit to user space (${io_w:-none})" test "${io_w:-0}" -gt 0
    expect_re '^  pic-master +PIO +[1-9][0-9]* +[1-9][0-9]*$'
else
    check "no IOAPIC_EOI row in kernel mode (got '${eoi_exits}')" test -z "$eoi_exits"
    expect_not "pic-master"
    expect_not "nmisc"
fi
printf '   user-space exits total: %s\n' "${total:-none}"
end_case
}

# ------------------------------------------------------------------
noapic_tests() {
local mode="$1"
boot_case "[$mode] noapic：所有 ISA 中断走 8259A（XT-PIC）" \
    --cmd 'grep -E "timer|ttyS0" /proc/interrupts' \
    --host "$CLIENT $SOCK 'info irqchip'" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 256M --kernel-irqchip "$mode" --append noapic \
          --monitor "$SOCK" --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "guest requested poweroff via ACPI S5"
expect_re '^ +0: +[1-9][0-9]* +XT-PIC +timer'
expect_re '^ +4: +[1-9][0-9]* +XT-PIC +ttyS0'
if [ "$mode" = split ]; then
    local inj
    inj="$(printf '%s\n' "$CUR_OUT" | sed -n 's/.*extint injected \([0-9]*\).*/\1/p' | head -1)"
    check "ExtINT injections via KVM_INTERRUPT (${inj:-none}) > 0" test "${inj:-0}" -gt 0
fi
end_case
}

# ------------------------------------------------------------------
# 没有 kvmclock 时 TSC 频率要靠 PIT 校准。三种结局都合法（见 lib.sh
# expect_tsc_calibrated），走了哪条打印出来，两种模式的差别写在笔记里
calibrate_tests() {
local mode="$1" path
boot_case "[$mode] no-kvmclock：TSC 经 PIT 校准" \
    --cmd 'echo booted' \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 256M --kernel-irqchip "$mode" --append no-kvmclock \
          --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "guest requested poweroff via ACPI S5"
expect_tsc_calibrated
path="$(printf '%s\n' "$CUR_OUT" | grep -oE 'tsc: (Fast TSC calibration using PIT|Using PIT calibration value|Unable to calibrate against PIT)' | head -1)"
printf '   calibration path: %s\n' "${path:-none}"
end_case
}

# ------------------------------------------------------------------
# t 打印 "TICK <CLOCK_MONOTONIC ns> <jiffies> <IRQ0 次数>"。标记拆成 "T" "ICK"
# 写：回显的命令行里不出现完整的 TICK，断言只会匹配到真正的输出行
TICK_FN='t() { awk '\''/^now at/{n=$3} /^jiffies:/{j=$2} $1=="0:"{i=$2} END{print "T" "ICK", n, j, i}'\'' /proc/timer_list /proc/interrupts; }'

reinject_tests() {
local mode="$1" policy="$2"
boot_case "[$mode] --pit-reinject $policy：VMM 停 1 秒后的 tick 数与漂移" \
    --cmd "$TICK_FN" \
    --cmd 't' \
    --host 'kill -STOP $VMM_PID; sleep 1; kill -CONT $VMM_PID' \
    --cmd 'sleep 2; t' \
    --host "$CLIENT $SOCK 'info irqchip'" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 256M --kernel-irqchip "$mode" --pit-reinject "$policy" \
          --append 'noapictimer nohz=off highres=off' \
          --monitor "$SOCK" --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "guest requested poweroff via ACPI S5"

local ticks n0 j0 i0 n1 j1 i1 ms jd id drift
ticks="$(printf '%s\n' "$CUR_OUT" | tr -d '\r' | grep -E '^TICK [0-9]+ [0-9]+ [0-9]+$')"
read -r _ n0 j0 i0 <<< "$(printf '%s\n' "$ticks" | sed -n 1p)"
read -r _ n1 j1 i1 <<< "$(printf '%s\n' "$ticks" | sed -n 2p)"
if [ -z "${n1:-}" ]; then
    check "two TICK samples in the guest output" false
    end_case
    return
fi
ms=$(( (n1 - n0) / 1000000 ))
jd=$(( j1 - j0 ))
id=$(( i1 - i0 ))
drift=$(( ms - jd ))
printf '   elapsed %d ms, jiffies +%d, IRQ0 +%d, drift %d ms\n' "$ms" "$jd" "$id" "$drift"
check "every IRQ0 is one jiffy (IRQ0 +$id, jiffies +$jd)" \
    test "$(( id > jd ? id - jd : jd - id ))" -le 10
if [ "$policy" = on ]; then
    check "reinject on: backlog caught up, |drift| <= 50 ms (got $drift)" \
        test "${drift#-}" -le 50
else
    check "reinject off: the stalled second is lost, drift >= 500 ms (got $drift)" \
        test "$drift" -ge 500
fi

if [ "$mode" = split ]; then
    local coal backlog
    coal="$(printf '%s\n' "$CUR_OUT" | sed -n 's/.*coalesced \([0-9]*\),.*/\1/p' | head -1)"
    backlog="$(printf '%s\n' "$CUR_OUT" | sed -n 's/.*backlog \([0-9]*\),.*/\1/p' | head -1)"
    expect_re "^  pit: reinject $policy, "
    if [ "$policy" = on ]; then
        check "reinject on merges nothing (coalesced ${coal:-none})" test "${coal:-1}" -eq 0
        check "reinject on: backlog drained (${backlog:-none} <= 2)" test "${backlog:-99}" -le 2
    else
        check "reinject off merged the stalled ticks (coalesced ${coal:-none} >= 500)" \
            test "${coal:-0}" -ge 500
    fi
fi
end_case
}

# ------------------------------------------------------------------
auto_tests() {
cmdline_tests

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "guest 内核用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi
if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

local mode policy
for mode in on split; do
    main_boot_tests "$mode"
done

head1 "XT-PIC 路径与 TSC 校准"
for mode in on split; do
    noapic_tests "$mode"
    calibrate_tests "$mode"
done

head1 "PIT 补发策略（noapictimer，tick 只来自 PIT）"
for mode in on split; do
    for policy in on off; do
        reinject_tests "$mode" "$policy"
    done
done
}

manual_guide() {
guide "在 host 真实过载下对比补发策略" \
"# 另开终端制造负载：stress-ng --cpu \$(nproc) --timeout 60
./build/vmm --mode linux --kernel-irqchip split --pit-reinject off \\
    --append 'noapictimer nohz=off highres=off' --monitor /tmp/mon.sock \\
    --initrd build/initramfs.cpio.gz guest-kernel/vmlinux
# guest 里每隔几秒看一次：
awk '/^now at/{n=\$3} /^jiffies:/{j=\$2} END{print n/1e6 - j}' /proc/timer_list
# host 上：socat - UNIX-CONNECT:/tmp/mon.sock，输入 info irqchip" \
"reinject off 时差值随负载持续增大，info irqchip 的 coalesced 增长；
换成 --pit-reinject on 差值保持在几毫秒内，acks 跟着 irqs 增长"
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
