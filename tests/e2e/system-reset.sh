#!/usr/bin/env bash
# tests/e2e/system-reset.sh — system-reset 里程碑：guest 请求复位时整机回到上电状态再启动
#
# 判据：
#   - 每次启动都重新进入 shell，所有 AP 都重新上线（/sys/devices/system/cpu/online
#     是 0-N）：AP 的 MP_STATE 没回到 UNINITIALIZED 时，新内核发 INIT/SIPI 唤不醒它；
#   - 每次启动的设备探测结果一致：PCI/平台/serio 设备列表、磁盘容量、
#     /proc/interrupts 的中断源名字取一个哈希，几次启动的哈希必须相同。设备
#     状态或中断线电平没复位时，探测会走到不同的分支（驱动报错、中断源缺失）；
#   - 磁盘镜像在复位前后保持打开：第一次启动写进盘的标记，之后每次启动都读得到；
#   - --no-reboot 时复位请求让 VMM 退出，panic=-1 不会无限重启。
# gdb 跨越复位的用例在 gdbstub suite 里，那里有 gdb 的前置检查。
#
# 用法：./tests/e2e/system-reset.sh [-a | -m]

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SMP=4
REBOOTS=3

# 每次启动打印一行 BOOT：在线 CPU、探测结果的哈希、盘上的标记。标记由命令
# 替换展开，回显的命令行里只有 $(...)，不会被误认
BOOT_LINE='echo BOOT cpus=$(cat /sys/devices/system/cpu/online) probe=$( (ls /sys/bus/pci/devices /sys/bus/platform/devices /sys/bus/serio/devices; cat /sys/block/sda/size; awk "{print \$NF}" /proc/interrupts) | md5sum | cut -c1-8) disk=$(head -c 6 /dev/sda)'

# 当前用例输出里 BOOT 行的个数必须是 $1，而且全部相同
expect_boots_identical() {
    local lines n uniq
    lines="$(printf '%s' "$CUR_OUT" | grep -aoE 'BOOT cpus=[^ ]+ probe=[0-9a-f]{8} disk=[A-Za-z0-9]*')"
    n="$(printf '%s\n' "$lines" | grep -c BOOT)"
    uniq="$(printf '%s\n' "$lines" | sort -u)"
    if [ "$n" -ne "$1" ] || [ "$(printf '%s\n' "$uniq" | wc -l)" -ne 1 ]; then
        printf '   %sFAIL%s 期望 %d 行相同的 BOOT 行，实际 %d 行：\n%s\n' \
            "$C_RED" "$C_RST" "$1" "$n" "$lines"
        CUR_BAD=1
    fi
}

auto_tests() {

head1 "命令行约束"

run_case "--no-reboot 只能和 --mode linux 一起用" \
    "$VMM" --mode long64 --no-reboot "$ROOT/build/serial64.bin"
expect_rc 1
expect_has "--no-reboot requires --mode linux"
end_case

if [ -z "${VMLINUX:-}" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "guest 用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi
if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

head1 "整机复位"

truncate -s 64M "$WORK/disk.img"
local steps=(--cmd 'printf MARK42 | dd of=/dev/sda conv=fsync 2>/dev/null')
local i
for i in $(seq "$REBOOTS"); do
    steps+=(--cmd "$BOOT_LINE" --cmd 'reboot -f')
done
steps+=(--cmd "$BOOT_LINE")

run_case "--smp $SMP：连续 $REBOOTS 次 reboot -f，每次启动 AP 全部上线、探测结果一致" \
    python3 "$GUEST_SHELL" --timeout 180 "${steps[@]}" --stop 'poweroff -f' \
    -- "$VMM" --mode linux --smp "$SMP" --mem 512M \
          --drive id=d0,file="$WORK/disk.img" --device ahci,drive=d0 \
          --device pci-test --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_boots_identical $((REBOOTS + 1))
expect_has "BOOT cpus=0-$((SMP - 1)) "
expect_has "disk=MARK42"
expect_has "reset: guest reset #$REBOOTS done"
expect_not "guest reset #$((REBOOTS + 1))"
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
expect_not "stopped with error"
expect_not "reset: failed"
end_case

# sysrq-b 走 emergency_restart：不先停其它 CPU，直接经 i8042 复位，复位时
# 每个 AP 都在跑忙循环。AP 的状态没有回到上电状态（UNINITIALIZED）时，它在
# 新装载的内核上接着执行上一次启动的指令，新内核等不到它报到。reboot -f
# 测不出这一点：那条路径先让 AP 停在 hlt 里，新内核的 INIT 会把 AP 复位
run_case "sysrq-b：AP 正在运行时复位，下一次启动所有 AP 仍然上线" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --cmd 'for i in 1 2 3; do (while :; do :; done) & done' \
        --cmd 'echo b > /proc/sysrq-trigger' \
        --cmd 'echo AFTER-SYSRQ cpus=$(cat /sys/devices/system/cpu/online)' \
        --stop 'poweroff -f' \
    -- "$VMM" --mode linux --smp "$SMP" --mem 512M --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "sysrq: Resetting"
expect_has "reset: guest reset #1 done"
expect_has "AFTER-SYSRQ cpus=0-$((SMP - 1))"
expect_not "Kernel panic"
expect_not "stopped with error"
end_case

# 设备寄存器回到上电值：Linux 启动时会重新初始化它驱动的设备，看不出设备
# 有没有复位，所以改的是没有驱动碰的地方：pci-test 的 scratch 寄存器和
# 配置空间的 cache line size（规范第 6.2.4 节：软件写、硬件只是记住，复位后为 0）
P=/sys/bus/pci/devices/0000:00:01.0
SET_B="B=\$(head -1 $P/resource | cut -c1-18)"
CLS="\$(dd if=$P/config bs=1 skip=12 count=1 2>/dev/null | od -An -tx1 | tr -d ' ')"
run_case "设备寄存器和 PCI 配置空间回到上电值" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --cmd "$SET_B; devmem \$((B + 4)) 32 0x12345678; printf '\\x10' | dd of=$P/config bs=1 seek=12 conv=notrunc 2>/dev/null" \
        --cmd "echo BEFORE-\$(devmem \$((B + 4)))-$CLS" \
        --cmd 'reboot -f' \
        --cmd "$SET_B; echo AFTER-\$(devmem \$((B + 4)))-$CLS" \
        --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M --device pci-test --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "BEFORE-0x12345678-10"
expect_has "AFTER-0x00000000-00"
expect_has "reset: guest reset #1 done"
end_case

if [ -n "${BZIMAGE:-}" ] && [ -f "$BZIMAGE" ]; then
    # bzImage 的内核由 guest 里的解压器写到内存里，复位后重新装载的是压缩
    # 镜像，解压器在上一次启动留下的内存上再解压一次
    run_case "bzImage：reboot -f 之后重新解压并进入 shell" \
        python3 "$GUEST_SHELL" --timeout 120 --cmd 'reboot -f' \
            --cmd 'echo SECOND-BOOT-$((6*7))' --stop 'poweroff -f' \
        -- "$VMM" --mode linux --smp 2 --mem 512M --initrd "$INITRD" "$BZIMAGE"
    expect_rc 0
    expect_has "reset: guest reset #1 done"
    expect_has "SECOND-BOOT-42"
    expect_has "guest requested poweroff via ACPI S5"
    expect_not "Kernel panic"
    end_case
else
    skip_case "bzImage 复位" "run tools/build_guest_kernel.sh or set BZIMAGE=/path/to/bzImage"
fi

head1 "--no-reboot"

run_case "reboot -f 让 VMM 退出" \
    python3 "$GUEST_SHELL" --timeout 90 --stop 'reboot -f' \
    -- "$VMM" --mode linux --no-reboot --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "guest requested reset via i8042"
expect_has "--no-reboot: stopping instead of resetting"
expect_not "guest reset #"
end_case

# panic=-1（默认命令行）让 panic 立刻请求复位，--no-reboot 时只启动一次
run_case "panic 后退出，不会无限重启" \
    "$VMM" --mode linux --no-reboot --initrd "$INITRD" \
    --append "rdinit=/nonexistent" "$VMLINUX"
expect_rc 0
expect_has "Kernel panic - not syncing: VFS: Unable to mount root fs"
expect_has "--no-reboot: stopping instead of resetting"
expect_not "guest reset #"
end_case
}

manual_guide() {
guide "观察整机复位" \
"build/vmm --mode linux --smp 4 --mem 512M --initrd build/initramfs.cpio.gz guest-kernel/vmlinux
[guest /]# reboot -f" \
"日志依次出现 'guest requested reset via i8042' 和 'reset: guest reset #1 done'，
内核从 [    0.000000] 重新打印启动信息，回到 shell；Ctrl-A x 退出"
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

main "$@"
