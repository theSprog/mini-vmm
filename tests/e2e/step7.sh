#!/usr/bin/env bash
# tests/step7.sh — Step 7 验收：传统设备的驱动探测
#
#   Step 7.1  四个传统设备都被探测出来，而且一次超时都没有；guest 自报的
#             串口型号与中断计数也对得上
#   Step 7.2  PIO trace 的结构：PIT 不落用户态、0x87 读回 0xFF、S5 分两次写
#   Step 7.3  退出路径：reboot -f 走 i8042 的 0xFE 脉冲
#
# 这些断言都是“驱动探测走对了分支”的证据，配套 learning-note
# 《07. Linux 眼中的 PC 传统设备》。失败时先看 --trace-pio 的记录。
#
# 用法：
#   VMLINUX=/path/to/vmlinux ./tests/step7.sh        全部
#   VMLINUX=/path/to/vmlinux ./tests/step7.sh -a     只跑自动用例
#   ./tests/step7.sh -m                              只看人工指引
#
# 没设 VMLINUX 时全部 SKIP。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
MEM=512M

TRACE=""
cleanup() { [ -n "$TRACE" ] && rm -f "$TRACE"; }
trap cleanup EXIT

# 同 step2/step6：等 shell 起来后逐条发送，最后按参数决定怎么停机。
# $1 = 停机命令（poweroff -f 或 reboot -f），其余是要执行的命令。
feed_shell() {
    local stop="$1"; shift
    sleep "${FEED_WAIT:-8}"
    local c
    for c in "$@"; do
        printf '%s\n' "$c"
        sleep 1
    done
    printf '%s\n' "$stop"
    sleep 5
}

# ------------------------------------------------------------------
auto_tests() {

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "Step 7 全部用例" "set VMLINUX=/path/to/vmlinux to enable"
    return
fi
if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "initramfs" "tools/build_initramfs.sh failed"
        return
    }
fi

head1 "Step 7.1 — 设备探测的结论"

TRACE="$(mktemp -t mini-vmm-pio.XXXXXX)"

CUR_NAME="四个传统设备都被正确探测，且一次超时都没有"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(feed_shell 'poweroff -f' \
    'echo SER-$(sed -n "s/^0: uart:\([^ ]*\).*/\1/p" /proc/tty/driver/serial)' \
    'echo SER1-$(sed -n "s/^1: uart:\([^ ]*\).*/\1/p" /proc/tty/driver/serial)' \
    'echo SERIRQ-$(sed -n "s/^ *4: *\([0-9]*\).*/\1/p" /proc/interrupts)-END' \
    'echo KBDIRQ-$(sed -n "s/^ *1: *\([0-9]*\).*/\1/p" /proc/interrupts)' \
    'echo RTCIRQ-$(sed -n "s/^ *8: *\([0-9]*\).*/\1/p" /proc/interrupts)' \
    | timeout 90 "$VMM" --mode linux --mem "$MEM" --trace-pio "$TRACE" \
          --initrd "$INITRD" "$VMLINUX" 2>&1)"
CUR_RC=$?
expect_rc 0

# 串口：autoconfig 认成 16550A，IRQ 写死 4
expect_has "ttyS0 at I/O 0x3f8 (irq = 4, base_baud = 115200) is a 16550A"
expect_has "SER-16550A"
# 另外三个 ISA 口没人应答，必须是 unknown 而不是被误认
expect_has "SER1-unknown"
expect_not "ttyS1 at I/O 0x2f8"
# 8250 探测失败时的两条典型日志
expect_not "LSR safety check engaged"
expect_not "detected caps"

# i8042：KBD 口在，AUX 口不在，全程不超时
expect_has "serio: i8042 KBD port at 0x60,0x64 irq 1"
expect_has "AT Translated Set 2 keyboard"
expect_not "i8042 AUX port"
expect_not "Can't read CTR"
expect_not "Unable to get stable CTR read"
expect_not "No controller found"

# RTC：探测通过并把系统时间设上；y3k 说明 FADT 的世纪寄存器索引对上了
expect_has "rtc_cmos rtc_cmos: registered as rtc0"
expect_has "rtc_cmos rtc_cmos: setting system clock to "
expect_has "alarms up to one day, y3k, 114 bytes nvram"
expect_not "Unable to read current time from RTC"
expect_not "broken or not accessible"
expect_not "only 24-hr supported"
expect_not "Reading current time from RTC took around"

# PIT：TSC 校准有三种合法结局，取决于读一次 PIT 有多贵。
#   裸机 KVM（soc63）：quick_pit_calibrate() 成功 -> Fast TSC calibration using PIT
#   嵌套 KVM（WSL2）  ：快路在 500ppm 外推处静默返回 0，慢路接手
#                       -> Using PIT calibration value
#   host 负载大时      ：慢路的 tscmax > 10*tscmin 抽查把三轮全判废
#                       -> Unable to calibrate against PIT，退回 refined-jiffies
# 三种都不是 VMM 的问题（PIT 通道 2 由 KVM 内核模块提供），一并接受。
# 详见 learning-note 卷七第 3 节。
expect_any "tsc: Fast TSC calibration using PIT" \
           "tsc: Using PIT calibration value" \
           "tsc: Unable to calibrate against PIT"

# PCI 不存在是预期结果，不是故障
expect_has "PCI: System does not support PCI"

# 中断确实在走：串口和键盘都有计数，RTC 我们不产生中断
expect_not "SERIRQ-0-END"
expect_not "SERIRQ--END"
# 键盘应答的 9 个字节各产生一次边沿：3（GETID）+ 1×6（F5/ED+00/F3+00/F4）
expect_has "KBDIRQ-9"
expect_has "RTCIRQ-0"

expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
expect_not "stopped with error"
end_case

head1 "Step 7.2 — PIO trace 的结构"

CUR_NAME="trace 里 PIT 端口为空、0x87 读回 0xFF、S5 分两次写"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
# 上一条用例已经把 trace 写出来了，这里只做静态检查。
# 标记串由本地命令替换生成，不会和 trace 内容混淆。
CUR_OUT="$(
    printf 'TRACE-LINES-%s\n' "$(wc -l < "$TRACE")"
    # 0x40-0x43 和 0x61 由 KVM 内核态 PIT 接管（KVM_PIT_SPEAKER_DUMMY），
    # 一条都不该落到用户态
    printf 'PIT-USERSPACE-%s\n' \
        "$(grep -cE ' 004[0-3] | 0061 ' "$TRACE" || true)"
    # 0x87 = DMA_PAGE_0，i8237A_init_ops() 靠读回 0xFF 判定没有 8237
    printf 'DMA0-%s\n' \
        "$(awk '$3 == "0087" { print $4 "-" $5 }' "$TRACE" | tr '\n' ' ')"
    # ACPI S5：先写 SLP_TYP，再写 SLP_TYP|SLP_EN（hwsleep.c 的两次写）
    printf 'S5-%s\n' \
        "$(awk '$3 == "0604" && $4 == "W2" { print $5 }' "$TRACE" \
           | tail -2 | tr '\n' '-')"
)"
CUR_RC=0
expect_rc 0
expect_not "TRACE-LINES-0"
expect_has "PIT-USERSPACE-0"
expect_has "DMA0-R1-ff"
expect_has "S5-1401-3401-"
end_case

head1 "Step 7.3 — 两条退出路径"

CUR_NAME="reboot -f 走 i8042 的 0xFE 脉冲"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(feed_shell 'reboot -f' \
    | timeout 90 "$VMM" --mode linux --mem "$MEM" --trace-pio "$TRACE" \
          --initrd "$INITRD" "$VMLINUX" 2>&1)"
CUR_RC=$?
# trace 的最后一行就是触发退出的那次写，见 vm.c 里 pio_trace() 的位置
CUR_OUT="$CUR_OUT
LAST-$(awk '{ print $2 "-" $3 "-" $4 "-" $5 }' "$TRACE" | tail -1)"
expect_rc 0
expect_has "reboot: Restarting system"
expect_has "guest requested reset via i8042"
expect_has "LAST-i8042-cmd-0064-W1-fe"
expect_not "guest requested poweroff"
end_case

}

# ------------------------------------------------------------------
manual_guide() {

head1 "人工验收"

guide "把一次启动的寄存器序列录下来并逐段读" \
"./build/vmm --mode linux --mem 512M --trace-pio /tmp/pio.txt \\
    --initrd build/initramfs.cpio.gz \$VMLINUX
./tools/pio_decode.py /tmp/pio.txt --summary
./tools/pio_decode.py /tmp/pio.txt --console | less
./tools/pio_decode.py /tmp/pio.txt --dev unhandled --syms \$VMLINUX" \
"--summary: 串口占 98% 左右，没有 0x40-0x43 / 0x61
--console: 每行日志对应的 trace 行号区间，用来定位某条消息前后发生了什么
--dev unhandled --syms: 0x87 来自 i8237A_init_ops，0xcf8 来自 pci_direct_probe
  与 read_pci_config，0x80 来自 native_io_delay"

guide "确认串口是中断驱动而不是轮询" \
"# guest 里：
cat /proc/interrupts | grep ttyS0
cat /proc/tty/driver/serial" \
"IRQ 4 的计数随输出增长；serial 里是 uart:16550A ... RTS|CTS|DTR|DSR|CD
如果 IRQ 4 恒为 0，检查 MCR.OUT2 门控和 IIR 的 THRI 语义"

guide "不模拟 RTC 的代价（改代码后重编，仅供对照）" \
"# 把 src/main.c 里的 rtc_attach(vm) 去掉再编译，然后：
./build/vmm --mode linux --mem 512M --trace-pio /tmp/pio-nortc.txt \\
    --initrd build/initramfs.cpio.gz \$VMLINUX" \
"多出约 8 万次 0x70/0x71 访问、启动慢约 2 秒
日志里出现 Unable to read current time from RTC
和 rtc_cmos rtc_cmos: broken or not accessible"

}

# ------------------------------------------------------------------
need_kvm
need_build

case "${1:-}" in
    -a) auto_tests ;;
    -m) manual_guide ;;
    *)  auto_tests; manual_guide ;;
esac

summary
