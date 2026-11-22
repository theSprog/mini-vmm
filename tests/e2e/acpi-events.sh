#!/usr/bin/env bash
# tests/e2e/acpi-events.sh — 里程碑 acpi-events 的验收
#
#   monitor 的 system_powerdown 按下 ACPI 固定电源按钮：PM1_STS.PWRBTN_STS
#   置位，guest 打开了 PWRBTN_EN 时 SCI（GSI 9，电平）拉高；guest 的 ACPI
#   驱动读状态、写 1 清零、SCI 落下，Linux 按钮驱动报出 KEY_POWER；完整
#   发行版的 systemd-logind 据此关机，最后经 \_S5 写 PM1_CNT，VMM 退出。
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/e2e/acpi-events.sh       全部
#   VMLINUX=/path/to/vmlinux ./tests/e2e/acpi-events.sh -a    只跑自动用例
#   ./tests/e2e/acpi-events.sh -m                             只看人工指引
#
# 判据的来源（ACPI 6.5，ref/ACPI_Spec_6_5_Aug29.pdf）：
#   FADT PWR_BUTTON 清 0 表示固定功能电源按钮（Table 5.10），Linux 为它建
#   LNXPWRBN 设备，button 驱动的输入设备名是 "Power Button"
#   （drivers/acpi/button.c），按一次报 KEY_POWER(116) 按下、松开两个事件。
#   SCI 是共享、电平、低有效（FADT SCI_INT，Table 5.9）；MADT 里没有 ISO
#   时 Linux 照这个默认配置 IOAPIC 引脚 9，/proc/interrupts 里的流控是
#   fasteoi（电平）而不是 edge。
#   每按一次，SCI 中断计数和 /sys/firmware/acpi/interrupts/ff_pwr_btn 都
#   正好加 1：PWRBTN_STS 写 1 清零之后 SCI 落下（Table 4.11），不会在 EOI
#   之后重新触发。写 1 清零坏掉时电平一直有效，计数会一路涨上去。
#   FACS 里有全局锁，PM1_EN.GBL_EN 能写进去，ACPICA 不报全局锁不存在
#   （它靠写使能位再读回探测固定事件的硬件）。
#   guest 没打开 PWRBTN_EN（acpi=off）时按钮只锁存状态位、不发 SCI
#   （4.8.2.2.1.1），monitor 的回复要把这一点说出来。
# 每个 BusyBox 用例在内核 irqchip 和 split irqchip 下各跑一遍：两种模式下
# SCI 分别经 KVM 的 IOAPIC 和用户态 IOAPIC（remote IRR、EOI 广播），电平
# 中断的确认路径不同。
# 寄存器逐位、逐字节的语义由单元测试 acpi_pm_test 验证。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
# 每条回复的兜底超时：正常回复是毫秒级，10 秒只在 VMM 卡死时触发
CLIENT="python3 $ROOT/tests/monitor_client.py --timeout 10"
IMG="${ARCH_IMAGE:-$ROOT/arch-image}"
KEY_POWER=116

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

# 输出里 "<前缀><数字>" 的数字，取第 $2 个（默认第一个）。只认数字：
# 回显的命令行里也有同样的前缀，后面跟的是 shell 代码
field() {
    printf '%s\n' "$CUR_OUT" | tr -d '\r' | grep -aoE -- "$1[0-9]+" |
        sed -n "${2:-1}p" | cut -c$((${#1} + 1))-
}

# guest 里定义 scistat：打印 SCI 的中断计数（各 CPU 之和）和流控类型，以及
# ff_pwr_btn 计数。单独一条命令定义成 shell 函数，因为每条命令不能超过串口
# RX FIFO 能装下的长度（guest_shell.py 会拒绝）
SCI_STAT_DEF='scistat() { awk '"'"'$1=="9:"{s=0;for(i=2;i<=NF&&$i~/^[0-9]+$/;i++)s+=$i;print "SCI-COUNT-"s" SCI-FLOW-"$(NF-1)" SCI-NAME-"$NF}'"'"' /proc/interrupts; read n x < /sys/firmware/acpi/interrupts/ff_pwr_btn; echo FF-$n; }'

# 一轮按键：guest 里先开始监听（evkey 返回时设备已经打开），host 按下
# 按钮，guest 读 FIFO（阻塞到两个按键事件收齐），再打印计数
press_round() {
    local n="$1"
    STEPS+=(
        --cmd "mkfifo /tmp/pb$n && evkey 'Power Button' 2 /tmp/pb$n"
        --host "$CLIENT $SOCK system_powerdown"
        --cmd "cat /tmp/pb$n | sed 's/^/ROUND$n /'; scistat"
    )
}

# ------------------------------------------------------------------
busybox_tests() {
local mode c0 c1 c2

for mode in on split; do
head1 "BusyBox guest，--kernel-irqchip $mode"

STEPS=(--cmd "$SCI_STAT_DEF"
       --cmd "ls /sys/bus/acpi/devices | grep -c LNXPWRBN | sed 's/^/PWRBTN-DEVS-/'; scistat"
       --cmd "echo GLOCK-ERRORS-\$(dmesg | grep -ac 'Global Lock\|GlobalLock')")
press_round 1
press_round 2
run_case "按两次电源按钮：每次一个 KEY_POWER 按下/松开，SCI 正好一次（$mode）" \
    python3 "$GUEST_SHELL" --timeout 120 "${STEPS[@]}" \
        --stop 'poweroff -f' \
        -- "$VMM_BIN" --mode linux --kernel-irqchip "$mode" --smp 2 --mem 256M \
              --monitor "$SOCK" --initrd "$INITRD" "$VMLINUX"
expect_rc 0
# FADT 声明的是固定功能按钮：只有一个 LNXPWRBN，没有 PNP0C0C 之类的设备
expect_has "PWRBTN-DEVS-1"
# FACS 里有全局锁、GBL_EN 能写进去：ACPICA 不报 "No response from Global
# Lock hardware"（drivers/acpi/acpica/evglock.c）
expect_has "GLOCK-ERRORS-0"
# 默认（无 ISO）的 SCI 是电平触发
expect_has "SCI-FLOW-9-fasteoi"
expect_has "SCI-NAME-acpi"
# monitor 的回复：按下时 guest 已打开 PWRBTN_EN（bit 8）和 GBL_EN（bit 5），SCI 拉高
expect_has "power button pressed: PM1_STS=0x0100 PM1_EN=0x0120 SCI=1"
expect_has "acpi: power button pressed (PM1_STS=0x0100 PM1_EN=0x0120 SCI=1)"
expect_not "PWRBTN_EN is clear"
# 第二次按下前 guest 已清掉状态位，SCI 是新拉起的一次，不是一直挂着
expect_has "presses since reset: 2, SCI raised 2 times"
expect_has "ROUND1 key $KEY_POWER 1"
expect_has "ROUND1 key $KEY_POWER 0"
expect_has "ROUND2 key $KEY_POWER 1"
expect_has "ROUND2 key $KEY_POWER 0"
expect_not "evkey: timeout"
c0="$(field SCI-COUNT- 1)"; c1="$(field SCI-COUNT- 2)"; c2="$(field SCI-COUNT- 3)"
check "SCI count goes up by exactly one per press: $c0 -> $c1 -> $c2" \
    test -n "$c2" -a "$c1" = "$((c0 + 1))" -a "$c2" = "$((c0 + 2))"
check "ff_pwr_btn counts the presses: $(field FF- 1) $(field FF- 2) $(field FF- 3)" \
    test "$(field FF- 1)" = 0 -a "$(field FF- 2)" = 1 -a "$(field FF- 3)" = 2
# busybox 的 poweroff -f 走内核 acpi_power_off：SLP_TYP=5|SLP_EN 写 PM1_CNT
expect_re 'guest requested poweroff via ACPI S5 \(port 0x604, value 0x[0-9a-f]+\)'
expect_not "TIMEOUT"
end_case
done

head1 "guest 没有打开 PWRBTN_EN"

# 没有 ACPI 时 guest 的 poweroff 只能停机，VMM 由 monitor 的 quit 结束
run_case "acpi=off：按钮只锁存 PWRBTN_STS，不发 SCI，回复说明原因" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --cmd 'echo acpi-$(ls /sys/firmware/acpi 2>/dev/null | wc -l)' \
        --host "$CLIENT $SOCK system_powerdown" \
        --cmd 'echo still-alive-$((40+2))' \
        --stop-host "$CLIENT $SOCK quit" \
        -- "$VMM" --mode linux --smp 1 --mem 256M --monitor "$SOCK" \
              --append acpi=off --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "acpi-0"
expect_has "power button pressed: PM1_STS=0x0100 PM1_EN=0x0000 SCI=0"
expect_has "PWRBTN_EN is clear: the guest has not enabled the fixed power button, no SCI raised"
expect_has "presses since reset: 1, SCI raised 0 times"
expect_has "still-alive-42"
expect_not "TIMEOUT"
end_case
}

# ------------------------------------------------------------------
# 完整发行版：logind 打开 Power Button 的 evdev，KEY_POWER 的默认动作
# （HandlePowerKey=poweroff）是正常关机，最后由内核写 \_S5。这里不需要
# 网络，只挂系统盘。
arch_tests() {
local f disk uuid
head1 "Arch guest：systemd-logind 按电源键关机"
for f in disk.img vmlinuz-linux initramfs-linux.img; do
    if [ ! -f "$IMG/$f" ]; then
        skip_case "Arch guest 用例" "$IMG/$f missing: run tools/build_arch_image.sh (needs network) or set ARCH_IMAGE=/path"
        return
    fi
done
mkdir -p "$ROOT/build"
disk="$(mktemp "$ROOT/build/acpi-events.XXXXXX.img")"
cp --sparse=always "$IMG/disk.img" "$disk"
uuid="$(blkid -p -o value -s UUID "$disk")"

run_case "logind 收到 KEY_POWER，systemd 正常关机，经 S5 退出" \
    python3 "$GUEST_SHELL" --timeout 300 --login root \
        --cmd "journalctl -b -u systemd-logind --no-pager | grep -o 'Watching system buttons on /dev/input/event[0-9]* (Power Button)'" \
        --stop-host "$CLIENT $SOCK system_powerdown" \
        -- "$VMM" --mode linux --smp 2 --mem 1G --monitor "$SOCK" \
              --device ahci,drive=d0 --drive id=d0,file="$disk" \
              --initrd "$IMG/initramfs-linux.img" \
              --append "root=UUID=$uuid rw ignore_loglevel" "$IMG/vmlinuz-linux"
rm -f "$disk"
expect_rc 0
expect_re 'Watching system buttons on /dev/input/event[0-9]+ \(Power Button\)'
expect_has "power button pressed: PM1_STS=0x0100 PM1_EN=0x0120 SCI=1"
expect_has "reboot: Power down"
expect_re 'guest requested poweroff via ACPI S5 \(port 0x604, value 0x[0-9a-f]+\)'
expect_not "TIMEOUT"
end_case
}

auto_tests() {
if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "BusyBox guest 用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
else
    if initrd_ready "$INITRD"; then
        busybox_tests
    else
        skip_case "initramfs" "tools/build_initramfs.sh failed"
    fi
fi
arch_tests
}

manual_guide() {
guide "手动按电源按钮" \
"./build/vmm --mode linux --mem 256M --monitor /tmp/mon.sock \\
    --initrd build/initramfs.cpio.gz guest-kernel/vmlinux
# guest 里
cat /sys/firmware/acpi/interrupts/ff_pwr_btn
# 另一个终端
python3 tests/monitor_client.py /tmp/mon.sock system_powerdown" \
"monitor 回复 PM1_STS=0x0100 PM1_EN=0x0120 SCI=1；guest 里 ff_pwr_btn 的计数加一，
/proc/interrupts 里 IRQ 9（acpi，9-fasteoi）加一。BusyBox 里没有守护进程处理
按键，guest 继续运行；Arch guest 会正常关机，VMM 打印 guest requested poweroff
via ACPI S5 后退出"
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
