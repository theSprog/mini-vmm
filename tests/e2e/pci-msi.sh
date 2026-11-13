#!/usr/bin/env bash
# tests/e2e/pci-msi.sh — 里程碑 pci-msi 的验收
#
#   AHCI 打开 MSI 后 guest 走 PCI-MSI，读盘的每次完成都经 MSI 送达
#   guest 运行中关掉 MSI 回到 INTx、再打开切回 MSI，两种方式下读盘都正确
#   guest 把 MSI 地址或数据写坏：VMM 报出 BDF、地址、数据和原因，不静默丢弃
#
# 用法：
#   ./tests/e2e/pci-msi.sh       全部
#   ./tests/e2e/pci-msi.sh -a    只跑自动用例
#   ./tests/e2e/pci-msi.sh -m    只看人工指引
#
# capability 的布局、MSI Enable 与 INTx 的互斥、消息格式检查由单元测试
# pci_bus_test 验证，AHCI 什么时候发消息（AHCI 1.3.1 第 10.7.2.1 节图 22）
# 由 ahci_test 验证；这里验证没改过的 Linux 驱动真的把它们用起来了。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

PCIDEV=/sys/bus/pci/devices/0000:00:01.0
DRV=/sys/bus/pci/drivers/ahci
SDATTR=/sys/class/scsi_disk/0:0:0:0

BOOT_OUT=""
BOOT_RC=0

boot() {
    BOOT_OUT="$(python3 "$GUEST_SHELL" --timeout 90 "$@" 2>&1)"
    BOOT_RC=$?
}

check_case() {
    CUR_NAME="$1"
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=0
    CUR_OUT="$BOOT_OUT"
    CUR_RC=$BOOT_RC
}

# 一轮读盘：前后各读一次 ahci 那一行的中断计数，中间按 4 KiB 直读 1 MiB
# （256 条命令，每条完成至少一次中断）。打印 <P>-SHA（读到的内容）、
# <P>-DELTA（计数增量够不够 256）、<P>-IRQ（/proc/interrupts 的那一行）。
# 标记都由 guest 的命令替换展开生成，回显的命令行里只有 $(...)。
# 一条命令不能超过 guest 串口 FIFO 能装下的长度，所以拆成几条，变量留在
# 同一个 shell 里。命令追加到数组 ARGS。
read_round() {
    local p="$1"
    ARGS+=(--cmd "a=\$(awk '/ahci/ {print \$2}' /proc/interrupts)")
    ARGS+=(--cmd "echo $p-SHA-\$(dd if=/dev/sda bs=4k count=256 iflag=direct 2>/dev/null | sha256sum | cut -c1-64)")
    ARGS+=(--cmd "b=\$(awk '/ahci/ {print \$2}' /proc/interrupts); echo $p-DELTA-\$([ \$((b - a)) -ge 256 ] && echo enough || echo \$((b - a)))")
    ARGS+=(--cmd "echo $p-IRQ-\$(grep -a ahci /proc/interrupts | tr -s ' ')")
}

# 解绑再绑定 ahci 驱动，等新的 sda 出现。msi_bus 决定驱动 probe 时能不能
# 申请 MSI（drivers/pci/pci-sysfs.c msi_bus_store 只影响以后的驱动）。
# 解绑时驱动 pci_free_irq_vectors 关掉 MSI Enable，这是 guest 在运行中把
# 设备从 MSI 切回 INTx 的真实路径。等待的信号是 /sys/block/sda 消失、再
# 重新出现；循环没有自己的上限，卡住时由 guest_shell 的 --timeout 兜底。
rebind() {
    ARGS+=(--cmd "echo $1 > $PCIDEV/msi_bus; echo 0000:00:01.0 > $DRV/unbind; until [ ! -e /sys/block/sda ]; do sleep 0.1; done")
    ARGS+=(--cmd "echo 0000:00:01.0 > $DRV/bind; until [ -e /sys/block/sda ]; do sleep 0.1; done")
}

# ------------------------------------------------------------------
auto_tests() {
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

switch_tests
bad_msi_tests
}

switch_tests() {
head1 "MSI 与 INTx 之间切换"

local sha
head -c 64M /dev/urandom > "$WORK/disk.img"
sha="$(head -c 1M "$WORK/disk.img" | sha256sum | cut -c1-64)"

ARGS=()
read_round A
rebind 0
read_round B
rebind 1
read_round C
boot "${ARGS[@]}" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem 512M --drive id=d0,file="$WORK/disk.img" \
          --device ahci,drive=d0 --initrd "$INITRD" "$VMLINUX"

check_case "guest 启动、两次重新绑定驱动后正常关机"
expect_rc 0
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
expect_not "not delivered"
end_case

# Linux 先写好地址和数据再置 MSI Enable（drivers/pci/msi/msi.c
# msi_capability_init），所以打开那一刻记下的就是真正使用的消息
check_case "MSI capability 被驱动打开，guest 用 PCI-MSI，每次完成都送达"
expect_has "pci 00:01.0 ahci [1234:0003] class 010601, MSI capable"
expect_re '^\[INFO\] pci 00:01\.0 ahci: MSI enabled, address 0xfee[0-9a-f]{5} data 0x00[1-9a-f][0-9a-f]$'
expect_re '^A-IRQ- ?[0-9]+: [0-9]+ PCI-MSI-0000:00:01\.0 0-edge ahci\[0000:00:01\.0\]'
expect_has "A-DELTA-enough"
expect_has "A-SHA-$sha"
end_case

# 关掉 MSI 的那一刻，ahci 回到 INTA：_PRT 给出的 GSI 17，电平触发
check_case "guest 关掉 MSI 后回到 INTx，读盘照常"
expect_has "pci 00:01.0 ahci: MSI disabled, interrupts go to INTA"
expect_re '^B-IRQ- ?17: [0-9]+ IO-APIC 17-fasteoi ahci\[0000:00:01\.0\]'
expect_has "B-DELTA-enough"
expect_has "B-SHA-$sha"
end_case

check_case "重新允许 MSI 后切回 PCI-MSI，读盘照常"
expect_re '^C-IRQ- ?[0-9]+: [0-9]+ PCI-MSI-0000:00:01\.0 0-edge ahci\[0000:00:01\.0\]'
expect_has "C-DELTA-enough"
expect_has "C-SHA-$sha"
end_case
}

# guest 用 sysfs 的 config 文件改写 MSI capability 里的一个字节，然后发一条
# 读命令。消息发不出去，命令的完成中断就到不了，请求停在 inflight 里；
# 等待的信号就是 inflight 变成非零，之后 VMM 日志里应当有这次丢弃的原因。
#
# 中断丢了之后 IS 一直是置位的，后面的完成不再改变 IS，也就不再发消息
# （AHCI 1.3.1 图 22），驱动要等 30 秒超时才恢复，所以一种坏消息单独启动
# 一次。关机时 sd_shutdown 会发 FLUSH 和 STANDBY 并等它们的中断，同样要
# 等超时；事先把写缓存改成 write through、关掉 start/stop 管理
# （drivers/scsi/sd.c sd_shutdown），关机就不再发命令。
bad_msi() {
    local name="$1" off="$2" byte="$3" want="$4"

    boot \
        --cmd "echo 'write through' > $SDATTR/cache_type; echo 0 > $SDATTR/manage_system_start_stop; echo 0 > $SDATTR/manage_shutdown" \
        --cmd "printf '\\$byte' | dd of=$PCIDEV/config bs=1 seek=$off conv=notrunc 2>/dev/null" \
        --cmd "dd if=/dev/sda of=/dev/null bs=4k count=1 skip=100 iflag=direct 2>/dev/null &" \
        --cmd "until [ \"\$(awk '{print \$1 + \$2}' /sys/block/sda/inflight)\" -ge 1 ]; do sleep 0.1; done; echo STUCK-\$(cat /sys/block/sda/inflight | tr -s ' ')" \
        --stop 'poweroff -f' \
        -- "$VMM" --mode linux --mem 512M --drive id=d0,file="$WORK/disk.img" \
              --device ahci,drive=d0 --initrd "$INITRD" "$VMLINUX"

    check_case "$name"
    expect_rc 0
    expect_has "guest requested poweroff via ACPI S5"
    expect_re '^STUCK- ?[1-9]'
    expect_has "$want"
    end_case
}

bad_msi_tests() {
head1 "非法的 MSI 地址和数据"

truncate -s 64M "$WORK/disk.img"

# Linux 给出的消息是地址 0xfee01004、数据 0x0021 这类（向量随分配变化，
# 所以只改不随分配变化的那个字节）。偏移：capability 在 0x40，地址在
# +4（0x44-0x47），数据在 +0xC（0x4C）。
bad_msi "向量写成 0x05（异常向量）：报出原因，不投递" 76 005 \
    "data 0x0005 not delivered: vector outside 0x10-0xfe (1 with this address/data, 1 in total)"
bad_msi "地址最高字节写成 0（不在中断窗口里）：报出原因，不投递" 71 000 \
    "not delivered: address is outside the interrupt window 0xfee00000-0xfeefffff (1 with this address/data, 1 in total)"
bad_msi "目标 ID 写成 0（没有 vCPU 匹配）：KVM 没有收下，报出原因" 69 000 \
    "not delivered: no vCPU accepted it"
}

manual_guide() {
guide "看 guest 用的是哪种中断" \
"grep ahci /proc/interrupts                 # PCI-MSI-... 或 IO-APIC 17-fasteoi
echo 0 > /sys/bus/pci/devices/0000:00:01.0/msi_bus   # 之后重新绑定驱动就回到 INTx" \
"VMM 日志里 'MSI enabled, address ... data ...' 是 guest 打开 MSI 时写进去的消息"
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
