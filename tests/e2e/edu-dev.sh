#!/usr/bin/env bash
# tests/e2e/edu-dev.sh — 里程碑 edu-dev 的验收
#
#   Arch guest 用镜像里的 linux-headers 编译 tools/guest/edu 的驱动和 eductl
#   驱动探测 edu 设备：映射 BAR0、申请中断、注册 /dev/edu0
#   MSI 和 INTx 两种中断下各跑 eductl selftest：DMA 双向搬运，经 MMIO 窗口
#     独立核对内容；每一步都等完成中断，中断数和 DMA 完成数按轮数精确相等
#   卸载后模块、字符设备、中断、BAR 占用、Bus Master、MSI Enable 全部归还
#   加载卸载 100 次（MSI/INTx 交替）：每次探测结果一致，DMA tracepoint 记下
#     的一致性分配和释放次数配平；漏掉 dma_free_coherent 的驱动副本在同一个
#     判据下配不平，证明这个判据真能抓到泄漏
#
# 用法：
#   ./tests/e2e/edu-dev.sh       全部
#   ./tests/e2e/edu-dev.sh -a    只跑自动用例
#   ./tests/e2e/edu-dev.sh -m    只看人工指引
#
# 镜像和 host 工具的要求见 tests/edu_guest.sh。
#
# 寄存器语义、DMA 的三种结果、MSI 什么时候发、复位丢弃命令由单元测试
# edu_test 逐条验证；这里验证的是一个真实的 Linux 驱动按规范把设备用起来。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"
source "$ROOT/tests/edu_guest.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

BDF=$EDU_BDF
INTX_GSI=$EDU_INTX_GSI
ROUNDS=20
CYCLES=100
LEAK_CYCLES=10

BOOT_OUT=""
BOOT_RC=0

check_case() {
    CUR_NAME="$1"
    printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
    CUR_BAD=0
    CUR_OUT="$BOOT_OUT"
    CUR_RC=$BOOT_RC
}

# guest 里加载卸载 n 次，MSI 和 INTx 交替，每次跑一轮 selftest。tracepoint
# dma:dma_alloc / dma:dma_free 只记这台设备（按 device 名过滤）上的一致性
# 分配和释放，计数配平说明每次 probe 分配的 DMA 缓冲区都在 remove 里还了。
# 一条 --cmd 装不下这么多命令（guest 串口 FIFO 的上限），所以写成脚本放进镜像。
write_cycle_script() {
cat > "$1" <<'EOF'
# 用法: cycle.sh KO N TAG
ko=$1 n=$2 tag=$3
T=/sys/kernel/tracing
BDF=0000:00:01.0
cd /root/edu || exit 1
echo 0 > $T/tracing_on
echo > $T/trace
for e in dma_alloc dma_free; do
    echo "device == \"$BDF\"" > $T/events/dma/$e/filter
    echo 1 > $T/events/dma/$e/enable
done
echo 1 > $T/tracing_on
dmesg -C
for i in $(seq "$n"); do
    m=msi
    [ $((i % 2)) = 0 ] && m=intx
    insmod "$ko" irq_mode=$m || { echo "$tag-FAIL insmod $i"; break; }
    ./eductl selftest 1 > /dev/null || echo "$tag-FAIL selftest $i $m"
    rmmod edu || { echo "$tag-FAIL rmmod $i"; break; }
done
echo 0 > $T/tracing_on
for e in dma_alloc dma_free; do
    echo 0 > $T/events/dma/$e/enable
    echo 0 > $T/events/dma/$e/filter
done
echo "$tag-ALLOC-$(grep -c ' dma_alloc: ' $T/trace) $tag-FREE-$(grep -c ' dma_free: ' $T/trace)"
echo "$tag-PROBE-MSI-$(dmesg | grep -c '/dev/edu0, irq [0-9]* (MSI)') $tag-PROBE-INTX-$(dmesg | grep -c '/dev/edu0, irq [0-9]* (INTx)')"
# 加载树外、未签名的模块时内核各打一条，和驱动本身无关
dmesg --level=err,warn | grep -v -e 'taints kernel' -e 'module verification failed' > /tmp/warn
echo "$tag-DMESG-WARN-$(wc -l < /tmp/warn)"
sed "s/^/$tag-DMESG /" /tmp/warn
EOF
}

# 驱动占用的每一样资源各打一行。加载时打一次，证明这些检查看得见资源；
# 卸载后再打，证明全部还回去了。pci_request_regions 占的是设备的全部 BAR：
# BAR0 和放 MSI-X table 的 BAR1，所以 /proc/iomem 里有两项。
write_state_script() {
cat > "$1" <<'EOF'
# 用法: state.sh TAG
tag=$1
D=/sys/bus/pci/devices/0000:00:01.0
echo "$tag-MOD-$(grep -c '^edu ' /proc/modules)"
echo "$tag-CDEV-$([ -e /dev/edu0 ] && echo present || echo absent)"
echo "$tag-IRQ-$(grep -c ' edu$' /proc/interrupts)"
echo "$tag-IOMEM-$(grep -c ' : edu$' /proc/iomem)"
echo "$tag-DRV-$([ -e $D/driver ] && basename "$(readlink $D/driver)" || echo none)"
echo "$tag-ENABLE-$(cat $D/enable)"
# command 寄存器（偏移 4）的 bit 2 是 Bus Master；MSI capability 在 0x40，
# Message Control（0x42）的 bit 0 是 MSI Enable
cmd=$(od -An -tu2 -j4 -N2 $D/config | tr -d ' ')
msi=$(od -An -tu2 -j66 -N2 $D/config | tr -d ' ')
echo "$tag-BUSMASTER-$(( (cmd >> 2) & 1 )) $tag-MSIEN-$(( msi & 1 ))"
EOF
}

# 漏掉 remove 里 dma_free_coherent 的驱动副本。按文字定位那一处（前一行
# 是 edu_teardown_irqs，probe 出错路径里的那一处前面是标号），找不到时
# 打印 STALE，对照用例因此失败，而不是悄悄跑一个没改过的驱动。
make_leaky_copy() {
    local dst="$1"
    mkdir -p "$dst"
    cp "$EDU_SRC/Makefile" "$EDU_SRC/edu_ioctl.h" "$ROOT/include/devices/edu_regs.h" "$dst/"
    python3 - "$EDU_SRC/edu.c" "$dst/edu.c" <<'EOF'
import sys
s = open(sys.argv[1]).read()
old = ("\tedu_teardown_irqs(e);\n"
       "\tdma_free_coherent(&pdev->dev, EDU_BUF_SIZE, e->dma_buf, e->dma_handle);\n"
       "\tpci_clear_master(pdev);\n")
if s.count(old) != 1:
    print("STALE: edu_remove no longer matches the leaky-copy pattern")
    sys.exit(1)
open(sys.argv[2], "w").write(s.replace(old, "\tedu_teardown_irqs(e);\n\tpci_clear_master(pdev);\n"))
EOF
}

auto_tests() {
local f
head1 "Arch guest 里编译加载 edu 驱动"

write_cycle_script "$WORK/cycle.sh"
write_state_script "$WORK/state.sh"
make_leaky_copy "$WORK/leak" > "$WORK/leak.log" 2>&1
{
    echo "mkdir /root/edu-leak"
    for f in "$WORK"/leak/*; do
        echo "write $f /root/edu-leak/$(basename "$f")"
    done
} > "$WORK/leak.cmds"
edu_guest_prepare "edu 驱动用例" "$WORK/leak.cmds" "$WORK/cycle.sh" "$WORK/state.sh" || return

BOOT_OUT="$(edu_guest_boot \
    --cmd "cd /root/edu && make 2>&1 | tail -3; echo BUILD-RC-\${PIPESTATUS[0]}" \
    --cmd "echo VERMAGIC-\$([ \"\$(modinfo -F vermagic edu.ko | cut -d' ' -f1)\" = \"\$(uname -r)\" ] && echo match || echo mismatch)" \
    --cmd "insmod edu.ko; echo INSMOD-RC-\$?; dmesg | grep -a 'edu $BDF'" \
    --cmd "bash state.sh LOADED; grep -a ' edu\$' /proc/interrupts" \
    --cmd "./eductl selftest $ROUNDS; echo SELFTEST-MSI-RC-\$?" \
    --cmd "rmmod edu && insmod edu.ko irq_mode=intx; echo INTX-RC-\$?; grep -a ' edu\$' /proc/interrupts" \
    --cmd "./eductl selftest $ROUNDS; echo SELFTEST-INTX-RC-\$?" \
    --cmd "rmmod edu; bash state.sh UNLOADED" \
    --cmd "bash cycle.sh /root/edu/edu.ko $CYCLES CYC" \
    --cmd "bash state.sh AFTER" \
    --cmd "make -C /root/edu-leak modules > /dev/null 2>&1; echo LEAKBUILD-RC-\$?" \
    --cmd "bash cycle.sh /root/edu-leak/edu.ko $LEAK_CYCLES LEAK")"
BOOT_RC=$?

check_case "guest 用 linux-headers 编出模块和 eductl，vermagic 与运行中的内核一致"
expect_rc 0
expect_has "BUILD-RC-0"
expect_has "VERMAGIC-match"
expect_has "guest requested poweroff via ACPI S5"
expect_not "TIMEOUT"
end_case

check_case "探测：映射 BAR0、申请 MSI、注册 /dev/edu0，占用的资源都看得见"
expect_has "INSMOD-RC-0"
expect_re "edu $BDF: /dev/edu0, irq [0-9]+ \\(MSI\\), BAR0 \\[mem 0x[0-9a-f]+-0x[0-9a-f]+\\]"
expect_re "PCI-MSI-$BDF +0-edge +edu"
expect_has "LOADED-MOD-1"
expect_has "LOADED-CDEV-present"
expect_has "LOADED-IRQ-1"
expect_has "LOADED-IOMEM-2"
expect_has "LOADED-DRV-edu"
expect_has "LOADED-ENABLE-1"
expect_has "LOADED-BUSMASTER-1 LOADED-MSIEN-1"
expect_re "pci 00:01\\.0 edu: MSI enabled, address 0xfee[0-9a-f]{5} data 0x[0-9a-f]{4}"
end_case

check_case "MSI：$ROUNDS 轮 DMA 双向搬运经 MMIO 核对，每次完成都经中断确认"
expect_has "selftest: $ROUNDS rounds ok (MSI), irqs +$((4 * ROUNDS)), dma +$((3 * ROUNDS))"
expect_has "SELFTEST-MSI-RC-0"
expect_not "not delivered"
end_case

check_case "INTx：同样的闭环走 IOAPIC 引脚，电平触发"
expect_has "INTX-RC-0"
expect_re "IO-APIC +$INTX_GSI-fasteoi +edu"
expect_has "pci 00:01.0 edu: MSI disabled, interrupts go to INTA"
expect_has "selftest: $ROUNDS rounds ok (INTx), irqs +$((4 * ROUNDS)), dma +$((3 * ROUNDS))"
expect_has "SELFTEST-INTX-RC-0"
end_case

check_case "卸载：模块、字符设备、中断、BAR 占用、Bus Master、MSI Enable 全部归还"
expect_has "UNLOADED-MOD-0"
expect_has "UNLOADED-CDEV-absent"
expect_has "UNLOADED-IRQ-0"
expect_has "UNLOADED-IOMEM-0"
expect_has "UNLOADED-DRV-none"
expect_has "UNLOADED-ENABLE-0"
expect_has "UNLOADED-BUSMASTER-0 UNLOADED-MSIEN-0"
end_case

check_case "加载卸载 $CYCLES 次：每次探测一致，一致性 DMA 分配与释放配平，之后资源全部归还"
expect_has "CYC-ALLOC-$CYCLES CYC-FREE-$CYCLES"
expect_has "CYC-PROBE-MSI-$((CYCLES / 2)) CYC-PROBE-INTX-$((CYCLES / 2))"
expect_has "CYC-DMESG-WARN-0"
expect_not "CYC-FAIL"
expect_has "AFTER-MOD-0"
expect_has "AFTER-CDEV-absent"
expect_has "AFTER-IRQ-0"
expect_has "AFTER-IOMEM-0"
expect_has "AFTER-DRV-none"
expect_has "AFTER-ENABLE-0"
expect_has "AFTER-BUSMASTER-0 AFTER-MSIEN-0"
end_case

check_case "对照：remove 漏掉 dma_free_coherent 的驱动，同一判据下分配和释放配不平"
CUR_OUT="$CUR_OUT
$(cat "$WORK/leak.log")"
expect_not "STALE"
expect_has "LEAKBUILD-RC-0"
expect_has "LEAK-ALLOC-$LEAK_CYCLES LEAK-FREE-0"
expect_not "LEAK-FAIL"
end_case

check_case "VMM 侧没有异常访问的日志"
expect_not "ignored while a command is running"
expect_not "registers take aligned 32-bit access"
expect_not "DMA did not complete"
end_case
}

manual_guide() {
guide "在 Arch guest 里自己编译、加载驱动并跑 selftest" \
"cp --sparse=always arch-image/disk.img /path/to/copy.img
# 把 tools/guest/edu/ 下的文件和 include/devices/edu_regs.h 放进 guest 的同一个目录（例如 scp，或在 host 上用 debugfs -w 写进镜像副本）
./build/vmm --mode linux --smp 2 --mem 1G --device edu \\
    --device ahci,drive=d0 --drive id=d0,file=/path/to/copy.img \\
    --initrd arch-image/initramfs-linux.img \\
    --append \"root=UUID=\$(blkid -p -o value -s UUID /path/to/copy.img) rw\" arch-image/vmlinuz-linux
# guest 里：
make && insmod edu.ko            # irq_mode=intx 改用 INTx
./eductl selftest 100 && ./eductl info" \
"dmesg 有 'edu 0000:00:01.0: /dev/edu0, irq N (MSI)'；selftest 打印 'selftest: 100 rounds ok (MSI), irqs +400, dma +300'"
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
