#!/usr/bin/env bash
# tests/e2e/pci-bus.sh — 里程碑 pci-bus 的验收
#
#   0xCF8/0xCFC 配置空间、host bridge、BAR 探测与路由、DSDT 的 PCI0 窗口、
#   INTx 路由（ACPI _PRT 与 MP table 一致）
#
# 用法：
#   ./tests/e2e/pci-bus.sh       全部
#   ./tests/e2e/pci-bus.sh -a    只跑自动用例
#   ./tests/e2e/pci-bus.sh -m    只看人工指引
#
# 被测设备是 --device pci-test 挂上的 pci-test（1234:0002，src/devices/pci_test.c）：
# BAR0 是 4KiB MEM32，BAR1 是 16 字节 IO，寄存器 0x0 是魔数 0x74736574，
# 0x4 是两个 BAR 共用的 scratch，0x8 读出访问走的是哪个 BAR。
#
# 访问 BAR 用 initramfs 里的 /sbin/devmem（tools/guest/devmem.c）：x86 的
# /dev/mem 的 read()/write() 拒绝非 RAM 地址，只能 mmap。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
MEM=512M
D=/sys/bus/pci/devices/0000:00:01.0

# BAR 地址是 VMM 按固件职责预分配的：MMIO 从 MMIO hole 起点 0xC0000000，
# IO 从 0x1000 起，见 src/pci/pci.c。MOVED 是测试改写 BAR0 后的新地址。
BAR0=0xc0000000
MOVED=0xc0100000

# 同一次启动的输出给多个用例断言，每个用例只看自己关心的那几行
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

# ------------------------------------------------------------------
auto_tests() {

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "guest 内核用例" "run tools/build_guest_kernel.sh or set VMLINUX=/path/to/vmlinux"
    return
fi

# 早于 devmem 生成的 initramfs 里没有它，按构建产物过期处理，重新生成
if [ ! -f "$INITRD" ] || ! gzip -dc "$INITRD" | grep -qa 'sbin/devmem'; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

head1 "ACPI 启动：枚举、BAR、中断"

# 标记串（IRQ-、MAGIC- 等）由 guest 的命令替换展开生成，回显的命令行里
# 只有 $(...)，不会误匹配
boot \
    --cmd "dmesg | grep -aE 'PCI|pci |PCI0|root bus|BAR|GSI'" \
    --cmd "echo ACPI-ERRORS-\$(dmesg | grep -ac 'ACPI Error')" \
    --cmd 'lspci' \
    --cmd "echo IRQ-BEFORE-\$(cat $D/irq)" \
    --cmd "echo 1 > $D/enable; echo IRQ-AFTER-\$(cat $D/irq)" \
    --cmd "echo MAGIC-\$(devmem $BAR0)-\$(devmem $((BAR0 + 8)))" \
    --cmd "devmem $((BAR0 + 4)) 32 0x12345678" \
    --cmd "echo IO-\$(dd if=$D/resource1 bs=4 count=3 2>/dev/null | od -An -tx4)" \
    --cmd "printf '\\x00\\x00\\x10\\xc0' | dd of=$D/config bs=4 seek=4 conv=notrunc 2>/dev/null" \
    --cmd "echo MOVED-\$(devmem $MOVED)-OLD-\$(devmem $BAR0)" \
    --cmd "printf '\\x00\\x00' | dd of=$D/config bs=2 seek=2 conv=notrunc 2>/dev/null" \
    --cmd "echo OFF-\$(devmem $MOVED)-\$(dd if=$D/resource1 bs=4 count=1 2>/dev/null | od -An -tx4)" \
    --stop 'poweroff -f' \
    -- "$VMM" --mode linux --mem "$MEM" --device pci-test --exit-stats - \
          --initrd "$INITRD" "$VMLINUX"

check_case "guest 启动并正常关机"
expect_rc 0
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
expect_not "stopped with error"
end_case

# 没有 0xCF8/0xCFC 时内核打印 "PCI: Fatal: No config space access function found"
check_case "conf1 配置空间：内核选用 type 1，lspci 看到 host bridge 和 pci-test"
expect_has "PCI: Using configuration type 1 for base access"
expect_has "00:00.0 Class 0600: 1234:0001"
expect_has "00:01.0 Class ff00: 1234:0002"
expect_not "No config space access function"
end_case

# 窗口来自 DSDT 里 PCI0 的 _CRS（src/boot/acpi.c）。资源不在窗口里时内核会
# 打印 "BAR 0: assigned" 或 "no space for" 并重新分配
check_case "DSDT 描述 PCI0 的窗口，BAR 原样收下，不重新分配"
expect_has "ACPI: PCI Root Bridge [PCI0] (domain 0000 [bus 00-ff])"
expect_has "root bus resource [io  0x0000-0x0cf7 window]"
expect_has "root bus resource [io  0x0d00-0xffff window]"
expect_has "root bus resource [mem 0xc0000000-0xfebfffff window]"
expect_has "reg 0x10: [mem 0xc0000000-0xc0000fff]"
expect_has "reg 0x14: [io  0x1000-0x100f]"
expect_not ": assigned ["
expect_not "no space for"
expect_has "ACPI-ERRORS-0"
end_case

check_case "BAR 路由：MMIO 和 IO 落到同一个设备，改写 BAR 后跟着走，关 decode 后都不响应"
expect_has "MAGIC-0x74736574-0x00000000"
expect_has "IO- 74736574 12345678 00000001"
expect_has "MOVED-0x74736574-OLD-0xFFFFFFFF"
expect_has "OFF-0xFFFFFFFF- ffffffff"
expect_re '^  pci-test\.bar0 +MMIO +[1-9][0-9]* +[1-9][0-9]*$'
expect_re '^  pci-test\.bar1 +PIO +[1-9][0-9]* +0$'
end_case

# ACPI 模式下 IRQ 在 pci_enable_device 时才经 _PRT 分配，之前 sysfs 里是 0
# 或 255 都合法；GSI 17 = 16 + (slot 1 + INTA - 1) % 4，见 pci_intx_gsi()
check_case "INTx：ACPI 启动经 _PRT 分到 GSI 17"
expect_has "PCI: Using ACPI for IRQ routing"
expect_has "IRQ-AFTER-17"
end_case

head1 "acpi=off 启动：中断路由改由 MP table 给出"

# acpi=off 时没有 ACPI S5，用 i8042 复位退出
boot \
    --cmd "dmesg | grep -aE 'PCI->APIC|INT A'" \
    --cmd "echo 1 > $D/enable; echo IRQ-AFTER-\$(cat $D/irq)" \
    --stop 'reboot -f' \
    -- "$VMM" --mode linux --mem "$MEM" --device pci-test --append acpi=off \
          --initrd "$INITRD" "$VMLINUX"

check_case "MP table 的 PCI 中断项与 _PRT 一致：同样是 GSI 17"
expect_rc 0
expect_has "guest requested reset via i8042"
expect_has "PCI->APIC IRQ transform: INT A -> IRQ 17"
expect_has "IRQ-AFTER-17"
expect_not "Kernel panic"
end_case

head1 "命令行约束"

run_case "--device 只能和 --mode linux 一起用" \
    "$VMM" --mode long64 --device pci-test "$ROOT/build/serial64.bin"
expect_rc 1
expect_has "--device requires --mode linux"
end_case

# 参数错误必须在创建 VM 之前报出：输出里不能出现 KVM 初始化的日志
run_case "--device 的属性名拼错：启动前报错并列出合法属性" \
    "$VMM" --mode linux --device pci-test,bar=1 --initrd "$INITRD" "$VMLINUX"
expect_rc 1
expect_has "--device pci-test,bar=1: unknown property 'bar' (valid: driver)"
expect_not "KVM API"
end_case

run_case "--device 的 driver 不存在" \
    "$VMM" --mode linux --device pci-tset --initrd "$INITRD" "$VMLINUX"
expect_rc 1
expect_has "unknown driver 'pci-tset' (see --device help)"
expect_not "KVM API"
end_case

run_case "--device help 列出可用的 driver" "$VMM" --device help
expect_rc 0
expect_re '^  pci-test +PCI bus test device'
end_case

}

manual_guide() {
guide "看 guest 怎样探测 BAR 大小" \
"./build/vmm --mode linux --mem $MEM --device pci-test --trace-pio /tmp/pio.txt \\
    --initrd $INITRD \$VMLINUX
grep -n -A26 'W4 80000804' /tmp/pio.txt | sed -n '/W2 0000/,/W2 0003/p'" \
"先往 COMMAND（0x04）写 0 关掉 decode，再对 BAR0（0x10）依次：读原值
c0000000、写 ffffffff、读回 fffff000、写回原值，最后恢复 COMMAND 为 0003。
读回值低位 0 的个数就是大小：fffff000 -> 4KiB。关 decode 是因为全 1 那一刻
BAR 指向 0xfffff000，打开着 decode 就会和那里的其他设备冲突"
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
