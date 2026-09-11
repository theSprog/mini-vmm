#!/usr/bin/env bash
# tests/step2.sh — Step 2 验收
#
#   Step 2.1  8250 UART：THR 输出、SCR 读写、MCR.LOOP 回环
#   Step 2.2  vmlinux ELF 加载 + boot_params + MP/ACPI 表，内核打印横幅
#   Step 2.3  initramfs，busybox shell 交互，poweroff 退出 VMM
#   Step 2.4  bzImage：64 位启动协议 + 解压器 + KASLR，同样进 shell 并关机
#
# 用法：
#   VMLINUX=/path/to/vmlinux BZIMAGE=/path/to/bzImage ./tests/step2.sh   全部
#   VMLINUX=/path/to/vmlinux ./tests/step2.sh -a     只跑自动用例
#   ./tests/step2.sh -m                              只看人工指引
#
# 没设 VMLINUX 时 2.2/2.3 的用例 SKIP，没设 BZIMAGE 时 2.4 SKIP。
# Linux 模式必须带 --initrd；initramfs 不存在时用 tools/build_initramfs.sh 现做。

set -u
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

SERIAL64="$ROOT/build/serial64.bin"
INITRD="$ROOT/build/initramfs.cpio.gz"
VMLINUX="${VMLINUX:-}"
BZIMAGE="${BZIMAGE:-}"
MEM=512M

# 给 guest 喂一串命令：先等 shell 起来，再逐条发送，最后 poweroff。
# 每条之间留点时间，串口 FIFO 只有 256 字节，一次灌太多会丢。
feed_shell() {
    sleep 6
    local c
    for c in "$@"; do
        printf '%s\n' "$c"
        sleep 1
    done
    printf 'poweroff -f\n'
    sleep 5
}

# ------------------------------------------------------------------
auto_tests() {

head1 "Step 2.1 — 8250 UART"

# payload 轮询 LSR.THRE 再写 THR，然后做 SCR 与 MCR.LOOP 自检，
# 两项都过才往调试端口写 'S'（0x53），失败写 'x'（0x78）。
run_case "serial: THR 输出 + SCR + LOOP 自检" \
    "$VMM" --mode long64 "$SERIAL64"
expect_rc 0
expect_has "serial8250 @ PIO 0x3f8, IRQ 4"
expect_has "Hello from guest via 8250 UART"
expect_has "guest wrote port 0x10: 0x53"
expect_not "guest wrote port 0x10: 0x78"
end_case

run_case "--initrd 只能配合 --mode linux" \
    "$VMM" --mode long64 --initrd /dev/null "$SERIAL64"
expect_rc 1
expect_has "require --mode linux"
end_case

run_case "--mode linux 缺少 --initrd 时拒绝启动" \
    "$VMM" --mode linux --mem "$MEM" "$SERIAL64"
expect_rc 1
expect_has "--mode linux requires --initrd"
end_case

if [ ! -f "$INITRD" ]; then
    "$ROOT/tools/build_initramfs.sh" "$INITRD" >/dev/null || {
        skip_case "Step 2.2 / 2.3 / 2.4" "tools/build_initramfs.sh failed"
        return
    }
fi

run_case "既不是 ELF 也不是 bzImage 的内核给出明确报错" \
    "$VMM" --mode linux --mem "$MEM" --initrd "$INITRD" "$SERIAL64"
expect_rc 1
expect_has "is neither an ELF vmlinux nor a bzImage"
end_case

if [ -z "$VMLINUX" ] || [ ! -f "$VMLINUX" ]; then
    skip_case "Step 2.2 / 2.3" "set VMLINUX=/path/to/vmlinux to enable"
else

head1 "Step 2.2 — vmlinux 直启"

# rdinit=/nonexistent：initramfs 照常提供，但内核在里面找不到 init，
# 转去挂载 root=，没有 root= 就以 VFS panic 结束；panic=-1 触发 reboot，
# reboot=k 走 i8042 reset，VMM 据此正常退出。这一条把整条启动路径
# 和"guest 能让 VMM 退出"一起验了。
run_case "内核启动到 VFS panic 并经 i8042 reset 退出" \
    timeout 60 "$VMM" --mode linux --mem "$MEM" --initrd "$INITRD" \
        --append "rdinit=/nonexistent" "$VMLINUX"
expect_rc 0
expect_has "kernel format: vmlinux (ELF)"
expect_has "Linux version"
expect_has "BIOS-e820: [mem 0x0000000000100000-"
expect_has "ACPI: Using ACPI (MADT) for SMP configuration information"
expect_has "APIC: Switch to symmetric I/O mode setup"
# 快速 PIT 校准要求读 PIT 的抖动外推后低于 500 ppm，VM exit 偶尔被 host
# 打断就会失败并悄悄退回慢速 PIT 校准（实测约三成）；两条都是 PIT，都算对
expect_any "tsc: Fast TSC calibration using PIT" "tsc: Using PIT calibration value"
expect_has "tsc: Detected"
expect_has "rtc_cmos rtc_cmos: setting system clock"
expect_has "VFS: Unable to mount root fs"
expect_has "guest requested reset via i8042"
# i8042：控制器命令全有应答，KBD 口注册成功，AUX 口被快速判定不存在
expect_has "serio: i8042 KBD port at 0x60,0x64 irq 1"
expect_has "input: AT Translated Set 2 keyboard"
expect_not "Can't read CTR"
expect_not "i8042: probe of i8042 failed"
expect_not "AUX port"
# 没有 PIT gate（0x61）时 TSC 校准会间歇失败，jiffies 停住
expect_not "Unable to read current time from RTC"
expect_not "SHUTDOWN"
end_case

head1 "Step 2.3 — initramfs + busybox shell"

CUR_NAME="initramfs: shell 交互 + poweroff"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(feed_shell 'echo MARK-$((6*7))' 'cat /proc/interrupts' \
           | timeout 60 "$VMM" --mode linux --mem "$MEM" \
                 --initrd "$INITRD" "$VMLINUX" 2>&1)"
CUR_RC=$?
expect_rc 0
expect_has "Run /init as init process"
expect_has "mini-vmm initramfs: Linux 6."
# 42 只能来自 guest shell 真的执行了算术展开，回显的命令行里只有 6*7
expect_has "MARK-42"
expect_has "IO-APIC   4-edge      ttyS0"
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
end_case

fi  # VMLINUX

if [ -z "$BZIMAGE" ] || [ ! -f "$BZIMAGE" ]; then
    skip_case "Step 2.4" "set BZIMAGE=/path/to/bzImage to enable"
    return
fi

head1 "Step 2.4 — bzImage"

# 同一套 boot_params/E820/ACPI，只是入口换成解压器：VMM 原样拷 setup
# header、装保护模式部分、从装载地址 + 0x200 进入，其余交给解压器
CUR_NAME="bzImage: 解压 + shell 交互 + poweroff"
printf '%s->%s %s\n' "$C_CYA" "$C_RST" "$CUR_NAME"
CUR_BAD=0
CUR_OUT="$(feed_shell 'echo MARK-$((6*7))' \
           | timeout 60 "$VMM" --mode linux --mem "$MEM" \
                 --initrd "$INITRD" "$BZIMAGE" 2>&1)"
CUR_RC=$?
expect_rc 0
expect_has "kernel format: bzImage"
expect_has "bzImage: protocol 2."
# 解压器自己的输出（earlyprintk=serial 对解压器同样有效）
expect_has "Decompressing Linux..."
expect_has "Booting the kernel"
expect_has "Run /init as init process"
expect_has "MARK-42"
expect_has "guest requested poweroff via ACPI S5"
expect_not "Kernel panic"
end_case

run_case "bzImage: VFS panic 并经 i8042 reset 退出" \
    timeout 60 "$VMM" --mode linux --smp 2 --mem "$MEM" --initrd "$INITRD" \
        --append "rdinit=/nonexistent" "$BZIMAGE"
expect_rc 0
expect_has "kernel format: bzImage"
expect_has "smp: Brought up 1 node, 2 CPUs"
expect_has "VFS: Unable to mount root fs"
expect_has "guest requested reset via i8042"
expect_not "SHUTDOWN"
end_case

}

# ------------------------------------------------------------------
manual_guide() {

head1 "人工验收"

guide "交互式 shell" \
"./tools/build_initramfs.sh
./build/vmm --mode linux --mem 512M --initrd build/initramfs.cpio.gz \$VMLINUX" \
"看到 [guest /]# 提示符后可以正常输入命令；Ctrl-C 只打断 guest 里的
前台命令，不会杀掉 VMM；Ctrl-A x 立即退出 VMM 并打印 vCPU 现场；
poweroff -f 正常退出，终端恢复到 cooked 模式。"

guide "观察 exit 分布" \
"sudo perf kvm stat record -- ./build/vmm --mode linux --mem 512M --initrd build/initramfs.cpio.gz \$VMLINUX
sudo perf kvm stat report --event=ioport" \
"IO exit 主要集中在 0x3f8-0x3ff（串口，每个字符若干次）。
0x70/0x71 的 exit 应只有几十次（RTC 模拟之前是约 4 万次）。"

guide "对照 MP table 路径" \
"# guest 内核开了 CONFIG_X86_MPPARSE 时：
./build/vmm --mode linux --mem 512M --initrd build/initramfs.cpio.gz --append acpi=off \$VMLINUX" \
"应看到 'Intel MultiProcessor Specification v1.4' 与
'MPTABLE: OEM ID: MINIVMM'，而不是 virtual wire 模式。
内核没开 MPPARSE 时这条会退回 virtual wire（预期如此）。"

}

# ------------------------------------------------------------------
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
