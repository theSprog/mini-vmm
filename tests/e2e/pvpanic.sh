#!/usr/bin/env bash
# tests/e2e/pvpanic.sh — 里程碑 pvpanic 的验收
#
#   --device pvpanic 在端口 0x505 挂上 panic 通知设备，DSDT 里用 _HID
#   "QEMU0001" 描述它；guest 的 pvpanic-mmio 驱动绑定后，panic 时往端口写
#   PANICKED，VMM 记录事件并以退出码 3 退出，和正常关机、--no-reboot 下的
#   复位（退出码 0）区分开。
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/e2e/pvpanic.sh       全部
#   VMLINUX=/path/to/vmlinux ./tests/e2e/pvpanic.sh -a    只跑自动用例
#   ./tests/e2e/pvpanic.sh -m                             只看人工指引
#
# 判据的来源：
#   端口语义：QEMU docs/specs/pvpanic.rst（读返回设备认识的位，写时忽略不
#   认识的位），位定义 Linux include/uapi/misc/pvpanic.h（bit 0 PANICKED、
#   bit 1 CRASH_LOADED、bit 2 SHUTDOWN）。
#   驱动行为：drivers/misc/pvpanic/pvpanic-mmio.c 把 capability 设成“驱动
#   认识的位 & 读端口的值”，events 初值等于 capability，两者在 sysfs 里以
#   十六进制给出。7.x 的驱动三个位都认识（capability 7），更早的驱动没有
#   SHUTDOWN（capability 3）。pvpanic.c 的 panic notifier 在没有
#   装载 kdump 内核时发 PANICKED，并且只发 events 里打开的位。
#   SHUTDOWN 挂在 SYS_OFF_PRIO_LOW 的 sys_off handler 上，ACPI 的关机
#   handler 优先级是 SYS_OFF_PRIO_FIRMWARE（include/linux/reboot.h），S5 能
#   用时 guest 根本不会发 SHUTDOWN。
#   对照组：同一个 sysrq-c，不挂设备或者 guest 把 events 清零时，panic 只
#   能走 panic=-1 的复位路径，--no-reboot 下退出码 0。这证明退出码 3 来自
#   驱动的那次端口写，而不是 VMM 从串口输出里认出了 "Kernel panic"。
#   guest 侧直接读写端口用 /dev/port（CONFIG_DEVPORT，偏移就是端口号，每个
#   字节一次 inb/outb），不经过驱动，用来检验驱动不会发出的取值。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
IMG="${ARCH_IMAGE:-$ROOT/arch-image}"
PORT=1285                                   # 0x505
SYSFS=/sys/bus/platform/devices/QEMU0001:00

# 往端口写一个字节，值用八进制转义（busybox 和 coreutils 的 printf 都认）
outb() { printf "printf '\\\\%s' | dd of=/dev/port bs=1 seek=$PORT count=1 2>/dev/null" "$1"; }

# ------------------------------------------------------------------
busybox_tests() {
head1 "BusyBox guest（驱动编进内核）"

run_case "驱动绑定，端口语义，CRASH_LOADED 不打断 guest，sysrq-c 以退出码 3 结束" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --cmd "echo DRV-\$(basename \$(readlink $SYSFS/driver))" \
        --cmd "echo CAP-\$(cat $SYSFS/capability) EV-\$(cat $SYSFS/events)" \
        --cmd "echo PORT-\$(dd if=/dev/port bs=1 skip=$PORT count=1 2>/dev/null | od -An -tx1 | tr -d ' ')" \
        --cmd "$(outb 370); echo alive-\$((40+1))" \
        --cmd "$(outb 372); echo alive-\$((40+2))" \
        --stop 'echo c > /proc/sysrq-trigger' \
        -- "$VMM" --mode linux --smp 2 --mem 256M --device pvpanic \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 3
expect_has "DRV-pvpanic-mmio"
# capability = 驱动认识的位 & 读端口的值，两边都认识三个位
expect_has "CAP-7 EV-7"
# 设备自己认识三个位，读端口原样给出
expect_has "PORT-07"
# 0xf8 没有一个认识的位：整个写入被忽略，guest 继续
expect_has "pvpanic: ignored write 0xf8 (no recognized event bits)"
expect_has "alive-41"
# 0xfa = 不认识的高位 | CRASH_LOADED：只留下 CRASH_LOADED，guest 继续
expect_has "pvpanic: guest panicked and handles it itself (events 0x02: CRASH_LOADED), guest keeps running"
expect_has "alive-42"
expect_has "Kernel panic - not syncing: sysrq triggered crash"
expect_has "pvpanic: guest panicked (events 0x01: PANICKED), exiting with code 3"
# panic=-1 的复位请求没有机会发出
expect_not "guest requested reset"
expect_not "TIMEOUT"
end_case

head1 "对照组：同一个 panic 不经过 pvpanic"

run_case "不挂设备：DSDT 里没有 QEMU0001，panic 走复位路径，退出码 0" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --cmd 'echo QEMU-DEVS-$(ls /sys/bus/acpi/devices | grep -c QEMU0001)' \
        --stop 'echo c > /proc/sysrq-trigger' \
        -- "$VMM" --mode linux --smp 1 --mem 256M --no-reboot \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "QEMU-DEVS-0"
expect_has "Kernel panic - not syncing: sysrq triggered crash"
expect_has "--no-reboot: stopping instead of resetting"
# 只认 VMM 日志的前缀：输出里有 initramfs 的完整路径，路径里可能恰好含
# pvpanic（mutate.py 的副本目录就以 mutant 名命名）
expect_not "pvpanic:"
expect_not "TIMEOUT"
end_case

run_case "guest 关掉 events：驱动不写端口，panic 走复位路径，退出码 0" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --cmd "echo 0 > $SYSFS/events; echo EV-\$(cat $SYSFS/events)" \
        --stop 'echo c > /proc/sysrq-trigger' \
        -- "$VMM" --mode linux --smp 1 --mem 256M --no-reboot --device pvpanic \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "EV-0"
expect_has "Kernel panic - not syncing: sysrq triggered crash"
expect_has "--no-reboot: stopping instead of resetting"
expect_not "pvpanic: guest panicked"
expect_not "TIMEOUT"
end_case

head1 "SHUTDOWN 事件"

# 驱动只在 ACPI 关机没生效时才发 SHUTDOWN（见文件头），这台机器上造不出
# 这种情形，所以由 guest 直接写端口
run_case "写 SHUTDOWN：VMM 按关机处理，退出码 0" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --stop "$(outb 004)" \
        -- "$VMM" --mode linux --smp 1 --mem 256M --device pvpanic \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 0
expect_has "pvpanic: guest reported shutdown (events 0x04: SHUTDOWN), powering off"
expect_not "TIMEOUT"
end_case

# 驱动不会同时发两个位，这里检验设备头文件里写的优先级：PANICKED 要求
# host 介入，SHUTDOWN 也要求 host 介入，两者同时出现时按 panic 处理
run_case "PANICKED|SHUTDOWN 同时写入：按 panic 处理，退出码 3" \
    python3 "$GUEST_SHELL" --timeout 120 \
        --stop "$(outb 005)" \
        -- "$VMM" --mode linux --smp 1 --mem 256M --device pvpanic \
              --initrd "$INITRD" "$VMLINUX"
expect_rc 3
expect_has "pvpanic: guest panicked (events 0x05: PANICKED|SHUTDOWN), exiting with code 3"
expect_not "TIMEOUT"
end_case

head1 "命令行"

run_case "两个 pvpanic：端口固定，第二个被拒绝" \
    "$VMM" --mode linux --mem 256M --device pvpanic --device pvpanic \
        --initrd "$INITRD" "$VMLINUX"
expect_rc 1
expect_has "--device pvpanic: only one pvpanic device (port 0x505) is supported"
end_case
}

# ------------------------------------------------------------------
# 发行版内核：驱动是模块，靠 udev 按 ACPI modalias（acpi:QEMU0001:）自动
# 加载
arch_tests() {
local f disk uuid
head1 "Arch guest（发行版内核，驱动为模块）"
for f in disk.img vmlinuz-linux initramfs-linux.img; do
    if [ ! -f "$IMG/$f" ]; then
        skip_case "Arch guest 用例" "$IMG/$f missing: run tools/build_arch_image.sh (needs network) or set ARCH_IMAGE=/path"
        return
    fi
done
mkdir -p "$ROOT/build"
disk="$(mktemp "$ROOT/build/pvpanic.XXXXXX.img")"
cp --sparse=always "$IMG/disk.img" "$disk"
uuid="$(blkid -p -o value -s UUID "$disk")"

run_case "模块自动加载，capability 7；poweroff 走 S5，不发 SHUTDOWN" \
    python3 "$GUEST_SHELL" --timeout 300 --login root \
        --cmd "echo DRV-\$(basename \$(readlink $SYSFS/driver)) CAP-\$(cat $SYSFS/capability) EV-\$(cat $SYSFS/events)" \
        --cmd "echo MOD-\$(grep -c '^pvpanic_mmio ' /proc/modules)" \
        --stop 'systemctl poweroff' \
        -- "$VMM" --mode linux --smp 2 --mem 1G --device pvpanic \
              --device ahci,drive=d0 --drive id=d0,file="$disk" \
              --initrd "$IMG/initramfs-linux.img" \
              --append "root=UUID=$uuid rw" "$IMG/vmlinuz-linux"
expect_rc 0
expect_has "DRV-pvpanic-mmio CAP-7 EV-7"
expect_has "MOD-1"
expect_re 'guest requested poweroff via ACPI S5 \(port 0x604, value 0x[0-9a-f]+\)'
expect_not "pvpanic: guest reported shutdown"
expect_not "TIMEOUT"
end_case

run_case "sysrq-c：退出码 3" \
    python3 "$GUEST_SHELL" --timeout 300 --login root \
        --stop 'echo c > /proc/sysrq-trigger' \
        -- "$VMM" --mode linux --smp 2 --mem 1G --device pvpanic \
              --device ahci,drive=d0 --drive id=d0,file="$disk" \
              --initrd "$IMG/initramfs-linux.img" \
              --append "root=UUID=$uuid rw" "$IMG/vmlinuz-linux"
rm -f "$disk"
expect_rc 3
expect_has "Kernel panic - not syncing: sysrq triggered crash"
expect_has "pvpanic: guest panicked (events 0x01: PANICKED), exiting with code 3"
expect_not "TIMEOUT"
end_case
}

auto_tests() {
if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "BusyBox guest 用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
elif [ -f "$(dirname "$VMLINUX")/.config" ] &&
     ! grep -qx 'CONFIG_PVPANIC_MMIO=y' "$(dirname "$VMLINUX")/.config"; then
    # 早于本里程碑构建的 guest 内核没有驱动，用例会以“驱动没绑定”的样子
    # 失败，看不出真正的原因
    skip_case "BusyBox guest 用例" "$VMLINUX was built without CONFIG_PVPANIC_MMIO=y: rerun tools/build_guest_kernel.sh"
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
guide "让 guest panic" \
"./build/vmm --mode linux --mem 256M --device pvpanic \\
    --initrd build/initramfs.cpio.gz guest-kernel/vmlinux; echo exit=\$?
# guest 里
cat /sys/bus/platform/devices/QEMU0001:00/capability
echo c > /proc/sysrq-trigger" \
"capability 是 7；panic 后 VMM 打印 'pvpanic: guest panicked (events 0x01:
PANICKED), exiting with code 3'，shell 里 exit=3。去掉 --device pvpanic 再来一次，
guest 按 panic=-1 复位、重新启动"
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
